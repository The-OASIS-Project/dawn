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
 *
 * Discord driver — channel-history READ path (pull-only REST).  Discovery
 * (GET /users/@me/guilds + /guilds/{id}/channels, cached) and history
 * (GET /channels/{id}/messages, backward-paginated by snowflake cursor).
 * Entirely separate from the DM-only gateway listener in messaging_discord.c —
 * reads happen only when the engine asks, never as a push firehose.  Split out
 * of messaging_discord.c for size; see messaging_discord_internal.h.
 */
#define _GNU_SOURCE /* strdup */

#include <curl/curl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/curl_buffer.h"
#include "core/iso8601.h"
#include "core/tz.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_discord_internal.h"

/* =============================================================================
 * Read-path constants
 * ============================================================================= */
#define DC_SNOWFLAKE_EPOCH_MS 1420070400000LL /* Discord epoch: 2015-01-01T00:00Z */
#define DC_SNOWFLAKE_TS_SHIFT 22              /* snowflake: timestamp occupies bits 63..22 */
#define DC_MS_PER_SEC 1000LL
#define DC_SNOWFLAKE_BUF_SIZE (DC_SNOWFLAKE_MAX_DIGITS + 2) /* 20 digits + NUL + margin */
#define DC_READ_PAGE_MAX 100  /* Get-Channel-Messages per-request cap */
#define DC_READ_TOTAL_MAX 300 /* hard cap on messages across pagination */
/* DC_READ_TOTAL_MAX mirrors MSG_READ_LIMIT_MAX in messaging_engine_read.c. */
#define DC_READ_MAX_PAGES 5 /* pagination page-count safety bound */
#define DC_REST_TIMEOUT_SEC 30L
#define DC_GUILD_SCAN_MAX 25        /* cap guilds enumerated per discovery */
#define DC_CHAN_TYPE_TEXT 0         /* GUILD_TEXT */
#define DC_CHAN_TYPE_ANNOUNCEMENT 5 /* GUILD_ANNOUNCEMENT */
#define DC_CHAN_CACHE_TTL_SEC 300   /* discovery cache TTL (5 min) */
/* A name-resolution miss only busts the cache if it's older than this — so a
 * plain typo'd channel name (which will never resolve) doesn't trigger a full
 * guild re-enumeration on every attempt, while a channel genuinely created a
 * little while ago is still picked up. */
#define DC_CACHE_MIN_REFRESH_SEC 30
#define DC_REST_RESP_MAX (2 * 1024 * 1024) /* 2 MB cap on a REST response body */
#define DC_MAX_CHANNELS 500                /* defensive cap on discovered channels */
#define DC_RATE_BACKOFF_DEFAULT_SEC 5      /* backoff after a 429 with no Retry-After */
#define DC_RATE_BACKOFF_MAX_SEC 60         /* clamp on a server-supplied Retry-After */

/* =============================================================================
 * Read-path state
 * ============================================================================= */

/* Dedicated REST handle for the read path so a whole-server sweep (up to ~20
 * sequential round-trips) serializes only against other reads — never against
 * latency-sensitive outbound DMs on the send handle. */
static CURL *s_read_curl = NULL;
static pthread_mutex_t s_read_curl_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Discord 429 backoff: when the API rate-limits us, fast-fail every read until
 * this wall-clock time (Unix secs) so we don't keep hammering the route and risk
 * a token-wide ban — the real danger during a multi-channel sweep.  Guarded by
 * s_read_curl_mutex (only ever read/written inside dc_rest_get). */
static int64_t s_rate_backoff_until = 0;

/* Channel-discovery cache.  The driver owns the only TTL; the engine treats
 * each list_readable_channels() result as authoritative and never caches the
 * parsed form. */
static char *s_chan_cache_json = NULL;
static int64_t s_chan_cache_at = 0;
static pthread_mutex_t s_chan_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * REST helpers
 * ============================================================================= */

/* Perform an authenticated REST GET into `resp` (caller inits/frees).
 * Returns SUCCESS on HTTP 2xx, FAILURE otherwise.  Logs the URL only (the
 * token rides the Authorization header, never the URL). */
static int dc_rest_get(const char *url, curl_buffer_t *resp) {
   if (!url || !resp || s_bot_token[0] == '\0') {
      return FAILURE;
   }
   char auth_header[DC_BOT_TOKEN_MAX + 32];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bot %s", s_bot_token);

   int rc = FAILURE;
   pthread_mutex_lock(&s_read_curl_mutex);
   /* If Discord recently 429'd us, fast-fail without a network call until the
    * backoff expires — protects the bot token from a route-wide ban when a
    * sweep would otherwise fire the next channel's request immediately. */
   int64_t now = (int64_t)time(NULL);
   if (s_rate_backoff_until > now) {
      pthread_mutex_unlock(&s_read_curl_mutex);
      OLOG_WARNING("discord: read skipped — backing off %lld s after a 429",
                   (long long)(s_rate_backoff_until - now));
      return FAILURE;
   }
   if (!s_read_curl) {
      s_read_curl = curl_easy_init();
   }
   if (s_read_curl) {
      curl_easy_reset(s_read_curl);
      curl_easy_setopt(s_read_curl, CURLOPT_URL, url);
      curl_easy_setopt(s_read_curl, CURLOPT_HTTPGET, 1L);
      curl_apply_dawn_defaults(s_read_curl, DC_USER_AGENT, DC_REST_TIMEOUT_SEC, resp);

      struct curl_slist *hdrs = curl_slist_append(NULL, auth_header);
      if (!hdrs) {
         /* Bail rather than send an unauthenticated GET (→ confusing 401). */
         pthread_mutex_unlock(&s_read_curl_mutex);
         return FAILURE;
      }
      curl_easy_setopt(s_read_curl, CURLOPT_HTTPHEADER, hdrs);

      CURLcode cc = curl_easy_perform(s_read_curl);
      long http_status = 0;
      curl_easy_getinfo(s_read_curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_slist_free_all(hdrs);

      if (cc == CURLE_OK && http_status >= 200 && http_status < 300) {
         rc = SUCCESS;
      } else {
         if (http_status == 429) {
            /* Honor Retry-After when curl exposes it; otherwise a fixed backoff.
             * Clamp so a hostile/huge value can't wedge reads for too long. */
            curl_off_t retry_after = 0;
            curl_easy_getinfo(s_read_curl, CURLINFO_RETRY_AFTER, &retry_after);
            int64_t backoff = retry_after > 0 ? (int64_t)retry_after : DC_RATE_BACKOFF_DEFAULT_SEC;
            if (backoff > DC_RATE_BACKOFF_MAX_SEC) {
               backoff = DC_RATE_BACKOFF_MAX_SEC;
            }
            s_rate_backoff_until = now + backoff;
            OLOG_WARNING("discord: GET %s rate-limited (429) — backing off %lld s", url,
                         (long long)backoff);
         } else {
            OLOG_WARNING("discord: GET %s failed (curl=%d http=%ld)", url, cc, http_status);
         }
      }
   }
   pthread_mutex_unlock(&s_read_curl_mutex);
   return rc;
}

/* Map a Unix-seconds bound to a synthetic Discord snowflake usable as an
 * `after`/`before` cursor.  Returns 0 ("no bound") for any pre-epoch input —
 * guards the (ms - EPOCH) << 22 shift against underflow/wrap (e.g. a zero or
 * negative target_ts from the temporal parser, or a literal pre-2015 date). */
static uint64_t dc_snowflake_from_ts(int64_t ts) {
   if (ts <= 0) {
      return 0;
   }
   int64_t ms = ts * DC_MS_PER_SEC;
   if (ms < DC_SNOWFLAKE_EPOCH_MS) {
      return 0;
   }
   return ((uint64_t)(ms - DC_SNOWFLAKE_EPOCH_MS)) << DC_SNOWFLAKE_TS_SHIFT;
}

/* Extract a message's snowflake id as a number (0 if missing/malformed). */
static uint64_t dc_msg_snowflake(struct json_object *msg) {
   struct json_object *id_obj = NULL;
   if (!msg || !json_object_object_get_ex(msg, "id", &id_obj) || !id_obj) {
      return 0;
   }
   const char *id_str = json_object_get_string(id_obj);
   if (!id_str || !dc_is_valid_snowflake(id_str)) {
      return 0;
   }
   return strtoull(id_str, NULL, 10);
}

/* Append one Discord message JSON object (from the API) to `out_arr` in the
 * neutral read_history shape.  Returns false if the message lacks a usable
 * id. */
static bool dc_append_message(struct json_object *msg, struct json_object *out_arr) {
   if (!msg || !out_arr) {
      return false;
   }
   struct json_object *id_obj = NULL;
   if (!json_object_object_get_ex(msg, "id", &id_obj) || !id_obj) {
      return false;
   }
   const char *id_str = json_object_get_string(id_obj);
   if (!id_str || !dc_is_valid_snowflake(id_str)) {
      return false;
   }

   /* author: global_name → username; bot flag */
   const char *author = NULL;
   int is_bot = 0;
   struct json_object *author_obj = NULL;
   if (json_object_object_get_ex(msg, "author", &author_obj) && author_obj) {
      struct json_object *gn = NULL, *un = NULL, *bot = NULL;
      if (json_object_object_get_ex(author_obj, "global_name", &gn) && gn &&
          !json_object_is_type(gn, json_type_null)) {
         author = json_object_get_string(gn);
      }
      if (!author && json_object_object_get_ex(author_obj, "username", &un) && un) {
         author = json_object_get_string(un);
      }
      if (json_object_object_get_ex(author_obj, "bot", &bot) && json_object_get_boolean(bot)) {
         is_bot = 1;
      }
   }
   if (!author) {
      author = "unknown";
   }

   const char *content = "";
   struct json_object *content_obj = NULL;
   if (json_object_object_get_ex(msg, "content", &content_obj) && content_obj) {
      content = json_object_get_string(content_obj);
   }

   /* Attachment/embed-only messages are common in image-heavy channels.  When
    * there's no text, describe what's there ("[image]", "[2 attachments]",
    * "[embed]") so the summary isn't a wall of empty placeholders.  `desc` is
    * copied by json_object_new_string below, so the stack buffer is safe. */
   char desc[64];
   if (!content || !content[0]) {
      struct json_object *atts = NULL, *embs = NULL;
      int n_att = 0, n_emb = 0;
      if (json_object_object_get_ex(msg, "attachments", &atts) &&
          json_object_is_type(atts, json_type_array)) {
         n_att = (int)json_object_array_length(atts);
      }
      if (json_object_object_get_ex(msg, "embeds", &embs) &&
          json_object_is_type(embs, json_type_array)) {
         n_emb = (int)json_object_array_length(embs);
      }
      if (n_att > 0) {
         const char *kind = "attachment";
         struct json_object *a0 = json_object_array_get_idx(atts, 0), *ct = NULL;
         if (a0 && json_object_object_get_ex(a0, "content_type", &ct)) {
            const char *ctype = json_object_get_string(ct);
            if (ctype) {
               if (strncmp(ctype, "image/", 6) == 0) {
                  kind = "image";
               } else if (strncmp(ctype, "video/", 6) == 0) {
                  kind = "video";
               } else if (strncmp(ctype, "audio/", 6) == 0) {
                  kind = "audio";
               }
            }
         }
         if (n_att > 1) {
            snprintf(desc, sizeof(desc), "[%d %ss]", n_att, kind);
         } else {
            snprintf(desc, sizeof(desc), "[%s]", kind);
         }
         content = desc;
      } else if (n_emb > 0) {
         if (n_emb > 1) {
            snprintf(desc, sizeof(desc), "[%d embeds]", n_emb);
         } else {
            snprintf(desc, sizeof(desc), "[embed]");
         }
         content = desc;
      }
   }

   int64_t ts = 0;
   struct json_object *ts_obj = NULL;
   if (json_object_object_get_ex(msg, "timestamp", &ts_obj) && ts_obj) {
      const char *ts_str = json_object_get_string(ts_obj);
      if (ts_str) {
         time_t t;
         if (tz_parse(ts_str, tz_lookup("UTC"), &t)) {
            ts = (int64_t)t;
         }
      }
   }

   int type = 0;
   struct json_object *type_obj = NULL;
   if (json_object_object_get_ex(msg, "type", &type_obj) && type_obj) {
      type = json_object_get_int(type_obj);
   }

   struct json_object *o = json_object_new_object();
   if (!o) {
      return false;
   }
   json_object_object_add(o, "id", json_object_new_string(id_str));
   json_object_object_add(o, "author", json_object_new_string(author)); /* non-null (see above) */
   json_object_object_add(o, "timestamp", json_object_new_int64(ts));
   json_object_object_add(o, "content", json_object_new_string(content ? content : ""));
   json_object_object_add(o, "type", json_object_new_int(type));
   json_object_object_add(o, "is_bot", json_object_new_int(is_bot));
   json_object_array_add(out_arr, o);
   return true;
}

/* =============================================================================
 * Read path (driver hooks)
 * ============================================================================= */

int dc_read_history(const char *channel_id,
                    const messaging_read_window_t *window,
                    char **out_json) {
   if (!channel_id || !window || !out_json) {
      return FAILURE;
   }
   *out_json = NULL;
   if (!dc_is_valid_snowflake(channel_id) || s_bot_token[0] == '\0') {
      return FAILURE;
   }
   const char *before_id = window->before_id;
   int limit = window->limit;
   if (limit < 1) {
      limit = 1;
   }
   if (limit > DC_READ_TOTAL_MAX) {
      limit = DC_READ_TOTAL_MAX;
   }
   uint64_t after_sf = dc_snowflake_from_ts(window->after_ts);
   uint64_t before_sf = dc_snowflake_from_ts(window->before_ts);
   /* Inverted range (since > until, e.g. "since yesterday until last week") →
    * swap so the caller still gets the intended span instead of an empty set.
    * Skip when an explicit `before_id` cursor is supplied: there the cursor (not
    * before_sf) is the upper bound, so swapping would only corrupt the `after`
    * floor. */
   const bool have_before_id = (before_id && before_id[0]);
   if (!have_before_id && after_sf > 0 && before_sf > 0 && after_sf > before_sf) {
      uint64_t tmp = after_sf;
      after_sf = before_sf;
      before_sf = tmp;
   }

   struct json_object *out_arr = json_object_new_array();
   if (!out_arr) {
      return FAILURE;
   }

   /* Seed the cursor with the upper bound so the first page starts there.
    * An explicit `before_id` (exact pagination cursor) wins over the
    * `before_ts`-derived snowflake; empty → first page is newest (up to now). */
   char before_cursor[DC_SNOWFLAKE_BUF_SIZE] = { 0 };
   if (before_id && before_id[0] && dc_is_valid_snowflake(before_id)) {
      snprintf(before_cursor, sizeof(before_cursor), "%s", before_id);
   } else if (before_sf > 0) {
      snprintf(before_cursor, sizeof(before_cursor), "%" PRIu64, before_sf);
   }
   int collected = 0;
   int pages = 0;
   bool reached_boundary = false;
   int rc = SUCCESS;

   /* Backward pagination: Discord returns newest-first regardless of cursor.
    * Page 1 (no cursor) is the newest page; subsequent pages use
    * before=<oldest id seen> to walk older.  Stop at the message cap, the
    * page cap, an `after` boundary crossing, or a short/empty page. */
   while (collected < limit && pages < DC_READ_MAX_PAGES && !reached_boundary) {
      int page_limit = limit - collected;
      if (page_limit > DC_READ_PAGE_MAX) {
         page_limit = DC_READ_PAGE_MAX;
      }
      char url[256];
      if (before_cursor[0]) {
         snprintf(url, sizeof(url), "%s/channels/%s/messages?limit=%d&before=%s", DC_REST_BASE_URL,
                  channel_id, page_limit, before_cursor);
      } else {
         snprintf(url, sizeof(url), "%s/channels/%s/messages?limit=%d", DC_REST_BASE_URL,
                  channel_id, page_limit);
      }

      curl_buffer_t resp;
      curl_buffer_init_with_max(&resp, DC_REST_RESP_MAX);
      if (dc_rest_get(url, &resp) != SUCCESS) {
         /* First-page failure (403/404 no VIEW_CHANNEL, or network) → hard
          * fail so the engine can show "couldn't read".  A LATER page
          * failing (e.g. a 429 mid-pagination) keeps what we already have. */
         curl_buffer_free(&resp);
         if (collected == 0) {
            rc = FAILURE;
         }
         break;
      }
      struct json_object *page = resp.data ? json_tokener_parse(resp.data) : NULL;
      curl_buffer_free(&resp);
      if (!page || !json_object_is_type(page, json_type_array)) {
         if (page) {
            json_object_put(page);
         }
         break; /* malformed / empty — treat what we have as the result */
      }
      int n = (int)json_object_array_length(page);
      if (n == 0) {
         json_object_put(page);
         break; /* end of history */
      }
      for (int i = 0; i < n && collected < limit; i++) {
         struct json_object *msg = json_object_array_get_idx(page, i);
         uint64_t mid = dc_msg_snowflake(msg);
         if (mid == 0) {
            continue;
         }
         /* Advance the `before` cursor for every message we walk past (even
          * boundary/filtered ones) so the next page continues from here. */
         snprintf(before_cursor, sizeof(before_cursor), "%" PRIu64, mid);
         if (after_sf > 0 && mid <= after_sf) {
            reached_boundary = true;
            break;
         }
         if (dc_append_message(msg, out_arr)) {
            collected++;
         }
      }
      json_object_put(page);
      if (n < page_limit) {
         break; /* short page → no more history */
      }
      pages++;
   }

   *out_json = strdup(json_object_to_json_string_ext(out_arr, JSON_C_TO_STRING_PLAIN));
   json_object_put(out_arr);
   return (*out_json) ? rc : FAILURE;
}

/* Append all readable channels from one guild into `out_arr`. */
static void dc_collect_guild_channels(const char *guild_id,
                                      const char *guild_name,
                                      struct json_object *out_arr) {
   if (!guild_id || !dc_is_valid_snowflake(guild_id) || !out_arr) {
      return;
   }
   char url[256];
   snprintf(url, sizeof(url), "%s/guilds/%s/channels", DC_REST_BASE_URL, guild_id);
   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, DC_REST_RESP_MAX);
   if (dc_rest_get(url, &resp) != SUCCESS) {
      curl_buffer_free(&resp);
      return;
   }
   struct json_object *arr = resp.data ? json_tokener_parse(resp.data) : NULL;
   curl_buffer_free(&resp);
   if (!arr || !json_object_is_type(arr, json_type_array)) {
      if (arr) {
         json_object_put(arr);
      }
      return;
   }
   int n = (int)json_object_array_length(arr);
   for (int i = 0; i < n; i++) {
      if (json_object_array_length(out_arr) >= DC_MAX_CHANNELS) {
         break; /* defensive: keep the cached list (and downstream scans) bounded */
      }
      struct json_object *ch = json_object_array_get_idx(arr, i);
      struct json_object *type_obj = NULL, *id_obj = NULL, *name_obj = NULL;
      if (!json_object_object_get_ex(ch, "type", &type_obj)) {
         continue;
      }
      int type = json_object_get_int(type_obj);
      if (type != DC_CHAN_TYPE_TEXT && type != DC_CHAN_TYPE_ANNOUNCEMENT) {
         continue; /* text + announcement only in v1 */
      }
      if (!json_object_object_get_ex(ch, "id", &id_obj) ||
          !json_object_object_get_ex(ch, "name", &name_obj)) {
         continue;
      }
      const char *cid = json_object_get_string(id_obj);
      const char *cname = json_object_get_string(name_obj);
      if (!cid || !dc_is_valid_snowflake(cid) || !cname) {
         continue;
      }
      struct json_object *o = json_object_new_object();
      if (!o) {
         continue;
      }
      json_object_object_add(o, "container_id", json_object_new_string(guild_id));
      json_object_object_add(o, "container_name",
                             json_object_new_string(guild_name ? guild_name : ""));
      json_object_object_add(o, "channel_id", json_object_new_string(cid));
      json_object_object_add(o, "channel_name", json_object_new_string(cname));
      json_object_object_add(o, "type", json_object_new_int(type));
      json_object_array_add(out_arr, o);
   }
   json_object_put(arr);
}

int dc_list_readable_channels(char **out_json) {
   if (!out_json) {
      return FAILURE;
   }
   *out_json = NULL;
   int64_t now = (int64_t)time(NULL);

   /* Cache hit? */
   pthread_mutex_lock(&s_chan_cache_mutex);
   if (s_chan_cache_json && (now - s_chan_cache_at) < DC_CHAN_CACHE_TTL_SEC) {
      *out_json = strdup(s_chan_cache_json);
      pthread_mutex_unlock(&s_chan_cache_mutex);
      return (*out_json) ? SUCCESS : FAILURE;
   }
   pthread_mutex_unlock(&s_chan_cache_mutex);

   if (s_bot_token[0] == '\0') {
      return FAILURE;
   }

   /* Build fresh OUTSIDE the cache lock (network I/O). */
   char url[256];
   snprintf(url, sizeof(url), "%s/users/@me/guilds", DC_REST_BASE_URL);
   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, DC_REST_RESP_MAX);
   if (dc_rest_get(url, &resp) != SUCCESS) {
      curl_buffer_free(&resp);
      return FAILURE;
   }
   struct json_object *guilds = resp.data ? json_tokener_parse(resp.data) : NULL;
   curl_buffer_free(&resp);

   /* Fail (don't cache) on an unparseable/non-array body — a truncated or
    * malformed response must NOT poison discovery with an empty list for the
    * whole TTL.  A genuinely empty array (bot in 0 guilds) is valid and caches. */
   if (!guilds || !json_object_is_type(guilds, json_type_array)) {
      if (guilds) {
         json_object_put(guilds);
      }
      OLOG_WARNING(
          "discord: /users/@me/guilds returned an unparseable/non-array body — not caching");
      return FAILURE;
   }

   struct json_object *out_arr = json_object_new_array();
   if (!out_arr) {
      json_object_put(guilds);
      return FAILURE;
   }
   {
      int n = (int)json_object_array_length(guilds);
      if (n > DC_GUILD_SCAN_MAX) {
         OLOG_WARNING("discord: bot in %d guilds; scanning first %d for readable channels", n,
                      DC_GUILD_SCAN_MAX);
         n = DC_GUILD_SCAN_MAX;
      }
      for (int i = 0; i < n; i++) {
         struct json_object *g = json_object_array_get_idx(guilds, i);
         struct json_object *gid = NULL, *gname = NULL;
         if (!json_object_object_get_ex(g, "id", &gid)) {
            continue;
         }
         json_object_object_get_ex(g, "name", &gname);
         dc_collect_guild_channels(json_object_get_string(gid),
                                   gname ? json_object_get_string(gname) : NULL, out_arr);
         /* Stop fetching further guilds once the channel cap is reached —
          * dc_collect_guild_channels only gates appends, so without this we'd
          * keep issuing a REST call per remaining guild for results we'd drop. */
         if (json_object_array_length(out_arr) >= DC_MAX_CHANNELS) {
            break;
         }
      }
   }
   json_object_put(guilds);

   char *built = strdup(json_object_to_json_string_ext(out_arr, JSON_C_TO_STRING_PLAIN));
   json_object_put(out_arr);
   if (!built) {
      return FAILURE;
   }

   /* Publish to cache + return a copy. */
   pthread_mutex_lock(&s_chan_cache_mutex);
   free(s_chan_cache_json);
   s_chan_cache_json = built;
   s_chan_cache_at = now;
   *out_json = strdup(s_chan_cache_json);
   pthread_mutex_unlock(&s_chan_cache_mutex);
   return (*out_json) ? SUCCESS : FAILURE;
}

/* Drop the discovery cache so the next dc_list_readable_channels() refetches —
 * used by the engine to recover from a name-resolution miss caused by a
 * just-created channel still inside the cache TTL.  No-op if the cache is very
 * fresh (< DC_CACHE_MIN_REFRESH_SEC): a channel can't have appeared that
 * recently relative to its own enumeration, so re-scanning every guild on a
 * fast-repeated typo would be wasted round-trips. */
void dc_invalidate_channel_cache(void) {
   int64_t now = (int64_t)time(NULL);
   pthread_mutex_lock(&s_chan_cache_mutex);
   if (s_chan_cache_at == 0 || (now - s_chan_cache_at) >= DC_CACHE_MIN_REFRESH_SEC) {
      s_chan_cache_at = 0; /* force the TTL check to miss on the next call */
   }
   pthread_mutex_unlock(&s_chan_cache_mutex);
}

void dc_read_shutdown(void) {
   pthread_mutex_lock(&s_read_curl_mutex);
   if (s_read_curl) {
      curl_easy_cleanup(s_read_curl);
      s_read_curl = NULL;
   }
   pthread_mutex_unlock(&s_read_curl_mutex);
   pthread_mutex_lock(&s_chan_cache_mutex);
   free(s_chan_cache_json);
   s_chan_cache_json = NULL;
   s_chan_cache_at = 0;
   pthread_mutex_unlock(&s_chan_cache_mutex);
}
