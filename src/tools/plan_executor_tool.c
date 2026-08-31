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
 * Plan Executor Tool — tool registry entry and callback wrapper
 *
 * Bridges the tool registry to the plan execution engine.
 * Receives plan JSON from LLM, creates execution context,
 * runs the plan, and returns accumulated output.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "tools/plan_executor.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Configuration
 * ============================================================================= */

#include "tools/toml.h"

typedef struct {
   int timeout_seconds;
} plan_executor_config_t;

static plan_executor_config_t s_config = {
   .timeout_seconds = PLAN_TIMEOUT_DEFAULT_S,
};

static void plan_executor_parse_config(toml_table_t *table, void *config) {
   plan_executor_config_t *cfg = (plan_executor_config_t *)config;
   if (!table)
      return;

   toml_datum_t timeout = toml_int_in(table, "timeout_seconds");
   if (timeout.ok) {
      int val = (int)timeout.u.i;
      if (val >= 5 && val <= 300) {
         cfg->timeout_seconds = val;
      } else {
         OLOG_WARNING("plan_executor: timeout_seconds %d out of range [5-300], using default %d",
                      val, PLAN_TIMEOUT_DEFAULT_S);
      }
   }
}

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *plan_executor_callback(const char *action, char *value, int *should_respond);

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const treg_param_t plan_params[] = {
   { .name = "plan",
     .type = TOOL_PARAM_TYPE_STRING,
     .required = true,
     .description =
         "JSON ARRAY of step objects, top-level shape `[{...},{...}]`. "
         "Do NOT wrap in `{\"plan\":[...]}` — the top-level value IS the array. "
         "Pass as a JSON-encoded string of the array (escaped quotes inside, but the array "
         "itself is the outer value of this `plan` argument).\n"
         "\n"
         "Step types and required fields:\n"
         "  call:  {\"type\":\"call\",\"tool\":\"<name>\",\"args\":{...},\"store\":\"<var>\"}\n"
         "  if:    {\"type\":\"if\",\"condition\":\"<expr>\",\"then\":[...],\"else\":[...]}\n"
         "  loop:  {\"type\":\"loop\",\"over\":\"{{var}}\",\"as\":\"item\",\"steps\":[...]}\n"
         "  set:   {\"type\":\"set\",\"var\":\"<name>\",\"value\":\"<expr or literal>\"}\n"
         "  log:   {\"type\":\"log\",\"message\":\"<text with {{var}} interpolation>\"}\n"
         "  sleep: {\"type\":\"sleep\",\"seconds\":N}     // N is 1-300\n"
         "\n"
         "Variable interpolation: `store: \"foo\"` on a `call` step saves its result; "
         "later steps reference it as `{{foo}}` inside any string field. Variable names "
         "must match [a-z_][a-z0-9_]* (lowercase letters, digits, underscores).\n"
         "\n"
         "When to use this tool: reach for it ONLY when you need conditional branching, "
         "looping over results, or chaining where step N consumes step M's stored output. "
         "If you just need 2-3 unrelated tool calls, call them directly — don't wrap them "
         "in a plan.\n"
         "\n"
         "Example (sequential fetch + summarize):\n"
         "  "
         "[{\"type\":\"call\",\"tool\":\"weather\",\"args\":{\"action\":\"today\",\"value\":"
         "\"Atlanta\"},"
         "\"store\":\"w\"},\n"
         "   {\"type\":\"log\",\"message\":\"Got weather: {{w}}\"}]" },
};

static const tool_metadata_t plan_executor_metadata = {
   .name = "execute_plan",
   .device_string = "plan executor",
   .description = "Execute a multi-step tool plan locally. Use this when a task "
                  "requires multiple tool calls with conditional logic or data "
                  "dependencies between steps. The plan runs entirely on the server "
                  "without additional LLM round trips. Returns aggregated results. "
                  "NOTE: only schedulable, non-dangerous tools may run inside a plan "
                  "(e.g. search, weather, calendar, home_assistant). Stateful tools such "
                  "as `memory` and `phone` are NOT allowed in plans — call those directly. "
                  "If a step's tool is rejected, the result reports the failure; do not "
                  "claim the action succeeded.",
   .params = plan_params,
   .param_count = 1,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = 0,

   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = plan_executor_parse_config,
   .config_section = "plan_executor",

   .callback = plan_executor_callback,
};

/* =============================================================================
 * Callback Implementation
 * ============================================================================= */

/**
 * @brief Plan executor tool callback
 *
 * Receives plan JSON string from LLM tool dispatch, creates an execution
 * context, parses the plan, executes all steps, and returns accumulated
 * output. Handles both direct JSON arrays and double-encoded JSON strings.
 *
 * @param action  Unused (single-action tool)
 * @param value   Plan JSON string (may be a JSON array or escaped string)
 * @param should_respond  Set to 1 — always return result to LLM
 * @return Heap-allocated result string (caller frees)
 */
static char *plan_executor_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || !value[0]) {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: empty plan");
   }

   OLOG_INFO("plan_executor: received plan (%zu bytes)", strlen(value));

   /* Parse the plan JSON.  Use the diag variant so we can surface
    * json-c's parse-error reason + byte position + a snippet of the
    * offending input — bare error codes don't tell weaker models how
    * to recover. */
   struct json_object *plan = NULL;
   char diag[512];
   int rc = plan_parse_with_diag(value, &plan, diag, sizeof(diag));
   if (rc != PLAN_OK || !plan) {
      char err[768];
      snprintf(err, sizeof(err),
               TOOL_RESULT_ERROR_MARK
               "Error: plan parse failed — %s\n"
               "Reminder: the `plan` argument must be a JSON array `[{...},{...}]`. Do NOT "
               "wrap it in `{\"plan\":[...]}` — the top-level value IS the array.",
               diag[0] ? diag : "no diagnostic available");
      OLOG_WARNING("plan_executor: plan parse failed (code %d): %s", rc,
                   diag[0] ? diag : "(no diag)");
      return strdup(err);
   }

   /* Initialize execution context */
   plan_context_t ctx = { 0 };
   ctx.timeout_s = s_config.timeout_seconds;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   /* Notify: plan start */
   plan_notify_progress("{\"type\":\"plan_progress\",\"payload\":{\"action\":\"start\"}}");

   /* Execute */
   rc = plan_execute_steps(&ctx, plan);

   /* Calculate total elapsed time */
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   long total_ms = (now.tv_sec - ctx.start_time.tv_sec) * 1000 +
                   (now.tv_nsec - ctx.start_time.tv_nsec) / 1000000;

   /* Build result */
   char *result = NULL;
   if (rc != PLAN_OK) {
      /* Partial output + error */
      size_t len = strlen(ctx.output) + strlen(ctx.error) + 64;
      result = malloc(len);
      if (result) {
         if (ctx.output[0]) {
            snprintf(result, len, TOOL_RESULT_ERROR_MARK "%s\n[Plan stopped: %s]", ctx.output,
                     ctx.error);
         } else {
            snprintf(result, len, TOOL_RESULT_ERROR_MARK "[Plan error: %s]", ctx.error);
         }
      }
      OLOG_WARNING("plan_executor: failed (code %d): %s", rc, ctx.error);

      /* Notify: plan error — use json-c to safely escape error text */
      {
         struct json_object *nerr = json_object_new_object();
         struct json_object *perr = json_object_new_object();
         if (nerr && perr) {
            json_object_object_add(nerr, "type", json_object_new_string("plan_progress"));
            json_object_object_add(perr, "action", json_object_new_string("error"));
            json_object_object_add(perr, "error", json_object_new_string(ctx.error));
            json_object_object_add(nerr, "payload", perr);
            const char *ns = json_object_to_json_string_ext(nerr, JSON_C_TO_STRING_PLAIN);
            if (ns)
               plan_notify_progress(ns);
            json_object_put(nerr);
         } else {
            json_object_put(nerr);
            json_object_put(perr);
         }
      }
   } else if (ctx.failed_steps > 0) {
      /* Fail-forward steps don't abort the plan, but the result MUST say so — otherwise
       * the model is told "executed successfully" for steps that did nothing and will
       * report success to the user (e.g. claiming a journal was saved when every memory
       * step was rejected). */
      size_t len = strlen(ctx.output) + 192;
      result = malloc(len);
      if (result) {
         snprintf(result, len,
                  TOOL_RESULT_ERROR_MARK
                  "Plan completed but %d of %d step(s) FAILED — the actions those steps "
                  "intended were NOT performed. Details:\n%s",
                  ctx.failed_steps, ctx.total_tool_calls,
                  ctx.output[0] ? ctx.output : "(no detail captured)");
         OLOG_WARNING("plan_executor: completed with failures — %d of %d tool call(s) failed",
                      ctx.failed_steps, ctx.total_tool_calls);
      } else {
         OLOG_ERROR("plan_executor: malloc failed building failure summary (%d of %d failed)",
                    ctx.failed_steps, ctx.total_tool_calls);
      }
   } else {
      if (ctx.output[0]) {
         result = strdup(ctx.output);
      } else {
         result = strdup("Plan executed successfully (no output).");
      }
      OLOG_INFO("plan_executor: completed — %d steps, %d tool calls", ctx.total_steps_executed,
                ctx.total_tool_calls);

      /* Notify: plan done */
      char notify[256];
      snprintf(notify, sizeof(notify),
               "{\"type\":\"plan_progress\",\"payload\":{\"action\":\"done\","
               "\"total_ms\":%ld,\"tool_calls\":%d}}",
               total_ms, ctx.total_tool_calls);
      plan_notify_progress(notify);
   }

   /* Cleanup */
   plan_context_cleanup(&ctx);
   json_object_put(plan);

   return result ? result : strdup(TOOL_RESULT_ERROR_MARK "Error: allocation failed");
}

/* =============================================================================
 * Tool Registration
 * ============================================================================= */

int plan_executor_tool_register(void) {
   return tool_registry_register(&plan_executor_metadata);
}
