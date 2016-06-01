#include "kitchensink/internal/kitdecthread.h"
#include "kitchensink/kiterror.h"

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>

#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#define KIT_INPUT 0
#define KIT_OUTPUT 1

enum Kit_DecoderThreadStatus {
    KIT_DECTHREAD_CLOSED = 0,
    KIT_DECTHREAD_RUNNING,
    KIT_DECTHREAD_FLUSHING,
    KIT_DECTHREAD_CLOSING,
};

static int _DecoderThread(void *ptr) {
    Kit_DecoderThread *thread = ptr;
    int ret = 0;
    bool running = true;

    while(ret == 0 && running) {
        // Flush required, do so
        if(SDL_AtomicGet(&thread->state) == KIT_DECTHREAD_FLUSHING) {
            for(int i = 0; i < 2; i++) {
                if(SDL_LockMutex(thread->locks[i]) == 0) {
                    Kit_ClearBuffer(thread->buffers[i]);
                    SDL_CondSignal(thread->conditions[i]);
                    SDL_UnlockMutex(thread->locks[i]);
                }
            }
            SDL_AtomicSet(&thread->state, KIT_DECTHREAD_RUNNING);
        }

        // Make sure we want to keep running
        if(SDL_AtomicGet(&thread->state) != KIT_DECTHREAD_RUNNING) {
            running = false;
            continue;
        }

        // Call the external thread handler
        ret = thread->handler_cb(thread, thread->local);
    }

    fprintf(stderr, "Dec Thread done.\n");

    return 0;
}

Kit_DecoderThread* Kit_CreateDecoderThread(
    const Kit_Source *src,
    int stream_index,
    Kit_BufferFreeCallback inbuffer_free_cb,
    Kit_BufferFreeCallback outbuffer_free_cb,
    int inbuffer_size,
    int outbuffer_size,
    Kit_ThreadGetPTS get_pts_cb,
    Kit_ThreadHandler handler_cb,
    Kit_ThreadFree free_cb,
    void *local)
{
    assert(src != NULL);
    assert(handler_cb != NULL);
    assert(free_cb != NULL);
    assert(local != NULL);
    assert(inbuffer_size > 0);
    assert(outbuffer_size > 0);
    assert(inbuffer_free_cb != NULL);
    assert(outbuffer_free_cb != NULL);
    assert(stream_index >= 0);

    // Temporaries
    AVCodec *codec = NULL;
    AVStream *stream = NULL;
    Kit_DecoderThread *dst = NULL;
    AVFormatContext *format_ctx = (AVFormatContext *)src->format_ctx;

    // Make sure the stream index seems correct
    if(stream_index >= (int)format_ctx->nb_streams) {
        Kit_SetError("Invalid stream index #%d", stream_index);
        return NULL;
    }

    // Allocate a new thread
    dst = calloc(1, sizeof(Kit_DecoderThread));
    if(dst == NULL) {
        Kit_SetError("Unable to allocate a new decoder thread for stream #%d", stream_index);
        return NULL;
    }

    // Get the stream for easier access :)
    stream = format_ctx->streams[stream_index];

    // Find decoder
    codec = avcodec_find_decoder(stream->codec->codec_id);
    if(!codec) {
        Kit_SetError("No suitable decoder found for stream #%d", stream_index);
        goto failure_0;
    }

    // Copy the original codec context
    dst->codec_ctx = avcodec_alloc_context3(codec);
    if(avcodec_copy_context(dst->codec_ctx, stream->codec) != 0) {
        Kit_SetError("Unable to copy codec context for stream #%d", stream_index);
        goto failure_0;
    }

    // Create an decoder context
    if(avcodec_open2(dst->codec_ctx, codec, NULL) < 0) {
        Kit_SetError("Unable to open codec context for stream #%d", stream_index);
        goto failure_1;
    }

    // Create locks
    dst->locks[KIT_INPUT] = SDL_CreateMutex();
    dst->locks[KIT_OUTPUT] = SDL_CreateMutex();
    if(dst->locks[KIT_INPUT] == NULL || dst->locks[KIT_OUTPUT] == NULL) {
        Kit_SetError("Unable to allocate mutexes for stream #%d", stream_index);
        goto failure_3;
    }

    dst->conditions[KIT_INPUT] = SDL_CreateCond();
    dst->conditions[KIT_OUTPUT] = SDL_CreateCond();
    if(dst->conditions[KIT_INPUT] == NULL || dst->conditions[KIT_OUTPUT] == NULL) {
        Kit_SetError("Unable to allocate conditions for stream #%d", stream_index);
        goto failure_3;
    }

    // Create buffers
    dst->buffers[KIT_INPUT] = Kit_CreateBuffer(inbuffer_size, inbuffer_free_cb);
    dst->buffers[KIT_OUTPUT] = Kit_CreateBuffer(outbuffer_size, outbuffer_free_cb);
    if(dst->buffers[KIT_INPUT] == NULL || dst->buffers[KIT_OUTPUT] == NULL) {
        Kit_SetError("Unable to allocate buffers for stream #%d", stream_index);
        goto failure_3;
    }

    // Set the rest of the args
    dst->handler_cb = handler_cb;
    dst->free_cb = free_cb;
    dst->local = local;
    dst->stream_index = stream_index;
    dst->stream = stream;

    // Create the decoder thread.
    SDL_AtomicSet(&dst->state, KIT_DECTHREAD_RUNNING);
    dst->thread = SDL_CreateThread(_DecoderThread, NULL, dst);
    if(dst->thread == NULL) {
        Kit_SetError("Unable to create a decoder thread for stream #%d: %s", stream_index, SDL_GetError());
        goto failure_3;
    }

    return dst;

failure_3:
    for(int i = 0; i < 2; i++) {
        SDL_DestroyMutex(dst->locks[i]);
        SDL_DestroyCond(dst->conditions[i]);
        Kit_DestroyBuffer(dst->buffers[i]);
    }
    avcodec_close(dst->codec_ctx);
failure_1:
    avcodec_free_context(&dst->codec_ctx);
failure_0:
    free(dst);
    return NULL;
}

void Kit_FreeDecoderThread(Kit_DecoderThread **thread_ptr) {
    if(thread_ptr == NULL) return;
    if(*thread_ptr == NULL) return;
    Kit_DecoderThread *thread = *thread_ptr;

    // Signal all conditions
    for(int i = 0; i < 2; i++) {
        if(SDL_LockMutex(thread->locks[i]) == 0) {
            SDL_CondBroadcast(thread->conditions[i]);
            SDL_UnlockMutex(thread->locks[i]);
        }
    }

    // Close thread gracefully
    SDL_AtomicSet(&thread->state, KIT_DECTHREAD_CLOSING);
    SDL_WaitThread(thread->thread, NULL);

    // Free decoder local stuff
    thread->free_cb(thread->local);

    // Free up local resources
    for(int i = 0; i < 2; i++) {
        SDL_DestroyMutex(thread->locks[i]);
        SDL_DestroyCond(thread->conditions[i]);
        Kit_DestroyBuffer(thread->buffers[i]);
    }

    // Free up ffmpeg resources
    avcodec_close(thread->codec_ctx);
    avcodec_free_context(&thread->codec_ctx);

    // Free the thread struct
    free(thread);
    *thread_ptr = NULL;
}

static int _ThreadWrite(int num, Kit_DecoderThread *thread, void *packet) {
    assert(thread != NULL);
    assert(packet != NULL);

    int ret = 0;
    if(SDL_LockMutex(thread->locks[num]) == 0) {
        if(Kit_GetBufferState(thread->buffers[num]) == KIT_BUFFER_FULL) {
            fprintf(stderr, "KIT_BUFFER_FULL; waiting for %d\n", num);
            SDL_CondWait(thread->conditions[num], thread->locks[num]);
        }
        fprintf(stderr, "writing to %d\n", num);
        ret = Kit_WriteBuffer(thread->buffers[num], packet);
        SDL_UnlockMutex(thread->locks[num]);
    }
    return ret;
}

static int _ThreadRead(int num, Kit_DecoderThread *thread, void **packet) {
    assert(thread != NULL);

    int ret = 0;
    if(SDL_LockMutex(thread->locks[num]) == 0) {
        if(packet != NULL) {
            *packet = Kit_ReadBuffer(thread->buffers[num]);
            if(*packet == NULL) {
                ret = 1;
            }
        }
        SDL_CondSignal(thread->conditions[num]);
        SDL_UnlockMutex(thread->locks[num]);
    }
    return ret;
}

static int _ThreadPeek(int num, Kit_DecoderThread *thread, void **packet) {
    assert(thread != NULL);
    assert(packet != NULL);

    int ret = 0;
    if(SDL_LockMutex(thread->locks[num]) == 0) {
        *packet = Kit_PeekBuffer(thread->buffers[num]);
        if(*packet == NULL) {
            ret = 1;
        }
        SDL_UnlockMutex(thread->locks[num]);
    }
    return ret;
}


int Kit_ThreadWriteInput(Kit_DecoderThread *thread, void *packet) {
    return _ThreadWrite(KIT_INPUT, thread, packet);
}

int Kit_ThreadReadInput(Kit_DecoderThread *thread, void **packet) {
    return _ThreadRead(KIT_INPUT, thread, packet);
}

int Kit_ThreadWriteOutput(Kit_DecoderThread *thread, void *packet) {
    return _ThreadWrite(KIT_OUTPUT, thread, packet);
}

int Kit_ThreadReadOutput(Kit_DecoderThread *thread, void **packet) {
    return _ThreadRead(KIT_OUTPUT, thread, packet);
}

int Kit_ThreadPeekOutput(Kit_DecoderThread *thread, void **packet) {
    return _ThreadPeek(KIT_OUTPUT, thread, packet);
}

int Kit_ThreadAdvanceOutput(Kit_DecoderThread *thread) {
    return _ThreadRead(KIT_OUTPUT, thread, NULL);
}
