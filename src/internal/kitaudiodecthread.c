#include "kitchensink/internal/kitaudiodecthread.h"
#include "kitchensink/internal/kitdecthread.h"
#include "kitchensink/internal/kitavutils.h"
#include "kitchensink/internal/kitringbuffer.h"
#include "kitchensink/internal/kitbuffer.h"
#include "kitchensink/kitformats.h"
#include "kitchensink/kiterror.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/avstring.h>

#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

#define AUDIO_SYNC_THRESHOLD 0.05

typedef struct Kit_AudioDecThread {
    Kit_AudioFormat format;
    AVFrame *tmp_frame;
    struct SwrContext *swr;
} Kit_AudioDecThread;

typedef struct Kit_AudioPacket {
    double pts;
    size_t original_size;
    Kit_RingBuffer *rb;
} Kit_AudioPacket;

static void _FreeAVPacket(void *ptr) {
    AVPacket *packet = ptr;
    av_packet_unref(packet);
    free(packet);
}

static Kit_AudioPacket* _CreateAudioPacket(const char* data, size_t len, double pts) {
    Kit_AudioPacket *p = calloc(1, sizeof(Kit_AudioPacket));
    p->rb = Kit_CreateRingBuffer(len);
    Kit_WriteRingBuffer(p->rb, data, len);
    p->pts = pts;
    return p;
}

static void _FreeAudioPacket(void *ptr) {
    Kit_AudioPacket *packet = ptr;
    Kit_DestroyRingBuffer(packet->rb);
    free(packet);
}

static void _FreeAudioDecoderThread(void *ptr) {
    if(ptr == NULL) return;
    Kit_AudioDecThread *athread = ptr;
    av_frame_free(&athread->tmp_frame);
    swr_free(&athread->swr);
    free(athread);
}

static double _GetPacketPTS(void *ptr) {
    Kit_AudioPacket *packet = ptr;
    return packet->pts;
}

static int _HandleAudioPacket(Kit_DecoderThread *thread, void *local) {
    assert(thread != NULL);
    assert(local != NULL);

    int frame_finished;
    int len, len2;
    int dst_linesize;
    int dst_nb_samples, dst_bufsize;
    unsigned char **dst_data;
    AVPacket *packet = NULL;
    Kit_AudioDecThread *athread = local;

    // Read a packet from queue.
    if(Kit_ThreadReadInput(thread, (void**)&packet) != 0) {
        return 0;
    }

    // Handle packet while there is more data
    while(packet->size > 0) {
        len = avcodec_decode_audio4(thread->codec_ctx, athread->tmp_frame, &frame_finished, packet);
        if(len < 0) {
            _FreeAVPacket(packet);
            return 1;
        }

        if(frame_finished) {
            dst_nb_samples = av_rescale_rnd(
                athread->tmp_frame->nb_samples,
                athread->format.samplerate,
                thread->codec_ctx->sample_rate,
                AV_ROUND_UP);

            av_samples_alloc_array_and_samples(
                &dst_data,
                &dst_linesize,
                athread->format.channels,
                dst_nb_samples,
                Kit_FindAVSampleFormat(athread->format.format),
                0);

            len2 = swr_convert(
                athread->swr,
                dst_data,
                athread->tmp_frame->nb_samples,
                (const unsigned char **)athread->tmp_frame->extended_data,
                athread->tmp_frame->nb_samples);

            dst_bufsize = av_samples_get_buffer_size(
                &dst_linesize,
                athread->format.channels,
                len2,
                Kit_FindAVSampleFormat(athread->format.format), 1);

            // Get pts
            double pts = 0;
            if(packet->dts != AV_NOPTS_VALUE) {
                pts = av_frame_get_best_effort_timestamp(athread->tmp_frame);
                pts *= av_q2d(thread->stream->time_base);
            }

            // Just seeked, set sync clock & pos.
            /*if(player->seek_flag == 1) {
                player->vclock_pos = pts;
                player->clock_sync = _GetSystemTime() - pts;
                player->seek_flag = 0;
            }*/

            fprintf(stderr, "Audio: Got here!\n");

            // Lock, write to audio buffer, unlock
            Kit_AudioPacket *apacket = _CreateAudioPacket((char*)dst_data[0], (size_t)dst_bufsize, pts);
            if(Kit_ThreadWriteOutput(thread, apacket) != 0) {
                _FreeAudioPacket(apacket);
            }

            av_freep(&dst_data[0]);
            av_freep(&dst_data);
        }

        packet->size -= len;
        packet->data += len;
        fprintf(stderr, "Audio: Handled %d, left %d\n", len, packet->size);
    }
    _FreeAVPacket(packet);
    return 0;
}


Kit_DecoderThread* Kit_CreateAudioDecoderThread(const Kit_Source *src, int stream_index) {
    Kit_DecoderThread *thread = NULL;
    Kit_AudioDecThread *adec = NULL;

    adec = calloc(1, sizeof(Kit_AudioDecThread));
    if(adec == NULL) {
        Kit_SetError("Unable to allocate audio decoder thread");
        return NULL;
    }

    // Create the thread for decoding
    thread = Kit_CreateDecoderThread(
        src, // Source contexts
        stream_index, // Stream index we are playing
        _FreeAVPacket, // For freeing the input buffer AVPackets
        _FreeAudioPacket, // For freeing the output buffer frames
        3, // Input buffer size (in packets)
        64, // Output buffer size (in packets)
        _GetPacketPTS, // for getting a Presentation TimeStamp from a packet
        _HandleAudioPacket, // Function for decoding video packets
        _FreeAudioDecoderThread, // For freeing video decoder contexts
        adec // Video decoder local struct
    );
    if(thread == NULL) {
        free(adec);
        return NULL;
    }
    
    // Create a temporary frame
    adec->tmp_frame = av_frame_alloc();
    if(adec->tmp_frame == NULL) {
        Kit_SetError("Unable to initialize temporary audio frame");
        goto error;
    }

    // Set up format
    adec->format.samplerate = thread->codec_ctx->sample_rate;
    adec->format.channels = thread->codec_ctx->channels > 2 ? 2 : thread->codec_ctx->channels;
    adec->format.is_enabled = true;
    adec->format.stream_idx = stream_index;
    Kit_FindAudioFormat(thread->codec_ctx->sample_fmt, &adec->format.bytes, &adec->format.is_signed, &adec->format.format);

    // Audio converter context
    adec->swr = swr_alloc_set_opts(
        NULL,
        Kit_FindAVChannelLayout(adec->format.channels), // Target channel layout
        Kit_FindAVSampleFormat(adec->format.format), // Target fmt
        adec->format.samplerate, // Target samplerate
        thread->codec_ctx->channel_layout, // Source channel layout
        thread->codec_ctx->sample_fmt, // Source fmt
        thread->codec_ctx->sample_rate, // Source samplerate
        0, NULL);
    if(swr_init(adec->swr) != 0) {
        Kit_SetError("Unable to initialize audio converter context");
        goto error;
    }

    return thread;

error:
    Kit_FreeDecoderThread(&thread);
    return NULL;
}

void Kit_GetAudioDecoderInfo(Kit_DecoderThread *thread, Kit_AudioFormat *format) {
    Kit_AudioDecThread *athread = thread->local;
    memcpy(format, &athread->format, sizeof(Kit_AudioFormat));
}

int Kit_GetAudioDecoderData(Kit_DecoderThread *thread, double clock_sync, unsigned char *buffer, int length, int cur_buf_len) {
    //assert(player != NULL);
    assert(buffer != NULL);

    // Read a packet from buffer, if one exists. Stop here if not.
    int ret = 0;
    Kit_AudioPacket *packet = NULL;
    Kit_AudioPacket *n_packet = NULL;
    Kit_AudioDecThread *athread = thread->local;

    fprintf(stderr, "Audio: Attempting to get data 0\n");

    if(Kit_ThreadPeekOutput(thread, (void**)&packet) == 1) {
        return 0;
    }

    fprintf(stderr, "Audio: Attempting to get data 1\n");

    int bytes_per_sample = athread->format.bytes * athread->format.channels;
    double bps = bytes_per_sample * athread->format.samplerate;
    double cur_audio_ts = Kit_GetSystemTime() - clock_sync + ((double)cur_buf_len / bps);
    double diff = cur_audio_ts - packet->pts;
    int diff_samples = fabs(diff) * athread->format.samplerate;

    if(packet->pts > cur_audio_ts + AUDIO_SYNC_THRESHOLD) {
        // Audio is ahead, fill buffer with some silence
        int max_diff_samples = length / bytes_per_sample;
        int max_samples = (max_diff_samples < diff_samples) ? max_diff_samples : diff_samples;

        av_samples_set_silence(
            &buffer,
            0, // Offset
            max_samples,
            athread->format.channels,
            Kit_FindAVSampleFormat(athread->format.format));

        int diff_bytes = max_samples * bytes_per_sample;

        return diff_bytes;

    } else if(packet->pts < cur_audio_ts - AUDIO_SYNC_THRESHOLD) {
        // Audio is lagging, skip until good pts is found

        while(1) {
            Kit_ThreadAdvanceOutput(thread);
            if(Kit_ThreadPeekOutput(thread, (void**)&n_packet) == 0) {
                packet = n_packet;
            } else {
                break;
            }
            if(packet->pts > cur_audio_ts - AUDIO_SYNC_THRESHOLD) {
                break;
            }
        }
    }

    fprintf(stderr, "Audio: Attempting to get data 2\n");

    if(length > 0) {
        ret = Kit_ReadRingBuffer(packet->rb, (char*)buffer, length);
    }

    if(Kit_GetRingBufferLength(packet->rb) == 0) {
        Kit_ThreadReadOutput(thread, (void**)&packet);
        _FreeAudioPacket(packet);
    } else {
        double adjust = (double)ret / bps;
        packet->pts += adjust;
    }

    fprintf(stderr, "Audio: Attempting to get data 3\n");

    return ret;
}
