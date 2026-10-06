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
 * Mock state shared between test_external_focus_adapters.c and
 * test_external_focus_adapters_stub.c.  Each test function programs
 * the relevant fields, then calls the code-under-test, then asserts
 * on outputs.  `ext_mock_reset()` between tests zeroes everything.
 */

#ifndef TEST_EXTERNAL_FOCUS_ADAPTERS_MOCKS_H
#define TEST_EXTERNAL_FOCUS_ADAPTERS_MOCKS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "tools/calendar_db.h"
#include "tools/document_db.h"

#define EXT_MOCK_MAX_CHUNKS 32
#define EXT_MOCK_MAX_ACCOUNTS 8
#define EXT_MOCK_MAX_CALENDARS 8
#define EXT_MOCK_MAX_OCCURRENCES 32
#define EXT_MOCK_DIMS 4 /* keep tiny for fast cosine; production uses 384 */

typedef struct {
   /* Document chunks — each owns its embedding pointer (heap-owned by
    * the test fixture; the stub copies into the caller's flat buffer). */
   document_chunk_t chunks[EXT_MOCK_MAX_CHUNKS];
   int chunk_user_id[EXT_MOCK_MAX_CHUNKS];             /* parallel — chunk struct
                                                          has no user_id field */
   const float *chunk_embeddings[EXT_MOCK_MAX_CHUNKS]; /* fixture-owned */
   int chunk_count;
   int chunk_dim; /* per-fixture; honors expected_dims contract */
   /* Keyword channel: a chunk with a score > 0 is returned by
    * document_db_chunk_search_bm25 with that normalized score. */
   float chunk_bm25[EXT_MOCK_MAX_CHUNKS];
   int call_count_bm25;

   /* Calendar accounts. */
   calendar_account_t accounts[EXT_MOCK_MAX_ACCOUNTS];
   int account_count;

   /* Calendars per account.  Linear array; lookup by `account_id`. */
   calendar_calendar_t calendars[EXT_MOCK_MAX_CALENDARS];
   int calendar_count;

   /* Occurrences.  Linear array.  Real schema joins occurrence ->
    * event -> calendar; tests skip the event indirection and carry
    * calendar_id in a parallel array (same pattern as 1c's
    * relation_user_id[]). */
   calendar_occurrence_t occurrences[EXT_MOCK_MAX_OCCURRENCES];
   int64_t occurrence_calendar_id[EXT_MOCK_MAX_OCCURRENCES];
   int occurrence_count;

   /* Embedding engine.  Tests programmatically toggle availability
    * for the document adapter's requires_embedding=true gate. */
   bool embeddings_available;

   /* Behavior toggles.  Each is asserted on in dedicated tests. */
   bool fail_chunk_search; /* force chunk_search_load FAILURE */
   bool fail_account_list; /* force account_list FAILURE */
   bool fail_occurrences_in_range;
   bool fail_occurrences_search;
   bool fail_events_nearest;

   /* Call-counting counters for the network-invariant assertions —
    * the test asserts that ONLY the expected DB-layer calls fired
    * during a compose, never a service-layer call (which would
    * fail to link in the first place — counters are belt-and-suspenders). */
   int call_count_chunk_search_load;

   /* Chunk generation: when pinned, document_db_chunk_generation returns
    * `generation` (so the real embedding cache can be reused); otherwise a
    * new value every call. */
   bool pin_generation;
   int64_t generation;
   int call_count_account_list;
   int call_count_calendar_list;
   int call_count_occurrences_in_range;
   int call_count_occurrences_search;
   int call_count_events_nearest;
   int call_count_allday_in_range;
} ext_mock_state_t;

extern ext_mock_state_t s_ext_mock;

void ext_mock_reset(void);

#endif /* TEST_EXTERNAL_FOCUS_ADAPTERS_MOCKS_H */
