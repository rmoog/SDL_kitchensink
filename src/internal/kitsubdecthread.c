#include "kitchensink/internal/kitsubdecthread.h"
#include "kitchensink/internal/kitdecthread.h"
#include "kitchensink/internal/kitavutils.h"
#include "kitchensink/internal/kitringbuffer.h"
#include "kitchensink/kitformats.h"
#include "kitchensink/kiterror.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/avstring.h>

#include <SDL2/SDL.h>
#include <ass/ass.h>

#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

// For compatibility
#ifndef ASS_FONTPROVIDER_AUTODETECT
#define ASS_FONTPROVIDER_AUTODETECT 1
#endif

typedef struct Kit_SubtitlePacket {
    double pts_start;
    double pts_end;
    SDL_Rect *rect;
    SDL_Surface *surface;
    SDL_Texture *texture;
} Kit_SubtitlePacket;


static Kit_SubtitlePacket* _CreateSubtitlePacket(double pts_start, double pts_end, SDL_Rect *rect, SDL_Surface *surface) {
    Kit_SubtitlePacket *p = calloc(1, sizeof(Kit_SubtitlePacket));
    p->pts_start = pts_start;
    p->pts_end = pts_end;
    p->surface = surface;
    p->rect = rect;
    p->texture = NULL; // Cached texture
    return p;
}

static void _FreeSubtitlePacket(void *ptr) {
    Kit_SubtitlePacket *packet = ptr;
    SDL_FreeSurface(packet->surface);
    if(packet->texture) {
        SDL_DestroyTexture(packet->texture);
    }
    free(packet->rect);
    free(packet);
}


static int reset_libass_track(Kit_Player *player) {
    AVCodecContext *scodec_ctx = player->scodec_ctx;

    if(scodec_ctx == NULL) {
        return 0;
    }

    // Flush libass track events
    ass_flush_events(player->ass_track);
    return 0;
}


static void _HandleBitmapSubtitle(Kit_SubtitlePacket** spackets, int *n, Kit_Player *player, double pts, AVSubtitle *sub, AVSubtitleRect *rect) {
    if(rect->nb_colors == 256) {
        // Paletted image based subtitles. Convert and set palette.
        SDL_Surface *s = SDL_CreateRGBSurfaceFrom(
            rect->data[0],
            rect->w, rect->h, 8,
            rect->linesize[0],
            0, 0, 0, 0);

        SDL_SetPaletteColors(s->format->palette, (SDL_Color*)rect->pict.data[1], 0, 256);

        Uint32 rmask, gmask, bmask, amask;
        #if SDL_BYTEORDER == SDL_BIG_ENDIAN
            rmask = 0xff000000;
            gmask = 0x00ff0000;
            bmask = 0x0000ff00;
            amask = 0x000000ff;
        #else
            rmask = 0x000000ff;
            gmask = 0x0000ff00;
            bmask = 0x00ff0000;
            amask = 0xff000000;
        #endif
        SDL_Surface *tmp = SDL_CreateRGBSurface(
            0, rect->w, rect->h, 32,
            rmask, gmask, bmask, amask);
        SDL_BlitSurface(s, NULL, tmp, NULL);
        SDL_FreeSurface(s);

        SDL_Rect *dst_rect = malloc(sizeof(SDL_Rect));
        dst_rect->x = rect->x;
        dst_rect->y = rect->y;
        dst_rect->w = rect->w;
        dst_rect->h = rect->h;

        double start = pts + (sub->start_display_time / 1000.0f);
        double end = -1;
        if(sub->end_display_time < UINT_MAX) {
            end = pts + (sub->end_display_time / 1000.0f);
        }

        spackets[(*n)++] = _CreateSubtitlePacket(start, end, dst_rect, tmp);
    }
}

static void _ProcessAssSubtitleRect(Kit_Player *player, AVSubtitleRect *rect) {
    ass_process_data((ASS_Track*)player->ass_track, rect->ass, strlen(rect->ass));
}

static void _ProcessAssImage(SDL_Surface *surface, const ASS_Image *img) {
    int x, y;
    // libass headers claim img->color is RGBA, but the alpha is 0.
    unsigned char r = ((img->color) >> 24) & 0xFF;
    unsigned char g = ((img->color) >> 16) & 0xFF;
    unsigned char b = ((img->color) >>  8) & 0xFF;
    unsigned char *src = img->bitmap;
    unsigned char *dst = (unsigned char*)surface->pixels;

    for(y = 0; y < img->h; y++) {
        for(x = 0; x < img->w; x++) {
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = src[x];
        }
        src += img->stride;
        dst += surface->pitch;
    }
}

static void _HandleAssSubtitle(Kit_SubtitlePacket** spackets, int *n, Kit_Player *player, double pts, AVSubtitle *sub) {
    double start = pts + (sub->start_display_time / 1000.0f);
    double end = pts + (sub->end_display_time / 1000.0f);

    // Process current chunk of data
    unsigned int now = start * 1000;
    int change = 0;
    ASS_Image *images = ass_render_frame((ASS_Renderer*)player->ass_renderer, (ASS_Track*)player->ass_track, now, &change);

    // Convert to SDL_Surfaces
    if(change > 0) {
        ASS_Image *now = images;
        if(now != NULL) {
            do {
                Uint32 rmask, gmask, bmask, amask;
                #if SDL_BYTEORDER == SDL_BIG_ENDIAN
                    rmask = 0xff000000;
                    gmask = 0x00ff0000;
                    bmask = 0x0000ff00;
                    amask = 0x000000ff;
                #else
                    rmask = 0x000000ff;
                    gmask = 0x0000ff00;
                    bmask = 0x00ff0000;
                    amask = 0xff000000;
                #endif
                SDL_Surface *tmp = SDL_CreateRGBSurface(
                    0, now->w, now->h, 32,
                    rmask, gmask, bmask, amask);

                _ProcessAssImage(tmp, now);

                SDL_Rect *dst_rect = malloc(sizeof(SDL_Rect));
                dst_rect->x = now->dst_x;
                dst_rect->y = now->dst_y;
                dst_rect->w = now->w;
                dst_rect->h = now->h;

                spackets[(*n)++] = _CreateSubtitlePacket(start, end, dst_rect, tmp);
            } while((now = now->next) != NULL);
        }
    }
}

static void _HandleSubtitlePacket(Kit_Player *player, AVPacket *packet) {
    assert(player != NULL);
    assert(packet != NULL);

    int frame_finished;
    int len;
    AVCodecContext *scodec_ctx = (AVCodecContext*)player->scodec_ctx;
    AVFormatContext *fmt_ctx = (AVFormatContext *)player->src->format_ctx;
    Kit_SubtitlePacket *tmp = NULL;
    unsigned int it;
    AVSubtitle sub;
    memset(&sub, 0, sizeof(AVSubtitle));

    if(packet->size > 0) {
        len = avcodec_decode_subtitle2(scodec_ctx, &sub, &frame_finished, packet);
        if(len < 0) {
            return;
        }

        if(frame_finished) {
            // Get pts
            double pts = 0;
            if(packet->dts != AV_NOPTS_VALUE) {
                pts = packet->pts;
                pts *= av_q2d(fmt_ctx->streams[player->src->sstream_idx]->time_base);
            }

            // Convert subtitles to SDL_Surface and create a packet
            Kit_SubtitlePacket *spackets[KIT_SBUFFERSIZE];
            memset(spackets, 0, sizeof(Kit_SubtitlePacket*) * KIT_SBUFFERSIZE);

            int n = 0;
            bool has_ass = false;
            for(int r = 0; r < sub.num_rects; r++) {
                switch(sub.rects[r]->type) {
                    case SUBTITLE_BITMAP:
                        _HandleBitmapSubtitle(spackets, &n, player, pts, &sub, sub.rects[r]);
                        break;
                    case SUBTITLE_ASS:
                        _ProcessAssSubtitleRect(player, sub.rects[r]);
                        has_ass = true;
                        break;
                    case SUBTITLE_TEXT:
                        break;
                    case SUBTITLE_NONE:
                        break;
                }
            }

            // Process libass content
            if(has_ass) {
                _HandleAssSubtitle(spackets, &n, player, pts, &sub);
            }

            // Lock, write to subtitle buffer, unlock
            if(SDL_LockMutex(player->smutex) == 0) {
                if(has_ass) {
                    Kit_ClearList((Kit_List*)player->sbuffer);
                } else {
                    // Clear out old subtitles that should only be valid until next (this) subtitle
                    it = 0;
                    while((tmp = Kit_IterateList((Kit_List*)player->sbuffer, &it)) != NULL) {
                        if(tmp->pts_end < 0) {
                            Kit_RemoveFromList((Kit_List*)player->sbuffer, it);
                        }
                    }
                }

                // Add new subtitle
                for(int i = 0; i < KIT_SBUFFERSIZE; i++) {
                    Kit_SubtitlePacket *spacket = spackets[i];
                    if(spacket != NULL) {
                        if(Kit_WriteList((Kit_List*)player->sbuffer, spacket) == 0) {
                            spackets[i] = NULL;
                        }
                    }
                }

                // Unlock subtitle buffer
                SDL_UnlockMutex(player->smutex);
            }

            // Couldn't write packet, free memory
            for(int i = 0; i < KIT_SBUFFERSIZE; i++) {
                if(spackets[i] != NULL) {
                    _FreeSubtitlePacket(spackets[i]);
                }
            }
        }
    }
}

/*

    scodec_ctx = (AVCodecContext*)player->scodec_ctx;
    if(scodec_ctx != NULL) {
        player->sformat.is_enabled = true;
        player->sformat.stream_idx = src->sstream_idx;

        // subtitle packet buffer
        player->sbuffer = Kit_CreateList(KIT_SBUFFERSIZE, _FreeSubtitlePacket);
        if(player->sbuffer == NULL) {
            Kit_SetError("Unable to initialize active subtitle list");
            goto error;
        }

        // Initialize libass renderer
        Kit_LibraryState *state = Kit_GetLibraryState();
        player->ass_renderer = ass_renderer_init(state->libass_handle);
        if(player->ass_renderer == NULL) {
            Kit_SetError("Unable to initialize libass renderer");
            goto error;
        }

        // Read fonts from attachment streams and give them to libass
        AVFormatContext *format_ctx = player->src->format_ctx;
        for (int j = 0; j < format_ctx->nb_streams; j++) {
            AVStream *st = format_ctx->streams[j];
            if(st->codec->codec_type == AVMEDIA_TYPE_ATTACHMENT && attachment_is_font(st)) {
                const AVDictionaryEntry *tag = av_dict_get(
                    st->metadata,
                    "filename",
                    NULL,
                    AV_DICT_MATCH_CASE);
                if(tag) {
                    ass_add_font(
                        state->libass_handle,
                        tag->value, 
                        (char*)st->codec->extradata,
                        st->codec->extradata_size);
                }
            }
        }

        // Init libass fonts and window frame size
        ass_set_fonts(player->ass_renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
        ass_set_frame_size(player->ass_renderer, vcodec_ctx->width, vcodec_ctx->height);
        ass_set_hinting(player->ass_renderer, ASS_HINTING_NONE);

        // Initialize libass track
        player->ass_track = ass_new_track(state->libass_handle);
        if(player->ass_track == NULL) {
            Kit_SetError("Unable to initialize libass track");
            goto error;
        }

        // Set up libass track headers (ffmpeg provides these)
        if(scodec_ctx->subtitle_header) {
            ass_process_codec_private(
                (ASS_Track*)player->ass_track,
                (char*)scodec_ctx->subtitle_header,
                scodec_ctx->subtitle_header_size);
        }
    }
*/
