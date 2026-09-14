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
 * Plan Executor — client-side multi-step tool plan execution engine
 *
 * Interprets a constrained JSON DSL: call, if, loop, set, log steps.
 * Executes tools via llm_tools_execute() to preserve all dispatch logic.
 */

#define _GNU_SOURCE /* strcasestr */

#include "tools/plan_executor.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "logging.h"
#include "utils/string_utils.h"

#ifdef ENABLE_WEBUI
#include "core/session_manager.h"
#include "webui/webui_server.h"

void __attribute__((weak)) webui_broadcast_plan_progress(session_t *s, const char *json) {
   (void)s;
   (void)json;
}
#endif

/**
 * Send a plan progress notification to the active WebUI session.
 * No-op when WebUI is disabled or session is non-WebUI (local mic, satellite).
 */
void plan_notify_progress(const char *json_str) {
#ifdef ENABLE_WEBUI
   session_t *session = session_get_command_context();
   if (!session || session->type != SESSION_TYPE_WEBUI)
      return;
   webui_broadcast_plan_progress(session, json_str);
#else
   (void)json_str;
#endif
}

/* =============================================================================
 * Internal Helpers — Progress Notification Builder
 * ============================================================================= */

/**
 * Build a plan_progress JSON notification and send it.
 * Uses json-c to safely escape all string values.
 */
static void plan_send_progress(const char *action,
                               int index,
                               const char *name,
                               long duration_ms,
                               int has_duration,
                               int success,
                               const char *error) {
   struct json_object *root = json_object_new_object();
   struct json_object *payload = json_object_new_object();
   if (!root || !payload) {
      json_object_put(root);
      json_object_put(payload);
      return;
   }

   json_object_object_add(root, "type", json_object_new_string("plan_progress"));
   json_object_object_add(payload, "action", json_object_new_string(action));

   if (index >= 0)
      json_object_object_add(payload, "index", json_object_new_int(index));
   if (name)
      json_object_object_add(payload, "name", json_object_new_string(name));
   if (has_duration)
      json_object_object_add(payload, "duration_ms", json_object_new_int64(duration_ms));
   if (has_duration)
      json_object_object_add(payload, "success", json_object_new_boolean(success));
   if (error)
      json_object_object_add(payload, "error", json_object_new_string(error));

   json_object_object_add(root, "payload", payload);

   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   if (json_str)
      plan_notify_progress(json_str);

   json_object_put(root);
}

/* =============================================================================
 * Internal Helpers — JSON accessors
 * ============================================================================= */

/** Get a string field from a JSON object, or NULL if missing/wrong type */
static const char *jobj_get_string(struct json_object *obj, const char *key) {
   struct json_object *val;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   if (!json_object_is_type(val, json_type_string))
      return NULL;
   return json_object_get_string(val);
}

/** Get a JSON sub-object by key, or NULL if missing */
static struct json_object *jobj_get(struct json_object *obj, const char *key) {
   struct json_object *val;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return val;
}

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

static bool check_timeout(const plan_context_t *ctx) {
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   long elapsed = (now.tv_sec - ctx->start_time.tv_sec);
   return elapsed >= ctx->timeout_s;
}

static void output_append(plan_context_t *ctx, const char *text) {
   if (!text || !text[0])
      return;
   int remaining = (int)sizeof(ctx->output) - ctx->output_len - 1;
   if (remaining <= 0)
      return;

   int text_len = (int)strlen(text);
   if (text_len > remaining) {
      /* Truncate and add marker */
      int marker_len = 12; /* "\n[truncated]" */
      if (remaining > marker_len) {
         memcpy(ctx->output + ctx->output_len, text, remaining - marker_len);
         ctx->output_len += remaining - marker_len;
         memcpy(ctx->output + ctx->output_len, "\n[truncated]", marker_len);
         ctx->output_len += marker_len;
      }
      ctx->output[ctx->output_len] = '\0';
   } else {
      memcpy(ctx->output + ctx->output_len, text, text_len);
      ctx->output_len += text_len;
      ctx->output[ctx->output_len] = '\0';
   }
}

/* =============================================================================
 * Variable Operations
 * ============================================================================= */

bool plan_validate_var_name(const char *name) {
   if (!name || !name[0])
      return false;

   /* First char: [a-z_] */
   if (name[0] != '_' && (name[0] < 'a' || name[0] > 'z'))
      return false;

   int len = 0;
   for (int i = 0; name[i]; i++) {
      char c = name[i];
      if (c != '_' && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9'))
         return false;
      len++;
      if (len > PLAN_VAR_NAME_MAX)
         return false;
   }
   return true;
}

void plan_vars_set(plan_context_t *ctx, const char *name, const char *value) {
   if (!ctx || !name)
      return;

   /* Check for existing variable — overwrite */
   for (int i = 0; i < ctx->var_count; i++) {
      if (strcmp(ctx->vars[i].name, name) == 0) {
         free(ctx->vars[i].value);
         ctx->vars[i].value = value ? strdup(value) : NULL;
         return;
      }
   }

   /* New variable — check capacity */
   if (ctx->var_count >= PLAN_MAX_VARS)
      return;

   safe_strscpy(ctx->vars[ctx->var_count].name, name);
   ctx->vars[ctx->var_count].value = value ? strdup(value) : NULL;
   ctx->vars[ctx->var_count].success = false;
   ctx->var_count++;
}

const char *plan_vars_get(const plan_context_t *ctx, const char *name) {
   if (!ctx || !name)
      return NULL;
   for (int i = 0; i < ctx->var_count; i++) {
      if (strcmp(ctx->vars[i].name, name) == 0)
         return ctx->vars[i].value;
   }
   return NULL;
}

void plan_vars_set_success(plan_context_t *ctx, const char *name, bool success) {
   if (!ctx || !name)
      return;
   for (int i = 0; i < ctx->var_count; i++) {
      if (strcmp(ctx->vars[i].name, name) == 0) {
         ctx->vars[i].success = success;
         return;
      }
   }
}

static bool plan_vars_get_success(const plan_context_t *ctx, const char *name) {
   if (!ctx || !name)
      return false;
   for (int i = 0; i < ctx->var_count; i++) {
      if (strcmp(ctx->vars[i].name, name) == 0)
         return ctx->vars[i].success;
   }
   return false;
}

void plan_context_cleanup(plan_context_t *ctx) {
   if (!ctx)
      return;
   for (int i = 0; i < ctx->var_count; i++) {
      free(ctx->vars[i].value);
      ctx->vars[i].value = NULL;
   }
   ctx->var_count = 0;
}

/* =============================================================================
 * Condition Evaluation
 * ============================================================================= */

/**
 * Check if a value is considered "empty" for condition purposes.
 * Empty means: NULL, "", "[]", "none", "no results"
 */
static bool is_value_empty(const char *val) {
   if (!val || !val[0])
      return true;
   if (strcmp(val, "[]") == 0)
      return true;
   if (strcasecmp(val, "none") == 0)
      return true;
   if (strcasecmp(val, "no results") == 0)
      return true;
   return false;
}

bool plan_eval_condition(const plan_context_t *ctx, const char *expr) {
   if (!ctx || !expr || !expr[0])
      return false;

   /* Literal booleans */
   if (strcmp(expr, "true") == 0)
      return true;
   if (strcmp(expr, "false") == 0)
      return false;

   /* Find the dot separator: varname.operator[:arg] */
   const char *dot = strchr(expr, '.');
   if (!dot)
      return false;

   /* Extract variable name */
   size_t name_len = dot - expr;
   if (name_len == 0 || name_len > PLAN_VAR_NAME_MAX)
      return false;

   char var_name[PLAN_VAR_NAME_MAX + 1];
   memcpy(var_name, expr, name_len);
   var_name[name_len] = '\0';

   const char *op = dot + 1;
   const char *val = plan_vars_get(ctx, var_name);

   /* .empty */
   if (strcmp(op, "empty") == 0)
      return is_value_empty(val);

   /* .notempty */
   if (strcmp(op, "notempty") == 0)
      return !is_value_empty(val);

   /* .success */
   if (strcmp(op, "success") == 0)
      return plan_vars_get_success(ctx, var_name);

   /* .failed */
   if (strcmp(op, "failed") == 0)
      return !plan_vars_get_success(ctx, var_name);

   /* .contains:text */
   if (strncmp(op, "contains:", 9) == 0) {
      const char *needle = op + 9;
      if (!val || !val[0])
         return false;
      return strcasestr(val, needle) != NULL;
   }

   /* .equals:text */
   if (strncmp(op, "equals:", 7) == 0) {
      const char *target = op + 7;
      if (!val)
         return false;
      return strcasecmp(val, target) == 0;
   }

   /* Unknown operator — safe default */
   return false;
}

/* =============================================================================
 * Interpolation
 * ============================================================================= */

/* A variable-name / field-name character: [a-z0-9_]. */
static inline bool plan_is_var_char(char c) {
   return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/**
 * @brief Resolve a variable (with optional .field JSON access) and append to out
 *
 * Shared by both interpolation syntaxes ($var/${var} and {{var}}) so they
 * resolve identically — including dot-access (`$var.field` == `{{var.field}}`).
 * Missing variables append nothing (matches prior behavior). Truncates to fit.
 */
static void plan_append_resolved_var(const plan_context_t *ctx,
                                     const char *name_start,
                                     size_t name_len,
                                     const char *field_name,
                                     size_t field_len,
                                     char *out,
                                     size_t *pos,
                                     size_t out_size) {
   char var_name[PLAN_VAR_NAME_MAX + 1];
   size_t copy_len = name_len < PLAN_VAR_NAME_MAX ? name_len : PLAN_VAR_NAME_MAX;
   memcpy(var_name, name_start, copy_len);
   var_name[copy_len] = '\0';

   const char *val = plan_vars_get(ctx, var_name);
   if (!val)
      return;

   const char *resolved = val;
   char field_buf[512];

   /* Dot-access: parse JSON and extract named field */
   if (field_name && field_len > 0) {
      char field_key[PLAN_VAR_NAME_MAX + 1];
      size_t fk_len = field_len < PLAN_VAR_NAME_MAX ? field_len : PLAN_VAR_NAME_MAX;
      memcpy(field_key, field_name, fk_len);
      field_key[fk_len] = '\0';

      struct json_object *jobj = json_tokener_parse(val);
      if (jobj && json_object_is_type(jobj, json_type_object)) {
         struct json_object *field_val;
         if (json_object_object_get_ex(jobj, field_key, &field_val)) {
            const char *fv_str = json_object_get_string(field_val);
            if (fv_str) {
               safe_strscpy(field_buf, fv_str);
               resolved = field_buf;
            } else {
               resolved = ""; /* field exists but not stringifiable */
            }
         } else {
            resolved = ""; /* field not found in JSON object */
         }
         json_object_put(jobj);
      } else if (jobj) {
         json_object_put(jobj);
         /* val is not a JSON object — fall back to raw value */
      }
      /* If val doesn't parse as JSON at all, fall back to raw value */
   }

   size_t val_len = strlen(resolved);
   size_t avail = out_size - 1 - *pos;
   size_t copy = val_len < avail ? val_len : avail;
   memcpy(out + *pos, resolved, copy);
   *pos += copy;
}

int plan_interpolate(const plan_context_t *ctx,
                     const char *template_str,
                     char *out,
                     size_t out_size) {
   if (!out || out_size == 0)
      return 1;
   out[0] = '\0';

   if (!ctx || !template_str) {
      return 0;
   }

   size_t pos = 0;
   const char *p = template_str;

   while (*p && pos < out_size - 1) {
      /* {{var}} or {{var.field}} — the syntax the tool description documents. */
      if (p[0] == '{' && p[1] == '{') {
         const char *q = p + 2;
         const char *name_start = q;
         while (plan_is_var_char(*q))
            q++;
         size_t name_len = q - name_start;

         /* Optional .field dot-access */
         const char *field_name = NULL;
         size_t field_len = 0;
         if (name_len > 0 && *q == '.') {
            const char *field_start = q + 1;
            const char *fp = field_start;
            while (plan_is_var_char(*fp))
               fp++;
            field_len = fp - field_start;
            if (field_len > 0) {
               field_name = field_start;
               q = fp;
            }
         }

         /* Require a valid name and closing "}}" — otherwise emit literally. */
         if (name_len > 0 && q[0] == '}' && q[1] == '}') {
            plan_append_resolved_var(ctx, name_start, name_len, field_name, field_len, out, &pos,
                                     out_size);
            p = q + 2; /* skip closing }} */
            continue;
         }

         /* Not a valid {{...}} reference — emit the '{' literally and advance. */
         out[pos++] = *p++;
         continue;
      }

      if (*p != '$') {
         out[pos++] = *p++;
         continue;
      }

      /* Found $, try to extract variable name */
      p++; /* skip $ */
      bool braced = false;
      if (*p == '{') {
         braced = true;
         p++;
      }

      /* Collect variable name chars */
      const char *name_start = p;
      while (plan_is_var_char(*p))
         p++;

      size_t name_len = p - name_start;
      if (name_len == 0) {
         /* Not a valid variable reference — output the $ literally */
         if (pos < out_size - 1)
            out[pos++] = '$';
         if (braced && pos < out_size - 1 && *p == '}')
            p++; /* skip closing brace */
         continue;
      }

      if (braced) {
         if (*p == '}')
            p++; /* skip closing brace */
      }

      /* Check for dot-access: $var.field extracts field from JSON object */
      const char *field_name = NULL;
      size_t field_len = 0;
      if (!braced && *p == '.') {
         const char *field_start = p + 1;
         const char *fp = field_start;
         while (plan_is_var_char(*fp))
            fp++;
         field_len = fp - field_start;
         if (field_len > 0) {
            field_name = field_start;
            p = fp; /* advance past .field */
         }
      }

      plan_append_resolved_var(ctx, name_start, name_len, field_name, field_len, out, &pos,
                               out_size);
   }

   out[pos] = '\0';
   return 0;
}

/* =============================================================================
 * Argument Construction
 * ============================================================================= */

int plan_build_args_json(plan_context_t *ctx,
                         struct json_object *args,
                         char *out_json,
                         size_t out_size) {
   if (!ctx || !args || !out_json || out_size == 0)
      return 1;

   /* Build a new JSON object with interpolated string values */
   struct json_object *result = json_object_new_object();
   if (!result)
      return 1;

   json_object_object_foreach(args, key, val) {
      if (json_object_is_type(val, json_type_string)) {
         char interpolated[LLM_TOOLS_RESULT_LEN];
         plan_interpolate(ctx, json_object_get_string(val), interpolated, sizeof(interpolated));
         json_object_object_add(result, key, json_object_new_string(interpolated));
      } else if (json_object_is_type(val, json_type_int)) {
         json_object_object_add(result, key, json_object_new_int64(json_object_get_int64(val)));
      } else if (json_object_is_type(val, json_type_double)) {
         json_object_object_add(result, key, json_object_new_double(json_object_get_double(val)));
      } else if (json_object_is_type(val, json_type_boolean)) {
         json_object_object_add(result, key, json_object_new_boolean(json_object_get_boolean(val)));
      } else if (json_object_is_type(val, json_type_array)) {
         /* Deep copy array by re-parsing its string representation */
         const char *arr_str = json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN);
         struct json_object *arr_copy = json_tokener_parse(arr_str);
         json_object_object_add(result, key, arr_copy ? arr_copy : json_object_new_array());
      } else if (json_object_is_type(val, json_type_null)) {
         json_object_object_add(result, key, NULL);
      }
   }

   const char *json_str = json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN);
   if (!json_str) {
      json_object_put(result);
      return 1;
   }

   safe_strncpy(out_json, json_str, out_size);
   json_object_put(result);
   return 0;
}

/* =============================================================================
 * Tool Capability Check
 * ============================================================================= */

bool plan_tool_is_allowed(const tool_metadata_t *meta) {
   if (!meta)
      return false;

   /* Block self-reference */
   if (strcmp(meta->name, "execute_plan") == 0)
      return false;

   /* Must have SCHEDULABLE capability */
   if (!(meta->capabilities & TOOL_CAP_SCHEDULABLE))
      return false;

   /* Never allow DANGEROUS tools */
   if (meta->capabilities & TOOL_CAP_DANGEROUS)
      return false;

   return true;
}

/* =============================================================================
 * Plan Parsing
 * ============================================================================= */

static int validate_step_depth(struct json_object *step, int depth);

static int validate_step_array_depth(struct json_object *steps, int depth) {
   int len = json_object_array_length(steps);
   for (int i = 0; i < len; i++) {
      struct json_object *step = json_object_array_get_idx(steps, i);
      int rc = validate_step_depth(step, depth);
      if (rc != PLAN_OK)
         return rc;
   }
   return PLAN_OK;
}

static int validate_step_depth(struct json_object *step, int depth) {
   /* Note: depth limit is enforced at execution time (plan_execute_steps),
    * not during parsing. The parser validates structure only. */
   if (!json_object_is_type(step, json_type_object))
      return PLAN_ERR_PARSE;

   const char *type = jobj_get_string(step, "type");
   if (!type)
      return PLAN_ERR_PARSE;

   if (strcmp(type, "log") == 0) {
      /* Requires 'message' string */
      if (!jobj_get_string(step, "message"))
         return PLAN_ERR_PARSE;
      return PLAN_OK;
   }

   if (strcmp(type, "set") == 0) {
      const char *var = jobj_get_string(step, "var");
      const char *val = jobj_get_string(step, "value");
      if (!var || !val)
         return PLAN_ERR_PARSE;
      if (!plan_validate_var_name(var))
         return PLAN_ERR_INVALID_VAR;
      return PLAN_OK;
   }

   if (strcmp(type, "sleep") == 0) {
      struct json_object *sec_obj = jobj_get(step, "seconds");
      if (!sec_obj)
         return PLAN_ERR_PARSE;
      int seconds = json_object_get_int(sec_obj);
      if (seconds < 1 || seconds > PLAN_MAX_SLEEP_S)
         return PLAN_ERR_PARSE;
      return PLAN_OK;
   }

   if (strcmp(type, "call") == 0) {
      if (!jobj_get_string(step, "tool"))
         return PLAN_ERR_PARSE;
      struct json_object *args = jobj_get(step, "args");
      if (!args || !json_object_is_type(args, json_type_object))
         return PLAN_ERR_PARSE;
      /* Validate optional store variable name */
      const char *store = jobj_get_string(step, "store");
      if (store) {
         if (!plan_validate_var_name(store))
            return PLAN_ERR_INVALID_VAR;
      }
      return PLAN_OK;
   }

   if (strcmp(type, "if") == 0) {
      if (!jobj_get_string(step, "condition"))
         return PLAN_ERR_PARSE;
      struct json_object *then_branch = jobj_get(step, "then");
      if (!then_branch || !json_object_is_type(then_branch, json_type_array))
         return PLAN_ERR_PARSE;
      int rc = validate_step_array_depth(then_branch, depth + 1);
      if (rc != PLAN_OK)
         return rc;
      struct json_object *else_branch = jobj_get(step, "else");
      if (else_branch) {
         if (!json_object_is_type(else_branch, json_type_array))
            return PLAN_ERR_PARSE;
         rc = validate_step_array_depth(else_branch, depth + 1);
         if (rc != PLAN_OK)
            return rc;
      }
      return PLAN_OK;
   }

   if (strcmp(type, "loop") == 0) {
      struct json_object *over = jobj_get(step, "over");
      const char *as = jobj_get_string(step, "as");
      struct json_object *steps_arr = jobj_get(step, "steps");
      if (!over || !json_object_is_type(over, json_type_array))
         return PLAN_ERR_PARSE;
      if (!as)
         return PLAN_ERR_PARSE;
      if (!plan_validate_var_name(as))
         return PLAN_ERR_INVALID_VAR;
      if (!steps_arr || !json_object_is_type(steps_arr, json_type_array))
         return PLAN_ERR_PARSE;
      return validate_step_array_depth(steps_arr, depth + 1);
   }

   /* Unknown step type */
   return PLAN_ERR_UNKNOWN_STEP;
}

/* Append a short snippet of `json` around `offset` to the diagnostic
 * buffer.  Used when the tokener fails mid-parse so the LLM can see
 * which bytes the parser was looking at.  Up to 80 chars, trimmed to
 * single-line by replacing whitespace runs with single spaces. */
static void append_parse_context(char *diag,
                                 size_t diag_size,
                                 const char *json,
                                 size_t offset,
                                 size_t json_len) {
   if (!diag || diag_size == 0 || !json)
      return;
   size_t off = strlen(diag);
   if (off + 16 >= diag_size)
      return;
   size_t start = offset > 40 ? offset - 40 : 0;
   size_t end = offset + 40;
   if (end > json_len)
      end = json_len;
   off += snprintf(diag + off, diag_size - off, " near: '");
   for (size_t i = start; i < end && off + 4 < diag_size; i++) {
      unsigned char c = (unsigned char)json[i];
      if (c == '\n' || c == '\r' || c == '\t')
         diag[off++] = ' ';
      else if (c < 0x20 || c == 0x7f)
         diag[off++] = '?';
      else
         diag[off++] = (char)c;
   }
   if (off + 2 < diag_size) {
      diag[off++] = '\'';
      diag[off] = '\0';
   } else {
      diag[diag_size - 1] = '\0';
   }
}

int plan_parse_with_diag(const char *json, struct json_object **out, char *diag, size_t diag_size) {
   if (diag && diag_size > 0)
      diag[0] = '\0';
   if (out)
      *out = NULL;
   if (!json || !json[0] || !out) {
      if (diag && diag_size > 0)
         snprintf(diag, diag_size, "empty plan input");
      return PLAN_ERR_PARSE;
   }

   /* Size check before parsing */
   size_t len = strlen(json);
   if (len > PLAN_MAX_PLAN_SIZE) {
      if (diag && diag_size > 0)
         snprintf(diag, diag_size, "plan too large (%zu bytes, max %d)", len, PLAN_MAX_PLAN_SIZE);
      return PLAN_ERR_PARSE;
   }

   /* Use a stateful tokener so we can recover the parse-error reason and
    * byte position on failure.  json_tokener_parse() (the convenience
    * one-shot) discards both. */
   struct json_tokener *tok = json_tokener_new();
   if (!tok) {
      if (diag && diag_size > 0)
         snprintf(diag, diag_size, "json_tokener_new() failed (out of memory)");
      return PLAN_ERR_PARSE;
   }
   struct json_object *root = json_tokener_parse_ex(tok, json, (int)len);
   enum json_tokener_error jerr = json_tokener_get_error(tok);
   int parse_end = json_tokener_get_parse_end(tok);
   json_tokener_free(tok);

   if (!root || jerr != json_tokener_success) {
      if (diag && diag_size > 0) {
         snprintf(diag, diag_size, "JSON parse error at byte %d: %s.", parse_end,
                  json_tokener_error_desc(jerr));
         append_parse_context(diag, diag_size, json, (size_t)parse_end, len);
      }
      if (root)
         json_object_put(root);
      return PLAN_ERR_PARSE;
   }

   if (!json_object_is_type(root, json_type_array)) {
      if (diag && diag_size > 0)
         snprintf(diag, diag_size,
                  "top-level value must be a JSON array `[...]`, not %s. Do NOT wrap the "
                  "plan in `{\"plan\":[...]}` — pass the array as the value of the `plan` "
                  "argument directly.",
                  json_type_to_name(json_object_get_type(root)));
      json_object_put(root);
      return PLAN_ERR_PARSE;
   }

   /* Validate all steps */
   int rc = validate_step_array_depth(root, 0);
   if (rc != PLAN_OK) {
      if (diag && diag_size > 0) {
         /* The step validators don't currently report which step or which
          * field failed (would need an out-param thread-through).  Give a
          * code-specific recovery hint instead. */
         switch (rc) {
            case PLAN_ERR_UNKNOWN_STEP:
               snprintf(diag, diag_size,
                        "a step has an unknown `type`. Valid types: call, if, loop, set, log, "
                        "sleep.");
               break;
            case PLAN_ERR_INVALID_VAR:
               snprintf(diag, diag_size,
                        "a step has an invalid variable name. Variable names must match "
                        "[a-z_][a-z0-9_]* (lowercase letters, digits, underscores).");
               break;
            case PLAN_ERR_MAX_STEPS:
               snprintf(diag, diag_size, "plan exceeds the maximum step count.");
               break;
            default:
               snprintf(diag, diag_size,
                        "a step is missing a required field (call/loop need `tool`, set needs "
                        "`var` + `value`, log needs `message`, sleep needs `seconds`).");
               break;
         }
      }
      json_object_put(root);
      return rc;
   }

   *out = root;
   return PLAN_OK;
}

int plan_parse(const char *json, struct json_object **out) {
   return plan_parse_with_diag(json, out, NULL, 0);
}

/* =============================================================================
 * Step Executors
 * ============================================================================= */

static int plan_step_sleep(plan_context_t *ctx, struct json_object *step) {
   struct json_object *sec_obj = jobj_get(step, "seconds");
   if (!sec_obj)
      return PLAN_ERR_PARSE;

   int seconds = json_object_get_int(sec_obj);
   if (seconds < 1)
      seconds = 1;
   if (seconds > PLAN_MAX_SLEEP_S)
      seconds = PLAN_MAX_SLEEP_S;

   OLOG_INFO("plan_executor: sleeping %d seconds", seconds);
   plan_send_progress("step_start", ctx->call_index, "sleep", 0, 0, 0, NULL);

   /* Sleep in 1-second intervals to check for timeout */
   for (int i = 0; i < seconds; i++) {
      if (check_timeout(ctx)) {
         OLOG_WARNING("plan_executor: timeout during sleep at %d/%d seconds", i, seconds);
         snprintf(ctx->error, sizeof(ctx->error), "Plan timeout (%ds) exceeded during sleep",
                  ctx->timeout_s);
         return PLAN_ERR_TIMEOUT;
      }
      sleep(1);
   }

   plan_send_progress("step_complete", ctx->call_index, "sleep", 0, 0, 0, NULL);
   ctx->call_index++;
   return PLAN_OK;
}

static int plan_step_log(plan_context_t *ctx, struct json_object *step) {
   const char *msg = jobj_get_string(step, "message");
   if (!msg)
      return PLAN_ERR_PARSE;

   char interpolated[LLM_TOOLS_RESULT_LEN];
   plan_interpolate(ctx, msg, interpolated, sizeof(interpolated));
   output_append(ctx, interpolated);
   output_append(ctx, "\n");
   return PLAN_OK;
}

static int plan_step_set(plan_context_t *ctx, struct json_object *step) {
   const char *var = jobj_get_string(step, "var");
   const char *val = jobj_get_string(step, "value");
   if (!var || !val)
      return PLAN_ERR_PARSE;

   char interpolated[LLM_TOOLS_RESULT_LEN];
   plan_interpolate(ctx, val, interpolated, sizeof(interpolated));
   plan_vars_set(ctx, var, interpolated);
   return PLAN_OK;
}

/**
 * Build a display name for a plan step, e.g. "scheduler:query".
 * Uses tool_name + first short string arg as qualifier.
 */
static void plan_build_step_name(const char *tool_name,
                                 struct json_object *args_obj,
                                 char *out,
                                 size_t out_size) {
   /* Try to extract a short qualifier from args (e.g., action="query") */
   const char *qualifier = NULL;
   const char *try_keys[] = { "action", "command", "mode", "op", NULL };
   for (int i = 0; try_keys[i]; i++) {
      const char *val = jobj_get_string(args_obj, try_keys[i]);
      if (val && strlen(val) <= 20) {
         qualifier = val;
         break;
      }
   }

   if (qualifier)
      snprintf(out, out_size, "%s:%s", tool_name, qualifier);
   else
      snprintf(out, out_size, "%s", tool_name);
}

static int plan_step_call(plan_context_t *ctx, struct json_object *step) {
   const char *tool_name = jobj_get_string(step, "tool");
   struct json_object *args_obj = jobj_get(step, "args");
   const char *store_name = jobj_get_string(step, "store");

   if (!tool_name || !args_obj)
      return PLAN_ERR_PARSE;

   /* Check tool call limit */
   if (ctx->total_tool_calls >= PLAN_MAX_TOOL_CALLS) {
      snprintf(ctx->error, sizeof(ctx->error), "Max tool calls (%d) exceeded", PLAN_MAX_TOOL_CALLS);
      return PLAN_ERR_MAX_TOOL_CALLS;
   }

   /* Build display name for progress UI */
   char step_name[64];
   plan_build_step_name(tool_name, args_obj, step_name, sizeof(step_name));
   int current_index = ctx->call_index;

   /* Validate tool: name-only lookup (no alias/device_string resolution) */
   const tool_metadata_t *meta = tool_registry_lookup(tool_name);
   if (!meta) {
      /* Fail-forward: store error in variable if requested */
      if (store_name) {
         char err_msg[256];
         snprintf(err_msg, sizeof(err_msg), "Error: unknown tool '%s'", tool_name);
         plan_vars_set(ctx, store_name, err_msg);
         plan_vars_set_success(ctx, store_name, false);
      }
      OLOG_WARNING("plan_executor: unknown tool '%s'", tool_name);
      plan_send_progress("step_error", current_index, NULL, 0, 0, 0, "unknown tool");
      {
         char line[128];
         snprintf(line, sizeof(line), "[step %d] FAILED: unknown tool '%.48s'\n", current_index,
                  tool_name);
         output_append(ctx, line);
      }
      ctx->total_tool_calls++;
      ctx->failed_steps++;
      ctx->call_index++;
      return PLAN_OK; /* fail-forward */
   }

   /* Capability whitelist */
   if (!plan_tool_is_allowed(meta)) {
      if (store_name) {
         char err_msg[256];
         snprintf(err_msg, sizeof(err_msg), "Error: tool '%s' not allowed in plans", tool_name);
         plan_vars_set(ctx, store_name, err_msg);
         plan_vars_set_success(ctx, store_name, false);
      }
      OLOG_WARNING("plan_executor: tool '%s' not allowed in plans", tool_name);
      plan_send_progress("step_error", current_index, NULL, 0, 0, 0, "not allowed in plans");
      {
         char line[192];
         snprintf(line, sizeof(line),
                  "[step %d] FAILED: tool '%.48s' is not allowed inside a plan — call it "
                  "directly instead (only schedulable, non-dangerous tools run in plans).\n",
                  current_index, tool_name);
         output_append(ctx, line);
      }
      ctx->total_tool_calls++;
      ctx->failed_steps++;
      ctx->call_index++;
      return PLAN_OK; /* fail-forward */
   }

   /* Runtime enabled check */
   if (!tool_registry_is_enabled(tool_name)) {
      if (store_name) {
         char err_msg[256];
         snprintf(err_msg, sizeof(err_msg), "Error: tool '%s' is disabled", tool_name);
         plan_vars_set(ctx, store_name, err_msg);
         plan_vars_set_success(ctx, store_name, false);
      }
      OLOG_WARNING("plan_executor: tool '%s' is disabled", tool_name);
      plan_send_progress("step_error", current_index, NULL, 0, 0, 0, "tool disabled");
      {
         char line[128];
         snprintf(line, sizeof(line), "[step %d] FAILED: tool '%.48s' is disabled\n", current_index,
                  tool_name);
         output_append(ctx, line);
      }
      ctx->total_tool_calls++;
      ctx->failed_steps++;
      ctx->call_index++;
      return PLAN_OK; /* fail-forward */
   }

   /* Notify: step_start */
   plan_send_progress("step_start", current_index, step_name, 0, 0, 0, NULL);

   /* Build tool call with interpolated arguments */
   tool_call_t call = { 0 };
   snprintf(call.id, sizeof(call.id), "plan_%d", ctx->total_steps_executed);
   safe_strscpy(call.name, tool_name);
   plan_build_args_json(ctx, args_obj, call.arguments, sizeof(call.arguments));

   /* Execute through the standard dispatch path */
   struct timespec step_start;
   clock_gettime(CLOCK_MONOTONIC, &step_start);

   tool_result_t result = { 0 };
   llm_tools_execute(&call, &result);
   ctx->total_tool_calls++;

   /* Calculate step duration */
   struct timespec step_end;
   clock_gettime(CLOCK_MONOTONIC, &step_end);
   long duration_ms = (step_end.tv_sec - step_start.tv_sec) * 1000 +
                      (step_end.tv_nsec - step_start.tv_nsec) / 1000000;

   /* Notify: step_done */
   plan_send_progress("step_done", current_index, NULL, duration_ms, 1, result.success, NULL);
   ctx->call_index++;

   /* Store result in variable if requested */
   if (store_name) {
      const char *result_text = tool_result_content(&result);
      plan_vars_set(ctx, store_name, result_text);
      plan_vars_set_success(ctx, store_name, result.success);
   }

   /* Surface a genuine tool failure even when the step has no `store` — otherwise the
    * error text vanishes and the plan looks like it succeeded.  (This is how the
    * "fact too long" rejection silently disappeared.) */
   if (!result.success) {
      const char *result_text = tool_result_content(&result);
      char line[256];
      snprintf(line, sizeof(line), "[step %d] FAILED (%.32s): %.180s\n", current_index, tool_name,
               (result_text && result_text[0]) ? result_text : "tool reported failure");
      output_append(ctx, line);
      ctx->failed_steps++;
   }

   /* Free extended result if allocated */
   if (result.result_extended) {
      free(result.result_extended);
      result.result_extended = NULL;
   }
   if (result.vision_image) {
      free(result.vision_image);
      result.vision_image = NULL;
   }

   return PLAN_OK;
}

static int plan_step_if(plan_context_t *ctx, struct json_object *step) {
   const char *cond = jobj_get_string(step, "condition");
   if (!cond)
      return PLAN_ERR_PARSE;

   bool cond_result = plan_eval_condition(ctx, cond);

   struct json_object *branch;
   if (cond_result) {
      branch = jobj_get(step, "then");
   } else {
      branch = jobj_get(step, "else");
   }

   if (branch && json_object_is_type(branch, json_type_array)) {
      return plan_execute_steps(ctx, branch);
   }

   return PLAN_OK;
}

static int plan_step_loop(plan_context_t *ctx, struct json_object *step) {
   struct json_object *over = jobj_get(step, "over");
   const char *as = jobj_get_string(step, "as");
   struct json_object *steps = jobj_get(step, "steps");

   if (!over || !as || !steps)
      return PLAN_ERR_PARSE;

   const char *var_name = as;
   int count = json_object_array_length(over);

   /* Server-enforced iteration cap */
   if (count > PLAN_MAX_LOOP_ITERATIONS)
      count = PLAN_MAX_LOOP_ITERATIONS;

   for (int i = 0; i < count; i++) {
      struct json_object *item = json_object_array_get_idx(over, i);
      if (!item)
         continue;

      /* Set loop variable */
      if (json_object_is_type(item, json_type_string)) {
         plan_vars_set(ctx, var_name, json_object_get_string(item));
      } else if (json_object_is_type(item, json_type_int) ||
                 json_object_is_type(item, json_type_double)) {
         char num_buf[32];
         snprintf(num_buf, sizeof(num_buf), "%.0f", json_object_get_double(item));
         plan_vars_set(ctx, var_name, num_buf);
      } else if (json_object_is_type(item, json_type_object) ||
                 json_object_is_type(item, json_type_array)) {
         /* Serialize structured types — enables $var.field dot-access */
         const char *json_str = json_object_to_json_string_ext(item, JSON_C_TO_STRING_PLAIN);
         plan_vars_set(ctx, var_name, json_str ? json_str : "{}");
      }

      /* Execute loop body */
      int rc = plan_execute_steps(ctx, steps);
      if (rc != PLAN_OK)
         return rc;
   }

   return PLAN_OK;
}

/* =============================================================================
 * Plan Execution
 * ============================================================================= */

int plan_execute_steps(plan_context_t *ctx, struct json_object *steps) {
   if (!ctx || !steps)
      return PLAN_ERR_PARSE;

   /* Depth check */
   ctx->depth++;
   if (ctx->depth > PLAN_MAX_DEPTH) {
      snprintf(ctx->error, sizeof(ctx->error), "Max nesting depth (%d) exceeded", PLAN_MAX_DEPTH);
      ctx->depth--;
      return PLAN_ERR_MAX_DEPTH;
   }

   int len = json_object_array_length(steps);
   for (int i = 0; i < len; i++) {
      struct json_object *step = json_object_array_get_idx(steps, i);

      /* Step limit check */
      if (ctx->total_steps_executed >= PLAN_MAX_STEPS) {
         snprintf(ctx->error, sizeof(ctx->error), "Max steps (%d) exceeded", PLAN_MAX_STEPS);
         ctx->depth--;
         return PLAN_ERR_MAX_STEPS;
      }

      /* Timeout check */
      if (check_timeout(ctx)) {
         snprintf(ctx->error, sizeof(ctx->error), "Plan timeout (%ds) exceeded", ctx->timeout_s);
         ctx->depth--;
         return PLAN_ERR_TIMEOUT;
      }

      ctx->total_steps_executed++;

      const char *type = jobj_get_string(step, "type");
      if (!type)
         continue;

      int rc = PLAN_OK;

      if (strcmp(type, "log") == 0)
         rc = plan_step_log(ctx, step);
      else if (strcmp(type, "set") == 0)
         rc = plan_step_set(ctx, step);
      else if (strcmp(type, "call") == 0)
         rc = plan_step_call(ctx, step);
      else if (strcmp(type, "if") == 0)
         rc = plan_step_if(ctx, step);
      else if (strcmp(type, "loop") == 0)
         rc = plan_step_loop(ctx, step);
      else if (strcmp(type, "sleep") == 0)
         rc = plan_step_sleep(ctx, step);

      /* Safety violations abort the plan */
      if (rc != PLAN_OK) {
         ctx->depth--;
         return rc;
      }
   }

   ctx->depth--;
   return PLAN_OK;
}
