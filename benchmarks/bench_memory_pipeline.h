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
 * Memory-pipeline mode public surface for bench_retrieval.
 */

#ifndef BENCH_MEMORY_PIPELINE_H
#define BENCH_MEMORY_PIPELINE_H

struct json_object;

/**
 * Initialize :memory: SQLite + apply BENCH_MEMORY_DDL.
 * Inserts a benchmark user with id=1.  Must be called instead of (not
 * alongside) the existing setup_db() — the two DDLs use incompatible
 * users-table schemas.
 *
 * Returns 0 on success, 1 on failure.
 */
int bench_mp_init(void);

/**
 * Close the in-memory database.
 */
void bench_mp_teardown(void);

/**
 * Run a smoke-test pass: ingest one LoCoMo conversation, fire extraction
 * at each session boundary, dump facts + provenance to stdout as JSON.
 *
 * @param locomo_path  Path to LoCoMo dataset (e.g., locomo10.json)
 * @param conv_idx     Index of the conversation to process (0-9)
 * @return 0 on success, 1 on failure.
 */
int bench_mp_run_smoke(const char *locomo_path, int conv_idx);

/**
 * Dispatch a JSON command in memory-pipeline mode.
 *
 * Recognised commands (return 1 if handled, 0 if unknown):
 *   conv_create   {title}                   → {conv_id}
 *   add_message   {conv_id, dia_id, role, content}
 *                                           → {msg_id, dia_id}
 *   extract       {conv_id, session_id, timeout_ms}
 *                                           → {duration_ms, facts_added,
 *                                              entities_added}
 *   query_memory  {user_id, text, top_k}    → {results: [{fact_id, score,
 *                                              text, conv_id, msg_start,
 *                                              msg_end, covered_dia_ids}]}
 *   reset_memory  {user_id}                 → {ok}
 *
 * Unknown commands return 0 so the caller can fall through to its own
 * dispatcher or emit an error.
 *
 * @param cmd  Parsed JSON command
 * @return 1 if handled, 0 if unknown.
 */
int bench_mp_dispatch(struct json_object *cmd);

/**
 * Return SHA-256 (lowercase hex, 64 chars + NUL) of the extraction template in
 * use (the compiled MEMORY_EXTRACTION_PROMPT_TEMPLATE, or the
 * DAWN_EXTRACTION_PROMPT_FILE override).  Computed once on first call and
 * cached.
 *
 * The Python harness embeds this in the snapshot cache key so that any change
 * to the extraction prompt template invalidates older cached snapshots
 * (which were produced under a different prompt and therefore would yield
 * apples-to-oranges results if reused).  The hash is also written into the
 * snapshot map JSON on save and validated on load — a loud failure on
 * mismatch is preferred over silent re-extraction (which would cost real
 * money in LLM calls).
 *
 * @return Pointer to a NUL-terminated 64-char lowercase hex string.  The
 *         buffer is static; do not free.  Never NULL.
 */
const char *bench_mp_extraction_prompt_sha256(void);

#endif /* BENCH_MEMORY_PIPELINE_H */
