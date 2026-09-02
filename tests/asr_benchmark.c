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
 */

/**
 * @file asr_benchmark.c
 * @brief ASR engine benchmarking and comparison tool
 *
 * Compares multiple ASR engines on the same audio input.
 * Useful for unit testing the ASR abstraction layer and collecting accuracy metrics.
 *
 * Usage:
 *   asr_benchmark <wav_file> [--engines vosk,whisper] [--csv] [--vosk-model path] [--whisper-model
 * path]
 */

#include <getopt.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "asr/asr_interface.h"
#include "logging.h"

/* The ASR engine records per-utterance timing into production telemetry
 * (src/ui/metrics.c). This benchmark does its own timing and does not want
 * metrics.c's transitive deps (cloud provider strings, etc.), so stub the hook. */
void metrics_record_asr_timing(double time_ms, double rtf);
void metrics_record_asr_timing(double time_ms, double rtf) {
   (void)time_ms;
   (void)rtf;
}

#define DEFAULT_VOSK_MODEL "model"
#define DEFAULT_WHISPER_MODEL_TINY "whisper.cpp/models/ggml-tiny.bin"
#define DEFAULT_WHISPER_MODEL_BASE "whisper.cpp/models/ggml-base.bin"
#define DEFAULT_WHISPER_MODEL_SMALL "whisper.cpp/models/ggml-small.bin"

/**
 * @brief Benchmark result for a single ASR engine
 */
typedef struct {
   asr_engine_type_t engine;
   const char *engine_name;
   const char *model_path;
   asr_result_t *result;
   double model_load_time_ms;     // One-time model loading
   double transcription_time_ms;  // Per-utterance processing
   double total_time_ms;          // Load + transcription
   double rtf;                    // Real-time factor (transcription only)
   int success;
} benchmark_result_t;

/**
 * @brief Load WAV file into int16_t PCM buffer
 */
static int16_t *load_wav_file(const char *filepath, size_t *out_samples, int *out_sample_rate) {
   SF_INFO sfinfo;
   memset(&sfinfo, 0, sizeof(sfinfo));

   SNDFILE *sndfile = sf_open(filepath, SFM_READ, &sfinfo);
   if (!sndfile) {
      OLOG_ERROR("Failed to open WAV file: %s", filepath);
      return NULL;
   }

   if (sfinfo.channels != 1) {
      OLOG_ERROR("Only mono audio supported (file has %d channels)", sfinfo.channels);
      sf_close(sndfile);
      return NULL;
   }

   *out_samples = sfinfo.frames;
   *out_sample_rate = sfinfo.samplerate;

   int16_t *buffer = (int16_t *)malloc(sfinfo.frames * sizeof(int16_t));
   if (!buffer) {
      OLOG_ERROR("Failed to allocate audio buffer");
      sf_close(sndfile);
      return NULL;
   }

   sf_count_t read = sf_read_short(sndfile, buffer, sfinfo.frames);
   if (read != sfinfo.frames) {
      OLOG_ERROR("Failed to read all samples (got %ld, expected %ld)", read, sfinfo.frames);
      free(buffer);
      sf_close(sndfile);
      return NULL;
   }

   sf_close(sndfile);
   OLOG_INFO("Loaded WAV: %s (%zu samples, %d Hz, %.2f seconds)", filepath, *out_samples,
             *out_sample_rate, (double)*out_samples / *out_sample_rate);
   return buffer;
}

/**
 * @brief Run ASR engine on audio and collect metrics
 */
static benchmark_result_t run_engine_benchmark(asr_engine_type_t engine,
                                               const char *model_path,
                                               const int16_t *audio,
                                               size_t samples,
                                               int sample_rate,
                                               int audio_ctx) {
   benchmark_result_t result = { 0 };
   result.engine = engine;
   result.engine_name = asr_engine_name(engine);
   result.model_path = model_path;

   struct timeval load_start, load_end, trans_start, trans_end;

   // Time model loading separately
   gettimeofday(&load_start, NULL);
   asr_context_t *ctx = asr_init(engine, model_path, sample_rate);
   gettimeofday(&load_end, NULL);

   if (!ctx) {
      OLOG_ERROR("%s: Failed to initialize", result.engine_name);
      result.success = 0;
      return result;
   }

   result.model_load_time_ms = (load_end.tv_sec - load_start.tv_sec) * 1000.0 +
                               (load_end.tv_usec - load_start.tv_usec) / 1000.0;

   // E1: override the encoder audio context (0=model default, >0=fixed, <0=auto).
   // Whisper-only; no-op for Vosk.
   asr_set_audio_ctx(ctx, audio_ctx);

   // Time transcription (process + finalize)
   gettimeofday(&trans_start, NULL);

   // Process audio (simulate streaming in chunks)
   const size_t chunk_size = sample_rate / 10;  // 100ms chunks
   size_t offset = 0;

   while (offset < samples) {
      size_t chunk = (offset + chunk_size > samples) ? (samples - offset) : chunk_size;

      asr_result_t *partial = asr_process_partial(ctx, audio + offset, chunk);
      if (partial) {
         asr_result_free(partial);
      }

      offset += chunk;
   }

   // Get final result
   result.result = asr_finalize(ctx);

   gettimeofday(&trans_end, NULL);
   result.transcription_time_ms = (trans_end.tv_sec - trans_start.tv_sec) * 1000.0 +
                                  (trans_end.tv_usec - trans_start.tv_usec) / 1000.0;

   result.total_time_ms = result.model_load_time_ms + result.transcription_time_ms;

   // Calculate RTF based on TRANSCRIPTION time only (production metric)
   double audio_duration_ms = (double)samples / sample_rate * 1000.0;
   result.rtf = result.transcription_time_ms / audio_duration_ms;

   result.success = (result.result != NULL);

   // Cleanup
   asr_cleanup(ctx);

   return result;
}

/**
 * @brief Print benchmark results in human-readable format
 */
static void print_results_table(benchmark_result_t *results,
                                int num_results,
                                const char *wav_file,
                                size_t samples,
                                int sample_rate) {
   double audio_duration = (double)samples / sample_rate;

   printf("\n");
   printf("================================================================================\n");
   printf("ASR Benchmark Results\n");
   printf("================================================================================\n");
   printf("Audio file:     %s\n", wav_file);
   printf("Duration:       %.2f seconds (%zu samples @ %d Hz)\n", audio_duration, samples,
          sample_rate);
   printf("--------------------------------------------------------------------------------\n");
   printf("%-10s %-15s %-8s %-10s %-10s %-8s %s\n", "Engine", "Model", "RTF", "Load(ms)",
          "Trans(ms)", "Conf", "Transcription");
   printf("--------------------------------------------------------------------------------\n");

   for (int i = 0; i < num_results; i++) {
      benchmark_result_t *r = &results[i];

      if (!r->success) {
         printf("%-10s %-15s %s\n", r->engine_name, r->model_path, "FAILED");
         continue;
      }

      // Extract model filename from path
      const char *model_name = strrchr(r->model_path, '/');
      model_name = model_name ? model_name + 1 : r->model_path;

      // Truncate model name if too long
      char model_short[16];
      snprintf(model_short, sizeof(model_short), "%s", model_name);

      printf("%-10s %-15s %7.3f  %9.1f  %9.1f  %6.2f  \"%s\"\n", r->engine_name, model_short,
             r->rtf, r->model_load_time_ms, r->transcription_time_ms,
             (r->result->confidence >= 0) ? r->result->confidence : 0.0,
             r->result->text ? r->result->text : "(null)");
   }

   printf("================================================================================\n\n");
}

/**
 * @brief Print benchmark results in CSV format
 */
static void print_results_csv(benchmark_result_t *results,
                              int num_results,
                              const char *wav_file,
                              size_t samples,
                              int sample_rate,
                              int audio_ctx) {
   double audio_duration = (double)samples / sample_rate;

   // CSV header (audio_ctx = the E1 override this run used: 0 default / >0 fixed / <0 auto)
   printf("wav_file,duration_sec,samples,sample_rate,engine,model,audio_ctx,success,rtf,"
          "load_time_ms,transcription_time_ms,confidence,transcription\n");

   for (int i = 0; i < num_results; i++) {
      benchmark_result_t *r = &results[i];

      printf("%s,%.2f,%zu,%d,%s,%s,%d,%d,%.3f,%.1f,%.1f,%.2f,\"%s\"\n", wav_file, audio_duration,
             samples, sample_rate, r->engine_name, r->model_path, audio_ctx, r->success, r->rtf,
             r->model_load_time_ms, r->transcription_time_ms,
             (r->result && r->result->confidence >= 0) ? r->result->confidence : -1.0,
             (r->result && r->result->text) ? r->result->text : "");
   }
}

/**
 * @brief Print usage information
 */
static void print_usage(const char *prog_name) {
   printf("Usage: %s <wav_file> [OPTIONS]\n", prog_name);
   printf("\n");
   printf("Benchmark ASR engines on audio file.\n");
   printf("\n");
   printf("Options:\n");
   printf("  --engines <list>       Comma-separated list of engines (vosk,whisper)\n");
   printf("                         Default: vosk,whisper\n");
   printf("  --vosk-model <path>    Path to Vosk model directory\n");
   printf("                         Default: %s\n", DEFAULT_VOSK_MODEL);
   printf("  --whisper-model <path> Path to Whisper .bin model file\n");
   printf("                         Default: %s\n", DEFAULT_WHISPER_MODEL_BASE);
   printf("  --audio-ctx <n>        Whisper encoder audio_ctx (E1): 0=default(1500),\n");
   printf("                         >0=fixed, <0=auto-scale to clip length. Default: 0\n");
   printf("  --csv                  Output results in CSV format\n");
   printf("  --help                 Show this help message\n");
   printf("\n");
   printf("Examples:\n");
   printf("  %s test.wav\n", prog_name);
   printf("  %s test.wav --engines vosk\n", prog_name);
   printf("  %s test.wav --csv > results.csv\n", prog_name);
   printf("  %s test.wav --whisper-model whisper.cpp/models/ggml-small.bin\n", prog_name);
   printf("\n");
}

int main(int argc, char **argv) {
   // Default options
   const char *wav_file = NULL;
   const char *engines_str = "vosk,whisper";
   const char *vosk_model = DEFAULT_VOSK_MODEL;
   const char *whisper_model = DEFAULT_WHISPER_MODEL_BASE;
   int csv_output = 0;
   int audio_ctx = 0;  // E1: 0=model default(1500), >0=fixed, <0=auto-scale by length

   // Parse command-line options
   static struct option long_options[] = { { "engines", required_argument, 0, 'e' },
                                           { "vosk-model", required_argument, 0, 'v' },
                                           { "whisper-model", required_argument, 0, 'w' },
                                           { "audio-ctx", required_argument, 0, 'a' },
                                           { "csv", no_argument, 0, 'c' },
                                           { "help", no_argument, 0, 'h' },
                                           { 0, 0, 0, 0 } };

   int opt;
   while ((opt = getopt_long(argc, argv, "e:v:w:a:ch", long_options, NULL)) != -1) {
      switch (opt) {
         case 'e':
            engines_str = optarg;
            break;
         case 'v':
            vosk_model = optarg;
            break;
         case 'w':
            whisper_model = optarg;
            break;
         case 'a':
            audio_ctx = atoi(optarg);
            /* Bound to the encoder's valid range so a fat-fingered CLI value
             * can't pass an out-of-range (or INT_MIN-negation-UB) audio_ctx. */
            if (audio_ctx < -1500) {
               audio_ctx = -1500;
            } else if (audio_ctx > 1500) {
               audio_ctx = 1500;
            }
            break;
         case 'c':
            csv_output = 1;
            break;
         case 'h':
            print_usage(argv[0]);
            return 0;
         default:
            print_usage(argv[0]);
            return 1;
      }
   }

   // Get WAV file argument
   if (optind >= argc) {
      fprintf(stderr, "Error: WAV file required\n\n");
      print_usage(argv[0]);
      return 1;
   }
   wav_file = argv[optind];

   // Parse engines list
   int num_engines = 0;
   asr_engine_type_t engines[2];
   const char *model_paths[2];

   char *engines_copy = strdup(engines_str);
   char *token = strtok(engines_copy, ",");
   while (token && num_engines < 2) {
      if (strcmp(token, "vosk") == 0) {
         engines[num_engines] = ASR_ENGINE_VOSK;
         model_paths[num_engines] = vosk_model;
         num_engines++;
      } else if (strcmp(token, "whisper") == 0) {
         engines[num_engines] = ASR_ENGINE_WHISPER;
         model_paths[num_engines] = whisper_model;
         num_engines++;
      } else {
         fprintf(stderr, "Error: Unknown engine '%s'\n", token);
         free(engines_copy);
         return 1;
      }
      token = strtok(NULL, ",");
   }
   free(engines_copy);

   if (num_engines == 0) {
      fprintf(stderr, "Error: No valid engines specified\n");
      return 1;
   }

   // Load WAV file
   size_t samples;
   int sample_rate;
   int16_t *audio = load_wav_file(wav_file, &samples, &sample_rate);
   if (!audio) {
      return 1;
   }

   // Run benchmarks
   benchmark_result_t results[2];
   for (int i = 0; i < num_engines; i++) {
      if (!csv_output) {
         OLOG_INFO("Running %s benchmark...", asr_engine_name(engines[i]));
      }
      results[i] = run_engine_benchmark(engines[i], model_paths[i], audio, samples, sample_rate,
                                        audio_ctx);
   }

   // Output results
   if (csv_output) {
      print_results_csv(results, num_engines, wav_file, samples, sample_rate, audio_ctx);
   } else {
      print_results_table(results, num_engines, wav_file, samples, sample_rate);
   }

   // Cleanup
   for (int i = 0; i < num_engines; i++) {
      if (results[i].result) {
         asr_result_free(results[i].result);
      }
   }
   free(audio);

   return 0;
}
