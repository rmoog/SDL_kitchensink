#ifndef KITSUBDECTHREAD_H
#define KITSUBDECTHREAD_H

typedef struct Kit_DecoderThread Kit_DecoderThread;
typedef struct Kit_Source Kit_Source;

Kit_DecoderThread* Kit_CreateSubDecoderThread(const Kit_Source *src, int stream_index);

#endif // KITSUBDECTHREAD_H
