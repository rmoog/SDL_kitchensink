#ifndef KITAUDIODECTHREAD_H
#define KITAUDIODECTHREAD_H

typedef struct Kit_DecoderThread Kit_DecoderThread;
typedef struct Kit_Source Kit_Source;
typedef struct Kit_AudioFormat Kit_AudioFormat;

Kit_DecoderThread* Kit_CreateAudioDecoderThread(const Kit_Source *src, int stream_index);
void Kit_GetAudioDecoderInfo(Kit_DecoderThread *thread, Kit_AudioFormat *format);
int Kit_GetAudioDecoderData(Kit_DecoderThread *thread, double clock_sync, unsigned char *buffer, int length, int cur_buf_len);

#endif // KITAUDIODECTHREAD_H
