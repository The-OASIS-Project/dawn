#include "tts/text_to_speech.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <queue>
#include <sstream>
#include <vector>

#include "audio/audio_backend.h"
#include "audio/audio_capture_thread.h"
#include "audio/audio_converter.h"
#include "config/dawn_config.h"
#include "dawn.h"
#include "logging.h"
#include "ui/metrics.h"

#ifdef ENABLE_AEC
#include "audio/aec_calibration.h"
#include "audio/aec_processor.h"
#include "audio/resampler.h"

// Separate resamplers for thread safety:
// - g_tts_thread_resampler: Used by TTS playback thread to feed AEC reference
//
// NOTE: Network WAV generation (text_to_speech_to_wav) does NOT need to feed AEC reference
// because network clients (ESP32) play audio on THEIR speaker, not DAWN's local speaker.
// DAWN's microphone won't hear ESP32's output, so there's no echo to cancel.
//
// IMPORTANT: These must NOT be shared between threads - resampler_t is not thread-safe!
static resampler_t *g_tts_thread_resampler = nullptr;

// Pre-allocated resample buffer (one per resampler)
static int16_t g_tts_resample_buffer[RESAMPLER_MAX_SAMPLES];

// Rate-limited warning for resampler overflow (warn once per 60 seconds)
static time_t g_last_resample_warning = 0;

// Atomic sequence counter for detecting discard during unlocked audio write (TOCTOU protection).
// Incremented each time TTS is discarded. Used to detect if discard happened between
// releasing mutex and completing audio write.
static std::atomic<uint32_t> g_tts_discard_sequence{ 0 };
#endif
#include "network/dawn_wav_utils.h"
#include "text_to_command_nuevo.h"
#include "tts/piper.hpp"
#include "tts/tts_preprocessing.h"
#include "ui/metrics.h"

#define DEFAULT_RATE 22050
#define DEFAULT_CHANNELS 1
#define DEFAULT_FRAMES_PER_PERIOD 512

// Maximum conversion buffer size (1 second at 48kHz stereo)
// TTS processes audio in chunks, so we don't need to buffer full utterances.
// 1 second provides adequate margin for chunk processing.
#define TTS_CONV_BUFFER_SIZE (48000 * 2 * 1)

// Include samplerate for audio conversion (runtime, not compile-time conditional)
#include <samplerate.h>

using namespace std;
using namespace piper;

typedef struct {
   PiperConfig config;
   Voice voice;
   int is_initialized;
   char pcm_playback_device[MAX_WORD_LENGTH + 1];
   // Runtime audio backend handle (replaces compile-time ALSA/PA selection)
   audio_stream_playback_handle_t *playback_handle;
   audio_hw_params_t hw_params;
   size_t period_frames;
   // Actual hardware parameters (may differ from defaults)
   audio_sample_format_t hw_format;
   unsigned int hw_rate;
   unsigned int hw_channels;
   // Conversion state (used when hardware doesn't match TTS output format)
   bool needs_conversion;
   SRC_STATE *resampler;
   double resample_ratio;
   // Conversion buffers
   float *resample_in;
   float *resample_out;
   uint8_t *convert_out;
   size_t convert_out_size;
} TTS_Handle;

// Global TTS_Handle object
static TTS_Handle tts_handle;

/**
 * Convert S16_LE mono samples to hardware format
 * Handles: rate conversion, mono→stereo, S16→S24_3LE/S32_LE
 *
 * This function supports runtime audio backend selection.
 * Conversion is needed when hardware doesn't support the TTS native format.
 *
 * @param in_samples Input S16_LE mono samples at DEFAULT_RATE
 * @param num_in_samples Number of input samples
 * @param out_buffer Output buffer (must be pre-allocated)
 * @param out_buffer_size Size of output buffer in bytes
 * @return Number of frames written to output, or -1 on error
 */
static ssize_t tts_convert_audio(const int16_t *in_samples,
                                 size_t num_in_samples,
                                 uint8_t *out_buffer,
                                 size_t out_buffer_size) {
   if (!tts_handle.needs_conversion) {
      // No conversion needed - just copy
      size_t bytes = num_in_samples * sizeof(int16_t);
      if (bytes > out_buffer_size)
         bytes = out_buffer_size;
      memcpy(out_buffer, in_samples, bytes);
      return num_in_samples;
   }

   // Step 1: Convert S16 to float for resampler
   size_t max_in = num_in_samples;
   if (max_in > TTS_CONV_BUFFER_SIZE)
      max_in = TTS_CONV_BUFFER_SIZE;

   for (size_t i = 0; i < max_in; i++) {
      tts_handle.resample_in[i] = in_samples[i] / 32768.0f;
   }

   // Step 2: Resample if needed
   float *resampled_data = tts_handle.resample_in;
   size_t resampled_samples = max_in;

   if (tts_handle.resampler && tts_handle.resample_ratio != 1.0) {
      SRC_DATA src_data;
      src_data.data_in = tts_handle.resample_in;
      src_data.input_frames = (long)max_in;
      src_data.data_out = tts_handle.resample_out;
      src_data.output_frames = (long)(max_in * tts_handle.resample_ratio + 100);
      src_data.src_ratio = tts_handle.resample_ratio;
      src_data.end_of_input = 0;

      int err = src_process(tts_handle.resampler, &src_data);
      if (err) {
         LOG_ERROR("Resampler error: %s", src_strerror(err));
         return -1;
      }

      resampled_data = tts_handle.resample_out;
      resampled_samples = src_data.output_frames_gen;
   }

   // Step 3: Convert to hardware format (using audio_backend format enum)
   size_t out_frames = resampled_samples;
   size_t bytes_per_frame;

   if (tts_handle.hw_format == AUDIO_FORMAT_S24_3LE) {
      // 24-bit packed (3 bytes per sample)
      bytes_per_frame = 3 * tts_handle.hw_channels;
   } else if (tts_handle.hw_format == AUDIO_FORMAT_S32_LE) {
      bytes_per_frame = 4 * tts_handle.hw_channels;
   } else {
      // S16_LE (default)
      bytes_per_frame = 2 * tts_handle.hw_channels;
   }

   size_t needed_bytes = out_frames * bytes_per_frame;
   if (needed_bytes > out_buffer_size) {
      out_frames = out_buffer_size / bytes_per_frame;
   }

   uint8_t *out = out_buffer;

   for (size_t i = 0; i < out_frames; i++) {
      float sample = resampled_data[i];
      // Clamp
      if (sample > 1.0f)
         sample = 1.0f;
      if (sample < -1.0f)
         sample = -1.0f;

      if (tts_handle.hw_format == AUDIO_FORMAT_S24_3LE) {
         // Convert float to 24-bit signed
         int32_t s24 = (int32_t)(sample * 8388607.0f);
         // Write for each channel (mono→stereo duplication)
         for (unsigned int ch = 0; ch < tts_handle.hw_channels; ch++) {
            out[0] = (uint8_t)(s24 & 0xFF);
            out[1] = (uint8_t)((s24 >> 8) & 0xFF);
            out[2] = (uint8_t)((s24 >> 16) & 0xFF);
            out += 3;
         }
      } else if (tts_handle.hw_format == AUDIO_FORMAT_S32_LE) {
         int32_t s32 = (int32_t)(sample * 2147483647.0f);
         for (unsigned int ch = 0; ch < tts_handle.hw_channels; ch++) {
            memcpy(out, &s32, 4);
            out += 4;
         }
      } else {
         // S16_LE
         int16_t s16 = (int16_t)(sample * 32767.0f);
         for (unsigned int ch = 0; ch < tts_handle.hw_channels; ch++) {
            memcpy(out, &s16, 2);
            out += 2;
         }
      }
   }

   return (ssize_t)out_frames;
}

/**
 * @brief Open TTS playback device using runtime audio backend abstraction
 *
 * Opens the playback device using audio_backend API for runtime backend selection.
 * Configures audio conversion if hardware doesn't match TTS native format.
 *
 * @param pcm_device Device name (format depends on backend - ALSA or PulseAudio name)
 * @return 0 on success, 1 on failure
 */
static int openPlaybackDevice(const char *pcm_device) {
   LOG_INFO("TTS playback device: %s (backend: %s)", pcm_device,
            audio_backend_type_name(audio_backend_get_type()));

   // Check that audio backend is initialized
   if (audio_backend_get_type() == AUDIO_BACKEND_NONE) {
      LOG_ERROR("Audio backend not initialized. Call audio_backend_init() first.");
      return 1;
   }

   // Request configured output rate/channels for consistent quality and dmix compatibility
   // TTS generates 22050Hz mono audio which gets converted during playback
   // Benefits:
   //   - Consistent quality: high-quality libsamplerate conversion
   //   - dmix compatibility: ALSA dmix requires stereo at fixed rate
   //   - No hidden conversions: ALSA/Pulse pass-through at native rate
   unsigned int output_rate = audio_conv_get_output_rate();
   unsigned int output_channels = audio_conv_get_output_channels();
   audio_stream_params_t params = { .sample_rate = output_rate,
                                    .channels = output_channels,
                                    .format = AUDIO_FORMAT_S16_LE,
                                    .period_frames = 1024,
                                    .buffer_frames = 1024 * 4 };

   // Open playback stream using audio_backend abstraction
   tts_handle.playback_handle = audio_stream_playback_open(pcm_device, &params,
                                                           &tts_handle.hw_params);
   if (!tts_handle.playback_handle) {
      LOG_ERROR("Failed to open playback device: %s", pcm_device);
      return 1;
   }

   // Store hardware parameters
   tts_handle.hw_format = tts_handle.hw_params.format;
   tts_handle.hw_rate = tts_handle.hw_params.sample_rate;
   tts_handle.hw_channels = tts_handle.hw_params.channels;
   tts_handle.period_frames = tts_handle.hw_params.period_frames;

   LOG_INFO("TTS playback: rate=%u ch=%u period=%zu buffer=%zu", tts_handle.hw_rate,
            tts_handle.hw_channels, tts_handle.hw_params.period_frames,
            tts_handle.hw_params.buffer_frames);

   // Determine if conversion is needed (format, rate, or channels differ from TTS output)
   tts_handle.needs_conversion = (tts_handle.hw_format != AUDIO_FORMAT_S16_LE) ||
                                 (tts_handle.hw_channels != DEFAULT_CHANNELS) ||
                                 (tts_handle.hw_rate != DEFAULT_RATE);

   if (tts_handle.needs_conversion) {
      const char *format_name;
      switch (tts_handle.hw_format) {
         case AUDIO_FORMAT_S24_3LE:
            format_name = "S24_3LE";
            break;
         case AUDIO_FORMAT_S32_LE:
            format_name = "S32_LE";
            break;
         case AUDIO_FORMAT_FLOAT32:
            format_name = "FLOAT32";
            break;
         default:
            format_name = "S16_LE";
            break;
      }

      LOG_INFO("Audio conversion enabled: %dHz/%dch/S16 → %uHz/%uch/%s", DEFAULT_RATE,
               DEFAULT_CHANNELS, tts_handle.hw_rate, tts_handle.hw_channels, format_name);

      // Create resampler if rate differs
      tts_handle.resample_ratio = (double)tts_handle.hw_rate / (double)DEFAULT_RATE;
      if (tts_handle.hw_rate != DEFAULT_RATE) {
         int error;
         tts_handle.resampler = src_new(SRC_SINC_FASTEST, 1, &error);
         if (!tts_handle.resampler) {
            LOG_ERROR("Failed to create resampler: %s", src_strerror(error));
            audio_stream_playback_close(tts_handle.playback_handle);
            tts_handle.playback_handle = NULL;
            return 1;
         }
         LOG_INFO("Resampler created: ratio=%.4f", tts_handle.resample_ratio);
      } else {
         tts_handle.resampler = NULL;
      }

      // Allocate conversion buffers
      tts_handle.resample_in = (float *)malloc(TTS_CONV_BUFFER_SIZE * sizeof(float));
      tts_handle.resample_out = (float *)malloc(TTS_CONV_BUFFER_SIZE * sizeof(float));
      // Output buffer: worst case is S32 stereo at 2x rate
      tts_handle.convert_out_size = TTS_CONV_BUFFER_SIZE * 8;
      tts_handle.convert_out = (uint8_t *)malloc(tts_handle.convert_out_size);

      if (!tts_handle.resample_in || !tts_handle.resample_out || !tts_handle.convert_out) {
         LOG_ERROR("Failed to allocate conversion buffers");
         audio_stream_playback_close(tts_handle.playback_handle);
         tts_handle.playback_handle = NULL;
         return 1;
      }
   } else {
      tts_handle.resampler = NULL;
      tts_handle.resample_in = NULL;
      tts_handle.resample_out = NULL;
      tts_handle.convert_out = NULL;
      LOG_INFO("No audio conversion needed");
   }

   return 0;
}

/**
 * @brief Thread-safe queue for text-to-speech requests.
 */
std::queue<std::string> tts_queue;

/**
 * @brief Mutex for synchronizing access to the text-to-speech queue.
 */
pthread_mutex_t tts_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Condition variable to signal the worker thread when new data is available.
 */
pthread_cond_t tts_queue_cond = PTHREAD_COND_INITIALIZER;

/**
 * @brief Thread handle for the text-to-speech worker thread.
 */
pthread_t tts_thread;

/**
 * @brief Flag indicating whether the worker thread should continue running.
 */
bool tts_thread_running = false;

/**
 * @brief TTS playback state (mutex-protected)
 *
 * All accesses must be protected by tts_mutex to ensure thread safety.
 * Changed from std::atomic to plain int to avoid C/C++ boundary issues.
 */
int tts_playback_state = TTS_PLAYBACK_IDLE;

extern "C" {
/**
 * @brief Worker thread function that processes text-to-speech requests.
 *
 * This function runs in a separate thread and continuously processes text strings
 * from the queue, converting them to speech and playing them back using the audio
 * system.
 *
 * @param arg Unused parameter.
 * @return Always returns NULL.
 */
void *tts_thread_function(void *arg) {
   LOG_INFO("tts_thread_function() started.");

   // Declare the interruption flag
   std::atomic<bool> tts_stop_processing(false);

   while (!get_quit()) {
      pthread_mutex_lock(&tts_queue_mutex);

      LOG_INFO("Waiting on text...");
      // Wait until there's text to process or the thread is signaled to exit
      while (tts_queue.empty() && tts_thread_running) {
         pthread_cond_wait(&tts_queue_cond, &tts_queue_mutex);
      }

      // Exit if the thread is no longer running and the queue is empty
      if (!tts_thread_running && tts_queue.empty()) {
         pthread_mutex_unlock(&tts_queue_mutex);
         break;
      }

      // Retrieve text from the queue
      std::string inputText = tts_queue.front();
      tts_queue.pop();
      pthread_mutex_unlock(&tts_queue_mutex);

      // Process text-to-speech
      SynthesisResult result;
      std::vector<int16_t> audioBuffer;
      int rc = 0;
      int error = 0;

      // Reset the interruption flag
      tts_stop_processing.store(false);

      // Convert text to audio data
      textToAudio(
          tts_handle.config, tts_handle.voice, inputText, audioBuffer, result, tts_stop_processing,
          [&]() {
             pthread_mutex_lock(&tts_mutex);
             tts_playback_state = TTS_PLAYBACK_PLAY;
#ifdef ENABLE_AEC
             // Start calibration capture if pending (checked via calibration module API)
             if (aec_cal_is_pending()) {
                aec_cal_start();
                LOG_INFO("AEC calibration: started capture for greeting");
             }
             // Start AEC recording for this TTS session (if enabled)
             if (aec_is_recording_enabled()) {
                aec_start_recording();
             }
#endif
             // Start mic recording for this TTS session (if enabled)
             if (mic_is_recording_enabled()) {
                mic_start_recording();
             }
             pthread_mutex_unlock(&tts_mutex);

             // Play audio data using unified audio backend API
             const size_t chunk_frames = tts_handle.period_frames;
             for (size_t i = 0; i < audioBuffer.size(); i += chunk_frames) {
                // Check playback state
                pthread_mutex_lock(&tts_mutex);
                bool was_paused = false;
                while (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                   if (!was_paused) {
                      LOG_INFO("TTS playback is PAUSED.");
#ifdef ENABLE_AEC
                      // Signal AEC that playback is paused - stop underflow counting
                      aec_signal_playback_stop();
#endif
                      was_paused = true;
                   }
                   pthread_cond_wait(&tts_cond, &tts_mutex);
                }

                // Only log state transitions after being paused
                if (was_paused) {
                   if (tts_playback_state == TTS_PLAYBACK_DISCARD) {
                      LOG_WARNING("TTS unpaused to DISCARD.");
                      tts_playback_state = TTS_PLAYBACK_IDLE;
                      audioBuffer.clear();
                      LOG_WARNING("Emptying TTS queue.");
                      while (!tts_queue.empty()) {
                         tts_queue.pop();
                      }

#ifdef ENABLE_AEC
                      // Increment sequence counter to signal any in-flight playback
                      g_tts_discard_sequence.fetch_add(1, std::memory_order_release);

                      // Clear AEC reference buffer to prevent stale frames from
                      // contaminating future captures after discard
                      aec_reset();

                      // Stop AEC recording on discard (barge-in)
                      aec_stop_recording();
#endif
                      // Stop mic recording on discard (barge-in)
                      mic_stop_recording();

                      // Drop (flush) audio device buffer immediately to stop playback
                      audio_stream_playback_drop(tts_handle.playback_handle);
                      audio_stream_playback_recover(tts_handle.playback_handle, AUDIO_ERR_UNDERRUN);

                      pthread_mutex_unlock(&tts_mutex);

                      tts_stop_processing.store(true);
                      return;
                   } else if (tts_playback_state == TTS_PLAYBACK_PLAY) {
                      LOG_INFO("TTS unpaused to PLAY.");
                      // Prepare stream after unpause to avoid underrun on first write
                      audio_stream_playback_recover(tts_handle.playback_handle, AUDIO_ERR_UNDERRUN);
                   } else if (tts_playback_state == TTS_PLAYBACK_IDLE) {
                      LOG_WARNING("TTS unpaused to IDLE.");
                   } else {
                      LOG_ERROR("TTS unpaused to UNKNOWN.");
                   }
                }

#ifdef ENABLE_AEC
                // Capture sequence counter before releasing mutex (TOCTOU protection)
                uint32_t seq_before_write = g_tts_discard_sequence.load(std::memory_order_acquire);

                // Store info needed for AEC reference (will feed AFTER audio write)
                size_t frames_to_write = std::min(chunk_frames, audioBuffer.size() - i);
                bool should_feed_aec = (tts_playback_state == TTS_PLAYBACK_PLAY &&
                                        g_tts_thread_resampler && aec_is_enabled());
#endif
                pthread_mutex_unlock(&tts_mutex);

                // Write audio frames (blocking I/O - cannot hold mutex here)
                size_t samples_this_chunk = std::min(chunk_frames, audioBuffer.size() - i);
                ssize_t frames_written = 0;

                if (tts_handle.needs_conversion) {
                   // Convert audio to hardware format (rate/channels/bit-depth)
                   ssize_t converted_frames = tts_convert_audio(&audioBuffer[i], samples_this_chunk,
                                                                tts_handle.convert_out,
                                                                tts_handle.convert_out_size);

                   if (converted_frames > 0) {
                      frames_written = audio_stream_playback_write(tts_handle.playback_handle,
                                                                   tts_handle.convert_out,
                                                                   (size_t)converted_frames);
                   } else {
                      frames_written = -AUDIO_ERR_IO;
                      LOG_ERROR("Audio conversion failed");
                   }
                } else {
                   // No conversion needed - write directly
                   frames_written = audio_stream_playback_write(tts_handle.playback_handle,
                                                                &audioBuffer[i],
                                                                samples_this_chunk);
                }

                // Handle errors with recovery
                if (frames_written < 0) {
                   int err = (int)(-frames_written);
                   if (err == AUDIO_ERR_UNDERRUN) {
                      LOG_ERROR("Audio underrun occurred");
                   } else {
                      LOG_ERROR("Audio write error: %s", audio_error_string((audio_error_t)err));
                   }
                   // Attempt recovery
                   audio_stream_playback_recover(tts_handle.playback_handle, err);
                }
                rc = (int)frames_written;  // For AEC compatibility

#ifdef ENABLE_AEC
                // Feed AEC reference AFTER audio write completes
                // For AEC timing, we use a fixed acoustic delay (speaker -> mic)
                if (should_feed_aec && frames_written > 0) {
                   // Acoustic delay: speaker output -> microphone input (tunable)
                   const uint64_t acoustic_delay_us = 50000;  // 50ms
                   uint64_t playback_delay_us = acoustic_delay_us;

                   // Use original 22050Hz sample count, NOT converted frame count
                   // When needs_conversion is true, frames_written is at hw_rate (e.g., 48kHz)
                   // but audioBuffer is at DEFAULT_RATE (22050Hz)
                   size_t in_samples = samples_this_chunk;

                   // Enforce maximum chunk size
                   if (in_samples <= RESAMPLER_MAX_SAMPLES) {
                      size_t out_max = resampler_get_output_size(g_tts_thread_resampler,
                                                                 in_samples);

                      if (out_max <= RESAMPLER_MAX_SAMPLES) {
                         size_t resampled = resampler_process(g_tts_thread_resampler,
                                                              &audioBuffer[i], in_samples,
                                                              g_tts_resample_buffer,
                                                              RESAMPLER_MAX_SAMPLES);

                         if (resampled > 0) {
                            // Use delay-aware version for proper PTS calculation
                            aec_add_reference_with_delay(g_tts_resample_buffer, resampled,
                                                         playback_delay_us);
                         }
                      } else {
                         // Rate-limited warning (once per 60 seconds)
                         time_t now = time(NULL);
                         if (now - g_last_resample_warning >= 60) {
                            LOG_WARNING(
                                "AEC resampler output too large (%zu > %d), skipping reference",
                                out_max, RESAMPLER_MAX_SAMPLES);
                            g_last_resample_warning = now;
                         }
                      }
                   }
                }

                // Check if discard happened during audio write (TOCTOU detection)
                if (g_tts_discard_sequence.load(std::memory_order_acquire) != seq_before_write) {
                   // Discard occurred while we were writing - audio device already flushed
                   LOG_INFO("TTS discarded during audio write - exiting playback");
                   tts_stop_processing.store(true);
                   return;
                }
                (void)frames_to_write;  // Suppress unused warning when AEC enabled
#endif
             }

             // Drain audio buffer to ensure all audio is played before returning
             if (audio_stream_playback_drain(tts_handle.playback_handle) != AUDIO_SUCCESS) {
                LOG_ERROR("Audio drain error");
             }
             // Clear the audio buffer for the next request
             audioBuffer.clear();

             pthread_mutex_lock(&tts_mutex);
             tts_playback_state = TTS_PLAYBACK_IDLE;
#ifdef ENABLE_AEC
             // Finish calibration if it was pending (atomically clear flag)
             if (aec_cal_check_and_clear_pending()) {
                int measured_delay_ms = 0;
                int cal_result = aec_cal_finish(&measured_delay_ms);
                if (cal_result == AEC_CAL_SUCCESS) {
                   float correlation = aec_cal_get_last_correlation();
                   LOG_INFO("AEC calibration: SUCCESS - measured delay = %d ms", measured_delay_ms);
                   aec_set_delay_hint(measured_delay_ms);
                   metrics_record_aec_calibration(true, measured_delay_ms, correlation);
                   metrics_update_aec_enabled(true);  // AEC confirmed working
                } else {
                   LOG_WARNING("AEC calibration: failed (error %d), using default delay",
                               cal_result);
                   metrics_record_aec_calibration(false, 0, 0.0f);
                }
             }

             // Signal AEC that playback has stopped - this prevents
             // underflow counting when no audio is playing
             aec_signal_playback_stop();

             // Stop AEC recording after TTS playback completes
             aec_stop_recording();
#endif
             // Stop mic recording after TTS playback completes
             mic_stop_recording();
             pthread_mutex_unlock(&tts_mutex);
          });

      // Record TTS timing metrics after synthesis completes
      if (result.inferSeconds > 0 && !tts_stop_processing.load()) {
         double tts_time_ms = result.inferSeconds * 1000.0;
         metrics_record_tts_timing(tts_time_ms);
      }

      // Check if queue is empty and LLM is not streaming - signal done speaking
      pthread_mutex_lock(&tts_queue_mutex);
      bool queue_empty = tts_queue.empty();
      pthread_mutex_unlock(&tts_queue_mutex);
      if (queue_empty && !tts_stop_processing.load() && !is_llm_processing()) {
         LOG_INFO("TTS complete - done speaking");
      }

      tts_stop_processing.store(false);
   }

   return NULL;
}

/**
 * @brief Initializes the text-to-speech system.
 *
 * This function loads the voice model, initializes the TTS engine, opens the
 * audio playback device, and starts the worker thread that processes TTS requests.
 *
 * @param pcm_device The name of the PCM device to use for audio playback.
 */
void initialize_text_to_speech(char *pcm_device) {
   // Check if already initialized
   if (tts_handle.is_initialized) {
      LOG_WARNING("Text-to-Speech system already initialized");
      return;
   }

   // Validate input
   if (!pcm_device) {
      LOG_ERROR("Invalid PCM device parameter");
      return;
   }

   LOG_INFO("Initializing Text-to-Speech system...");

   // Store device name (with bounds checking)
   strncpy(tts_handle.pcm_playback_device, pcm_device, MAX_WORD_LENGTH);
   tts_handle.pcm_playback_device[MAX_WORD_LENGTH] = '\0';

   // Load the voice model from models/ directory
   // Construct paths from config voice_model name: ../models/{voice_model}.onnx and .json
   char model_path[512], model_json_path[512];
   snprintf(model_path, sizeof(model_path), "../models/%s.onnx", g_config.tts.voice_model);
   snprintf(model_json_path, sizeof(model_json_path), "../models/%s.onnx.json",
            g_config.tts.voice_model);

   std::optional<SpeakerId> speakerIdOpt = 0;
   try {
      loadVoice(tts_handle.config, model_path, model_json_path, tts_handle.voice, speakerIdOpt,
                false);
      LOG_INFO("Loaded TTS voice model: %s", g_config.tts.voice_model);
   } catch (const std::exception &e) {
      LOG_ERROR("Failed to load voice model '%s': %s", g_config.tts.voice_model, e.what());
      return;
   }

   // Initialize the TTS engine
   try {
      initialize(tts_handle.config);
   } catch (const std::exception &e) {
      LOG_ERROR("Failed to initialize TTS engine: %s", e.what());
      return;
   }

   // Configure synthesis parameters from config
   tts_handle.voice.synthesisConfig.lengthScale = g_config.tts.length_scale;

   // Initialize synchronization primitives
   pthread_mutex_init(&tts_queue_mutex, NULL);
   pthread_cond_init(&tts_queue_cond, NULL);

   // Open the audio playback device using runtime audio backend abstraction
   int rc = openPlaybackDevice(tts_handle.pcm_playback_device);
   if (rc) {
      LOG_ERROR("Error creating audio playback device");
      // Cleanup Piper
      terminate(tts_handle.config);
      // Cleanup synchronization primitives
      pthread_mutex_destroy(&tts_queue_mutex);
      pthread_cond_destroy(&tts_queue_cond);
      return;
   }

   // Start the worker thread
   tts_thread_running = true;
   int thread_result = pthread_create(&tts_thread, NULL, tts_thread_function, NULL);
   if (thread_result != 0) {
      LOG_ERROR("Failed to create TTS worker thread: %s", strerror(thread_result));
      tts_thread_running = false;

      // Cleanup audio device using unified backend API
      if (tts_handle.playback_handle) {
         audio_stream_playback_close(tts_handle.playback_handle);
         tts_handle.playback_handle = NULL;
      }

      // Cleanup Piper
      terminate(tts_handle.config);

      // Cleanup synchronization primitives
      pthread_mutex_destroy(&tts_queue_mutex);
      pthread_cond_destroy(&tts_queue_cond);
      return;
   }

   // Only mark as initialized if everything succeeded
   tts_handle.is_initialized = 1;

#ifdef ENABLE_AEC
   // Create resampler for AEC reference (22050Hz TTS -> 48000Hz AEC native rate)
   g_tts_thread_resampler = resampler_create(DEFAULT_RATE, AEC_SAMPLE_RATE, 1);
   if (!g_tts_thread_resampler) {
      LOG_WARNING("Failed to create TTS resampler for AEC - echo cancellation may be limited");
   } else {
      LOG_INFO("TTS resampler initialized for AEC (%d -> %d Hz)", DEFAULT_RATE, AEC_SAMPLE_RATE);
   }
#endif

   LOG_INFO("Text-to-Speech system initialized successfully");
}

/**
 * @brief Enqueues a text string for conversion to speech.
 *
 * This function can be safely called from multiple threads. It adds the provided
 * text to a queue that is processed by a dedicated worker thread.
 *
 * @param text The text to be converted to speech.
 */
void text_to_speech(char *text) {
   if (!tts_handle.is_initialized) {
      LOG_ERROR("Text-to-Speech system not initialized. Call initialize_text_to_speech() first.");
      return;
   }

   assert(text != nullptr && "Received a null pointer");
   std::string inputText = preprocess_text_for_tts(std::string(text));

   // Add text to the processing queue
   pthread_mutex_lock(&tts_queue_mutex);
   tts_queue.push(inputText);
   pthread_cond_signal(&tts_queue_cond);
   pthread_mutex_unlock(&tts_queue_mutex);
}

// Raw PCM generation using existing TTS system (base function)
int text_to_speech_to_pcm(const char *text,
                          int16_t **pcm_data_out,
                          size_t *pcm_samples_out,
                          uint32_t *sample_rate_out) {
   if (!text || !pcm_data_out || !pcm_samples_out) {
      LOG_ERROR("Invalid parameters for PCM generation");
      return -1;
   }

   if (!tts_handle.is_initialized) {
      LOG_ERROR("TTS not initialized for PCM generation");
      return -1;
   }

   // THREAD SAFETY: Lock the TTS mutex to prevent conflicts with local TTS
   pthread_mutex_lock(&tts_mutex);

   // Store original state (mutex already held)
   int original_state = tts_playback_state;

   try {
      LOG_INFO("Generating PCM audio: \"%s\"", text);

      // Pause local TTS to prevent conflicts (mutex already held)
      if (tts_playback_state == TTS_PLAYBACK_PLAY) {
         tts_playback_state = TTS_PLAYBACK_PAUSE;
         LOG_INFO("Paused local TTS for PCM generation");
      }

      // Preprocess text for better TTS output
      std::string processedText = preprocess_text_for_tts(std::string(text));

      // Use textToAudio to get raw PCM samples (no WAV header)
      std::vector<int16_t> audioBuffer;
      piper::SynthesisResult result;
      std::atomic<bool> stop_flag(false);

      // Generate PCM using shared TTS handle
      // Pass nullptr for callback - when a callback is provided, piper clears audioBuffer after
      // calling it (for streaming use). We want to keep the samples, so no callback.
      piper::textToAudio(tts_handle.config, tts_handle.voice, processedText, audioBuffer, result,
                         stop_flag, nullptr);

      // Record TTS timing metrics (inferSeconds is in seconds, convert to ms)
      double tts_time_ms = result.inferSeconds * 1000.0;
      metrics_record_tts_timing(tts_time_ms);
      LOG_INFO("TTS PCM synthesis completed: %.1f ms (RTF: %.3f)", tts_time_ms,
               result.realTimeFactor);

      // Restore local TTS state (mutex already held)
      tts_playback_state = original_state;
      if (original_state == TTS_PLAYBACK_PLAY) {
         pthread_cond_signal(&tts_cond);
         LOG_INFO("Resumed local TTS after PCM generation");
      }

      if (audioBuffer.empty()) {
         LOG_ERROR("Generated PCM data is empty");
         pthread_mutex_unlock(&tts_mutex);
         return -1;
      }

      // Allocate output buffer and copy samples
      *pcm_samples_out = audioBuffer.size();
      *pcm_data_out = (int16_t *)malloc(*pcm_samples_out * sizeof(int16_t));

      if (!*pcm_data_out) {
         LOG_ERROR("Failed to allocate PCM buffer (%zu samples)", *pcm_samples_out);
         pthread_mutex_unlock(&tts_mutex);
         return -1;
      }

      memcpy(*pcm_data_out, audioBuffer.data(), *pcm_samples_out * sizeof(int16_t));

      // Return sample rate if requested
      if (sample_rate_out) {
         *sample_rate_out = tts_handle.voice.synthesisConfig.sampleRate;
      }

      LOG_INFO("PCM generated: %zu samples at %d Hz", *pcm_samples_out,
               tts_handle.voice.synthesisConfig.sampleRate);

      pthread_mutex_unlock(&tts_mutex);
      return 0;

   } catch (const std::exception &e) {
      LOG_ERROR("TTS PCM generation failed: %s", e.what());

      // Restore TTS state on error (mutex already held)
      tts_playback_state = original_state;
      if (original_state == TTS_PLAYBACK_PLAY) {
         pthread_cond_signal(&tts_cond);
      }

      pthread_mutex_unlock(&tts_mutex);
      return -1;
   }
}

// Helper function to create WAV header
static void create_wav_header(WAVHeader *header,
                              uint32_t sample_rate,
                              uint16_t channels,
                              uint16_t bits_per_sample,
                              uint32_t data_size) {
   memcpy(header->riff_header, "RIFF", 4);
   header->wav_size = data_size + sizeof(WAVHeader) - 8;
   memcpy(header->wave_header, "WAVE", 4);
   memcpy(header->fmt_header, "fmt ", 4);
   header->fmt_chunk_size = 16;
   header->audio_format = 1;  // PCM
   header->num_channels = channels;
   header->sample_rate = sample_rate;
   header->byte_rate = sample_rate * channels * (bits_per_sample / 8);
   header->block_align = channels * (bits_per_sample / 8);
   header->bits_per_sample = bits_per_sample;
   memcpy(header->data_header, "data", 4);
   header->data_bytes = data_size;
}

// Network WAV generation - wraps text_to_speech_to_pcm with WAV header
int text_to_speech_to_wav(const char *text, uint8_t **wav_data_out, size_t *wav_size_out) {
   if (!text || !wav_data_out || !wav_size_out) {
      LOG_ERROR("Invalid parameters for WAV generation");
      return -1;
   }

   // Generate raw PCM samples
   int16_t *pcm_data = NULL;
   size_t pcm_samples = 0;
   uint32_t sample_rate = 0;

   int result = text_to_speech_to_pcm(text, &pcm_data, &pcm_samples, &sample_rate);
   if (result != 0 || !pcm_data || pcm_samples == 0) {
      LOG_ERROR("PCM generation failed for WAV wrapper");
      return -1;
   }

   // Calculate sizes
   size_t pcm_bytes = pcm_samples * sizeof(int16_t);
   size_t wav_size = sizeof(WAVHeader) + pcm_bytes;

   // Allocate WAV buffer
   uint8_t *wav_data = (uint8_t *)malloc(wav_size);
   if (!wav_data) {
      LOG_ERROR("Failed to allocate WAV buffer (%zu bytes)", wav_size);
      free(pcm_data);
      return -1;
   }

   // Create WAV header (16-bit mono at synthesized sample rate)
   WAVHeader header;
   create_wav_header(&header, sample_rate, 1, 16, (uint32_t)pcm_bytes);

   // Copy header and PCM data
   memcpy(wav_data, &header, sizeof(WAVHeader));
   memcpy(wav_data + sizeof(WAVHeader), pcm_data, pcm_bytes);

   // Free PCM buffer (now copied into WAV)
   free(pcm_data);

   *wav_data_out = wav_data;
   *wav_size_out = wav_size;

   LOG_INFO("WAV generated: %zu bytes (header: %zu, PCM: %zu)", wav_size, sizeof(WAVHeader),
            pcm_bytes);

   return 0;
}

uint8_t *error_to_wav(const char *error_message, size_t *tts_size_out) {
   uint8_t *tts_wav_data = NULL;
   size_t tts_wav_size = 0;

   LOG_INFO("Generating error TTS: \"%s\"", error_message);

   int tts_result = text_to_speech_to_wav(error_message, &tts_wav_data, &tts_wav_size);
   if (tts_result == 0 && tts_wav_data && tts_wav_size > 0) {
      // Apply same size/truncation logic as normal TTS
      if (!check_response_size_limit(tts_wav_size)) {
         uint8_t *truncated_data = NULL;
         size_t truncated_size = 0;

         if (truncate_wav_response(tts_wav_data, tts_wav_size, &truncated_data, &truncated_size) ==
             0) {
            if (truncated_data != NULL) {
               // Actual truncation occurred
               free(tts_wav_data);
               *tts_size_out = truncated_size;
               return truncated_data;
            }
            // Edge case: truncation succeeded but returned NULL
            // This shouldn't happen since we verified it needs truncation
            LOG_ERROR("Truncation logic error: succeeded but no data returned");
         } else {
            // Truncation failed - free and return NULL
            free(tts_wav_data);
            LOG_ERROR("Failed to truncate error TTS");
            *tts_size_out = 0;
            return NULL;
         }
      } else {
         // Fits in buffer, return original
         *tts_size_out = tts_wav_size;
         return tts_wav_data;
      }
   }
   if (tts_wav_data != NULL) {
      free(tts_wav_data);
   }

   LOG_ERROR("Failed to generate error TTS WAV");
   *tts_size_out = 0;
   return NULL;
}

/**
 * @brief Wait for TTS queue to empty and playback to complete
 *
 * Blocks until all queued TTS has finished playing.
 */
int tts_wait_for_completion(int timeout_ms) {
   if (!tts_handle.is_initialized) {
      return -1;
   }

   struct timespec start_time;
   clock_gettime(CLOCK_MONOTONIC, &start_time);

   while (1) {
      // Check if queue is empty and playback is idle
      pthread_mutex_lock(&tts_queue_mutex);
      bool queue_empty = tts_queue.empty();
      pthread_mutex_unlock(&tts_queue_mutex);

      pthread_mutex_lock(&tts_mutex);
      bool playback_idle = (tts_playback_state == TTS_PLAYBACK_IDLE);
      pthread_mutex_unlock(&tts_mutex);

      if (queue_empty && playback_idle) {
         return 0;  // TTS complete
      }

      // Check timeout
      if (timeout_ms > 0) {
         struct timespec now;
         clock_gettime(CLOCK_MONOTONIC, &now);
         int elapsed_ms = (int)((now.tv_sec - start_time.tv_sec) * 1000 +
                                (now.tv_nsec - start_time.tv_nsec) / 1000000);
         if (elapsed_ms >= timeout_ms) {
            LOG_WARNING("TTS wait timeout after %d ms", elapsed_ms);
            return -1;
         }
      }

      // Sleep briefly before checking again
      usleep(50000);  // 50ms
   }
}

/**
 * @brief Speaks the greeting with AEC delay calibration
 *
 * This function plays the greeting TTS and uses it to calibrate the
 * acoustic delay for echo cancellation. The measured delay is used
 * to update the AEC delay hint for optimal performance.
 *
 * @param greeting The greeting text to speak
 */
void tts_speak_greeting_with_calibration(const char *greeting) {
   if (!greeting || !tts_handle.is_initialized) {
      if (greeting) {
         text_to_speech((char *)greeting);
      }
      return;
   }

#ifdef ENABLE_AEC
   // Initialize calibration system if not already done
   if (!aec_cal_is_initialized()) {
      int cal_init = aec_cal_init(AEC_SAMPLE_RATE, 200);  // Max 200ms delay search
      if (cal_init != AEC_CAL_SUCCESS) {
         LOG_WARNING("AEC calibration init failed (%d), speaking without calibration", cal_init);
         text_to_speech((char *)greeting);
         return;
      }
   }

   // Request calibration via calibration module API (decouples TTS from calibration state)
   aec_cal_set_pending();
   LOG_INFO("AEC calibration: queued for next TTS playback");
#endif

   // Queue the greeting for playback
   text_to_speech((char *)greeting);
}

/**
 * @brief Cleans up the text-to-speech system.
 *
 * This function signals the worker thread to terminate, waits for it to finish,
 * closes the audio playback device, and then releases all resources used by the TTS engine.
 */
void cleanup_text_to_speech() {
   if (!tts_handle.is_initialized) {
      LOG_ERROR("Text-to-Speech system not initialized. Call initialize_text_to_speech() first.");
      return;
   }

   tts_handle.is_initialized = 0;

   // Signal the worker thread to exit
   pthread_mutex_lock(&tts_queue_mutex);
   tts_thread_running = false;
   pthread_cond_signal(&tts_queue_cond);
   pthread_mutex_unlock(&tts_queue_mutex);

   // Wait for the worker thread to finish
   pthread_join(tts_thread, NULL);

   // Close the audio playback device using unified backend API
   if (tts_handle.playback_handle) {
      audio_stream_playback_close(tts_handle.playback_handle);
      tts_handle.playback_handle = NULL;
   }

   // Clean up conversion resources (always cleanup, not just ALSA)
   if (tts_handle.resampler) {
      src_delete(tts_handle.resampler);
      tts_handle.resampler = NULL;
   }
   if (tts_handle.resample_in) {
      free(tts_handle.resample_in);
      tts_handle.resample_in = NULL;
   }
   if (tts_handle.resample_out) {
      free(tts_handle.resample_out);
      tts_handle.resample_out = NULL;
   }
   if (tts_handle.convert_out) {
      free(tts_handle.convert_out);
      tts_handle.convert_out = NULL;
   }
   tts_handle.needs_conversion = false;

   // Destroy synchronization primitives
   pthread_mutex_destroy(&tts_queue_mutex);
   pthread_cond_destroy(&tts_queue_cond);

#ifdef ENABLE_AEC
   // Clean up AEC calibration
   aec_cal_cleanup();

   // Clean up AEC resampler
   if (g_tts_thread_resampler) {
      resampler_destroy(g_tts_thread_resampler);
      g_tts_thread_resampler = nullptr;
   }
   // Note: g_tts_resample_buffer is static, no free needed
#endif

   // Clean up the TTS engine
   terminate(tts_handle.config);
}
}
