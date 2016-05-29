#ifndef KITVIDEODECTHREAD_H
#define KITVIDEODECTHREAD_H

#include <SDL2/SDL.h>

typedef struct Kit_DecoderThread Kit_DecoderThread;
typedef struct Kit_Source Kit_Source;
typedef struct Kit_VideoFormat Kit_VideoFormat;

Kit_DecoderThread* Kit_CreateVideoDecoderThread(const Kit_Source *src, int stream_index);
void Kit_GetVideoDecoderInfo(Kit_DecoderThread *thread, Kit_VideoFormat *format);
int Kit_GetVideoDecoderData(Kit_DecoderThread *thread, double clock_sync, SDL_Texture *texture);

#endif // KITVIDEODECTHREAD_H
