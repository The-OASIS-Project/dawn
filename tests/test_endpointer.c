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
 * Trace-replay tests for the pure endpointer. The load-bearing safety property:
 * COMMIT fires on the SAME frame whether adaptive is on or off — proving the
 * adaptive path never moves end-of-speech timing vs. the legacy dwell.
 */

#include "core/endpointer.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Match the daemon's cadence + defaults so float accumulation matches production. */
#define DT 0.05f
#define T_HUSH 0.3f
#define T_COMMIT 1.0f
#define MAX_REC 30.0f

/* Feed @speech_frames of speech then keep feeding silence until COMMIT (or a cap),
 * returning the 0-based frame index at which COMMIT fired, or -1. Records the last
 * non-NONE event seen before COMMIT into *last_pre for optional inspection. */
static int run_to_commit(endpointer_t *ep, int speech_frames, int max_frames) {
   for (int i = 0; i < max_frames; i++) {
      bool is_speech = (i < speech_frames);
      endpoint_event_t ev = endpointer_feed(ep, is_speech, DT);
      if (ev == ENDPOINT_COMMIT) {
         return i;
      }
   }
   return -1;
}

/* ---- legacy timing ---- */

/* Clean stop: COMMIT fires when accumulated silence first reaches t_commit, using
 * the exact float accumulation the daemon uses (so this pins the legacy frame). */
static void test_legacy_clean_stop(void) {
   endpointer_t ep;
   endpointer_init(&ep, T_HUSH, T_COMMIT, MAX_REC, false);
   int commit = run_to_commit(&ep, 10 /* 0.5s speech */, 200);
   /* silence starts at frame 10; commit on the first frame where silence_s >= 1.0 */
   int expected = 10;
   float s = 0.0f;
   while (s < T_COMMIT) {
      s += DT;
      expected++;
   }
   expected--; /* run_to_commit index is 0-based on the committing frame */
   TEST_ASSERT_EQUAL_INT(expected, commit);
   TEST_ASSERT_FALSE(endpointer_commit_forced(&ep));
}

/* The max-recording cap wins and is flagged forced. */
static void test_legacy_max_rec_forced(void) {
   endpointer_t ep;
   endpointer_init(&ep, T_HUSH, T_COMMIT, 1.0f /* tiny cap */, false);
   /* Continuous speech never reaches end-of-speech, so only the cap can fire. */
   int commit = run_to_commit(&ep, 1000, 40);
   TEST_ASSERT_NOT_EQUAL(-1, commit);
   TEST_ASSERT_TRUE(endpointer_commit_forced(&ep));
}

/* Speech resuming on the frame the cap is reached: the cap still wins. */
static void test_max_rec_wins_over_a_cancel(void) {
   endpointer_t ep;
   endpointer_init(&ep, T_HUSH, T_COMMIT, 0.5f, false);
   endpoint_event_t ev = ENDPOINT_NONE;
   for (int i = 0; i < 9; i++) { /* 0.45 s: speech, then a pause that arms */
      ev = endpointer_feed(&ep, i < 2, DT);
   }
   TEST_ASSERT_TRUE(ep.tentative);
   ev = endpointer_feed(&ep, true, DT); /* 0.5 s, and speech resumes */
   TEST_ASSERT_EQUAL_INT(ENDPOINT_COMMIT, ev);
   TEST_ASSERT_TRUE(endpointer_commit_forced(&ep));
}

/* A pause shorter than the commit dwell does not end speech; resumed speech
 * continues the same utterance. */
static void test_pause_below_commit_does_not_end(void) {
   endpointer_t ep;
   endpointer_init(&ep, T_HUSH, T_COMMIT, MAX_REC, false);
   /* 10 speech, 10 silence (0.5s < 1.0s), 10 speech, then silence to commit. */
   for (int i = 0; i < 10; i++) {
      TEST_ASSERT_NOT_EQUAL(ENDPOINT_COMMIT, endpointer_feed(&ep, true, DT));
   }
   for (int i = 0; i < 10; i++) {
      TEST_ASSERT_NOT_EQUAL(ENDPOINT_COMMIT, endpointer_feed(&ep, false, DT));
   }
   /* resumed speech */
   TEST_ASSERT_NOT_EQUAL(ENDPOINT_COMMIT, endpointer_feed(&ep, true, DT));
   /* now a full commit-length silence ends it */
   int commit = run_to_commit(&ep, 0, 40);
   TEST_ASSERT_NOT_EQUAL(-1, commit);
}

/* ---- tentative / cancel (shadow signal) ---- */

/* Silence crossing t_hush arms exactly one TENTATIVE; resumed speech CANCELs it
 * and records the pause length. */
static void test_tentative_then_cancel(void) {
   endpointer_t ep;
   endpointer_init(&ep, T_HUSH, T_COMMIT, MAX_REC, false);
   for (int i = 0; i < 5; i++) {
      endpointer_feed(&ep, true, DT); /* voiced */
   }
   /* 10 frames of silence (0.5s): crosses t_hush (0.3s) at frame 6 of silence. */
   int tentatives = 0, cancels = 0;
   for (int i = 0; i < 10; i++) {
      endpoint_event_t ev = endpointer_feed(&ep, false, DT);
      if (ev == ENDPOINT_TENTATIVE) {
         tentatives++;
      }
      TEST_ASSERT_NOT_EQUAL(ENDPOINT_COMMIT, ev); /* 0.5s < 1.0s: never commits */
   }
   TEST_ASSERT_EQUAL_INT(1, tentatives); /* armed once, not per-frame */
   endpoint_event_t ev = endpointer_feed(&ep, true, DT);
   TEST_ASSERT_EQUAL_INT(ENDPOINT_CANCEL, ev);
   cancels++;
   TEST_ASSERT_EQUAL_INT(1, ep.resume_count);
   TEST_ASSERT_EQUAL_INT(1, ep.tentative_count);
   TEST_ASSERT_TRUE(ep.max_pause_s >= 0.49f && ep.max_pause_s <= 0.51f);
   (void)cancels;
}

/* A stutter (several sub-commit pauses) yields several cancels but a single commit. */
static void test_stutter_one_commit(void) {
   endpointer_t ep;
   endpointer_init(&ep, T_HUSH, T_COMMIT, MAX_REC, false);
   int commits = 0, cancels = 0;
   /* pattern: (5 speech, 10 silence) x3, then trailing silence to commit */
   for (int rep = 0; rep < 3; rep++) {
      for (int i = 0; i < 5; i++) {
         if (endpointer_feed(&ep, true, DT) == ENDPOINT_COMMIT) {
            commits++;
         }
      }
      for (int i = 0; i < 10; i++) {
         endpoint_event_t ev = endpointer_feed(&ep, false, DT);
         if (ev == ENDPOINT_COMMIT) {
            commits++;
         }
         if (ev == ENDPOINT_CANCEL) {
            cancels++;
         }
      }
      /* resume */
      if (endpointer_feed(&ep, true, DT) == ENDPOINT_CANCEL) {
         cancels++;
      }
   }
   int commit = run_to_commit(&ep, 0, 40);
   TEST_ASSERT_NOT_EQUAL(-1, commit); /* exactly one real end-of-speech */
   TEST_ASSERT_EQUAL_INT(0, commits); /* none of the sub-pauses committed */
   TEST_ASSERT_TRUE(cancels >= 3);    /* each resumed pause cancelled */
}

/* ---- the load-bearing property: adaptive never moves the commit frame ---- */

static void assert_same_commit_frame(int speech_frames, int silence_gap_frames) {
   endpointer_t leg, adp;
   endpointer_init(&leg, T_HUSH, T_COMMIT, MAX_REC, false);
   endpointer_init(&adp, T_HUSH, T_COMMIT, MAX_REC, true);
   int leg_commit = -1, adp_commit = -1;
   for (int i = 0; i < 400; i++) {
      /* speech, then an interior gap, then speech, then trailing silence */
      bool is_speech;
      if (i < speech_frames) {
         is_speech = true;
      } else if (i < speech_frames + silence_gap_frames) {
         is_speech = false;
      } else if (i < speech_frames + silence_gap_frames + 3) {
         is_speech = true;
      } else {
         is_speech = false;
      }
      if (endpointer_feed(&leg, is_speech, DT) == ENDPOINT_COMMIT && leg_commit < 0) {
         leg_commit = i;
      }
      if (endpointer_feed(&adp, is_speech, DT) == ENDPOINT_COMMIT && adp_commit < 0) {
         adp_commit = i;
      }
   }
   TEST_ASSERT_NOT_EQUAL(-1, leg_commit);
   TEST_ASSERT_EQUAL_INT(leg_commit, adp_commit); /* identical timing */
}

static void test_adaptive_commit_same_frame_as_legacy(void) {
   assert_same_commit_frame(10, 0);  /* clean stop */
   assert_same_commit_frame(10, 6);  /* interior pause that crossed t_hush */
   assert_same_commit_frame(10, 18); /* pause just under commit (0.9s) then resume */
   assert_same_commit_frame(3, 4);   /* very short utterance */
}

/* Reset clears state + shadow stats but keeps config. */
static void test_reset_clears_state_keeps_config(void) {
   endpointer_t ep;
   endpointer_init(&ep, T_HUSH, T_COMMIT, MAX_REC, true);
   for (int i = 0; i < 5; i++) {
      endpointer_feed(&ep, true, DT);
   }
   for (int i = 0; i < 8; i++) {
      endpointer_feed(&ep, false, DT);
   }
   TEST_ASSERT_TRUE(ep.tentative_count > 0);
   endpointer_reset(&ep);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ep.silence_s);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ep.recording_s);
   TEST_ASSERT_EQUAL_INT(0, ep.tentative_count);
   TEST_ASSERT_FALSE(ep.tentative);
   TEST_ASSERT_TRUE(ep.adaptive); /* config survives */
   TEST_ASSERT_EQUAL_FLOAT(T_COMMIT, ep.t_commit);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_legacy_clean_stop);
   RUN_TEST(test_legacy_max_rec_forced);
   RUN_TEST(test_max_rec_wins_over_a_cancel);
   RUN_TEST(test_pause_below_commit_does_not_end);
   RUN_TEST(test_tentative_then_cancel);
   RUN_TEST(test_stutter_one_commit);
   RUN_TEST(test_adaptive_commit_same_frame_as_legacy);
   RUN_TEST(test_reset_clears_state_keeps_config);
   return UNITY_END();
}
