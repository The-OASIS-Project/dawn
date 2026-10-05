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
 * the project author(s).
 * Prompt-cache stats operator opcode (dawn-admin cache stats): the usage log
 * (llm_usage_log) as a table per provider, model and kind: calls, prompt
 * tokens, coverage (read / prompt), input tokens' worth saved (list prices;
 * writes count against), warm misses and thinking blocks dropped for a prefix
 * change.  Trusts SO_PEERCRED
 * like the other operator commands.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/admin_socket.h"
#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "llm/llm_pricing.h"

/* The longest window a stats query reads back: the usage log keeps 90 days. */
#define CACHE_STATS_MAX_HOURS (90 * 24)
/* Groups (provider, model, kind) a table shows: they fit the 4 KB reply. */
#define CACHE_STATS_ROWS 40

/* @p n tokens, short ("812", "45K", "3.2M"). */
static void short_count(int64_t n, char *out, size_t len) {
   const int64_t a = n < 0 ? -n : n;
   if (a >= 10000000) {
      snprintf(out, len, "%lldM", (long long)(n / 1000000));
   } else if (a >= 1000000) {
      snprintf(out, len, "%.1fM", (double)n / 1e6);
   } else if (a >= 10000) {
      snprintf(out, len, "%lldK", (long long)(n / 1000));
   } else {
      snprintf(out, len, "%lld", (long long)n);
   }
}

/* Input tokens' worth saved on @p read / @p write: the pricing takes ints, so
 * sums past them (90 days of a heavy user) are scaled down and back up. */
static int64_t saved_tokens(llm_type_t type,
                            cloud_provider_t provider,
                            const char *model,
                            int64_t read,
                            int64_t write) {
   const int64_t big = read > write ? read : write;
   const int64_t scale = big / INT32_MAX + 1;
   return (int64_t)llm_cache_saved_input_tokens(type, provider, model, (int)(read / scale),
                                                (int)(write / scale)) *
          scale;
}

void admin_cache_format_stats(const llm_usage_stat_t *rows,
                              int n,
                              bool more,
                              char *out,
                              size_t out_len) {
   if (!out || out_len == 0) {
      return;
   }
   size_t off = 0;
   int w = snprintf(out, out_len, "%-10s %-28s %-10s %6s %7s %5s %7s %6s %5s\n", "provider",
                    "model", "kind", "calls", "prompt", "cache", "saved", "misses", "drops");
   off = w > 0 && (size_t)w < out_len ? (size_t)w : out_len - 1;
   int shown = 0;
   for (int i = 0; i < n; i++) {
      const llm_usage_stat_t *r = &rows[i];
      llm_type_t type = LLM_CLOUD;
      cloud_provider_t provider = CLOUD_PROVIDER_NONE;
      const int64_t saved = cloud_provider_from_string(r->provider, &type, &provider) == SUCCESS
                                ? saved_tokens(type, provider, r->model, r->cache_read_tokens,
                                               r->cache_write_tokens)
                                : 0;
      char prompt[16];
      char saved_s[16];
      short_count(r->prompt_tokens, prompt, sizeof(prompt));
      short_count(saved, saved_s, sizeof(saved_s));
      const int cover = r->prompt_tokens > 0 ? (int)(r->cache_read_tokens * 100 / r->prompt_tokens)
                                             : 0;
      /* A local model is a file path: its name says which. */
      const char *model = r->model[0] ? r->model : "?";
      const char *slash = strcmp(r->provider, "local") == 0 ? strrchr(model, '/') : NULL;
      if (slash && slash[1]) {
         model = slash + 1;
      }
      char line[160];
      const int lw = snprintf(line, sizeof(line),
                              "%-10.10s %-28.28s %-10.10s %6d %7s %4d%% %7s %6d "
                              "%5lld\n",
                              r->provider, model, r->kind, r->calls, prompt, cover, saved_s,
                              r->warm_misses, (long long)r->binding_drops);
      /* Keep room for the note below; a cut table says how much it left out. */
      if (lw <= 0 || off + (size_t)lw + 256 >= out_len) {
         break;
      }
      memcpy(out + off, line, (size_t)lw);
      off += (size_t)lw;
      shown++;
   }
   if (shown < n || more) {
      w = snprintf(out + off, out_len - off, "... more not shown%s\n",
                   more ? " (narrow it with --since or --provider)" : "");
      off += w > 0 && (size_t)w < out_len - off ? (size_t)w : 0;
   }
   if (n == 0) {
      w = snprintf(out + off, out_len - off, "(no calls in that window)\n");
      off += w > 0 && (size_t)w < out_len - off ? (size_t)w : 0;
   }
   snprintf(out + off, out_len - off,
            "cache: read / prompt.  saved: input tokens' worth saved by caching (list prices;\n"
            "writes count against).  misses: warm misses.  drops: thinking blocks dropped\n"
            "for a prefix change (a DAWN bug).");
}


int handle_cache_stats_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   /* [hours i32][provider bytes, optional] */
   if (!payload || payload_len < 4) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Usage: cache stats [--since 24h|7d]");
   }
   int32_t hours = 0;
   memcpy(&hours, payload, sizeof(hours));
   if (hours <= 0 || hours > CACHE_STATS_MAX_HOURS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "--since must be between 1h and 90d");
   }
   char provider[16] = "";
   const uint16_t plen = (uint16_t)(payload_len - 4);
   if (plen >= sizeof(provider)) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Unknown provider");
   }
   memcpy(provider, payload + 4, plen);
   provider[plen] = '\0';

   char text[ADMIN_MSG_CONTENT_MAX + 1];
   const int64_t since = (int64_t)time(NULL) - (int64_t)hours * 3600;
   llm_usage_stat_t rows[CACHE_STATS_ROWS + 1]; /* one more: whether any were left out */
   int n = 0;
   if (auth_db_llm_usage_stats(since, provider[0] ? provider : NULL, rows, CACHE_STATS_ROWS + 1,
                               &n) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "The usage log couldn't be read.");
   }
   const bool more = n > CACHE_STATS_ROWS;
   admin_cache_format_stats(rows, more ? CACHE_STATS_ROWS : n, more, text, sizeof(text));
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, text);
}
