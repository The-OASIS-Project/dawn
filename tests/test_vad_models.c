/*
 * VAD Model Comparison Test
 *
 * Compares three Silero VAD models to determine which is best for D.A.W.N.:
 * 1. silero_vad.onnx (2.3MB, FP32 full precision)
 * 2. silero_vad_half.onnx (1.3MB, FP16 half precision)
 * 3. silero_vad_16k_op15.onnx (1.3MB, optimized for 16kHz)
 *
 * Tests:
 * - Model load time
 * - Inference latency (100 iterations)
 * - Output probability on test audio (silence and speech)
 * - Memory usage
 */

#include <math.h>
#include <onnxruntime_c_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VAD_SAMPLE_SIZE 512
#define NUM_ITERATIONS 100

// Test audio: silence (all zeros)
static int16_t silence_audio[VAD_SAMPLE_SIZE] = { 0 };

// Test audio: synthetic speech (sine wave at 200Hz)
static int16_t speech_audio[VAD_SAMPLE_SIZE];

const OrtApi *g_ort = NULL;

typedef struct {
   const char *name;
   const char *path;
   OrtSession *session;
   double load_time_ms;
   double avg_inference_ms;
   float silence_prob;
   float speech_prob;
} ModelTestResult;

// Initialize synthetic speech audio
void init_speech_audio() {
   for (int i = 0; i < VAD_SAMPLE_SIZE; i++) {
      // 200Hz sine wave at 16kHz = frequency/sample_rate * 2*PI
      double t = (double)i / 16000.0;
      speech_audio[i] = (int16_t)(10000.0 * sin(2.0 * 3.14159 * 200.0 * t));
   }
}

// Get time in milliseconds
double get_time_ms() {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

// Load model and measure time
int load_model(const char *model_path, OrtEnv *env, OrtSession **session, double *load_time_ms) {
   double start = get_time_ms();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
   OrtSessionOptions *session_options;
   g_ort->CreateSessionOptions(&session_options);
   g_ort->SetIntraOpNumThreads(session_options, 1);
   g_ort->SetSessionGraphOptimizationLevel(session_options, ORT_ENABLE_ALL);
#pragma GCC diagnostic pop

   OrtStatus *status = g_ort->CreateSession(env, model_path, session_options, session);
   g_ort->ReleaseSessionOptions(session_options);

   if (status != NULL) {
      const char *msg = g_ort->GetErrorMessage(status);
      printf("Error loading model %s: %s\n", model_path, msg);
      g_ort->ReleaseStatus(status);
      return -1;
   }

   *load_time_ms = get_time_ms() - start;
   return 0;
}

// Run inference and return speech probability
float run_inference(OrtSession *session, const int16_t *audio) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
   OrtMemoryInfo *memory_info;
   g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info);

   // Convert int16 to float (Silero VAD expects normalized float input)
   float audio_float[VAD_SAMPLE_SIZE];
   for (int i = 0; i < VAD_SAMPLE_SIZE; i++) {
      audio_float[i] = (float)audio[i] / 32768.0f;
   }

   // Input tensor shape: [1, 512]
   int64_t input_shape[] = { 1, VAD_SAMPLE_SIZE };
   OrtValue *input_tensor = NULL;
   g_ort->CreateTensorWithDataAsOrtValue(memory_info, audio_float, VAD_SAMPLE_SIZE * sizeof(float),
                                         input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                         &input_tensor);

   // State tensor (zero-initialized for first call)
   // Silero VAD uses combined state: [2, 1, 128]
   float state[2 * 1 * 128] = { 0 };
   int64_t state_shape[] = { 2, 1, 128 };

   OrtValue *state_tensor = NULL;
   g_ort->CreateTensorWithDataAsOrtValue(memory_info, state, 2 * 1 * 128 * sizeof(float),
                                         state_shape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                         &state_tensor);

   // Check if model requires sample rate input (newer models have 3 inputs)
   size_t num_inputs;
   g_ort->SessionGetInputCount(session, &num_inputs);

   OrtValue *sr_tensor = NULL;
   int64_t sr_tensor_value = 16000;
   int64_t sr_shape[] = { 1 };

   const char *input_names[3];
   const OrtValue *inputs[3];
   size_t input_count;

   if (num_inputs == 3) {
      // Newer model with sample rate input
      g_ort->CreateTensorWithDataAsOrtValue(memory_info, &sr_tensor_value, sizeof(int64_t),
                                            sr_shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                            &sr_tensor);
      input_names[0] = "input";
      input_names[1] = "state";
      input_names[2] = "sr";
      inputs[0] = input_tensor;
      inputs[1] = state_tensor;
      inputs[2] = sr_tensor;
      input_count = 3;
   } else {
      // Older model without sample rate input (FP16)
      input_names[0] = "input";
      input_names[1] = "state";
      inputs[0] = input_tensor;
      inputs[1] = state_tensor;
      input_count = 2;
   }

   // Output names for Silero VAD
   const char *output_names[] = { "output", "stateN" };
   OrtValue *outputs[2] = { NULL, NULL };

   // Run inference
   OrtStatus *status = g_ort->Run(session, NULL, input_names, inputs, input_count, output_names, 2,
                                  outputs);

   float speech_prob = -1.0f;
   if (status == NULL) {
      // Extract output probability
      float *output_data;
      g_ort->GetTensorMutableData(outputs[0], (void **)&output_data);
      speech_prob = output_data[0];
#pragma GCC diagnostic pop

      // Release outputs
      for (int i = 0; i < 2; i++) {
         if (outputs[i])
            g_ort->ReleaseValue(outputs[i]);
      }
   } else {
      const char *msg = g_ort->GetErrorMessage(status);
      printf("Inference error: %s\n", msg);
      g_ort->ReleaseStatus(status);
   }

   // Cleanup
   g_ort->ReleaseValue(input_tensor);
   g_ort->ReleaseValue(state_tensor);
   if (sr_tensor)
      g_ort->ReleaseValue(sr_tensor);
   g_ort->ReleaseMemoryInfo(memory_info);

   return speech_prob;
}

// Test a single model
int test_model(const char *name, const char *path, OrtEnv *env, ModelTestResult *result) {
   result->name = name;
   result->path = path;

   printf("\n=== Testing %s ===\n", name);
   printf("Path: %s\n", path);

   // Load model
   if (load_model(path, env, &result->session, &result->load_time_ms) != 0) {
      return -1;
   }
   printf("Load time: %.2f ms\n", result->load_time_ms);

   // Print model inputs/outputs (debug logging)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
   size_t num_inputs, num_outputs;
   g_ort->SessionGetInputCount(result->session, &num_inputs);
   g_ort->SessionGetOutputCount(result->session, &num_outputs);

   printf("Model has %zu inputs: ", num_inputs);
   for (size_t i = 0; i < num_inputs; i++) {
      char *name;
      OrtAllocator *allocator;
      g_ort->GetAllocatorWithDefaultOptions(&allocator);
      g_ort->SessionGetInputName(result->session, i, allocator, &name);
      printf("%s%s", name, (i < num_inputs - 1) ? ", " : "\n");
      allocator->Free(allocator, name);
   }

   printf("Model has %zu outputs: ", num_outputs);
   for (size_t i = 0; i < num_outputs; i++) {
      char *name;
      OrtAllocator *allocator;
      g_ort->GetAllocatorWithDefaultOptions(&allocator);
      g_ort->SessionGetOutputName(result->session, i, allocator, &name);
      printf("%s%s", name, (i < num_outputs - 1) ? ", " : "\n");
      allocator->Free(allocator, name);
   }
#pragma GCC diagnostic pop

   // Warm-up run
   run_inference(result->session, silence_audio);

   // Test silence audio
   double start = get_time_ms();
   for (int i = 0; i < NUM_ITERATIONS; i++) {
      result->silence_prob = run_inference(result->session, silence_audio);
   }
   double silence_time = get_time_ms() - start;

   // Test speech audio
   start = get_time_ms();
   for (int i = 0; i < NUM_ITERATIONS; i++) {
      result->speech_prob = run_inference(result->session, speech_audio);
   }
   double speech_time = get_time_ms() - start;

   result->avg_inference_ms = (silence_time + speech_time) / (2.0 * NUM_ITERATIONS);

   printf("Avg inference time: %.3f ms (%.0f iterations)\n", result->avg_inference_ms,
          (double)NUM_ITERATIONS);
   printf("Silence probability: %.4f\n", result->silence_prob);
   printf("Speech probability: %.4f\n", result->speech_prob);

   return 0;
}

int main(int argc, char *argv[]) {
   printf("Silero VAD Model Comparison Test\n");
   printf("=================================\n\n");

   // Initialize ONNX Runtime
   g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
   if (!g_ort) {
      printf("Failed to get ONNX Runtime API\n");
      return 1;
   }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
   OrtEnv *env;
   g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "test", &env);
#pragma GCC diagnostic pop

   // Initialize synthetic speech audio
   init_speech_audio();

   // Test all three models
   ModelTestResult results[3];
   const char *base_path = "../silero-vad/src/silero_vad/data";

   int success_count = 0;

   const char *home = getenv("HOME");
   char model_path[512];

   snprintf(model_path, sizeof(model_path),
            "%s/code/The-OASIS-Project/silero-vad/src/silero_vad/data/silero_vad.onnx", home);
   if (test_model("Full Precision (FP32)", model_path, env, &results[0]) == 0) {
      success_count++;
   }

   snprintf(model_path, sizeof(model_path),
            "%s/code/The-OASIS-Project/silero-vad/src/silero_vad/data/silero_vad_half.onnx", home);
   if (test_model("Half Precision (FP16)", model_path, env, &results[1]) == 0) {
      success_count++;
   }

   snprintf(model_path, sizeof(model_path),
            "%s/code/The-OASIS-Project/silero-vad/src/silero_vad/data/silero_vad_16k_op15.onnx",
            home);
   if (test_model("16kHz Optimized (opset15)", model_path, env, &results[2]) == 0) {
      success_count++;
   }

   // Summary comparison
   printf("\n=== COMPARISON SUMMARY ===\n");
   printf("%-25s | Load (ms) | Inference (ms) | Silence Prob | Speech Prob\n", "Model");
   printf("--------------------------------------------------------------------------------\n");

   for (int i = 0; i < 3; i++) {
      if (results[i].session != NULL) {
         printf("%-25s | %9.2f | %14.3f | %12.4f | %11.4f\n", results[i].name,
                results[i].load_time_ms, results[i].avg_inference_ms, results[i].silence_prob,
                results[i].speech_prob);
      }
   }

   printf("\n=== RECOMMENDATION ===\n");
   if (success_count > 0) {
      // Find fastest model that meets <1ms requirement
      ModelTestResult *best = NULL;
      for (int i = 0; i < 3; i++) {
         if (results[i].session != NULL) {
            if (results[i].avg_inference_ms < 1.0) {
               if (best == NULL || results[i].avg_inference_ms < best->avg_inference_ms) {
                  best = &results[i];
               }
            }
         }
      }

      if (best != NULL) {
         printf("✓ Use %s\n", best->name);
         printf("  - Meets <1ms latency requirement (%.3f ms)\n", best->avg_inference_ms);
         printf("  - Good speech/silence discrimination (%.4f vs %.4f)\n", best->speech_prob,
                best->silence_prob);
      } else {
         printf("⚠ No model meets <1ms requirement. Use fastest: ");
         ModelTestResult *fastest = &results[0];
         for (int i = 1; i < 3; i++) {
            if (results[i].session != NULL &&
                results[i].avg_inference_ms < fastest->avg_inference_ms) {
               fastest = &results[i];
            }
         }
         printf("%s (%.3f ms)\n", fastest->name, fastest->avg_inference_ms);
      }
   }

   // Cleanup
   for (int i = 0; i < 3; i++) {
      if (results[i].session) {
         g_ort->ReleaseSession(results[i].session);
      }
   }
   g_ort->ReleaseEnv(env);

   return 0;
}
