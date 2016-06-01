#ifndef KITDECTHREAD_H
#define KITDECTHREAD_H

#include "kitchensink/internal/kitbuffer.h"
#include "kitchensink/kitconfig.h"
#include "kitchensink/kitsource.h"

#include <SDL2/SDL_thread.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

typedef struct Kit_DecoderThread Kit_DecoderThread;

typedef int (*Kit_ThreadHandler)(Kit_DecoderThread*, void *local);
typedef void (*Kit_ThreadFree)(void *local);
typedef double (*Kit_ThreadGetPTS)(void *local);

struct Kit_DecoderThread {
    // Thread stuff
    SDL_Thread *thread;
    SDL_atomic_t state;

    // Buffer handling
    SDL_mutex *locks[2];
    Kit_Buffer *buffers[2];
    SDL_cond *conditions[2];

    // ffmpeg stuff
    AVCodecContext *codec_ctx;
    AVStream *stream; // A copy, no need to free
    int stream_index;

    // Local data and callbacks
    void *local;
    Kit_ThreadHandler handler_cb;
    Kit_ThreadFree free_cb;
};

KIT_LOCAL Kit_DecoderThread* Kit_CreateDecoderThread(
    const Kit_Source *src,
    int stream_index,
    Kit_BufferFreeCallback inbuffer_free_cb,
    Kit_BufferFreeCallback outbuffer_free_cb,
    int inbuffer_size,
    int outbuffer_size,
    Kit_ThreadGetPTS get_pts_cb,
    Kit_ThreadHandler handler_cb,
    Kit_ThreadFree free_cb,
    void *local);
KIT_LOCAL void Kit_FreeDecoderThread(Kit_DecoderThread **thread);
KIT_LOCAL void Kit_PrepareFreeDecoderThread(Kit_DecoderThread **thread_ptr);

KIT_LOCAL int Kit_ThreadWriteInput(Kit_DecoderThread *thread, void *packet);
KIT_LOCAL int Kit_ThreadReadInput(Kit_DecoderThread *thread, void **packet);
KIT_LOCAL int Kit_ThreadWriteOutput(Kit_DecoderThread *thread, void *packet);
KIT_LOCAL int Kit_ThreadReadOutput(Kit_DecoderThread *thread, void **packet);
KIT_LOCAL int Kit_ThreadPeekOutput(Kit_DecoderThread *thread, void **packet);
KIT_LOCAL int Kit_ThreadAdvanceOutput(Kit_DecoderThread *thread);

#endif // KITDECTHREAD_H
