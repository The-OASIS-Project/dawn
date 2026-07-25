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
 * Conversation-event payload construction + redaction.  See event_payload.h for
 * the redaction policy and why it is a denylist rather than an allowlist.
 */

#include "core/event_payload.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* Substrings that mark a key as credential-bearing.  Matched case-insensitively
 * anywhere in the key, so "api_key", "authToken" and "passphrase" all hit. */
static const char *const SENSITIVE_KEY_PATTERNS[] = {
   "pass", "secret", "token", "credential", "auth", "bearer", "key", NULL,
};

/* Floor below which a long unbroken alphanumeric run is treated as an opaque
 * credential rather than prose.  Real API keys/JWT segments are comfortably
 * above this; ordinary words, IDs and hashes-in-prose are below it. */
#define OPAQUE_RUN_MIN 40

static bool ascii_contains_ci(const char *haystack, const char *needle) {
   size_t nlen = strlen(needle);
   for (const char *p = haystack; *p; p++) {
      size_t i = 0;
      while (i < nlen && p[i] &&
             tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
         i++;
      }
      if (i == nlen) {
         return true;
      }
   }
   return false;
}

bool event_payload_key_is_sensitive(const char *key) {
   if (key == NULL) {
      return false;
   }
   for (int i = 0; SENSITIVE_KEY_PATTERNS[i] != NULL; i++) {
      if (ascii_contains_ci(key, SENSITIVE_KEY_PATTERNS[i])) {
         return true;
      }
   }
   return false;
}

/* Value-shape check: catches a secret pasted into an innocuously-named field. */
static bool value_looks_secret(const char *v) {
   if (v == NULL) {
      return false;
   }
   if (strncmp(v, "sk-", 3) == 0 || strncasecmp(v, "bearer ", 7) == 0) {
      return true;
   }
   size_t run = 0;
   for (const char *p = v; *p; p++) {
      if (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '+' || *p == '/' ||
          *p == '=') {
         if (++run >= OPAQUE_RUN_MIN) {
            return true;
         }
      } else {
         run = 0;
      }
   }
   return false;
}

size_t event_payload_utf8_floor(const char *s, size_t len) {
   if (s == NULL) {
      return 0;
   }
   /* Inspect the first EXCLUDED byte and walk back — see the header comment for
    * why the last-included-byte variant is wrong. */
   while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) {
      len--;
   }
   return len;
}

/* Cap from config, with a sane floor so a misconfigured 0 can't erase content
 * (config clamps negatives; 0 legitimately means "unset" here). */
static size_t payload_cap(void) {
   int cap = g_config.jobs.event_chunk_cap;
   return (cap > 0) ? (size_t)cap : 16384;
}

/* Head+tail truncation: the head says what happened, the tail usually carries
 * the conclusion (an error, a total). Dropping only the middle keeps both. */
static char *truncate_middle(const char *s) {
   size_t cap = payload_cap();
   size_t len = strlen(s);
   if (len <= cap) {
      return strdup(s);
   }
   static const char ELIDED[] = "\n…[truncated]…\n";
   size_t head = event_payload_utf8_floor(s, (cap * 2) / 3);
   size_t tail_want = cap - head - (sizeof(ELIDED) - 1);
   size_t tail_start = event_payload_utf8_floor(s, len - tail_want);

   size_t out_len = head + (sizeof(ELIDED) - 1) + (len - tail_start);
   char *out = malloc(out_len + 1);
   if (out == NULL) {
      return NULL;
   }
   memcpy(out, s, head);
   memcpy(out + head, ELIDED, sizeof(ELIDED) - 1);
   memcpy(out + head + sizeof(ELIDED) - 1, s + tail_start, len - tail_start);
   out[out_len] = '\0';
   return out;
}

static bool tool_handles_secrets(const char *tool_name) {
   if (tool_name == NULL) {
      return false;
   }
   const tool_metadata_t *meta = tool_registry_lookup(tool_name);
   return meta != NULL && (meta->capabilities & TOOL_CAP_SECRETS) != 0;
}

/* Walk a parsed args object, replacing sensitive values in place. */
static void redact_args_obj(struct json_object *args, bool redact_everything) {
   if (args == NULL || !json_object_is_type(args, json_type_object)) {
      return;
   }
   json_object_object_foreach(args, key, val) {
      bool sensitive = redact_everything || event_payload_key_is_sensitive(key);
      if (!sensitive && json_object_is_type(val, json_type_string)) {
         sensitive = value_looks_secret(json_object_get_string(val));
      }
      if (sensitive) {
         json_object_object_add(args, key, json_object_new_string(EVENT_REDACTED_MARKER));
      } else if (json_object_is_type(val, json_type_object)) {
         redact_args_obj(val, redact_everything); /* nested arg objects */
      }
   }
}

char *event_payload_tool_call(const char *tool_name, const char *args_json) {
   struct json_object *root = json_object_new_object();
   if (root == NULL) {
      return NULL;
   }
   json_object_object_add(root, "tool", json_object_new_string(tool_name ? tool_name : "?"));

   bool redact_all = tool_handles_secrets(tool_name);
   struct json_object *args = NULL;
   if (args_json && args_json[0]) {
      args = json_tokener_parse(args_json);
   }
   if (args != NULL && json_object_is_type(args, json_type_object)) {
      redact_args_obj(args, redact_all);
      json_object_object_add(root, "args", args);
   } else {
      /* Unparseable or non-object args: never persist raw — we can't tell what
       * is in there, and this is the one branch an attacker could aim for. */
      json_object_put(args);
      if (args_json && args_json[0]) {
         json_object_object_add(root, "args", json_object_new_string(EVENT_REDACTED_MARKER));
      }
   }

   const char *rendered = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *out = rendered ? truncate_middle(rendered) : NULL;
   json_object_put(root);
   return out;
}

char *event_payload_tool_result(const char *tool_name, const char *result_text) {
   struct json_object *root = json_object_new_object();
   if (root == NULL) {
      return NULL;
   }
   json_object_object_add(root, "tool", json_object_new_string(tool_name ? tool_name : "?"));

   /* Cap the result BEFORE embedding it, so the cap governs the payload the way
    * an operator expects rather than being diluted by JSON escaping. */
   char *body = truncate_middle(result_text ? result_text : "");
   if (body == NULL) {
      json_object_put(root);
      return NULL;
   }
   if (tool_handles_secrets(tool_name)) {
      json_object_object_add(root, "result", json_object_new_string(EVENT_REDACTED_MARKER));
   } else {
      json_object_object_add(root, "result", json_object_new_string(body));
   }
   free(body);

   const char *rendered = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *out = rendered ? strdup(rendered) : NULL;
   json_object_put(root);
   return out;
}

char *event_payload_status(bool generating) {
   struct json_object *root = json_object_new_object();
   if (root == NULL) {
      return NULL;
   }
   json_object_object_add(root, "state",
                          json_object_new_string(generating ? "generating" : "idle"));
   const char *rendered = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *out = rendered ? strdup(rendered) : NULL;
   json_object_put(root);
   return out;
}

char *event_payload_complete(const char *disposition,
                             const char *error_or_null,
                             int64_t final_message_id) {
   struct json_object *root = json_object_new_object();
   if (root == NULL) {
      return NULL;
   }
   json_object_object_add(root, "disposition",
                          json_object_new_string(disposition ? disposition : "failed"));
   if (error_or_null && error_or_null[0]) {
      /* §8.6: job_error is operator-facing text that can quote a failing
       * request — redact by the same value-shape rule as any other value. */
      const char *safe = value_looks_secret(error_or_null) ? EVENT_REDACTED_MARKER : error_or_null;
      char *capped = truncate_middle(safe);
      json_object_object_add(root, "error", json_object_new_string(capped ? capped : safe));
      free(capped);
   }
   if (final_message_id > 0) {
      json_object_object_add(root, "final_message_id", json_object_new_int64(final_message_id));
   }
   const char *rendered = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *out = rendered ? strdup(rendered) : NULL;
   json_object_put(root);
   return out;
}

char *event_payload_spawn(int64_t child_conv_id, const char *title) {
   struct json_object *root = json_object_new_object();
   if (root == NULL) {
      return NULL;
   }
   json_object_object_add(root, "conv_id", json_object_new_int64(child_conv_id));
   json_object_object_add(root, "title", json_object_new_string(title ? title : ""));
   const char *rendered = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *out = rendered ? strdup(rendered) : NULL;
   json_object_put(root);
   return out;
}
