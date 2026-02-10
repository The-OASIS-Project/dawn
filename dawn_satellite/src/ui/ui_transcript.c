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
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "tts/tts_preprocessing.h"
#include "ui/ui_colors.h"

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

   /* Cache body text texture */
   if (!entry->cached_texture && entry->text[0]) {
      SDL_Color text_color = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B,
                               255 };

      /* AI entries with completed streaming get markdown rendering */
      if (!entry->is_user && !entry->is_streaming && t->md_fonts.fonts[0]) {
         SDL_Color bold_color = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B,
                                  255 };
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
         role_color = (SDL_Color){ COLOR_SPEAKING_R, COLOR_SPEAKING_G, COLOR_SPEAKING_B, 255 };
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

void ui_transcript_render(ui_transcript_t *t, SDL_Renderer *renderer, voice_state_t state) {
   if (!t || !renderer)
      return;

   /* Draw panel background */
   SDL_Rect panel = { t->panel_x, t->panel_y, t->panel_w, t->panel_h };
   SDL_SetRenderDrawColor(renderer, COLOR_BG_SECONDARY_R, COLOR_BG_SECONDARY_G,
                          COLOR_BG_SECONDARY_B, 255);
   SDL_RenderFillRect(renderer, &panel);

   int x = t->panel_x + t->padding;
   int label_y = t->panel_y + t->padding;

   /* Render state label at top */
   if (t->label_font) {
      ui_color_t state_color = ui_label_color_for_state(state);
      SDL_Color label_sdl = { state_color.r, state_color.g, state_color.b, 255 };

      char label_text[32];
      snprintf(label_text, sizeof(label_text), "[%s]", ui_state_label(state));

      /* State label changes frequently so we don't cache it */
      SDL_Surface *label_surface = TTF_RenderUTF8_Blended(t->label_font, label_text, label_sdl);
      if (label_surface) {
         SDL_Texture *label_tex = SDL_CreateTextureFromSurface(renderer, label_surface);
         if (label_tex) {
            SDL_Rect dst = { x, label_y, label_surface->w, label_surface->h };
            SDL_RenderCopy(renderer, label_tex, NULL, &dst);
            SDL_DestroyTexture(label_tex);
         }
         SDL_FreeSurface(label_surface);
      }
   }

   /* Render date/time top-right */
   if (t->label_font) {
      time_t now = time(NULL);
      struct tm *tm_info = localtime(&now);
      char time_str[40];
      strftime(time_str, sizeof(time_str), "%a %b %-d  %-I:%M %p", tm_info);

      SDL_Color time_color = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                               COLOR_TEXT_SECONDARY_B, 255 };
      SDL_Surface *time_surface = TTF_RenderUTF8_Blended(t->label_font, time_str, time_color);
      if (time_surface) {
         int time_x = t->panel_x + t->panel_w - t->padding - time_surface->w;
         SDL_Texture *time_tex = SDL_CreateTextureFromSurface(renderer, time_surface);
         if (time_tex) {
            SDL_Rect dst = { time_x, label_y, time_surface->w, time_surface->h };
            SDL_RenderCopy(renderer, time_tex, NULL, &dst);
            SDL_DestroyTexture(time_tex);
         }

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
            int wifi_base_y = label_y + time_surface->h - 2;
            wifi_left_edge = wifi_x;

            for (int b = 0; b < 4; b++) {
               int bar_h = 4 + b * 4; /* Heights: 4, 8, 12, 16 */
               int bx = wifi_x + b * (bar_w + bar_gap);
               int by = wifi_base_y - bar_h;

               if (b < wifi_bars) {
                  SDL_SetRenderDrawColor(renderer, COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                                         COLOR_TEXT_SECONDARY_B, 255);
               } else {
                  SDL_SetRenderDrawColor(renderer, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G,
                                         COLOR_BG_TERTIARY_B, 255);
               }

               SDL_Rect bar_rect = { bx, by, bar_w, bar_h };
               SDL_RenderFillRect(renderer, &bar_rect);
            }
         }

         /* Music icon button (left of WiFi bars) */
         {
            /* Build cached white glyph on first use */
            if (!t->music_icon_tex && t->label_font) {
               SDL_Color white = { 255, 255, 255, 255 };
               SDL_Surface *ms = TTF_RenderUTF8_Blended(t->label_font, "\xE2\x99\xAA", white);
               if (ms) {
                  t->music_icon_tex = SDL_CreateTextureFromSurface(renderer, ms);
                  t->music_icon_w = ms->w;
                  t->music_icon_h = ms->h;
                  SDL_FreeSurface(ms);
               }
            }
            if (t->music_icon_tex) {
               if (t->music_playing) {
                  SDL_SetTextureColorMod(t->music_icon_tex, 0x2D, 0xD4, 0xBF);
               } else {
                  SDL_SetTextureColorMod(t->music_icon_tex, COLOR_TEXT_SECONDARY_R,
                                         COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B);
               }
               int icon_x = wifi_left_edge - t->music_icon_w - 14;
               int icon_y = label_y + (time_surface->h - t->music_icon_h) / 2;
               SDL_Rect mdst = { icon_x, icon_y, t->music_icon_w, t->music_icon_h };
               SDL_RenderCopy(renderer, t->music_icon_tex, NULL, &mdst);

               /* Store 48x48 hit area centered on the glyph */
               t->music_btn_w = 48;
               t->music_btn_h = 48;
               t->music_btn_x = icon_x + t->music_icon_w / 2 - t->music_btn_w / 2;
               t->music_btn_y = icon_y + t->music_icon_h / 2 - t->music_btn_h / 2;
            }
         }

         SDL_FreeSurface(time_surface);
      }
   }

   /* Render status detail below state label (tool calls, thinking info) */
   int detail_height = 0;
   if (t->label_font && t->status_detail[0]) {
      SDL_Color detail_color = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                                 COLOR_TEXT_SECONDARY_B, 255 };
      SDL_Surface *detail_surface = TTF_RenderUTF8_Blended(t->label_font, t->status_detail,
                                                           detail_color);
      if (detail_surface) {
         SDL_Texture *detail_tex = SDL_CreateTextureFromSurface(renderer, detail_surface);
         if (detail_tex) {
            int detail_y = label_y + LABEL_HEIGHT - 4;
            SDL_Rect dst = { x, detail_y, detail_surface->w, detail_surface->h };
            SDL_RenderCopy(renderer, detail_tex, NULL, &dst);
            SDL_DestroyTexture(detail_tex);
            detail_height = detail_surface->h + 4;
         }
         SDL_FreeSurface(detail_surface);
      }
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

      /* Render body text from cache */
      if (entry->cached_texture) {
         SDL_Rect dst = { x, y, entry->cached_w, entry->cached_h };
         SDL_RenderCopy(renderer, entry->cached_texture, NULL, &dst);
         y += entry->cached_h;
      }

      y += ENTRY_SPACING;
   }

   SDL_RenderSetClipRect(renderer, NULL);
   pthread_mutex_unlock(&t->mutex);
}
