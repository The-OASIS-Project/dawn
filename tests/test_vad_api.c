/*
 * VAD API Unit Test
 *
 * Tests basic Silero VAD functionality:
 * 1. Initialization with valid/invalid paths
 * 2. Processing audio samples
 * 3. State reset
 * 4. Cleanup
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asr/vad_silero.h"

#define VAD_SAMPLE_SIZE 512
#define TEST_PASS "\033[32m[PASS]\033[0m"
#define TEST_FAIL "\033[31m[FAIL]\033[0m"

static int tests_passed = 0;
static int tests_failed = 0;

void test_result(const char *test_name, int passed) {
   if (passed) {
      printf("%s %s\n", TEST_PASS, test_name);
      tests_passed++;
   } else {
      printf("%s %s\n", TEST_FAIL, test_name);
      tests_failed++;
   }
}

// Test 1: Initialization with invalid path
void test_init_invalid_path() {
   silero_vad_context_t *ctx = vad_silero_init("/nonexistent/path/model.onnx", NULL);
   test_result("Init with invalid path returns NULL", ctx == NULL);
}

// Test 2: Initialization with valid path
silero_vad_context_t *test_init_valid_path() {
   const char *home = getenv("HOME");
   if (!home) {
      test_result("Init with valid path", 0);
      printf("  (HOME env not set - skipping)\n");
      return NULL;
   }

   char model_path[512];
   snprintf(model_path, sizeof(model_path),
            "%s/code/The-OASIS-Project/silero-vad/src/silero_vad/data/silero_vad_16k_op15.onnx",
            home);

   silero_vad_context_t *ctx = vad_silero_init(model_path, NULL);
   test_result("Init with valid path returns context", ctx != NULL);
   return ctx;
}

// Test 3: Process silence audio
void test_process_silence(silero_vad_context_t *ctx) {
   if (!ctx) {
      test_result("Process silence audio", 0);
      printf("  (context is NULL - skipping)\n");
      return;
   }

   int16_t silence[VAD_SAMPLE_SIZE] = { 0 };
   float prob = vad_silero_process(ctx, silence, VAD_SAMPLE_SIZE);

   test_result("Process silence returns valid probability", prob >= 0.0f && prob <= 1.0f);
   test_result("Silence has low speech probability", prob < 0.5f);
}

// Test 4: Process synthetic speech
void test_process_speech(silero_vad_context_t *ctx) {
   if (!ctx) {
      test_result("Process speech audio", 0);
      printf("  (context is NULL - skipping)\n");
      return;
   }

   // Generate 200Hz sine wave (synthetic speech)
   int16_t speech[VAD_SAMPLE_SIZE];
   for (int i = 0; i < VAD_SAMPLE_SIZE; i++) {
      double t = (double)i / 16000.0;
      speech[i] = (int16_t)(10000.0 * sin(2.0 * 3.14159 * 200.0 * t));
   }

   float prob = vad_silero_process(ctx, speech, VAD_SAMPLE_SIZE);
   test_result("Process speech returns valid probability", prob >= 0.0f && prob <= 1.0f);

   // Note: Silero VAD on synthetic sine wave may not reliably detect as speech
   // This just verifies the API works, not the accuracy on synthetic signals
   printf("  Speech probability: %.4f (synthetic signal)\n", prob);
}

// Test 5: Invalid input handling
void test_invalid_input(silero_vad_context_t *ctx) {
   if (!ctx) {
      test_result("Invalid input handling", 0);
      printf("  (context is NULL - skipping)\n");
      return;
   }

   int16_t audio[VAD_SAMPLE_SIZE] = { 0 };

   // NULL context
   float prob = vad_silero_process(NULL, audio, VAD_SAMPLE_SIZE);
   test_result("NULL context returns error", prob < 0.0f);

   // NULL audio
   prob = vad_silero_process(ctx, NULL, VAD_SAMPLE_SIZE);
   test_result("NULL audio returns error", prob < 0.0f);

   // Wrong sample count
   prob = vad_silero_process(ctx, audio, 256);
   test_result("Wrong sample count returns error", prob < 0.0f);
}

// Test 6: State reset
void test_state_reset(silero_vad_context_t *ctx) {
   if (!ctx) {
      test_result("State reset", 0);
      printf("  (context is NULL - skipping)\n");
      return;
   }

   int16_t audio[VAD_SAMPLE_SIZE];
   for (int i = 0; i < VAD_SAMPLE_SIZE; i++) {
      audio[i] = (int16_t)(5000 * sin(2.0 * 3.14159 * 100.0 * i / 16000.0));
   }

   // Process some audio to build up state
   vad_silero_process(ctx, audio, VAD_SAMPLE_SIZE);
   vad_silero_process(ctx, audio, VAD_SAMPLE_SIZE);
   vad_silero_process(ctx, audio, VAD_SAMPLE_SIZE);

   // Reset state
   vad_silero_reset(ctx);

   // Process silence - should behave like first call
   int16_t silence[VAD_SAMPLE_SIZE] = { 0 };
   float prob = vad_silero_process(ctx, silence, VAD_SAMPLE_SIZE);

   test_result("State reset doesn't crash", 1);
   test_result("Post-reset processing works", prob >= 0.0f && prob <= 1.0f);
}

// Test 7: Multiple sequential inferences
void test_sequential_inference(silero_vad_context_t *ctx) {
   if (!ctx) {
      test_result("Sequential inference", 0);
      printf("  (context is NULL - skipping)\n");
      return;
   }

   int16_t audio[VAD_SAMPLE_SIZE] = { 0 };
   int all_valid = 1;

   for (int i = 0; i < 10; i++) {
      float prob = vad_silero_process(ctx, audio, VAD_SAMPLE_SIZE);
      if (prob < 0.0f || prob > 1.0f) {
         all_valid = 0;
         break;
      }
   }

   test_result("10 sequential inferences all return valid probabilities", all_valid);
}

// Test 8: Cleanup handling
void test_cleanup() {
   // NULL cleanup should be safe
   vad_silero_cleanup(NULL);
   test_result("NULL cleanup doesn't crash", 1);

   // Reset NULL context should be safe
   vad_silero_reset(NULL);
   test_result("NULL reset doesn't crash", 1);
}

int main() {
   printf("\n=== Silero VAD API Unit Tests ===\n\n");

   // Run tests
   test_init_invalid_path();
   silero_vad_context_t *ctx = test_init_valid_path();

   if (ctx) {
      test_process_silence(ctx);
      test_process_speech(ctx);
      test_invalid_input(ctx);
      test_state_reset(ctx);
      test_sequential_inference(ctx);

      // Cleanup
      vad_silero_cleanup(ctx);
      test_result("Cleanup succeeds", 1);
   }

   test_cleanup();

   // Summary
   printf("\n=== Test Summary ===\n");
   printf("Passed: %d\n", tests_passed);
   printf("Failed: %d\n", tests_failed);
   printf("Total:  %d\n", tests_passed + tests_failed);

   if (tests_failed == 0) {
      printf("\n\033[32mAll tests passed!\033[0m\n\n");
      return 0;
   } else {
      printf("\n\033[31m%d test(s) failed.\033[0m\n\n", tests_failed);
      return 1;
   }
}
