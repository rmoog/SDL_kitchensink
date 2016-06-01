#include "kitchensink/kitplayer.h"
#include "kitchensink/kiterror.h"
#include "kitchensink/internal/kitbuffer.h"
#include "kitchensink/internal/kitringbuffer.h"
#include "kitchensink/internal/kitlist.h"
#include "kitchensink/internal/kitlibstate.h"
#include "kitchensink/internal/kitaudiodecthread.h"
#include "kitchensink/internal/kitvideodecthread.h"
#include "kitchensink/internal/kitdemuxthread.h"
#include "kitchensink/internal/kitavutils.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>
#include <libavutil/samplefmt.h>
#include "libavutil/avstring.h"

#include <SDL2/SDL.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

// Threshold is in seconds
#define VIDEO_SYNC_THRESHOLD 0.01
#define AUDIO_SYNC_THRESHOLD 0.05


Kit_Player* Kit_CreatePlayer(const Kit_Source *src) {
    assert(src != NULL);

    Kit_Player *player = calloc(1, sizeof(Kit_Player));
    if(player == NULL) {
        Kit_SetError("Unable to allocate player object");
        return NULL;
    }

    // Initialize audio decoder
    if(src->astream_idx > -1) {
        player->dec_threads[THREAD_AUDIO] = Kit_CreateAudioDecoderThread(src, src->astream_idx);
        if(player->dec_threads[THREAD_AUDIO] == NULL) {
            goto error;
        }
    }

    // Initialize video decoder
    if(src->vstream_idx > -1) {
        player->dec_threads[THREAD_VIDEO] = Kit_CreateVideoDecoderThread(src, src->vstream_idx);
        if(player->dec_threads[THREAD_VIDEO] == NULL) {
            goto error;
        }
    }

    player->dec_threads[THREAD_SUBTITLE] = NULL;

    // Initialize demuxer
    player->demux_thread = Kit_CreateDemuxThread(
        src,
        player->dec_threads[THREAD_VIDEO],
        player->dec_threads[THREAD_AUDIO],
        player->dec_threads[THREAD_SUBTITLE]);
    if(player->demux_thread == NULL) {
        goto error;
    }

    // Set SRC locally
    player->src = src;

    return player;

error:
    Kit_ClosePlayer(player);
    return NULL;
}

void Kit_ClosePlayer(Kit_Player *player) {
    if(player == NULL) return;

    // Release threads
    Kit_FreeDemuxThread(&player->demux_thread);
    for(int i = 0; i < NB_THREAD_TYPES; i++) {
        Kit_FreeDecoderThread(&player->dec_threads[i]);
    }

    free(player);
}

int Kit_GetVideoData(Kit_Player *player, SDL_Texture *texture) {
    assert(player != NULL);

    if(player->src->vstream_idx == -1) {
        return 0;
    }

    assert(texture != NULL);

    // If paused or stopped, do nothing
    if(player->state == KIT_PAUSED) {
        return 0;
    }
    if(player->state == KIT_STOPPED) {
        return 0;
    }

    fprintf(stderr, "Kit_GetVideoData\n");

    return Kit_GetVideoDecoderData(player->dec_threads[THREAD_VIDEO], player->clock_sync, texture);
}

int Kit_GetSubtitleData(Kit_Player *player, SDL_Renderer *renderer) {
    assert(player != NULL);

    // If there is no audio stream, don't bother.
    if(player->src->sstream_idx == -1) {
        return 0;
    }

    assert(renderer != NULL);

    // If paused or stopped, do nothing
    if(player->state == KIT_PAUSED) {
        return 0;
    }
    if(player->state == KIT_STOPPED) {
        return 0;
    }

/*
    unsigned int it;
    Kit_SubtitlePacket *packet = NULL;

    // Current sync timestamp
    double cur_subtitle_ts = _GetSystemTime() - player->clock_sync;

    // Read a packet from buffer, if one exists. Stop here if not.
    if(SDL_LockMutex(player->smutex) == 0) {
        // Check if refresh is required and remove old subtitles
        it = 0;
        while((packet = Kit_IterateList((Kit_List*)player->sbuffer, &it)) != NULL) {
            if(packet->pts_end >= 0 && packet->pts_end < cur_subtitle_ts) {
                Kit_RemoveFromList((Kit_List*)player->sbuffer, it);
            }
        }

        // Render subtitle bitmaps
        it = 0;
        while((packet = Kit_IterateList((Kit_List*)player->sbuffer, &it)) != NULL) {
            if(packet->texture == NULL) {
                packet->texture = SDL_CreateTextureFromSurface(renderer, packet->surface);
                SDL_SetTextureBlendMode(packet->texture, SDL_BLENDMODE_BLEND);
            }
            SDL_RenderCopy(renderer, packet->texture, NULL, packet->rect);
        }

        // Unlock subtitle buffer mutex.
        SDL_UnlockMutex(player->smutex);
    } else {
        Kit_SetError("Unable to lock subtitle buffer mutex");
        return 0;
    }
*/
    return 0;
}

int Kit_GetAudioData(Kit_Player *player, unsigned char *buffer, int length, int cur_buf_len) {
    assert(player != NULL);

    // If there is no audio stream, don't bother.
    if(player->src->astream_idx == -1) {
        return 0;
    }

    // If asked for nothing, don't return anything either :P
    if(length == 0) {
        return 0;
    }

    assert(buffer != NULL);

    // If paused or stopped, do nothing
    if(player->state == KIT_PAUSED) {
        return 0;
    }
    if(player->state == KIT_STOPPED) {
        return 0;
    }

    fprintf(stderr, "Kit_GetAudioData\n");

    return Kit_GetAudioDecoderData(player->dec_threads[THREAD_AUDIO], player->clock_sync, buffer, length, cur_buf_len);
}

void Kit_GetPlayerInfo(const Kit_Player *player, Kit_PlayerInfo *info) {
    assert(player != NULL);
    assert(info != NULL);

    Kit_DecoderThread *athread = player->dec_threads[THREAD_AUDIO];
    Kit_DecoderThread *vthread = player->dec_threads[THREAD_VIDEO];
    Kit_DecoderThread *sthread = player->dec_threads[THREAD_SUBTITLE];
    AVCodecContext *acodec_ctx = athread ? athread->codec_ctx : NULL;
    AVCodecContext *vcodec_ctx = vthread ? vthread->codec_ctx : NULL;
    AVCodecContext *scodec_ctx = sthread ? sthread->codec_ctx : NULL;

    // Reset everything to 0. We might not fill all fields.
    memset(info, 0, sizeof(Kit_PlayerInfo));

    if(acodec_ctx != NULL) {
        strncpy(info->acodec, acodec_ctx->codec->name, KIT_CODECMAX-1);
        strncpy(info->acodec_name, acodec_ctx->codec->long_name, KIT_CODECNAMEMAX-1);
        Kit_GetAudioDecoderInfo(player->dec_threads[THREAD_AUDIO], &info->audio);
    }
    if(vcodec_ctx != NULL) {
        strncpy(info->vcodec, vcodec_ctx->codec->name, KIT_CODECMAX-1);
        strncpy(info->vcodec_name, vcodec_ctx->codec->long_name, KIT_CODECNAMEMAX-1);
        Kit_GetVideoDecoderInfo(player->dec_threads[THREAD_VIDEO], &info->video);
    }
    if(scodec_ctx != NULL) {
        strncpy(info->scodec, scodec_ctx->codec->name, KIT_CODECMAX-1);
        strncpy(info->scodec_name, scodec_ctx->codec->long_name, KIT_CODECNAMEMAX-1);
        //memcpy(&info->subtitle, &player->sformat, sizeof(Kit_SubtitleFormat));
        //Kit_GetSubtitleInfo(player->dec_threads[THREAD_SUBTITLE], info->subtitle);
    }
}

Kit_PlayerState Kit_GetPlayerState(const Kit_Player *player) {
    assert(player != NULL);

    return player->state;
}

void Kit_PlayerPlay(Kit_Player *player) {
    assert(player != NULL);

    if(player->state == KIT_PLAYING) {
        return;
    }
    if(player->state == KIT_STOPPED) {
        player->clock_sync = Kit_GetSystemTime();
    }
    if(player->state == KIT_PAUSED) {
        player->clock_sync += Kit_GetSystemTime() - player->pause_start;
    }
    player->state = KIT_PLAYING;
}

void Kit_PlayerStop(Kit_Player *player) {
    assert(player != NULL);

    if(player->state == KIT_STOPPED) {
        return;
    }
    player->state = KIT_STOPPED;
}

void Kit_PlayerPause(Kit_Player *player) {
    assert(player != NULL);

    if(player->state != KIT_PLAYING) {
        return;
    }
    player->pause_start = Kit_GetSystemTime();
    player->state = KIT_PAUSED;
}

int Kit_PlayerSeek(Kit_Player *player, double m_time) {
    assert(player != NULL);
/*
    // Send packets to control stream
    if(SDL_LockMutex(player->cmutex) == 0) {
        // Flush audio and video buffers, then set seek, then unlock control queue mutex.
        Kit_WriteBuffer((Kit_Buffer*)player->cbuffer, _CreateControlPacket(KIT_CONTROL_FLUSH, 0));
        Kit_WriteBuffer((Kit_Buffer*)player->cbuffer, _CreateControlPacket(KIT_CONTROL_SEEK, m_time));
        SDL_UnlockMutex(player->cmutex);
    } else {
        Kit_SetError("Unable to lock control queue mutex");
        return 1;
    }
*/
    return 0;
}

double Kit_GetPlayerDuration(const Kit_Player *player) {
    assert(player != NULL);

    AVFormatContext *fmt_ctx = (AVFormatContext *)player->src->format_ctx;
    return (fmt_ctx->duration / AV_TIME_BASE);
}

double Kit_GetPlayerPosition(const Kit_Player *player) {
    assert(player != NULL);

    return player->clock_sync;
}
