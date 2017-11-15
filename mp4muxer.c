// 包含头文件
#define LOG_TAG "camdev"
#include <utils/Log.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include "mp4muxer.h"

#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

// 内部类型定义
typedef struct
{
    MP4MUXER_PARAMS   params;

    AVStream         *astream;
    AVStream         *vstream;
    AVFormatContext  *ofctxt;
    AVCodec          *acodec;
    AVCodec          *vcodec;

    int               have_audio;
    int               have_video;

    #define MP4MUXER_TS_EXIT  (1 << 0)
    int               thread_state;

    //++ for packet queue
    #define PKT_QUEUE_SIZE 128
    AVPacket          pktq_b[PKT_QUEUE_SIZE]; // packets
    AVPacket         *pktq_f[PKT_QUEUE_SIZE]; // free packets queue
    AVPacket         *pktq_w[PKT_QUEUE_SIZE]; // used packets queue
    sem_t             pktq_semf;  // free
    sem_t             pktq_semw;  // used
    int               pktq_headf;
    int               pktq_tailf;
    int               pktq_headw;
    int               pktq_tailw;
    pthread_t         pktq_thread_id;
    pthread_mutex_t   pktq_mutex;
    //-- for packet queue
} mp4muxer;

// 内部全局变量定义
static MP4MUXER_PARAMS DEF_MP4MUXER_PARAMS =
{
    // output params
    (char*)"/sdcard/test.mp4",  // filename
    22050,                      // audio_bitrate
    AV_CH_LAYOUT_MONO,          // audio_channel_layout
    22050,                      // audio_sample_rate
    256000,                     // video_bitrate
    320,                        // video_width
    240,                        // video_height
    25,                         // video_frame_rate
};

// 内部函数实现
//++ video packet writing queue
static AVPacket* avpacket_dequeue(mp4muxer *muxer)
{
    AVPacket *pkt = NULL;
    sem_wait(&muxer->pktq_semf);
    pthread_mutex_lock  (&muxer->pktq_mutex);
    pkt = muxer->pktq_f[muxer->pktq_headf];
    if (++muxer->pktq_headf == PKT_QUEUE_SIZE) {
        muxer->pktq_headf = 0;
    }
    pthread_mutex_unlock(&muxer->pktq_mutex);
    return pkt;
}

static void avpacket_enqueue(mp4muxer *muxer, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;
    pthread_mutex_lock  (&muxer->pktq_mutex);
    muxer->pktq_w[muxer->pktq_tailw] = pkt;
    if (++muxer->pktq_tailw == PKT_QUEUE_SIZE) {
        muxer->pktq_tailw = 0;
    }
    pthread_mutex_unlock(&muxer->pktq_mutex);
    sem_post(&muxer->pktq_semw);
}
//-- video packet writing queue

static void* packet_thread_proc(void *param)
{
    mp4muxer *muxer  = (mp4muxer*)param;
    AVPacket *packet = NULL;
    int       ret;
    int       i;

    while (1) {
        if (0 != sem_trywait(&muxer->pktq_semw)) {
            if (muxer->thread_state & MP4MUXER_TS_EXIT) {
                break;
            } else {
                usleep(10*1000);
                continue;
            }
        }

        // dequeue packet from pktq_w
        packet = muxer->pktq_w[muxer->pktq_headw];
        if (++muxer->pktq_headw == PKT_QUEUE_SIZE) {
            muxer->pktq_headw = 0;
        }

        // write packet
        av_interleaved_write_frame(muxer->ofctxt, packet);

        // enqueue packet to pktq_f
        muxer->pktq_f[muxer->pktq_tailf] = packet;
        if (++muxer->pktq_tailf == PKT_QUEUE_SIZE) {
            muxer->pktq_tailf = 0;
        }

        sem_post(&muxer->pktq_semf);
    }

    return NULL;
}

static int add_astream(mp4muxer *muxer)
{
    enum AVCodecID  codec_id = muxer->ofctxt->oformat->audio_codec;
    AVCodecContext *c        = NULL;
    int i;

    if (codec_id == AV_CODEC_ID_NONE) return 0;

    muxer->acodec = avcodec_find_encoder(codec_id);
    if (!muxer->acodec) {
        printf("could not find muxer for '%s'\n", avcodec_get_name(codec_id));
        return -1;
    }

    muxer->astream = avformat_new_stream(muxer->ofctxt, muxer->acodec);
    if (!muxer->astream) {
        printf("could not allocate stream\n");
        return -1;
    }

    muxer->astream->id = muxer->ofctxt->nb_streams - 1;
    c                  = muxer->astream->codec;

    c->sample_fmt  = muxer->acodec->sample_fmts ? muxer->acodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    c->bit_rate    = muxer->params.audio_bitrate;
    c->sample_rate = muxer->params.audio_sample_rate;
    if (muxer->acodec->supported_samplerates)
    {
        c->sample_rate = muxer->acodec->supported_samplerates[0];
        for (i=0; muxer->acodec->supported_samplerates[i]; i++) {
            if (muxer->acodec->supported_samplerates[i] == muxer->params.audio_sample_rate)
                c->sample_rate = muxer->params.audio_sample_rate;
        }
    }

    c->channel_layout = muxer->params.audio_channel_layout;
    if (muxer->acodec->channel_layouts)
    {
        c->channel_layout = muxer->acodec->channel_layouts[0];
        for (i=0; muxer->acodec->channel_layouts[i]; i++) {
            if ((int)muxer->acodec->channel_layouts[i] == muxer->params.audio_channel_layout) {
                c->channel_layout = muxer->params.audio_channel_layout;
            }
        }
    }
    c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
    muxer->astream->time_base.num = 1;
    muxer->astream->time_base.den = c->sample_rate;

    /* some formats want stream headers to be separate. */
    if (muxer->ofctxt->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    muxer->have_audio = 1;
    return 0;
}

static int add_vstream(mp4muxer *muxer)
{
    enum AVCodecID  codec_id = muxer->ofctxt->oformat->video_codec;
    AVCodecContext *c        = NULL;

    if (codec_id == AV_CODEC_ID_NONE) return 0;

    muxer->vcodec = avcodec_find_encoder(codec_id);
    if (!muxer->vcodec) {
        printf("could not find muxer for '%s'\n", avcodec_get_name(codec_id));
        return -1;
    }

    muxer->vstream = avformat_new_stream(muxer->ofctxt, muxer->vcodec);
    if (!muxer->vstream) {
        printf("could not allocate stream\n");
        return -1;
    }

    muxer->vstream->id = muxer->ofctxt->nb_streams - 1;
    c                  = muxer->vstream->codec;

    c->codec_id = codec_id;
    c->bit_rate = muxer->params.video_bitrate;
    /* Resolution must be a multiple of two. */
    c->width    = muxer->params.video_width;
    c->height   = muxer->params.video_height;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    muxer->vstream->time_base.num = 1;
    muxer->vstream->time_base.den = muxer->params.video_frame_rate;
    c->time_base = muxer->vstream->time_base;
    c->gop_size  = muxer->params.video_frame_rate / 1;
    c->pix_fmt   = AV_PIX_FMT_YUV420P;
    if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        /* just for testing, we also add B frames */
        c->max_b_frames = 2;
    }
    if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        /* needed to avoid using macroblocks in which some coeffs overflow.
         * This does not happen with normal video, it just happens here as
         * the motion of the chroma plane does not match the luma plane. */
        c->mb_decision = 2;
    }
    if (c->codec_id == AV_CODEC_ID_MJPEG) {
        c->pix_fmt = AV_PIX_FMT_YUVJ420P;
    }

    /* some formats want stream headers to be separate. */
    if (muxer->ofctxt->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    muxer->have_video = 1;
    return 0;
}

static void open_audio(mp4muxer *muxer)
{
    AVCodec        *codec = muxer->acodec;
    AVCodecContext *c     = muxer->astream->codec;
    int             ret;

    /* open it */
    ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
        printf("could not open audio codec !\n");
        exit(1);
    }
}

static void open_video(mp4muxer *muxer)
{
    AVCodec        *codec = muxer->vcodec;
    AVCodecContext *c     = muxer->vstream->codec;
    AVDictionary   *param = NULL;
    int             ret;

    if (c->codec_id == AV_CODEC_ID_H264) {
        av_dict_set(&param, "preset" , "fast"    , 0);
        av_dict_set(&param, "profile", "baseline", 0);
    }

    /* open the codec */
    ret = avcodec_open2(c, codec, &param);
    if (ret < 0) {
        printf("could not open video codec !\n");
        exit(1);
    }
}

static void close_astream(mp4muxer *muxer)
{
    avcodec_close(muxer->astream->codec);
}

static void close_vstream(mp4muxer *muxer)
{
    avcodec_close(muxer->vstream->codec);
}

static void ffplayer_log_callback(void* ptr, int level, const char *fmt, va_list vl) {
    ptr = ptr;
    if (level <= av_log_get_level()) {
        char str[1024];
        vsprintf(str, fmt, vl);
        ALOGD("%s", str);
    }
}

// 函数实现
void* mp4muxer_init(MP4MUXER_PARAMS *params)
{
    char *str;
    int   len;
    int   ret;
    int   i;

    // allocate context for mp4muxer
    mp4muxer *muxer = (mp4muxer*)calloc(1, sizeof(mp4muxer));
    if (!muxer) {
        return NULL;
    }

    // using default params if not set
    if (!params                      ) params                       =&DEF_MP4MUXER_PARAMS;
    if (!params->filename            ) params->filename             = DEF_MP4MUXER_PARAMS.filename;
    if (!params->audio_bitrate       ) params->audio_bitrate        = DEF_MP4MUXER_PARAMS.audio_bitrate;
    if (!params->audio_channel_layout) params->audio_channel_layout = DEF_MP4MUXER_PARAMS.audio_channel_layout;
    if (!params->audio_sample_rate   ) params->audio_sample_rate    = DEF_MP4MUXER_PARAMS.audio_sample_rate;
    if (!params->video_bitrate       ) params->video_bitrate        = DEF_MP4MUXER_PARAMS.video_bitrate;
    if (!params->video_width         ) params->video_width          = DEF_MP4MUXER_PARAMS.video_width;
    if (!params->video_height        ) params->video_height         = DEF_MP4MUXER_PARAMS.video_height;
    if (!params->video_frame_rate    ) params->video_frame_rate     = DEF_MP4MUXER_PARAMS.video_frame_rate;

    memcpy(&muxer->params, params, sizeof(MP4MUXER_PARAMS));

    /* initialize libavcodec, and register all codecs and formats. */
    av_register_all();

    // init network
    avformat_network_init();

    // setup log
//  av_log_set_level(AV_LOG_WARNING);
//  av_log_set_callback(ffplayer_log_callback);

    /* allocate the output media context */
    avformat_alloc_output_context2(&muxer->ofctxt, NULL, NULL, params->filename);
    if (!muxer->ofctxt)
    {
        printf("could not deduce output format from file extension: using MPEG.\n");
        goto failed;
    }

    if (1) { // force using aac & h264 muxer
        muxer->ofctxt->oformat->audio_codec = AV_CODEC_ID_AAC;
        muxer->ofctxt->oformat->video_codec = AV_CODEC_ID_H264;
    }

    /* add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (add_astream(muxer) < 0)
    {
        printf("failed to add audio stream.\n");
        goto failed;
    }

    if (add_vstream(muxer) < 0)
    {
        printf("failed to add video stream.\n");
        goto failed;
    }

    // for packet queue
    pthread_mutex_init(&muxer->pktq_mutex, NULL  );
    sem_init(&muxer->pktq_semf, 0, PKT_QUEUE_SIZE);
    sem_init(&muxer->pktq_semw, 0, 0             );
    for (i=0; i<PKT_QUEUE_SIZE; i++) {
        muxer->pktq_f[i] = &muxer->pktq_b[i];
    }

    // create video packet writing thread
    pthread_create(&muxer->pktq_thread_id, NULL, packet_thread_proc, muxer);

    /* now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (muxer->have_audio) open_audio(muxer);
    if (muxer->have_video) open_video(muxer);

    /* open the output file, if needed */
    if (!(muxer->ofctxt->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary *param = NULL;
//      av_dict_set_int(&param, "blocksize", 128*1024, AV_OPT_FLAG_ENCODING_PARAM);
        ret = avio_open2(&muxer->ofctxt->pb, params->filename, AVIO_FLAG_WRITE, NULL, &param);
        if (ret < 0) {
            printf("could not open '%s' !\n", params->filename);
            goto failed;
        }
    }

    /* write the stream header, if any. */
    ret = avformat_write_header(muxer->ofctxt, NULL);
    if (ret < 0) {
        printf("error occurred when opening output file !\n");
        goto failed;
    }

    // successed
    return muxer;

failed:
    mp4muxer_free(muxer);
    return NULL;
}

void mp4muxer_free(void *ctxt)
{
    int       i;
    mp4muxer *muxer = (mp4muxer*)ctxt;
    if (!ctxt) return;

    /* close each codec. */
    if (muxer->have_audio) close_astream(muxer);
    if (muxer->have_video) close_vstream(muxer);

    muxer->thread_state |= MP4MUXER_TS_EXIT;
    pthread_join(muxer->pktq_thread_id, NULL);
    pthread_mutex_destroy(&muxer->pktq_mutex);
    sem_destroy(&muxer->pktq_semf);
    sem_destroy(&muxer->pktq_semw);
    for (i=0; i<PKT_QUEUE_SIZE; i++) {
        av_packet_unref(&muxer->pktq_b[i]);
    }

    /* write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    av_write_trailer(muxer->ofctxt);

    /* close the output file. */
    if (!(muxer->ofctxt->oformat->flags & AVFMT_NOFILE)) avio_close(muxer->ofctxt->pb);

    /* free the stream */
    avformat_free_context(muxer->ofctxt);

    // free muxer context
    free(muxer);

    // deinit network
    avformat_network_deinit();
}

int mp4muxer_audio(void *ctxt, int flags, void *data, int size, int64_t pts)
{
    mp4muxer  *muxer  = (mp4muxer*)ctxt;
    AVRational tbms   = { 1, 1000 };
    AVPacket  *packet = NULL;
    packet = avpacket_dequeue(muxer);
    av_new_packet(packet, size);
    memcpy(packet->data, data, size);
    packet->flags |= flags;
    packet->pts    = pts;
    packet->dts    = pts;
    avpacket_enqueue(muxer, &tbms, muxer->astream, packet);
    return 0;
}

int mp4muxer_video(void *ctxt, int flags, void *data, int size, int64_t pts)
{
    mp4muxer  *muxer  = (mp4muxer*)ctxt;
    AVRational tbms   = { 1, 1000 };
    AVPacket  *packet = NULL;
    packet = avpacket_dequeue(muxer);
    av_new_packet(packet, size);
    memcpy(packet->data, data, size);
    packet->flags |= flags;
    packet->pts    = pts;
    packet->dts    = pts;
    avpacket_enqueue(muxer, &tbms, muxer->vstream, packet);
    return 0;
}

#if TEST_MP4MUXER
static uint8_t g_test_data[1024];
int main(void)
{
    void *muxer = mp4muxer_init(NULL);
    int   i;
    for (i=0; i<100; i++) {
        mp4muxer_audio(muxer, 0, g_test_data, sizeof(g_test_data), i * 40);
        mp4muxer_video(muxer, 0, g_test_data, sizeof(g_test_data), i * 40);
    }
    mp4muxer_free(muxer);
    return 0;
}
#endif
