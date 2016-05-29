#ifndef KITDEMUXTHREAD_H
#define KITDEMUXTHREAD_H

#include "kitchensink/kitsource.h"
#include "kitchensink/internal/kitdecthread.h"

typedef struct Kit_DemuxThread {
    const Kit_Source *src;
    SDL_Thread *thread;
    SDL_atomic_t state;

    Kit_DecoderThread *video_thread;
    Kit_DecoderThread *audio_thread;
    Kit_DecoderThread *subtitle_thread;
} Kit_DemuxThread;

KIT_LOCAL Kit_DemuxThread* Kit_CreateDemuxThread(
    const Kit_Source *src,
    Kit_DecoderThread *video_thread,
    Kit_DecoderThread *audio_thread,
    Kit_DecoderThread *subtitle_thread);
KIT_LOCAL void Kit_FreeDemuxThread(Kit_DemuxThread **thread);


#endif // KITDEMUXTHREAD_H
