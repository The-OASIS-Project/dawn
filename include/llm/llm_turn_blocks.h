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
 * An assistant turn as provider-neutral blocks, in the order the model produced
 * them: text, tool calls, and reasoning tagged with the vendor that signed or
 * encrypted it.  Stored on the history message under LLM_TURN_BLOCKS_KEY; each
 * provider's formatter renders the turn in its own shape from them.
 *
 * Text and tool calls translate to every provider.  Reasoning can't: a vendor
 * signs or encrypts it for itself, so a formatter sends back only its own
 * vendor's reasoning, verbatim, and leaves any other out (never as text).  It
 * stays with the turn, so a conversation that returns to that vendor replays it.
 *
 *   { "type": "text", "text": ... }
 *   { "type": "tool_call", "id": ..., "name": ..., "arguments": "<json text>" }
 *   { "type": "reasoning", "carrier": "anthropic", "format": "anthropic",
 *     "model": ..., "native": {...} }
 *   (OpenAI Responses: carrier the endpoint and key, format "openai", native
 *   a reasoning item {type, id, summary, encrypted_content})
 *   { "type": "opaque", "carrier": ..., "format": ..., "model": ..., "native": {...} }
 *
 * Vendor-issued data is exactly this: reasoning and opaque blocks, and a
 * tool_call's "sig" (a Gemini thought signature for that call).  Each
 * carries the carrier, format and model that issued it, and goes back only
 * where they match (one rule, in llm_turn_blocks.c).
 *
 * "carrier" is who issued it (the API it came through); "format" is whose
 * object it is (the same vendor directly; through a gateway, the upstream
 * vendor); "native" is that object exactly as received (for Anthropic, a
 * "thinking" block with its signature, or a "redacted_thinking" block).  An
 * "opaque" block is vendor content DAWN doesn't interpret: replayed verbatim
 * to its vendor (an omission would be a history edit there) and left out
 * elsewhere.  A tool_call may later carry vendor data of its own ("call_id",
 * a per-call reasoning "sig"); the stored shape carries a version.
 *
 * Every key DAWN puts on a history message for itself starts with '_' (this
 * one included): none of them go on the wire or into a text another model
 * reads (llm_history_wire_copy / llm_history_strip_internal).
 */

#ifndef LLM_TURN_BLOCKS_H
#define LLM_TURN_BLOCKS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** The history message key the blocks live under. */
#define LLM_TURN_BLOCKS_KEY "_blocks"

/** Formats (whose object a vendor block is).  A block's carrier (who issued
 * it) is the request's endpoint and key: llm_request_carrier(). */
#define LLM_FORMAT_ANTHROPIC "anthropic"
#define LLM_FORMAT_OPENAI "openai"
/* OpenRouter's reasoning_details entries (any upstream vendor: the entry's own
 * "format" says which), replayed as a sequence to the carrier that issued it. */
#define LLM_FORMAT_OPENROUTER "openrouter"
/* A Gemini thought signature, carried on the tool call it was issued with. */
#define LLM_FORMAT_GEMINI "gemini"

/** Room for a carrier (llm_turn_blocks_carrier). */
#define LLM_CARRIER_MAX 128
/* Reasoning is carried by the endpoint and organization that issued it: its
 * carrier is the endpoint (host, and a digest of scheme, host and path) and
 * the API key's tag, for every vendor.  A
 * vendor signs or encrypts it for its own organization, and a stored turn must
 * never go to another endpoint (a gateway, another account, another install). */

/** A new, empty block list (caller owns it; NULL on allocation failure). */
struct json_object *llm_turn_blocks_new(void);

/** Append a text block (empty text is skipped). */
void llm_turn_blocks_add_text(struct json_object *blocks, const char *text);

/**
 * @brief The carrier of a request's reasoning: its endpoint and API key
 *
 * The endpoint and the key's tag ("api.openai.com/<endpoint digest>#<tag>",
 * the tag from llm_key_tag): reasoning is encrypted or signed for the
 * organization that produced it, and another key or another gateway on the
 * same host (routed by path) may belong to another one.  The digest covers
 * scheme, host and path; credentials, query and fragment are never included.
 *
 * @param key_tag The request key's tag (llm_key_tag)
 * @param out At least LLM_CARRIER_MAX bytes
 */
void llm_turn_blocks_carrier(const char *base_url, const char *key_tag, char *out, size_t out_len);

/** Append a tool call; @p arguments is its JSON argument text ("" = {}). */
void llm_turn_blocks_add_tool_call(struct json_object *blocks,
                                   const char *id,
                                   const char *name,
                                   const char *arguments);

/**
 * @brief Append a tool call with the signature a vendor issued for it
 *
 * Gemini signs its reasoning per call ("thought_signature"); it goes back only
 * on requests to the same @p carrier and @p model, and nowhere else.
 */
void llm_turn_blocks_add_signed_tool_call(struct json_object *blocks,
                                          const char *id,
                                          const char *name,
                                          const char *arguments,
                                          const char *carrier,
                                          const char *model,
                                          const char *signature);

/** Append a reasoning block, taking ownership of @p native. */
void llm_turn_blocks_add_reasoning(struct json_object *blocks,
                                   const char *carrier,
                                   const char *format,
                                   const char *model,
                                   struct json_object *native);

/**
 * @brief Whether a reasoning or opaque block belongs on a request to a vendor
 *
 * Its carrier and format both match: only a vendor's own reasoning goes back
 * to it.  (Per-model-family rules, where a vendor has them, are the caller's.)
 */
bool llm_turn_blocks_is_own(struct json_object *block, const char *carrier, const char *format);

/**
 * @brief The blocks of a Claude response's content array
 *
 * Thinking and redacted_thinking become Anthropic reasoning (copied verbatim,
 * signature and all, even with empty text), text becomes text, tool_use a tool
 * call, and any other block type is kept as Anthropic opaque content.
 *
 * @param content The response's content blocks, in order
 * @param carrier The request's carrier (llm_request_carrier); "" for none
 * @param model The model that produced them
 * @return New blocks (caller owns them), or NULL
 */
struct json_object *llm_turn_blocks_from_claude(struct json_object *content,
                                                const char *carrier,
                                                const char *model);

/**
 * @brief Render blocks as a Claude assistant content array
 *
 * Anthropic reasoning and opaque content verbatim, text as text, tool calls as
 * tool_use (their arguments parsed; unparsable arguments become {}).
 * Reasoning and opaque content from any other carrier are left out.
 *
 * @param carrier The request's carrier (llm_request_carrier)
 * @return A new array (caller owns it), or NULL
 */
struct json_object *llm_turn_blocks_render_claude(struct json_object *blocks, const char *carrier);

/**
 * @brief The final answer's blocks, with its text replaced by @p final_text
 *
 * The answer DAWN keeps (tags stripped, citations resolved) replaces the text
 * blocks: one text block where the first one was (at the end if none), the
 * reasoning left where it was.
 *
 * @return New blocks (caller owns them), or NULL
 */
struct json_object *llm_turn_blocks_with_final_text(struct json_object *blocks,
                                                    const char *final_text);

/**
 * @brief Render blocks as OpenAI Responses input items, appended to @p input
 *
 * Each run of text as one assistant message (output_text), tool calls as
 * function_call items under their call_id, and OpenAI reasoning items verbatim
 * when they came from @p carrier (the endpoint and key) and @p model: an item
 * is encrypted for the model and organization that produced it.  Other reasoning
 * and opaque content are left out.
 *
 * @param carrier The request's endpoint and key (see LLM_FORMAT_OPENAI)
 * @param model   The request's model (NULL: any)
 */
void llm_turn_blocks_render_responses(struct json_object *blocks,
                                      struct json_object *input,
                                      const char *carrier,
                                      const char *model);

/**
 * @brief Render blocks as one chat-completions assistant message
 *
 * The text joined as "content" ("" when none), tool calls as "tool_calls", a
 * Gemini signature on its call (extra_content.google.thought_signature) and
 * OpenRouter "reasoning_details" (in order, unmodified) when they came from
 * @p carrier and @p model.  Other vendors' reasoning is left out.
 *
 * @param carrier The request's carrier (llm_turn_blocks_carrier)
 * @param model   The request's model
 * @return A new message (caller owns it), or NULL
 */
struct json_object *llm_turn_blocks_render_chat(struct json_object *blocks,
                                                const char *carrier,
                                                const char *model);

/** Most tool calls one turn reconciles (at least the parallel tool-call limit). */
#define LLM_TURN_CALLS_MAX 16

/** A tool call that ran: what a turn's blocks record for it. */
typedef struct {
   const char *id;
   const char *name;
   const char *arguments;
} llm_turn_call_t;

/**
 * @brief The blocks with their tool calls replaced by the calls that ran
 *
 * A turn's blocks come from the response as streamed; the calls DAWN runs (and
 * answers) are admitted separately, under limits of their own.  The two must
 * agree, or a replay carries a call with no result (or a result with no call):
 * an API error on every later request.  A tool_call block whose id ran is kept
 * as the model sent it (the run copy may be cut to fit a buffer); one that
 * didn't run is dropped; a call that ran without a block goes after the last
 * call kept (or at the end), as it ran.  Text and
 * reasoning keep their order.  With no calls (a final answer), no tool calls.
 *
 * @return New blocks (caller owns them), or NULL
 */
struct json_object *llm_turn_blocks_with_calls(struct json_object *blocks,
                                               const llm_turn_call_t *calls,
                                               int count);

/**
 * @brief An assistant message's blocks, however it was recorded
 *
 * Its blocks when it has them; a Claude content array read into blocks (from
 * before blocks existed); otherwise its text and chat-completions tool_calls.
 *
 * @return Blocks (caller owns a reference), or NULL
 */
struct json_object *llm_turn_message_blocks(struct json_object *message);

/**
 * @brief Set an assistant message's text, keeping its blocks in step
 *
 * The one way to change a history turn's text after it's recorded (a
 * messaging reply truncated to what was sent, say): "content" and the blocks'
 * text change together, so no provider replays the old text.
 */
void llm_turn_message_set_text(struct json_object *message, const char *text);

/**
 * @brief Characters a message replays beyond its content, for size estimates
 *
 * Reasoning (its text; for an empty-text block, its signature as a stand-in for
 * the hidden reasoning it encodes; for an OpenAI reasoning item, its encrypted
 * content and summary) and opaque blocks (their whole JSON).  Every vendor's
 * reasoning counts, whichever the next request goes to.
 */
size_t llm_turn_message_reasoning_chars(struct json_object *message);

/**
 * @brief A copy of @p history for the wire: DAWN's own keys removed
 *
 * Shallow: a message without any is shared, one with some is copied without
 * them.  Caller owns the array.  NULL on allocation failure, never a partial
 * copy: the caller must fail rather than send the original.
 */
struct json_object *llm_history_wire_copy(struct json_object *history);

/**
 * @brief A deep copy of @p history without DAWN's own keys
 *
 * For anything that isn't a replay to the same vendor: memory extraction, a
 * summarizer, a log on disk.  None of those may see reasoning a vendor issued
 * for itself (it would be extracted as facts, sent to another model, or
 * written out): DAWN's own keys go, and so do the thinking and redacted
 * thinking parts a Claude turn's content array holds.  Everything else keeps
 * its shape.  Caller owns it; NULL on failure.
 */
struct json_object *llm_history_strip_internal(struct json_object *history);

/* ---- The stored shape (llm_turn_blocks_stored.c) ---- */

/** Version of the stored envelope ({"v":1,"blocks":[...]}). */
#define LLM_TURN_BLOCKS_STORED_VERSION 1

/**
 * Largest stored value, in bytes: the database's cap on `messages.llm_blocks`
 * (CONV_LLM_BLOCKS_MAX), so what is written is what reloads.
 */
#define LLM_TURN_BLOCKS_STORED_MAX 4194304

/** Most blocks a stored turn may hold. */
#define LLM_TURN_BLOCKS_STORED_ENTRIES_MAX 4096

/**
 * @brief Blocks as the text stored with their assistant row
 *
 * A versioned envelope, serialized exactly (no slash escaping, strings and
 * numbers as received), so a reload replays the bytes that were sent.
 *
 * @return A malloc'd string (caller frees), or NULL when there are no blocks,
 *         on allocation failure, or when it would pass
 *         LLM_TURN_BLOCKS_STORED_MAX (logged; the row then saves without them)
 */
char *llm_turn_blocks_to_stored(struct json_object *blocks);

/**
 * @brief A final answer's blocks as stored text
 *
 * llm_turn_blocks_to_stored() for the reply row of a turn, which records no
 * tool calls: blocks that hold any aren't that row's (logged) and give NULL.
 * NULL @p blocks gives NULL.
 */
char *llm_turn_blocks_answer_stored(struct json_object *blocks);

/**
 * @brief Blocks from a row's stored text, or NULL when it isn't valid
 *
 * Fails closed: the text must parse within a depth limit, carry the known
 * version, hold at most LLM_TURN_BLOCKS_STORED_ENTRIES_MAX blocks, and every
 * block must be a known type with its required fields.  Anything else loads
 * the row without blocks (logged by @p row_id and reason, never the content).
 *
 * @return New blocks (caller owns them), or NULL
 */
struct json_object *llm_turn_blocks_from_stored(const char *text, size_t len, long long row_id);

/**
 * @brief Whether blocks record the same tool calls as a message's tool_calls
 *
 * The same call ids in the same order (NULL or an empty array: no calls).
 * Blocks that disagree with their message describe some other turn and aren't
 * replayed.
 */
bool llm_turn_blocks_calls_match(struct json_object *blocks, struct json_object *tool_calls);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TURN_BLOCKS_H */
