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
 * Plan Executor — client-side multi-step tool plan execution
 *
 * Interprets a constrained JSON DSL emitted by LLMs, executing tools locally
 * with conditional logic, variable binding, and loop support. No external
 * scripting runtime required.
 */

#ifndef PLAN_EXECUTOR_H
#define PLAN_EXECUTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "llm/llm_tools.h"
#include "tools/tool_registry.h"

/* Forward-declare json_object to avoid requiring json-c/json.h in every consumer */
struct json_object;

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define PLAN_MAX_STEPS 50           /* Max total step executions (including nested) */
#define PLAN_MAX_TOOL_CALLS 20      /* Max tool call steps per plan */
#define PLAN_MAX_LOOP_ITERATIONS 10 /* Server-enforced loop iteration cap */
#define PLAN_MAX_VARS 32            /* Max variables in variable store */
#define PLAN_MAX_DEPTH 5            /* Max nesting depth (if/loop) */
#define PLAN_MAX_PLAN_SIZE 16384    /* Max plan JSON size in bytes */
#define PLAN_MAX_SLEEP_S 300        /* Max sleep duration per step (5 minutes) */
#define PLAN_VAR_NAME_MAX 32        /* Max variable name length */
#define PLAN_TIMEOUT_DEFAULT_S 60   /* Default execution timeout */

/* Error codes */
#define PLAN_OK 0
#define PLAN_ERR_PARSE 1
#define PLAN_ERR_MAX_STEPS 2
#define PLAN_ERR_MAX_DEPTH 3
#define PLAN_ERR_MAX_TOOL_CALLS 4
#define PLAN_ERR_TIMEOUT 5
#define PLAN_ERR_UNKNOWN_TOOL 6
#define PLAN_ERR_TOOL_NOT_ALLOWED 7
#define PLAN_ERR_TOOL_DISABLED 8
#define PLAN_ERR_UNKNOWN_STEP 9
#define PLAN_ERR_INVALID_VAR 10
#define PLAN_ERR_MAX_VARS 11

/* =============================================================================
 * Data Structures
 * ============================================================================= */

/**
 * @brief Variable in the plan's variable store
 *
 * Values are heap-allocated via strdup(), freed on overwrite and cleanup.
 */
typedef struct {
   char name[PLAN_VAR_NAME_MAX + 1];
   char *value;  /* heap-allocated, NULL if unset */
   bool success; /* whether the tool call that set this var succeeded */
} plan_var_t;

/**
 * @brief Plan execution context
 *
 * Tracks state during plan execution: variable store, output buffer,
 * safety counters, and timing.
 */
typedef struct {
   plan_var_t vars[PLAN_MAX_VARS];
   int var_count;
   char output[LLM_TOOLS_RESULT_LEN]; /* accumulated log output */
   int output_len;
   int depth;                /* current nesting depth */
   int total_steps_executed; /* counts ALL executions including nested */
   int total_tool_calls;     /* counts only 'call' steps */
   int failed_steps;         /* 'call' steps that failed (unknown/disallowed/disabled/tool error) */
   int call_index;           /* UI index for current call step */
   char error[256];          /* last error message */
   struct timespec start_time;
   int timeout_s;
   const char *third_party; /* a step's frame (tool_result_t.third_party): its text is the plan's */
} plan_context_t;

/* =============================================================================
 * Plan Parsing
 * ============================================================================= */

/**
 * @brief Parse and validate a plan JSON string
 *
 * Validates JSON size, structure, step types, and variable names.
 * Caller must free the returned json_object with json_object_put().
 *
 * @param json  Plan JSON string (array of step objects)
 * @param out   Output: parsed json_object array (NULL on error)
 * @return PLAN_OK on success, error code on failure
 */
int plan_parse(const char *json, struct json_object **out);

/**
 * @brief Like plan_parse(), but writes a human-readable diagnostic into
 *        `diag` on failure.
 *
 * For PLAN_ERR_PARSE the diagnostic includes the json-c tokener's error
 * description, the byte offset at which parsing failed, and up to 80
 * characters of the input near that offset.  For other error codes the
 * diagnostic gives a brief recovery hint.  On success (PLAN_OK) the diag
 * buffer is set to an empty string.  `diag` may be NULL (no diagnostic
 * written).
 *
 * @param json       Plan JSON string (array of step objects)
 * @param out        Output: parsed json_object array (NULL on error)
 * @param diag       Caller-supplied diagnostic buffer, may be NULL
 * @param diag_size  Size of `diag` in bytes (ignored when diag is NULL)
 * @return PLAN_OK on success, error code on failure
 */
int plan_parse_with_diag(const char *json, struct json_object **out, char *diag, size_t diag_size);

/* =============================================================================
 * Plan Execution
 * ============================================================================= */

/**
 * @brief Execute a parsed plan step array
 *
 * Recursive — called for top-level plan and for if/loop bodies.
 * Increments ctx->depth on entry, decrements on exit.
 *
 * @param ctx   Execution context (caller must initialize timeout/start_time)
 * @param steps json_object array of step objects
 * @return PLAN_OK on success, error code on safety violation
 */
int plan_execute_steps(plan_context_t *ctx, struct json_object *steps);

/* =============================================================================
 * Condition Evaluation
 * ============================================================================= */

/**
 * @brief Evaluate a condition expression against the variable store
 *
 * Supports: varname.empty, .notempty, .contains:text, .equals:text,
 *           .success, .failed, and literals "true"/"false".
 *
 * @param ctx   Execution context with variable store
 * @param expr  Condition expression string
 * @return true if condition is met, false otherwise (including malformed)
 */
bool plan_eval_condition(const plan_context_t *ctx, const char *expr);

/* =============================================================================
 * Variable Operations
 * ============================================================================= */

/**
 * @brief Set a variable in the variable store
 *
 * Overwrites if the variable already exists. Frees the old value.
 * Silently fails if the store is full (PLAN_MAX_VARS).
 */
void plan_vars_set(plan_context_t *ctx, const char *name, const char *value);

/**
 * @brief Get a variable value from the store
 * @return Variable value string, or NULL if not found
 */
const char *plan_vars_get(const plan_context_t *ctx, const char *name);

/**
 * @brief Set the success flag for a variable
 */
void plan_vars_set_success(plan_context_t *ctx, const char *name, bool success);

/**
 * @brief Validate a variable name
 *
 * Must match: ^[a-z_][a-z0-9_]{0,31}$
 * Must not contain: $ . { }
 */
bool plan_validate_var_name(const char *name);

/* =============================================================================
 * Interpolation
 * ============================================================================= */

/**
 * @brief Interpolate $var, ${var}, and $var.field references in a template string
 *
 * Dot-access ($var.field) parses the variable value as JSON and extracts
 * the named field. Only works with non-braced syntax.
 *
 * @param ctx       Execution context with variable store
 * @param template  Input template string
 * @param out       Output buffer
 * @param out_size  Output buffer size
 * @return 0 on success
 */
int plan_interpolate(const plan_context_t *ctx,
                     const char *template_str,
                     char *out,
                     size_t out_size);

/* =============================================================================
 * Argument Construction
 * ============================================================================= */

/**
 * @brief Build interpolated tool arguments as a JSON string
 *
 * Walks a json_object args template object, interpolates $var references in
 * string values, and serializes to a clean JSON string. Prevents
 * injection of JSON control characters or :: delimiters.
 *
 * @param ctx       Execution context with variable store
 * @param args      json_object template with $var references
 * @param out_json  Output JSON string buffer
 * @param out_size  Output buffer size
 * @return 0 on success
 */
int plan_build_args_json(plan_context_t *ctx,
                         struct json_object *args,
                         char *out_json,
                         size_t out_size);

/* =============================================================================
 * Tool Capability Check
 * ============================================================================= */

/**
 * @brief Check if a tool is allowed to be called from a plan
 *
 * Requires TOOL_CAP_SCHEDULABLE, blocks TOOL_CAP_DANGEROUS and self-reference.
 */
bool plan_tool_is_allowed(const tool_metadata_t *meta);

/* =============================================================================
 * Progress Notifications
 * ============================================================================= */

/**
 * @brief Send a plan progress notification to the active WebUI session
 *
 * No-op when WebUI is disabled or session is non-WebUI.
 * The json_str must be a complete JSON message string.
 *
 * @param json_str  Pre-formatted JSON string (not freed by callee)
 */
void plan_notify_progress(const char *json_str);

/* =============================================================================
 * Cleanup
 * ============================================================================= */

/**
 * @brief Free all heap-allocated variable values and reset context
 */
void plan_context_cleanup(plan_context_t *ctx);

/* =============================================================================
 * Tool Registration
 * ============================================================================= */

/**
 * @brief Register the plan executor as a tool in the registry
 */
int plan_executor_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* PLAN_EXECUTOR_H */
