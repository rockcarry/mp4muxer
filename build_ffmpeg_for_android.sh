#!/bin/bash
set -e

EXTRA_CFLAGS="-I$PWD/ffmpeg-android/include -march=armv7-a -Os -ffast-math -mfpu=neon-vfpv4 -mfloat-abi=softfp -DANDROID -DNDEBUG"
EXTRA_LDFLAGS="-L$PWD/ffmpeg-android/lib -march=armv7-a"

#++ build x264 ++#
if [ ! -d x264 ]; then
    git clone git://git.videolan.org/x264.git
fi
cd x264
./configure --prefix=$PWD/../ffmpeg-android \
--enable-strip \
--enable-static \
--enable-shared \
--host=arm-linux-androideabi \
--cross-prefix=arm-linux-androideabi- \
--extra-cflags="$EXTRA_CFLAGS" \
--extra-ldflags="$EXTRA_LDFLAGS"
make STRIP= -j8 && make install
cd -
#-- build x264 --#

if [ ! -d ffmpeg ]; then
  git clone git://source.ffmpeg.org/ffmpeg.git
fi
cd ffmpeg
./configure \
--pkg-config=pkg-config \
--arch=armv7 \
--cpu=armv7-a \
--target-os=android \
--enable-cross-compile \
--cross-prefix=arm-linux-androideabi- \
--prefix=$PWD/../ffmpeg-android \
--enable-static \
--enable-small \
--disable-shared \
--disable-symver \
--disable-debug \
--disable-programs \
--disable-doc \
--disable-avdevice \
--disable-avfilter \
--disable-swscale \
--disable-swresample \
--disable-postproc \
--disable-everything \
--disable-swscale-alpha \
--enable-encoder=libx264 \
--enable-encoder=aac \
--enable-muxer=mp4 \
--enable-protocol=file \
--enable-protocol=rtmp \
--enable-asm \
--enable-gpl \
--enable-version3 \
--enable-nonfree \
--enable-libx264 \
--extra-cflags="$EXTRA_CFLAGS" \
--extra-ldflags="$EXTRA_LDFLAGS"

make -j8 && make install

cd -

#rm -rf x264
#rm -rf ffmpeg

