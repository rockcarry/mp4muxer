LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := mp4muxer

LOCAL_MODULE_TAGS := optional

LOCAL_MODULE_PATH := $(TARGET_OUT)/bin

LOCAL_SRC_FILES := \
    mp4muxer.c

LOCAL_SHARED_LIBRARIES := \
    libutils \
    libcutils \
    libui \
    libgui \
    libandroid_runtime

#++ for ffmpeg library
LOCAL_CFLAGS += \
    -D__STDC_CONSTANT_MACROS \
    -DTEST_MP4MUXER

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/ffmpeg-android/include

LOCAL_LDFLAGS += -ldl \
    $(LOCAL_PATH)/ffmpeg-android/lib/libavformat.a \
    $(LOCAL_PATH)/ffmpeg-android/lib/libavcodec.a \
    $(LOCAL_PATH)/ffmpeg-android/lib/libavutil.a \
    $(LOCAL_PATH)/ffmpeg-android/lib/libx264.a
#-- for ffmpeg library

LOCAL_MULTILIB := 32

include $(BUILD_EXECUTABLE)



include $(CLEAR_VARS)

LOCAL_MODULE := libmp4muxer

LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := \
    mp4muxer.c

LOCAL_SHARED_LIBRARIES := \
    libutils \
    libcutils \
    libui \
    libgui \
    libandroid_runtime

#++ for ffmpeg library
LOCAL_CFLAGS += \
    -D__STDC_CONSTANT_MACROS

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/ffmpeg-android/include

LOCAL_LDFLAGS += -ldl \
    $(LOCAL_PATH)/ffmpeg-android/lib/libavformat.a \
    $(LOCAL_PATH)/ffmpeg-android/lib/libavcodec.a \
    $(LOCAL_PATH)/ffmpeg-android/lib/libavutil.a \
    $(LOCAL_PATH)/ffmpeg-android/lib/libx264.a
#-- for ffmpeg library

LOCAL_MULTILIB := 32

include $(BUILD_SHARED_LIBRARY)



