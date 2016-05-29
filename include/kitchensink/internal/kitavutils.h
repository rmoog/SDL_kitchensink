#ifndef KITAVUTILS_H
#define KITAVUTILS_H

#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <libavutil/pixfmt.h>
#include <stdbool.h>

void Kit_FindPixelFormat(enum AVPixelFormat fmt, unsigned int *out_fmt);
void Kit_FindAudioFormat(enum AVSampleFormat fmt, int *bytes, bool *is_signed, unsigned int *format);
enum AVPixelFormat Kit_FindAVPixelFormat(unsigned int fmt);
enum AVSampleFormat Kit_FindAVSampleFormat(int format);
unsigned int Kit_FindAVChannelLayout(int channels);
double Kit_GetSystemTime();
bool Kit_AttachmentIsFont(AVStream *stream);

#endif // KITAVUTILS_H
