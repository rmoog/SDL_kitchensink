#include "kitchensink/internal/kitdemuxthread.h"
#include "kitchensink/internal/kitdecthread.h"
#include "kitchensink/kiterror.h"

#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

enum Kit_DemuxThreadStatus {
    KIT_DEMUXTHREAD_CLOSED = 0,
    KIT_DEMUXTHREAD_RUNNING,
    KIT_DEMUXTHREAD_CLOSING
};

static int _DemuxThread(void *data) {
    Kit_DemuxThread *thread = data;
    AVFormatContext *format_ctx = (AVFormatContext *)thread->src->format_ctx;
    bool running = true;
    AVPacket *packet = NULL;

    while(running) {
        // Make sure we want to keep running
        if(SDL_AtomicGet(&thread->state) != KIT_DEMUXTHREAD_RUNNING) {
            running = false;
            continue;
        }

        // Read packet from demuxer
        packet = malloc(sizeof(AVPacket));
        if(av_read_frame(format_ctx, packet) < 0) {
            running = false;
            continue;
        }

        // Push it to queue for a decoder. Packet will be freed in the decoders.
        if(thread->video_thread != NULL
            && packet->stream_index == thread->video_thread->stream_index)
        {
            Kit_ThreadWriteInput(thread->video_thread, packet);
        }
        else if(thread->audio_thread != NULL
            && packet->stream_index == thread->audio_thread->stream_index)
        {
            Kit_ThreadWriteInput(thread->audio_thread, packet);
        }
        else if(thread->subtitle_thread != NULL
            && packet->stream_index == thread->subtitle_thread->stream_index)
        {
            Kit_ThreadWriteInput(thread->subtitle_thread, packet);
        }
    }

    return 0;
}

Kit_DemuxThread* Kit_CreateDemuxThread(
    const Kit_Source *src,
    Kit_DecoderThread *video_thread,
    Kit_DecoderThread *audio_thread,
    Kit_DecoderThread *subtitle_thread)
{
    assert(src != NULL);

    Kit_DemuxThread *demux = NULL;

    demux = calloc(1, sizeof(Kit_DemuxThread));
    if(demux == NULL) {
        Kit_SetError("Unable to allocate demuxer thread");
        goto error;
    }

    demux->video_thread = video_thread;
    demux->audio_thread = audio_thread;
    demux->subtitle_thread = subtitle_thread;
    demux->src = src;

    // Start up the demuxer thread
    SDL_AtomicSet(&demux->state, KIT_DEMUXTHREAD_RUNNING);
    demux->thread = SDL_CreateThread(_DemuxThread, NULL, demux);
    if(demux->thread == NULL) {
        Kit_SetError("Unable to create a demuxer thread: %s", SDL_GetError());
        goto error;
    }

    return demux;

error:
    free(demux);
    return NULL;
}

void Kit_FreeDemuxThread(Kit_DemuxThread **thread_ptr) {
    if(thread_ptr == NULL) return;
    if(*thread_ptr == NULL) return;
    Kit_DemuxThread *thread = *thread_ptr;

    // Close thread gracefully
    SDL_AtomicSet(&thread->state, KIT_DEMUXTHREAD_CLOSING);
    SDL_WaitThread(thread->thread, NULL);

    // Free everything else
    free(thread);

    *thread_ptr = NULL;
}
