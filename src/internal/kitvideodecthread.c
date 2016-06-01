#include "kitchensink/internal/kitdecthread.h"
#include "kitchensink/internal/kitavutils.h"
#include "kitchensink/internal/kitringbuffer.h"
#include "kitchensink/internal/kitbuffer.h"
#include "kitchensink/kitformats.h"
#include "kitchensink/kiterror.h"
#include "kitchensink/internal/kitvideodecthread.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>

#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

#define VIDEO_SYNC_THRESHOLD 0.01

typedef struct Kit_VideoDecThread {
    Kit_VideoFormat format;
    AVFrame *tmp_frame;
    struct SwsContext *sws;
} Kit_VideoDecThread;

typedef struct Kit_VideoPacket {
    double pts;
    AVFrame *frame;
} Kit_VideoPacket;

static void _FreeAVPacket(void *ptr) {
    AVPacket *packet = ptr;
    av_packet_unref(packet);
    free(packet);
}

static Kit_VideoPacket* _CreateVideoPacket(AVFrame *frame, double pts) {
    Kit_VideoPacket *p = calloc(1, sizeof(Kit_VideoPacket));
    p->frame = frame;
    p->pts = pts;
    return p;
}

static void _FreeVideoPacket(void *ptr) {
    Kit_VideoPacket *packet = ptr;
    av_frame_free(&packet->frame);
    free(packet);
}

static void _FreeVideoDecoderThread(void *ptr) {
    if(ptr == NULL) return;
    Kit_VideoDecThread *vthread = ptr;
    av_frame_free(&vthread->tmp_frame);
    sws_freeContext(vthread->sws);
    free(vthread);
}

static double _GetPacketPTS(void *ptr) {
    Kit_VideoPacket *packet = ptr;
    return packet->pts;
}

static int _HandleVideoPacket(Kit_DecoderThread *thread, void *local) {
    assert(thread != NULL);
    assert(local != NULL);

    int frame_finished = 0;
    AVPacket *packet = NULL;
    Kit_VideoDecThread *vthread = local;
    AVFrame *iframe = vthread->tmp_frame;

    // Read a packet from queue.
    if(Kit_ThreadReadInput(thread, (void**)&packet) != 0) {
        return 0;
    }

    while(packet->size > 0) {
        int len = avcodec_decode_video2(thread->codec_ctx, vthread->tmp_frame, &frame_finished, packet);
        if(len < 0) {
            _FreeAVPacket(packet);
            return 1;
        }

        if(frame_finished) {
            // Target frame
            AVFrame *oframe = av_frame_alloc();
            av_image_alloc(
                oframe->data,
                oframe->linesize,
                thread->codec_ctx->width,
                thread->codec_ctx->height,
                Kit_FindAVPixelFormat(vthread->format.format),
                1);

            // Scale from source format to target format, don't touch the size
            sws_scale(
                vthread->sws,
                (const unsigned char * const *)iframe->data,
                iframe->linesize,
                0,
                thread->codec_ctx->height,
                oframe->data,
                oframe->linesize);

            // Get pts
            double pts = 0;
            if(packet->dts != AV_NOPTS_VALUE) {
                pts = av_frame_get_best_effort_timestamp(vthread->tmp_frame);
                pts *= av_q2d(thread->stream->time_base);
            }

            // Just seeked, set sync clock & pos.
            /*if(player->seek_flag == 1) {
                player->vclock_pos = pts;
                player->clock_sync = Kit_GetSystemTime() - pts;
                player->seek_flag = 0;
            }*/
            fprintf(stderr, "Video: Got here!\n");

            // Lock, write to audio buffer, unlock
            Kit_VideoPacket *vpacket = _CreateVideoPacket(oframe, pts);
            if(Kit_ThreadWriteOutput(thread, vpacket) != 0) {
                _FreeVideoPacket(vpacket);
            }
        }
        packet->size -= len;
        packet->data += len;
        fprintf(stderr, "Video: Handled %d, left %d\n", len, packet->size);
    }
    _FreeAVPacket(packet);
    return 0;
}

Kit_DecoderThread* Kit_CreateVideoDecoderThread(const Kit_Source *src, int stream_index) {
    Kit_DecoderThread *thread = NULL;
    Kit_VideoDecThread *vdec = NULL;

    vdec = calloc(1, sizeof(Kit_VideoDecThread));
    if(vdec == NULL) {
        Kit_SetError("Unable to allocate video decoder thread");
        return NULL;
    }

    // Create the thread for decoding
    thread = Kit_CreateDecoderThread(
        src, // Source contexts
        stream_index, // Stream index we are playing
        _FreeAVPacket, // For freeing the input buffer AVPackets
        _FreeVideoPacket, // For freeing the output buffer frames
        2, // Input buffer size
        2, // Output buffer size (in frames; these may be LARGE)
        _GetPacketPTS, // for getting a Presentation TimeStamp from a packet
        _HandleVideoPacket, // Function for decoding video packets
        _FreeVideoDecoderThread, // For freeing video decoder contexts
        vdec // Video decoder local struct
    );
    if(thread == NULL) {
        free(vdec);
        return NULL;
    }

    // Create a temporary frame
    vdec->tmp_frame = av_frame_alloc();
    if(vdec->tmp_frame == NULL) {
        Kit_SetError("Unable to initialize temporary video frame");
        goto error;
    }

    // Find format information
    vdec->format.is_enabled = true;
    vdec->format.width = thread->codec_ctx->width;
    vdec->format.height = thread->codec_ctx->height;
    vdec->format.stream_idx = stream_index;
    Kit_FindPixelFormat(thread->codec_ctx->pix_fmt, &vdec->format.format);

    // Video converter context
    vdec->sws = sws_getContext(
        thread->codec_ctx->width, // Source w
        thread->codec_ctx->height, // Source h
        thread->codec_ctx->pix_fmt, // Source fmt
        thread->codec_ctx->width, // Target w
        thread->codec_ctx->height, // Target h
        Kit_FindAVPixelFormat(vdec->format.format), // Target fmt
        SWS_BICUBIC,
        NULL, NULL, NULL);
    if(vdec->sws == NULL) {
        Kit_SetError("Unable to initialize video converter context");
        goto error;
    }

    return thread;

error:
    Kit_FreeDecoderThread(&thread);
    return NULL;
}

void Kit_GetVideoDecoderInfo(Kit_DecoderThread *thread, Kit_VideoFormat *format) {
    Kit_VideoDecThread *vthread = thread->local;
    memcpy(format, &vthread->format, sizeof(Kit_VideoFormat));
}

int Kit_GetVideoDecoderData(Kit_DecoderThread *thread, double clock_sync, SDL_Texture *texture) {
    assert(thread != NULL);
    assert(texture != NULL);
    Kit_VideoDecThread *vthread = thread->local;

    // Read a packet from buffer, if one exists. Stop here if not.
    Kit_VideoPacket *packet = NULL;
    Kit_VideoPacket *n_packet = NULL;

    fprintf(stderr, "Video: Attempting to get data 0\n");

    if(Kit_ThreadPeekOutput(thread, (void**)&packet) == 1) {
        return 0;
    }

    fprintf(stderr, "Video: Attempting to get data 1\n");

    // Print some data
    double cur_video_ts = Kit_GetSystemTime() - clock_sync;

    // Check if we want the packet
    if(packet->pts > cur_video_ts + VIDEO_SYNC_THRESHOLD) {
        // Video is ahead, don't show yet.
        return 0;
    } else if(packet->pts < cur_video_ts - VIDEO_SYNC_THRESHOLD) {
        // Video is lagging, skip until we find a good PTS to continue from.
        while(packet != NULL) {
            Kit_ThreadAdvanceOutput(thread);
            if(Kit_ThreadPeekOutput(thread, (void**)&n_packet) == 0) {
                packet = n_packet;
            } else {
                break;
            }
            if(packet->pts > cur_video_ts - VIDEO_SYNC_THRESHOLD) {
                break;
            }
        }
    }

    fprintf(stderr, "Video: Attempting to get data 2\n");

    // Advance buffer one frame forwards
    Kit_ThreadAdvanceOutput(thread);

    fprintf(stderr, "Video: Attempting to get data 3\n");

    // Update textures as required. Handle UYV frames separately.
    if(vthread->format.format == SDL_PIXELFORMAT_YV12
        || vthread->format.format == SDL_PIXELFORMAT_IYUV)
    {
        SDL_UpdateYUVTexture(
            texture, NULL,
            packet->frame->data[0], packet->frame->linesize[0],
            packet->frame->data[1], packet->frame->linesize[1],
            packet->frame->data[2], packet->frame->linesize[2]);
    }
    else {
        SDL_UpdateTexture(
            texture, NULL,
            packet->frame->data[0],
            packet->frame->linesize[0]);
    }

    fprintf(stderr, "Video: Attempting to get data 4\n");

    _FreeVideoPacket(packet);


    return 0;
}
