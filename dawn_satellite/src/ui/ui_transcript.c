/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Transcript Panel Rendering
 */

#include "ui/ui_transcript.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef HAVE_SDL2_GFX
#include <SDL2/SDL2_gfxPrimitives.h>
#endif

#include "logging.h"
#include "tts/tts_preprocessing.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define LABEL_FONT_SIZE 18
#define BODY_FONT_SIZE 22
#define ROLE_FONT_SIZE 18
#define PADDING 20
#define LABEL_HEIGHT 36
#define ENTRY_SPACING 12
#define ROLE_SPACING 4

/* Fallback font paths if font_dir not specified */
#define FALLBACK_MONO_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define FALLBACK_BODY_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

/* =============================================================================
 * Texture Caching Helper
 * ============================================================================= */

static SDL_Texture *build_white_tex(SDL_Renderer *r,
                                    TTF_Font *font,
                                    const char *text,
                                    int *out_w,
                                    int *out_h) {
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, white);
   if (!s)
      return NULL;
   SDL_Texture *tex = SDL_CreateTextureFromSurface(r, s);
   if (out_w)
      *out_w = s->w;
   if (out_h)
      *out_h = s->h;
   SDL_FreeSurface(s);
   return tex;
}

/* =============================================================================
 * WiFi Signal Quality Reader
 * ============================================================================= */

/**
 * @brief Read WiFi link quality from /proc/net/wireless
 * @return 0-100 quality, or -1 if no wireless interface
 */
static int read_wifi_quality(void) {
   FILE *f = fopen("/proc/net/wireless", "r");
   if (!f)
      return -1;

   char line[256];
   int quality = -1;

   /* Skip 2 header lines */
   if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
      fclose(f);
      return -1;
   }

   /* Parse first interface line: "wlan0: 0000 70. -40. ..." */
   if (fgets(line, sizeof(line), f)) {
      float link_quality;
      if (sscanf(line, "%*s %*d %f", &link_quality) == 1) {
         quality = (int)link_quality;
         if (quality < 0)
            quality = 0;
         if (quality > 100)
            quality = 100;
      }
   }

   fclose(f);
   return quality;
}

/* =============================================================================
 * Font Loading Helper
 * ============================================================================= */

static TTF_Font *try_load_font(const char *font_dir,
                               const char *filename,
                               const char *fallback,
                               int size) {
   TTF_Font *font = NULL;
   char path[512];

   /* Try specified font_dir first */
   if (font_dir && font_dir[0]) {
      snprintf(path, sizeof(path), "%s/%s", font_dir, filename);
      font = TTF_OpenFont(path, size);
      if (font)
         return font;
   }

   /* Try relative to executable */
   snprintf(path, sizeof(path), "assets/fonts/%s", filename);
   font = TTF_OpenFont(path, size);
   if (font)
      return font;

   /* Try fallback system font */
   if (fallback) {
      font = TTF_OpenFont(fallback, size);
      if (font)
         return font;
   }

   return NULL;
}

/* =============================================================================
 * Texture Cache Helpers
 * ============================================================================= */

static void invalidate_entry_cache(transcript_entry_t *entry) {
   if (entry->cached_texture) {
      SDL_DestroyTexture(entry->cached_texture);
      entry->cached_texture = NULL;
   }
   if (entry->cached_role_tex) {
      SDL_DestroyTexture(entry->cached_role_tex);
      entry->cached_role_tex = NULL;
   }
   entry->cached_w = 0;
   entry->cached_h = 0;
   entry->cached_role_w = 0;
   entry->cached_role_h = 0;
}

static void ensure_entry_cached(ui_transcript_t *t, transcript_entry_t *entry) {
   if (!t->renderer || !t->body_font)
      return;

   /* Cache body text texture as white — tinted at render time for theme support */
   if (!entry->cached_texture && entry->text[0]) {
      SDL_Color text_color = { 255, 255, 255, 255 };

      /* AI entries with completed streaming get markdown rendering */
      if (!entry->is_user && !entry->is_streaming && t->md_fonts.fonts[0]) {
         SDL_Color bold_color = { 255, 255, 255, 255 };
         entry->cached_texture = md_render_text(t->renderer, &t->md_fonts, entry->text, text_color,
                                                bold_color, t->wrap_width, &entry->cached_w,
                                                &entry->cached_h);
      }

      /* User entries and streaming AI entries: plain text (fast path) */
      if (!entry->cached_texture) {
         SDL_Surface *surface = TTF_RenderUTF8_Blended_Wrapped(t->body_font, entry->text,
                                                               text_color, t->wrap_width);
         if (surface) {
            entry->cached_texture = SDL_CreateTextureFromSurface(t->renderer, surface);
            entry->cached_w = surface->w;
            entry->cached_h = surface->h;
            SDL_FreeSurface(surface);
         }
      }
   }

   /* Cache role label texture */
   if (!entry->cached_role_tex && entry->role[0] && t->label_font) {
      SDL_Color role_color;
      if (entry->is_user) {
         role_color = (SDL_Color){ COLOR_LISTENING_R, COLOR_LISTENING_G, COLOR_LISTENING_B, 255 };
      } else {
         ui_color_t ac = ui_theme_accent();
         role_color = (SDL_Color){ ac.r, ac.g, ac.b, 255 };
      }
      char role_label[40];
      snprintf(role_label, sizeof(role_label), "%s:", entry->role);
      if (!entry->is_user) {
         for (char *p = role_label; *p && *p != ':'; p++)
            *p = toupper((unsigned char)*p);
      }
      SDL_Surface *surface = TTF_RenderUTF8_Blended(t->label_font, role_label, role_color);
      if (surface) {
         entry->cached_role_tex = SDL_CreateTextureFromSurface(t->renderer, surface);
         entry->cached_role_w = surface->w;
         entry->cached_role_h = surface->h;
         SDL_FreeSurface(surface);
      }
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int ui_transcript_init(ui_transcript_t *t,
                       SDL_Renderer *renderer,
                       int panel_x,
                       int panel_y,
                       int panel_w,
                       int panel_h,
                       const char *font_dir,
                       const char *ai_name) {
   if (!t)
      return 1;

   memset(t, 0, sizeof(*t));
   pthread_mutex_init(&t->mutex, NULL);

   t->renderer = renderer;
   t->panel_x = panel_x;
   t->panel_y = panel_y;
   t->panel_w = panel_w;
   t->panel_h = panel_h;
   t->padding = PADDING;
   t->wrap_width = panel_w - 2 * PADDING;
   snprintf(t->ai_name, sizeof(t->ai_name), "%s", ai_name ? ai_name : "DAWN");
   t->auto_scroll = true;
   t->scroll_offset = 0;

   /* Load fonts */
   t->label_font = try_load_font(font_dir, "IBMPlexMono-Regular.ttf", FALLBACK_MONO_FONT,
                                 LABEL_FONT_SIZE);
   t->body_font = try_load_font(font_dir, "SourceSans3-Medium.ttf", FALLBACK_BODY_FONT,
                                BODY_FONT_SIZE);

   if (!t->label_font) {
      LOG_WARNING("Failed to load label font, transcript text disabled");
   }
   if (!t->body_font) {
      LOG_WARNING("Failed to load body font, transcript text disabled");
   }

   /* Initialize markdown font set for styled AI responses */
   if (md_fonts_init(&t->md_fonts, font_dir, BODY_FONT_SIZE) != 0) {
      LOG_WARNING("Markdown fonts init failed, AI responses will render as plain text");
   }

   return 0;
}

void ui_transcript_cleanup(ui_transcript_t *t) {
   if (!t)
      return;

   /* Destroy cached textures */
   for (int i = 0; i < TRANSCRIPT_MAX_ENTRIES; i++) {
      invalidate_entry_cache(&t->entries[i]);
   }
   if (t->music_icon_tex) {
      SDL_DestroyTexture(t->music_icon_tex);
      t->music_icon_tex = NULL;
   }
   if (t->cached_state_tex) {
      SDL_DestroyTexture(t->cached_state_tex);
      t->cached_state_tex = NULL;
   }
   if (t->cached_muted_tex) {
      SDL_DestroyTexture(t->cached_muted_tex);
      t->cached_muted_tex = NULL;
   }
   if (t->cached_time_tex) {
      SDL_DestroyTexture(t->cached_time_tex);
      t->cached_time_tex = NULL;
   }
   if (t->cached_detail_tex) {
      SDL_DestroyTexture(t->cached_detail_tex);
      t->cached_detail_tex = NULL;
   }

   if (t->label_font) {
      TTF_CloseFont(t->label_font);
      t->label_font = NULL;
   }
   if (t->body_font) {
      TTF_CloseFont(t->body_font);
      t->body_font = NULL;
   }

   md_fonts_cleanup(&t->md_fonts);

   pthread_mutex_destroy(&t->mutex);
}

void ui_transcript_add(ui_transcript_t *t, const char *role, const char *text, bool is_user) {
   if (!t || !role || !text)
      return;

   pthread_mutex_lock(&t->mutex);

   transcript_entry_t *entry = &t->entries[t->write_index];

   /* Invalidate old cached textures before reusing slot */
   invalidate_entry_cache(entry);

   snprintf(entry->role, sizeof(entry->role), "%s", role);
   snprintf(entry->text, sizeof(entry->text), "%s", text);
   remove_emojis(entry->text);
   entry->is_user = is_user;

   t->write_index = (t->write_index + 1) % TRANSCRIPT_MAX_ENTRIES;
   if (t->entry_count < TRANSCRIPT_MAX_ENTRIES) {
      t->entry_count++;
   }

   /* New entry: snap to bottom */
   t->scroll_offset = 0;
   t->auto_scroll = true;

   pthread_mutex_unlock(&t->mutex);
}

void ui_transcript_update_live(ui_transcript_t *t,
                               const char *role,
                               const char *text,
                               size_t text_len) {
   if (!t || !role || !text || text_len == 0)
      return;

   pthread_mutex_lock(&t->mutex);

   /* Find the most recent AI entry to update, or create one */
   transcript_entry_t *target = NULL;

   if (t->entry_count > 0) {
      int last_idx = (t->write_index - 1 + TRANSCRIPT_MAX_ENTRIES) % TRANSCRIPT_MAX_ENTRIES;
      transcript_entry_t *last = &t->entries[last_idx];
      if (!last->is_user) {
         target = last;
      }
   }

   if (!target) {
      /* No AI entry yet — create one (same as ui_transcript_add but without advancing) */
      target = &t->entries[t->write_index];
      invalidate_entry_cache(target);
      snprintf(target->role, sizeof(target->role), "%s", role);
      target->text[0] = '\0';
      target->is_user = false;
      target->is_streaming = true;

      t->write_index = (t->write_index + 1) % TRANSCRIPT_MAX_ENTRIES;
      if (t->entry_count < TRANSCRIPT_MAX_ENTRIES) {
         t->entry_count++;
      }
   }

   /* Update text if it has changed (compare lengths to avoid strcmp on every poll) */
   if (text_len != strlen(target->text)) {
      snprintf(target->text, sizeof(target->text), "%s", text);
      remove_emojis(target->text);
      /* Invalidate cached texture so it re-renders with new text */
      invalidate_entry_cache(target);
   }

   pthread_mutex_unlock(&t->mutex);
}

void ui_transcript_finalize_live(ui_transcript_t *t) {
   if (!t)
      return;

   pthread_mutex_lock(&t->mutex);

   if (t->entry_count > 0) {
      int last_idx = (t->write_index - 1 + TRANSCRIPT_MAX_ENTRIES) % TRANSCRIPT_MAX_ENTRIES;
      transcript_entry_t *last = &t->entries[last_idx];
      if (!last->is_user && last->is_streaming) {
         last->is_streaming = false;
         /* Invalidate cache so next frame re-renders with markdown styling */
         invalidate_entry_cache(last);
      }
   }

   pthread_mutex_unlock(&t->mutex);
}

void ui_transcript_scroll(ui_transcript_t *t, int delta_y) {
   if (!t)
      return;
   t->scroll_offset += delta_y;
   if (t->scroll_offset < 0)
      t->scroll_offset = 0;
   /* User is manually scrolling — disable auto-scroll until next interaction */
   t->auto_scroll = false;
}

void ui_transcript_scroll_to_bottom(ui_transcript_t *t) {
   if (!t)
      return;
   t->scroll_offset = 0;
   t->auto_scroll = true;
}

/* =============================================================================
 * Music Note Icon (SDL primitive, no font dependency)
 * ============================================================================= */

#define MUSIC_NOTE_DIM 18

/** Build double-note music icon: two stems, beam, two filled note heads */
static SDL_Texture *build_music_note_icon(SDL_Renderer *r, int sz) {
   SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sz,
                                        sz);
   if (!tex)
      return NULL;
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(r, tex);
   SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
   SDL_RenderClear(r);
   SDL_SetRenderDrawColor(r, 255, 255, 255, 255);

   /* Note head radius and positions (scaled to sz) */
   int head_r = sz / 6;
   if (head_r < 2)
      head_r = 2;

   /* Left note: head center at (head_r + 1, sz - head_r - 1) */
   int lhx = head_r + 1;
   int lhy = sz - head_r - 1;

   /* Right note: head center at (sz - head_r - 1, sz - head_r - 3) (slightly higher) */
   int rhx = sz - head_r - 1;
   int rhy = sz - head_r - 3;

   /* Left stem: from top of left head up to near top */
   int lstem_x = lhx + head_r;
   int lstem_top = 2;
   int lstem_bot = lhy - head_r / 2;
   SDL_RenderDrawLine(r, lstem_x, lstem_top, lstem_x, lstem_bot);
   SDL_RenderDrawLine(r, lstem_x + 1, lstem_top, lstem_x + 1, lstem_bot);

   /* Right stem: from top of right head up, slightly lower top */
   int rstem_x = rhx + head_r;
   int rstem_top = 4;
   int rstem_bot = rhy - head_r / 2;
   SDL_RenderDrawLine(r, rstem_x, rstem_top, rstem_x, rstem_bot);
   SDL_RenderDrawLine(r, rstem_x + 1, rstem_top, rstem_x + 1, rstem_bot);

   /* Beam connecting tops of stems (angled) */
   SDL_RenderDrawLine(r, lstem_x, lstem_top, rstem_x + 1, rstem_top);
   SDL_RenderDrawLine(r, lstem_x, lstem_top + 1, rstem_x + 1, rstem_top + 1);

   /* Filled note heads */
#ifdef HAVE_SDL2_GFX
   filledCircleRGBA(r, lhx, lhy, head_r, 255, 255, 255, 255);
   filledCircleRGBA(r, rhx, rhy, head_r, 255, 255, 255, 255);
   SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
#else
   for (int dy = -head_r; dy <= head_r; dy++) {
      int dx = (int)sqrtf((float)(head_r * head_r - dy * dy));
      SDL_RenderDrawLine(r, lhx - dx, lhy + dy, lhx + dx, lhy + dy);
      SDL_RenderDrawLine(r, rhx - dx, rhy + dy, rhx + dx, rhy + dy);
   }
#endif

   SDL_SetRenderTarget(r, NULL);
   return tex;
}

void ui_transcript_render(ui_transcript_t *t, SDL_Renderer *renderer, voice_state_t state) {
   if (!t || !renderer)
      return;
   ui_color_t bg1 = ui_theme_bg(1), bg2 = ui_theme_bg(2);
   ui_color_t txt0 = ui_theme_text(0), txt1 = ui_theme_text(1);

   /* Draw panel background */
   SDL_Rect panel = { t->panel_x, t->panel_y, t->panel_w, t->panel_h };
   SDL_SetRenderDrawColor(renderer, bg1.r, bg1.g, bg1.b, 255);
   SDL_RenderFillRect(renderer, &panel);

   int x = t->panel_x + t->padding;
   int label_y = t->panel_y + t->padding;

   /* Render state label at top (cached white texture + color mod) */
   if (t->label_font) {
      /* When disconnected, override to show [OFFLINE] in red */
      bool show_offline = !t->connected;
      ui_color_t state_color = show_offline
                                   ? (ui_color_t){ COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B }
                                   : ui_label_color_for_state(state);

      /* Rebuild state texture when state, muted, or connection status changes */
      if (!t->cached_state_tex || t->cached_state_val != state ||
          t->cached_state_muted != t->mic_muted || t->cached_state_connected != t->connected) {
         if (t->cached_state_tex)
            SDL_DestroyTexture(t->cached_state_tex);
         char state_str[32];
         if (show_offline)
            snprintf(state_str, sizeof(state_str), "[OFFLINE]");
         else
            snprintf(state_str, sizeof(state_str), "[%s]", ui_state_label(state));
         t->cached_state_tex = build_white_tex(renderer, t->label_font, state_str,
                                               &t->cached_state_w, &t->cached_state_h);
         t->cached_state_val = state;
         t->cached_state_muted = t->mic_muted;
         t->cached_state_connected = t->connected;
      }

      /* Draw state label with state-specific color */
      if (t->cached_state_tex) {
         SDL_SetTextureColorMod(t->cached_state_tex, state_color.r, state_color.g, state_color.b);
         SDL_Rect dst = { x, label_y, t->cached_state_w, t->cached_state_h };
         SDL_RenderCopy(renderer, t->cached_state_tex, NULL, &dst);
      }

      /* Draw [MUTED] indicator in red (cached, rarely changes) */
      if (t->mic_muted) {
         if (!t->cached_muted_tex) {
            t->cached_muted_tex = build_white_tex(renderer, t->label_font, "[MUTED]",
                                                  &t->cached_muted_w, &t->cached_muted_h);
         }
         if (t->cached_muted_tex) {
            SDL_SetTextureColorMod(t->cached_muted_tex, COLOR_ERROR_R, COLOR_ERROR_G,
                                   COLOR_ERROR_B);
            int muted_x = x + t->cached_state_w + 8;
            SDL_Rect dst = { muted_x, label_y, t->cached_muted_w, t->cached_muted_h };
            SDL_RenderCopy(renderer, t->cached_muted_tex, NULL, &dst);
         }
      }
   }

   /* Render date/time top-right (cached, invalidates once per minute) */
   if (t->label_font) {
      time_t now = time(NULL);
      struct tm *tm_info = localtime(&now);

      /* Rebuild time texture only when minute changes */
      if (!t->cached_time_tex || t->cached_time_min != tm_info->tm_min) {
         if (t->cached_time_tex)
            SDL_DestroyTexture(t->cached_time_tex);
         char time_str[40];
         if (t->time_24h)
            strftime(time_str, sizeof(time_str), "%a %b %-d  %H:%M", tm_info);
         else
            strftime(time_str, sizeof(time_str), "%a %b %-d  %-I:%M %p", tm_info);
         t->cached_time_tex = build_white_tex(renderer, t->label_font, time_str, &t->cached_time_w,
                                              &t->cached_time_h);
         t->cached_time_min = tm_info->tm_min;
      }

      if (t->cached_time_tex) {
         SDL_SetTextureColorMod(t->cached_time_tex, txt1.r, txt1.g, txt1.b);
         int time_x = t->panel_x + t->panel_w - t->padding - t->cached_time_w;
         SDL_Rect dst = { time_x, label_y, t->cached_time_w, t->cached_time_h };
         SDL_RenderCopy(renderer, t->cached_time_tex, NULL, &dst);

         /* WiFi signal indicator (4 bars to the left of date/time) */
         if (now != t->last_wifi_poll) {
            t->last_wifi_poll = now;
            t->wifi_quality = read_wifi_quality();
         }

         int wifi_left_edge = time_x; /* Track leftmost WiFi element */

         if (t->wifi_quality >= 0) {
            int wifi_bars;
            if (t->wifi_quality >= 70)
               wifi_bars = 4;
            else if (t->wifi_quality >= 50)
               wifi_bars = 3;
            else if (t->wifi_quality >= 30)
               wifi_bars = 2;
            else if (t->wifi_quality >= 10)
               wifi_bars = 1;
            else
               wifi_bars = 0;

            int bar_gap = 3;
            int bar_w = 4;
            int wifi_total_w = 4 * bar_w + 3 * bar_gap;
            int wifi_x = time_x - wifi_total_w - 12;
            int wifi_base_y = label_y + t->cached_time_h - 2;
            wifi_left_edge = wifi_x;

            for (int b = 0; b < 4; b++) {
               int bar_h = 4 + b * 4; /* Heights: 4, 8, 12, 16 */
               int bx = wifi_x + b * (bar_w + bar_gap);
               int by = wifi_base_y - bar_h;

               if (b < wifi_bars) {
                  SDL_SetRenderDrawColor(renderer, txt1.r, txt1.g, txt1.b, 255);
               } else {
                  SDL_SetRenderDrawColor(renderer, bg2.r, bg2.g, bg2.b, 255);
               }

               SDL_Rect bar_rect = { bx, by, bar_w, bar_h };
               SDL_RenderFillRect(renderer, &bar_rect);
            }
         }

         /* Music icon button (left of WiFi bars) — only when Opus is available */
         if (t->show_music_btn) {
            /* Build cached SDL primitive icon on first use */
            if (!t->music_icon_tex) {
               t->music_icon_tex = build_music_note_icon(renderer, MUSIC_NOTE_DIM);
               t->music_icon_w = MUSIC_NOTE_DIM;
               t->music_icon_h = MUSIC_NOTE_DIM;
            }
            if (t->music_icon_tex) {
               if (t->music_playing) {
                  ui_color_t mac = ui_theme_accent();
                  SDL_SetTextureColorMod(t->music_icon_tex, mac.r, mac.g, mac.b);
               } else {
                  SDL_SetTextureColorMod(t->music_icon_tex, txt1.r, txt1.g, txt1.b);
               }
               int icon_x = wifi_left_edge - t->music_icon_w - 14;
               int icon_y = label_y + (t->cached_time_h - t->music_icon_h) / 2;
               SDL_Rect mdst = { icon_x, icon_y, t->music_icon_w, t->music_icon_h };
               SDL_RenderCopy(renderer, t->music_icon_tex, NULL, &mdst);

               /* Store 48x48 hit area centered on the glyph */
               t->music_btn_w = 48;
               t->music_btn_h = 48;
               t->music_btn_x = icon_x + t->music_icon_w / 2 - t->music_btn_w / 2;
               t->music_btn_y = icon_y + t->music_icon_h / 2 - t->music_btn_h / 2;
            }
         }
      }
   }

   /* Render status detail below state label (cached, invalidates on text change) */
   int detail_height = 0;
   if (t->label_font && t->status_detail[0]) {
      /* Rebuild detail texture only when text changes */
      if (!t->cached_detail_tex || strcmp(t->cached_detail_str, t->status_detail) != 0) {
         if (t->cached_detail_tex)
            SDL_DestroyTexture(t->cached_detail_tex);
         t->cached_detail_tex = build_white_tex(renderer, t->label_font, t->status_detail,
                                                &t->cached_detail_w, &t->cached_detail_h);
         strncpy(t->cached_detail_str, t->status_detail, sizeof(t->cached_detail_str) - 1);
         t->cached_detail_str[sizeof(t->cached_detail_str) - 1] = '\0';
      }
      if (t->cached_detail_tex) {
         SDL_SetTextureColorMod(t->cached_detail_tex, txt1.r, txt1.g, txt1.b);
         int detail_y = label_y + LABEL_HEIGHT - 4;
         SDL_Rect dst = { x, detail_y, t->cached_detail_w, t->cached_detail_h };
         SDL_RenderCopy(renderer, t->cached_detail_tex, NULL, &dst);
         detail_height = t->cached_detail_h + 4;
      }
   } else if (!t->status_detail[0] && t->cached_detail_tex) {
      /* Detail cleared — free the cached texture */
      SDL_DestroyTexture(t->cached_detail_tex);
      t->cached_detail_tex = NULL;
      t->cached_detail_str[0] = '\0';
   }

   /* Transcript content area (below label + optional detail, above bottom padding) */
   int content_top = t->panel_y + t->padding + LABEL_HEIGHT + detail_height + ENTRY_SPACING;
   int content_bottom = t->panel_y + t->panel_h - t->padding;

   if (!t->body_font || t->entry_count == 0)
      return;

   pthread_mutex_lock(&t->mutex);

   /* Snapshot entry data under lock, then release before rendering */
   int count = t->entry_count;
   int start_idx;
   if (count < TRANSCRIPT_MAX_ENTRIES) {
      start_idx = 0;
   } else {
      start_idx = t->write_index; /* Oldest entry */
   }

   /* Ensure all entries have cached textures (under lock since we read entry data) */
   for (int i = 0; i < count; i++) {
      int idx = (start_idx + i) % TRANSCRIPT_MAX_ENTRIES;
      ensure_entry_cached(t, &t->entries[idx]);
   }

   /* Calculate total height of all entries */
   int total_height = 0;
   for (int i = 0; i < count; i++) {
      int idx = (start_idx + i) % TRANSCRIPT_MAX_ENTRIES;
      transcript_entry_t *entry = &t->entries[idx];
      if (entry->cached_role_tex)
         total_height += entry->cached_role_h + ROLE_SPACING;
      if (entry->cached_texture)
         total_height += entry->cached_h;
      if (i < count - 1)
         total_height += ENTRY_SPACING;
   }
   t->total_height = total_height;

   /* Clamp scroll_offset to valid range.
    * scroll_offset = 0 → at bottom (newest visible)
    * scroll_offset = max_scroll → at top (oldest visible) */
   int avail_height = content_bottom - content_top;
   int max_scroll = total_height > avail_height ? total_height - avail_height : 0;
   if (t->scroll_offset > max_scroll)
      t->scroll_offset = max_scroll;

   /* Calculate y start position */
   int y;
   if (total_height <= avail_height) {
      y = content_top; /* Content fits, render from top */
   } else if (t->auto_scroll) {
      y = content_bottom - total_height; /* Auto-scroll: newest at bottom */
   } else {
      y = content_bottom - total_height + t->scroll_offset;
   }

   /* Render entries top-to-bottom (oldest to newest) with clipping */
   SDL_Rect clip = { t->panel_x, content_top, t->panel_w, avail_height };
   SDL_RenderSetClipRect(renderer, &clip);

   for (int i = 0; i < count; i++) {
      int idx = (start_idx + i) % TRANSCRIPT_MAX_ENTRIES;
      transcript_entry_t *entry = &t->entries[idx];

      /* Skip entries entirely above the visible area */
      int entry_height = 0;
      if (entry->cached_role_tex)
         entry_height += entry->cached_role_h + ROLE_SPACING;
      if (entry->cached_texture)
         entry_height += entry->cached_h;

      if (y + entry_height < content_top) {
         y += entry_height + ENTRY_SPACING;
         continue;
      }

      /* Stop if we're past the bottom */
      if (y >= content_bottom)
         break;

      /* Render role label from cache */
      if (entry->cached_role_tex) {
         SDL_Rect dst = { x, y, entry->cached_role_w, entry->cached_role_h };
         SDL_RenderCopy(renderer, entry->cached_role_tex, NULL, &dst);
         y += entry->cached_role_h + ROLE_SPACING;
      }

      /* Render body text from cache (white texture, tinted for theme) */
      if (entry->cached_texture) {
         SDL_SetTextureColorMod(entry->cached_texture, txt0.r, txt0.g, txt0.b);
         SDL_Rect dst = { x, y, entry->cached_w, entry->cached_h };
         SDL_RenderCopy(renderer, entry->cached_texture, NULL, &dst);
         y += entry->cached_h;
      }

      y += ENTRY_SPACING;
   }

   SDL_RenderSetClipRect(renderer, NULL);
   pthread_mutex_unlock(&t->mutex);
}
