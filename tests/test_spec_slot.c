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
 * Unit tests for the speculative-decode slot state machine. Exercises the
 * arm/launch/store/take/invalidate transitions with a FAKE worker (no threads,
 * no audio), pinning the safety properties: a stale (superseded) decode can never
 * be stored or taken, the in-flight and per-utterance-fire caps hold, and the
 * fire cap persists across a stutter but resets on a new utterance.
 */

#include <stdlib.h>
#include <string.h>

#include "core/spec_slot.h"
#include "unity.h"

#define MAX_FIRES 3

void setUp(void) {
}
void tearDown(void) {
}

static char *dup(const char *s) {
   char *p = strdup(s);
   TEST_ASSERT_NOT_NULL(p);
   return p;
}

/* A speculative worker run: capture gen (as the real launcher does), then later
 * hand a transcript in. spec_slot_store consumes `heap` unconditionally (stores on
 * accept, frees on reject), so the worker never frees it — mirroring real code. */
static bool fake_worker(spec_slot_t *s,
                        uint64_t gen,
                        const char *text,
                        int64_t now,
                        bool recording) {
   return spec_slot_store(s, gen, dup(text), now, recording);
}

/* ---- init ---- */

static void test_init_is_empty(void) {
   spec_slot_t s;
   spec_slot_init(&s);
   TEST_ASSERT_EQUAL_UINT64(0, spec_slot_gen(&s));
   TEST_ASSERT_FALSE(s.armed);
   TEST_ASSERT_FALSE(s.inflight);
   TEST_ASSERT_EQUAL_INT(0, s.fires);
   TEST_ASSERT_FALSE(s.ready);
   TEST_ASSERT_NULL(s.text);
   TEST_ASSERT_NULL(spec_slot_take_if_current(&s));
}

/* ---- happy path: launch -> store -> take ---- */

static void test_launch_store_take(void) {
   spec_slot_t s;
   spec_slot_init(&s);

   TEST_ASSERT_TRUE(spec_slot_can_arm(&s, MAX_FIRES));
   uint64_t gen = spec_slot_gen(&s);
   spec_slot_mark_launched(&s, 100);
   TEST_ASSERT_TRUE(s.armed);
   TEST_ASSERT_TRUE(s.inflight);
   TEST_ASSERT_EQUAL_INT(1, s.fires);

   TEST_ASSERT_TRUE(fake_worker(&s, gen, "turn on the lights", 700, true));
   TEST_ASSERT_TRUE(s.ready);
   TEST_ASSERT_FALSE(s.inflight); /* decode finished */

   char *out = spec_slot_take_if_current(&s);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("turn on the lights", out);
   free(out);
   TEST_ASSERT_FALSE(s.ready);
   TEST_ASSERT_NULL(s.text); /* detached on take, no double-free */
}

/* ---- a resumed-speech cancel drops an in-flight decode's late store ---- */

static void test_cancel_drops_late_store(void) {
   spec_slot_t s;
   spec_slot_init(&s);

   uint64_t gen = spec_slot_gen(&s);
   spec_slot_mark_launched(&s, 100);

   /* Speech resumed before the decode returned -> invalidate (cap persists). */
   spec_slot_invalidate(&s, false);
   TEST_ASSERT_FALSE(s.armed);
   TEST_ASSERT_TRUE(s.inflight); /* worker still running; it clears this on store */

   /* The stale worker finishes and tries to store its now-superseded result. */
   TEST_ASSERT_FALSE(fake_worker(&s, gen, "stale text", 750, true));
   TEST_ASSERT_FALSE(s.ready);
   TEST_ASSERT_FALSE(s.inflight); /* cleared even on reject */
   TEST_ASSERT_NULL(spec_slot_take_if_current(&s));
}

/* ---- take rejects a result whose gen was bumped after it was stored ---- */

static void test_take_rejects_bumped_after_store(void) {
   spec_slot_t s;
   spec_slot_init(&s);
   uint64_t gen = spec_slot_gen(&s);
   spec_slot_mark_launched(&s, 100);
   TEST_ASSERT_TRUE(fake_worker(&s, gen, "hello", 700, true));

   /* A new utterance/cancel lands between store and take. */
   spec_slot_invalidate(&s, false);
   TEST_ASSERT_NULL(spec_slot_take_if_current(&s));
}

/* ---- store rejected when the connection has left RECORDING ---- */

static void test_store_rejected_when_not_recording(void) {
   spec_slot_t s;
   spec_slot_init(&s);
   uint64_t gen = spec_slot_gen(&s);
   spec_slot_mark_launched(&s, 100);

   TEST_ASSERT_FALSE(fake_worker(&s, gen, "text", 700, /*recording=*/false));
   TEST_ASSERT_FALSE(s.ready);
   TEST_ASSERT_FALSE(s.inflight);
}

/* ---- in-flight cap: cannot arm a second decode while one is running ---- */

static void test_cannot_arm_while_inflight(void) {
   spec_slot_t s;
   spec_slot_init(&s);
   spec_slot_mark_launched(&s, 100);
   TEST_ASSERT_FALSE(spec_slot_can_arm(&s, MAX_FIRES)); /* inflight blocks */

   /* After the decode returns (inflight cleared) but still armed, still blocked. */
   TEST_ASSERT_TRUE(fake_worker(&s, spec_slot_gen(&s), "x", 700, true));
   TEST_ASSERT_FALSE(spec_slot_can_arm(&s, MAX_FIRES)); /* armed blocks re-arm */
   spec_slot_free(&s);
}

/* ---- fire cap persists across stutter, resets on new utterance ---- */

static void test_fire_cap_persists_then_resets(void) {
   spec_slot_t s;
   spec_slot_init(&s);

   /* Three stutters, each: arm -> launch -> resume(cancel, no fire reset). */
   for (int i = 0; i < MAX_FIRES; i++) {
      TEST_ASSERT_TRUE(spec_slot_can_arm(&s, MAX_FIRES));
      spec_slot_mark_launched(&s, 100 + i);
      spec_slot_invalidate(&s, false); /* resumed speech, cap NOT reset */
      /* pretend the stale worker returns and clears inflight */
      TEST_ASSERT_FALSE(fake_worker(&s, spec_slot_gen(&s) - 1, "stale", 200 + i, true));
   }
   TEST_ASSERT_EQUAL_INT(MAX_FIRES, s.fires);
   TEST_ASSERT_FALSE(spec_slot_can_arm(&s, MAX_FIRES)); /* cap reached */

   /* A new utterance (set_state -> RECORDING) resets the cap. */
   spec_slot_invalidate(&s, true);
   TEST_ASSERT_EQUAL_INT(0, s.fires);
   TEST_ASSERT_TRUE(spec_slot_can_arm(&s, MAX_FIRES));
}

/* ---- invalidate frees a stored transcript (no leak, no double-free) ---- */

static void test_invalidate_frees_stored_text(void) {
   spec_slot_t s;
   spec_slot_init(&s);
   uint64_t gen = spec_slot_gen(&s);
   spec_slot_mark_launched(&s, 100);
   TEST_ASSERT_TRUE(fake_worker(&s, gen, "owned", 700, true));
   TEST_ASSERT_NOT_NULL(s.text);

   spec_slot_invalidate(&s, true); /* must free s.text internally */
   TEST_ASSERT_NULL(s.text);
   TEST_ASSERT_FALSE(s.ready);

   spec_slot_free(&s); /* idempotent on an already-empty slot */
   TEST_ASSERT_NULL(s.text);
}

/* ---- every invalidation advances the generation ---- */

static void test_invalidate_bumps_generation(void) {
   spec_slot_t s;
   spec_slot_init(&s);
   uint64_t g0 = spec_slot_gen(&s);
   spec_slot_invalidate(&s, false);
   uint64_t g1 = spec_slot_gen(&s);
   spec_slot_invalidate(&s, true);
   uint64_t g2 = spec_slot_gen(&s);
   TEST_ASSERT_TRUE(g1 == g0 + 1);
   TEST_ASSERT_TRUE(g2 == g1 + 1);
}

/* ---- hush-window timing predicate ---- */

static void test_hush_window(void) {
   const int64_t HUSH = 300, EOS = 1000;

   /* No speech yet -> never in window (the load-bearing guard). */
   TEST_ASSERT_FALSE(spec_in_hush_window(0, 999999, HUSH, EOS));

   /* Before hush, inside hush, at commit boundary, past commit. */
   TEST_ASSERT_FALSE(spec_in_hush_window(1000, 1000 + 299, HUSH, EOS));  /* gap 299 < hush */
   TEST_ASSERT_TRUE(spec_in_hush_window(1000, 1000 + 300, HUSH, EOS));   /* gap == hush */
   TEST_ASSERT_TRUE(spec_in_hush_window(1000, 1000 + 999, HUSH, EOS));   /* still in window */
   TEST_ASSERT_FALSE(spec_in_hush_window(1000, 1000 + 1000, HUSH, EOS)); /* gap == eos: commit */
   TEST_ASSERT_FALSE(spec_in_hush_window(1000, 1000 + 5000, HUSH, EOS)); /* well past */

   /* Disabled / degenerate config: hush 0, or hush not strictly < eos. */
   TEST_ASSERT_FALSE(spec_in_hush_window(1000, 1000 + 500, 0, EOS));
   TEST_ASSERT_FALSE(spec_in_hush_window(1000, 1000 + 500, EOS, EOS));     /* hush == eos */
   TEST_ASSERT_FALSE(spec_in_hush_window(1000, 1000 + 500, EOS + 1, EOS)); /* hush > eos */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_hush_window);
   RUN_TEST(test_init_is_empty);
   RUN_TEST(test_launch_store_take);
   RUN_TEST(test_cancel_drops_late_store);
   RUN_TEST(test_take_rejects_bumped_after_store);
   RUN_TEST(test_store_rejected_when_not_recording);
   RUN_TEST(test_cannot_arm_while_inflight);
   RUN_TEST(test_fire_cap_persists_then_resets);
   RUN_TEST(test_invalidate_frees_stored_text);
   RUN_TEST(test_invalidate_bumps_generation);
   return UNITY_END();
}
