#include "kitchensink/internal/kitavutils.h"

#include <SDL2/SDL.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>

static const char * const font_mime[] = {
    "application/x-font-ttf",
    "application/x-font-truetype",
    "application/x-truetype-font",
    "application/x-font-opentype",
    "application/vnd.ms-opentype",
    "application/font-sfnt",
    NULL
};

bool Kit_AttachmentIsFont(AVStream *stream) {
    AVDictionaryEntry *tag = av_dict_get(stream->metadata, "mimetype", NULL, AV_DICT_MATCH_CASE);
    if(tag) {
        for(int n = 0; font_mime[n]; n++) {
            if(av_strcasecmp(font_mime[n], tag->value) == 0) {
                return true;
            }
        }
    }
    return false;
}

void Kit_FindPixelFormat(enum AVPixelFormat fmt, unsigned int *out_fmt) {
    switch(fmt) {
        case AV_PIX_FMT_YUV420P9:
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUV420P14:
        case AV_PIX_FMT_YUV420P16:
        case AV_PIX_FMT_YUV420P:
            *out_fmt = SDL_PIXELFORMAT_YV12;
            break;
        case AV_PIX_FMT_YUYV422:
            *out_fmt = SDL_PIXELFORMAT_YUY2;
            break;
        case AV_PIX_FMT_UYVY422:
            *out_fmt = SDL_PIXELFORMAT_UYVY;
            break;
        default:
            *out_fmt = SDL_PIXELFORMAT_ABGR8888;
            break;
    }
}

void Kit_FindAudioFormat(enum AVSampleFormat fmt, int *bytes, bool *is_signed, unsigned int *format) {
    switch(fmt) {
        case AV_SAMPLE_FMT_U8:
            *bytes = 1;
            *is_signed = false;
            *format = AUDIO_U8;
            break;
        case AV_SAMPLE_FMT_S16:
            *bytes = 2;
            *is_signed = true;
            *format = AUDIO_S16SYS;
            break;
        case AV_SAMPLE_FMT_S32:
            *bytes = 4;
            *is_signed = true;
            *format = AUDIO_S32SYS;
            break;
        default:
            *bytes = 2;
            *is_signed = true;
            *format = AUDIO_S16SYS;
            break;
    }
}

enum AVPixelFormat Kit_FindAVPixelFormat(unsigned int fmt) {
    switch(fmt) {
        case SDL_PIXELFORMAT_IYUV: return AV_PIX_FMT_YUV420P;
        case SDL_PIXELFORMAT_YV12: return AV_PIX_FMT_YUV420P;
        case SDL_PIXELFORMAT_YUY2: return AV_PIX_FMT_YUYV422;
        case SDL_PIXELFORMAT_UYVY: return AV_PIX_FMT_UYVY422;
        case SDL_PIXELFORMAT_ARGB8888: return AV_PIX_FMT_BGRA;
        case SDL_PIXELFORMAT_ABGR8888: return AV_PIX_FMT_RGBA;
        default:
            return AV_PIX_FMT_NONE;
    }
}

enum AVSampleFormat Kit_FindAVSampleFormat(int format) {
    switch(format) {
        case AUDIO_U8: return AV_SAMPLE_FMT_U8;
        case AUDIO_S16SYS: return AV_SAMPLE_FMT_S16;
        case AUDIO_S32SYS: return AV_SAMPLE_FMT_S32;
        default:
            return AV_SAMPLE_FMT_NONE;
    }
}

unsigned int Kit_FindAVChannelLayout(int channels) {
    switch(channels) {
        case 1: return AV_CH_LAYOUT_MONO;
        case 2: return AV_CH_LAYOUT_STEREO;
        case 4: return AV_CH_LAYOUT_QUAD;
        case 6: return AV_CH_LAYOUT_5POINT1;
        default: return AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
}

double Kit_GetSystemTime() {
    return (double)av_gettime() / 1000000.0;
}
