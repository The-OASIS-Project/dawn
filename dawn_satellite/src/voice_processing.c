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
 * Voice-Activated Processing Loop Implementation
 */

#include "voice_processing.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio_capture.h"
#include "audio_playback.h"
#include "satellite_config.h"
#include "satellite_state.h"
#include "ws_client.h"

/* Common library includes (only when local processing enabled) */
#ifdef HAVE_VAD_SILERO
#include "asr/vad_silero.h"
#endif

#ifdef HAVE_ASR_ENGINE
#include "asr/asr_engine.h"
#elif defined(HAVE_ASR_WHISPER)
#include "asr/asr_whisper.h"
#endif

#ifdef HAVE_TTS_PIPER
#include "tts/tts_piper.h"
#include "tts/tts_preprocessing.h"
#endif

/* These are always available from common library */
#ifdef HAVE_VAD_SILERO
#include "audio/ring_buffer.h"
#include "utils/sentence_buffer.h"
#endif

/* Shared logging (same format as daemon) */
#include "logging.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define VAD_FRAME_SAMPLES 512 /* 32ms at 16kHz */
#define SAMPLE_RATE 16000
#define WS_SERVICE_TIMEOUT_MS 0  /* Non-blocking WebSocket poll (0 = immediate return) */
#define AUDIO_WAIT_TIMEOUT_MS 32 /* ~32ms to match VAD frame rate */

/* VAD threshold for speech detection (consecutive frames) */
#define SPEECH_START_FRAMES 3 /* ~96ms of speech to trigger */
#define SPEECH_END_FRAMES 25  /* ~800ms of silence to end (configurable) */

/* Barge-in threshold (interrupt TTS playback) */
#define BARGEIN_THRESHOLD 0.7f

/* Maximum audio buffer (15s * 16kHz * 2 bytes = 480KB) */
#define MAX_AUDIO_BUFFER_SAMPLES (15 * SAMPLE_RATE)

/* Pre-roll buffer: captures audio BEFORE VAD triggers to avoid missing first words
 * 500ms at 16kHz mono = 8000 samples = 16000 bytes */
#define PREROLL_MS 500
#define PREROLL_SAMPLES ((SAMPLE_RATE * PREROLL_MS) / 1000)

/* Wake word configuration - buffer must fit prefix (max ~14 chars) + ai_name (max 64 chars) */
#define WAKE_WORD_BUF_SIZE 96
static const char *wakeWordPrefixes[] = { "hello ",    "okay ",         "alright ",
                                          "hey ",      "hi ",           "good evening ",
                                          "good day ", "good morning ", "yeah ",
                                          "k " };
#define NUM_WAKE_WORDS (sizeof(wakeWordPrefixes) / sizeof(wakeWordPrefixes[0]))

/* Time-of-day greetings */
static const char *morning_greeting = "Good morning.";
static const char *day_greeting = "Good day.";
static const char *evening_greeting = "Good evening.";

/* Offline fallback message */
static const char *offline_message = "I'm sorry, I can't reach the server right now.";

/* =============================================================================
 * Internal Structures
 * ============================================================================= */

struct voice_ctx {
   /* State */
   voice_state_t state;
   volatile bool running;

   /* VAD */
#ifdef HAVE_VAD_SILERO
   silero_vad_context_t *vad;
#else
   void *vad; /* Placeholder */
#endif
   float vad_threshold;
   int speech_frame_count;
   int silence_frame_count;
   int silence_end_frames; /* Configurable from config */

   /* Pre-roll buffer: circular buffer capturing audio before VAD triggers */
   int16_t preroll_buffer[PREROLL_SAMPLES];
   size_t preroll_write_pos; /* Next write position (circular) */
   size_t preroll_valid;     /* Valid samples in buffer (up to PREROLL_SAMPLES) */

   /* Audio buffer for recording */
   int16_t *audio_buffer;
   size_t audio_buffer_capacity;
   size_t audio_buffer_len;

   /* ASR */
#ifdef HAVE_ASR_ENGINE
   asr_engine_context_t *asr;
#elif defined(HAVE_ASR_WHISPER)
   whisper_asr_context_t *asr;
#else
   void *asr; /* Placeholder */
#endif
   bool asr_loaded;
   char asr_engine_name[16]; /* "whisper" or "vosk" */
   char asr_model_path[256];
   char asr_language[8];
   int asr_n_threads;

   /* TTS */
#ifdef HAVE_TTS_PIPER
   tts_piper_context_t *tts;
#else
   void *tts;          /* Placeholder */
#endif
   bool tts_loaded;
   char tts_model_path[256];
   char tts_config_path[256];
   char tts_espeak_data[256];
   float tts_length_scale;

   /* Wake word */
   char ai_name[64];
   char wakeWordBuffers[NUM_WAKE_WORDS][WAKE_WORD_BUF_SIZE];
   char *wakeWords[NUM_WAKE_WORDS];

   /* Response handling - thread-safe for WebSocket callback thread */
   pthread_mutex_t response_mutex;
   char response_buffer[8192];
   size_t response_len;
   atomic_bool response_complete; /* Atomic flag for thread-safe signaling */

   /* Sentence buffer for streaming TTS */
#ifdef HAVE_VAD_SILERO
   sentence_buffer_t *sentence_buf;
#else
   void *sentence_buf; /* Placeholder */
#endif

   /* Audio playback handle for sentence-level TTS (set during loop) */
   audio_playback_t *playback;
   volatile int tts_stop_flag;
};

/* =============================================================================
 * State Names
 * ============================================================================= */

const char *voice_state_name(voice_state_t state) {
   switch (state) {
      case VOICE_STATE_SILENCE:
         return "SILENCE";
      case VOICE_STATE_WAKEWORD_LISTEN:
         return "WAKEWORD_LISTEN";
      case VOICE_STATE_COMMAND_RECORDING:
         return "COMMAND_RECORDING";
      case VOICE_STATE_PROCESSING:
         return "PROCESSING";
      case VOICE_STATE_WAITING:
         return "WAITING";
      case VOICE_STATE_SPEAKING:
         return "SPEAKING";
      default:
         return "UNKNOWN";
   }
}

/* =============================================================================
 * Callbacks
 * ============================================================================= */

static void on_stream_callback(const char *text, bool is_end, void *user_data) {
   voice_ctx_t *ctx = (voice_ctx_t *)user_data;
   if (!ctx)
      return;

   if (text && text[0]) {
      /* Append to response buffer (for logging/history) - protected by mutex */
      pthread_mutex_lock(&ctx->response_mutex);
      size_t len = strlen(text);
      if (ctx->response_len + len < sizeof(ctx->response_buffer) - 1) {
         memcpy(ctx->response_buffer + ctx->response_len, text, len);
         ctx->response_len += len;
         ctx->response_buffer[ctx->response_len] = '\0';
      }
      pthread_mutex_unlock(&ctx->response_mutex);

      /* Feed to sentence buffer for sentence-level TTS */
#ifdef HAVE_VAD_SILERO
      if (ctx->sentence_buf) {
         sentence_buffer_feed(ctx->sentence_buf, text);
      }
#endif
   }

   if (is_end) {
      /* Flush any remaining text in sentence buffer */
#ifdef HAVE_VAD_SILERO
      if (ctx->sentence_buf) {
         sentence_buffer_flush(ctx->sentence_buf);
      }
#endif
      /* Signal completion atomically - main loop checks this without lock */
      atomic_store(&ctx->response_complete, true);
   }
}

static void on_state_callback(const char *state, void *user_data) {
   voice_ctx_t *ctx = (voice_ctx_t *)user_data;
   (void)ctx;

   LOG_DEBUG("Server state: %s", state);
}

/* =============================================================================
 * Lazy Model Loading
 * ============================================================================= */

#ifdef HAVE_ASR_ENGINE
static bool ensure_asr_loaded(voice_ctx_t *ctx) {
   if (ctx->asr_loaded)
      return true;

   LOG_INFO("Loading ASR model (%s): %s", ctx->asr_engine_name, ctx->asr_model_path);

   asr_engine_type_t engine_type = ASR_ENGINE_WHISPER;
   if (strcmp(ctx->asr_engine_name, "vosk") == 0) {
      engine_type = ASR_ENGINE_VOSK;
   }

   asr_engine_config_t asr_config = {
      .engine = engine_type,
      .model_path = ctx->asr_model_path,
      .sample_rate = SAMPLE_RATE,
      .use_gpu = 1, /* Enable GPU if available (Jetson, etc.) */
      .n_threads = ctx->asr_n_threads,
      .language = ctx->asr_language,
      .max_audio_seconds = 15,
   };

   ctx->asr = asr_engine_init(&asr_config);
   if (!ctx->asr) {
      LOG_ERROR("Failed to load ASR model");
      return false;
   }

   ctx->asr_loaded = true;
   LOG_INFO("ASR model loaded (%s)", asr_engine_name(engine_type));
   return true;
}
#elif defined(HAVE_ASR_WHISPER)
static bool ensure_asr_loaded(voice_ctx_t *ctx) {
   if (ctx->asr_loaded)
      return true;

   LOG_INFO("Loading ASR model: %s", ctx->asr_model_path);

   asr_whisper_config_t asr_config = {
      .model_path = ctx->asr_model_path,
      .language = ctx->asr_language,
      .n_threads = ctx->asr_n_threads,
      .use_gpu = true, /* Enable GPU if available (Jetson, etc.) */
   };

   ctx->asr = asr_whisper_init(&asr_config);
   if (!ctx->asr) {
      LOG_ERROR("Failed to load ASR model");
      return false;
   }

   ctx->asr_loaded = true;
   LOG_INFO("ASR model loaded");
   return true;
}
#else
static bool ensure_asr_loaded(voice_ctx_t *ctx) {
   (void)ctx;
   LOG_ERROR("ASR not available (built without ASR support)");
   return false;
}
#endif

#ifdef HAVE_TTS_PIPER
static bool ensure_tts_loaded(voice_ctx_t *ctx) {
   if (ctx->tts_loaded)
      return true;

   LOG_INFO("Loading TTS model: %s", ctx->tts_model_path);

   tts_piper_config_t tts_config = {
      .model_path = ctx->tts_model_path,
      .model_config_path = ctx->tts_config_path,
      .espeak_data_path = ctx->tts_espeak_data,
      .length_scale = ctx->tts_length_scale,
      .use_cuda = 0,
   };

   ctx->tts = tts_piper_init(&tts_config);
   if (!ctx->tts) {
      LOG_ERROR("Failed to load TTS model");
      return false;
   }

   ctx->tts_loaded = true;
   LOG_INFO("TTS model loaded");
   return true;
}
#else
static bool ensure_tts_loaded(voice_ctx_t *ctx) {
   (void)ctx;
   LOG_ERROR("TTS not available (built without HAVE_TTS_PIPER)");
   return false;
}
#endif

/* =============================================================================
 * Wake Word Detection
 * ============================================================================= */

/**
 * Initialize wake words by combining prefixes with ai_name.
 * Called during voice_processing_init().
 */
static void init_wake_words(voice_ctx_t *ctx) {
   for (size_t i = 0; i < NUM_WAKE_WORDS; i++) {
      snprintf(ctx->wakeWordBuffers[i], WAKE_WORD_BUF_SIZE, "%s%s", wakeWordPrefixes[i],
               ctx->ai_name);
      ctx->wakeWords[i] = ctx->wakeWordBuffers[i];
   }
   LOG_INFO("Wake words configured for '%s' (e.g., '%s', '%s')", ctx->ai_name, ctx->wakeWords[0],
            ctx->wakeWords[1]);
}

/**
 * Normalize text for wake word matching.
 * Converts to lowercase, keeps only letters/digits/spaces, removes punctuation.
 *
 * @param input Original text
 * @return Normalized text (caller must free), or NULL on error
 */
static char *normalize_wake_word_text(const char *input) {
   if (!input) {
      return NULL;
   }

   size_t len = strlen(input);
   char *normalized = (char *)malloc(len + 1);
   if (!normalized) {
      return NULL;
   }

   size_t j = 0;
   for (size_t i = 0; i < len; i++) {
      char c = input[i];
      /* Convert to lowercase */
      if (c >= 'A' && c <= 'Z') {
         normalized[j++] = c + 32;
      }
      /* Keep lowercase letters, digits, and spaces */
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ') {
         normalized[j++] = c;
      }
      /* Skip punctuation and other characters */
   }
   normalized[j] = '\0';

   return normalized;
}

/**
 * Check if text contains any of the configured wake words.
 * If found, extracts the command portion after the wake word from the ORIGINAL text.
 *
 * @param ctx Voice context with initialized wake words
 * @param text Transcribed text to check (original, with punctuation/casing)
 * @param command_out If not NULL and wake word found, receives the command
 *                    portion after the wake word (caller must free)
 * @return true if wake word found, false otherwise
 */
static bool check_wake_word(voice_ctx_t *ctx, const char *text, char **command_out) {
   if (!ctx || !text)
      return false;

   if (command_out)
      *command_out = NULL;

   /* Normalize the text for matching */
   char *normalized = normalize_wake_word_text(text);
   if (!normalized)
      return false;

   /* Search for any wake word in normalized text */
   for (size_t i = 0; i < NUM_WAKE_WORDS; i++) {
      char *match = strstr(normalized, ctx->wakeWords[i]);
      if (match != NULL) {
         LOG_DEBUG("Wake word match: '%s' in '%s'", ctx->wakeWords[i], normalized);

         /* Calculate offset in normalized text where wake word ends */
         size_t norm_end_offset = (match - normalized) + strlen(ctx->wakeWords[i]);

         /* Map normalized offset back to original text position */
         size_t orig_offset = 0;
         size_t norm_offset = 0;
         while (norm_offset < norm_end_offset && text[orig_offset] != '\0') {
            char c = text[orig_offset];
            /* Only count characters that appear in normalized text */
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == ' ') {
               norm_offset++;
            }
            orig_offset++;
         }

         /* Extract command from ORIGINAL text after wake word */
         if (command_out) {
            const char *after_wake = text + orig_offset;
            /* Skip leading whitespace and punctuation */
            while (*after_wake != '\0' &&
                   (*after_wake == ' ' || *after_wake == ',' || *after_wake == '.' ||
                    *after_wake == '!' || *after_wake == '?')) {
               after_wake++;
            }

            if (*after_wake != '\0') {
               *command_out = strdup(after_wake);
               LOG_DEBUG("Command after wake word: '%s'", *command_out);
            }
         }

         free(normalized);
         return true;
      }
   }

   free(normalized);
   return false;
}

/* =============================================================================
 * Sentence Buffer Callback (for streaming TTS)
 * ============================================================================= */

#ifdef HAVE_VAD_SILERO
static void on_sentence_complete(const char *sentence, void *userdata) {
   voice_ctx_t *ctx = (voice_ctx_t *)userdata;
   if (!ctx || !sentence || !sentence[0])
      return;

   /* Skip if TTS not loaded or no playback handle */
   if (!ctx->tts || !ctx->playback) {
      LOG_DEBUG("Sentence (no TTS): %s", sentence);
      return;
   }

#ifdef HAVE_TTS_PIPER
   /* Preprocess the sentence for TTS */
   char preprocessed[4096];
   int preproc_len = preprocess_text_for_tts_c(sentence, preprocessed, sizeof(preprocessed));
   const char *tts_text = (preproc_len > 0) ? preprocessed : sentence;

   /* Skip empty sentences after preprocessing */
   if (!tts_text[0]) {
      return;
   }

   LOG_INFO("Speaking sentence: %.60s%s", tts_text, strlen(tts_text) > 60 ? "..." : "");

   /* Synthesize and play this sentence */
   int16_t *audio = NULL;
   size_t audio_len = 0;
   tts_piper_result_t tts_result;

   if (tts_piper_synthesize(ctx->tts, tts_text, &audio, &audio_len, &tts_result) == 0 && audio &&
       audio_len > 0) {
      int sample_rate = tts_piper_get_sample_rate(ctx->tts);
      audio_playback_play(ctx->playback, audio, audio_len, sample_rate, &ctx->tts_stop_flag);
      free(audio);

      /* Small pause between sentences for natural rhythm */
      if (!ctx->tts_stop_flag) {
         usleep(150000); /* 150ms pause */
      }
   }
#else
   LOG_DEBUG("Sentence (TTS disabled): %s", sentence);
#endif
}
#endif

/* =============================================================================
 * Time-of-Day Greeting
 * ============================================================================= */

/**
 * Get appropriate greeting based on current time of day.
 */
static const char *time_of_day_greeting(void) {
   time_t t = time(NULL);
   struct tm tm_storage;
   struct tm *local_time = localtime_r(&t, &tm_storage);

   int hour = local_time->tm_hour;

   if (hour >= 3 && hour < 12) {
      return morning_greeting;
   } else if (hour >= 12 && hour < 18) {
      return day_greeting;
   } else {
      return evening_greeting;
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

voice_ctx_t *voice_processing_init(const satellite_config_t *config) {
   if (!config) {
      LOG_ERROR("NULL config");
      return NULL;
   }

   voice_ctx_t *ctx = calloc(1, sizeof(voice_ctx_t));
   if (!ctx) {
      LOG_ERROR("Failed to allocate context");
      return NULL;
   }

   /* Initialize thread synchronization for response buffer */
   pthread_mutex_init(&ctx->response_mutex, NULL);
   atomic_store(&ctx->response_complete, false);

   ctx->state = VOICE_STATE_SILENCE;
   ctx->running = true;

   /* Store config for model initialization */
   snprintf(ctx->asr_engine_name, sizeof(ctx->asr_engine_name), "%s", config->asr.engine);
   snprintf(ctx->asr_model_path, sizeof(ctx->asr_model_path), "%s", config->asr.model_path);
   snprintf(ctx->asr_language, sizeof(ctx->asr_language), "%s", config->asr.language);
   ctx->asr_n_threads = config->asr.n_threads;

   snprintf(ctx->tts_model_path, sizeof(ctx->tts_model_path), "%s", config->tts.model_path);
   snprintf(ctx->tts_config_path, sizeof(ctx->tts_config_path), "%s", config->tts.config_path);
   snprintf(ctx->tts_espeak_data, sizeof(ctx->tts_espeak_data), "%s", config->tts.espeak_data);
   ctx->tts_length_scale = config->tts.length_scale;

   /* Wake word setup - use ai_name to build all variants */
   snprintf(ctx->ai_name, sizeof(ctx->ai_name), "%s", config->general.ai_name);
   init_wake_words(ctx);

   /* VAD settings */
   ctx->vad_threshold = config->vad.threshold;
   ctx->silence_end_frames = config->vad.silence_duration_ms / 32; /* 32ms per frame */

   /* Initialize VAD immediately (small footprint, always needed) */
#ifdef HAVE_VAD_SILERO
   if (config->vad.enabled) {
      ctx->vad = vad_silero_init(config->vad.model_path, NULL);
      if (!ctx->vad) {
         LOG_ERROR("Failed to initialize VAD");
         free(ctx);
         return NULL;
      }
      /* Store threshold for use in processing loop */
      ctx->vad_threshold = config->vad.threshold;
      /* Calculate silence end frames from config (frames = ms / 32ms per frame) */
      ctx->silence_end_frames = config->vad.silence_duration_ms / 32;
      LOG_INFO("VAD initialized (threshold=%.2f, silence_frames=%d)", config->vad.threshold,
               ctx->silence_end_frames);
   }
#else
   if (config->vad.enabled) {
      LOG_ERROR("VAD requested but not built with HAVE_VAD_SILERO");
      free(ctx);
      return NULL;
   }
#endif

   /* Allocate audio buffer (15s max per efficiency review) */
   size_t max_samples = config->asr.max_audio_seconds * SAMPLE_RATE;
   if (max_samples > MAX_AUDIO_BUFFER_SAMPLES) {
      max_samples = MAX_AUDIO_BUFFER_SAMPLES;
   }

   ctx->audio_buffer_capacity = max_samples;
   ctx->audio_buffer = malloc(ctx->audio_buffer_capacity * sizeof(int16_t));
   if (!ctx->audio_buffer) {
      LOG_ERROR("Failed to allocate audio buffer");
      vad_silero_cleanup(ctx->vad);
      free(ctx);
      return NULL;
   }

   /* Initialize sentence buffer for streaming TTS */
#ifdef HAVE_VAD_SILERO
   ctx->sentence_buf = sentence_buffer_create(on_sentence_complete, ctx);
   if (!ctx->sentence_buf) {
      LOG_ERROR("Failed to create sentence buffer");
      free(ctx->audio_buffer);
      if (ctx->vad)
         vad_silero_cleanup(ctx->vad);
      free(ctx);
      return NULL;
   }
#endif

   /* Load all models at startup for predictable latency (no lazy loading) */
#if defined(HAVE_ASR_ENGINE) || defined(HAVE_ASR_WHISPER)
   if (config->asr.model_path[0]) {
      if (!ensure_asr_loaded(ctx)) {
         LOG_ERROR("Failed to load ASR model - voice processing unavailable");
         sentence_buffer_free(ctx->sentence_buf);
         free(ctx->audio_buffer);
         if (ctx->vad)
            vad_silero_cleanup(ctx->vad);
         free(ctx);
         return NULL;
      }
   }
#endif

#ifdef HAVE_TTS_PIPER
   if (config->tts.model_path[0]) {
      LOG_INFO("Loading TTS model: %s", ctx->tts_model_path);
      if (!ensure_tts_loaded(ctx)) {
         LOG_ERROR("Failed to load TTS model - voice processing unavailable");
#ifdef HAVE_ASR_ENGINE
         if (ctx->asr)
            asr_engine_cleanup(ctx->asr);
#elif defined(HAVE_ASR_WHISPER)
         if (ctx->asr)
            asr_whisper_cleanup(ctx->asr);
#endif
         sentence_buffer_free(ctx->sentence_buf);
         free(ctx->audio_buffer);
         if (ctx->vad)
            vad_silero_cleanup(ctx->vad);
         free(ctx);
         return NULL;
      }
   }
#endif

   LOG_INFO("Voice processing initialized (buffer=%zus, ai_name=%s)",
            ctx->audio_buffer_capacity / SAMPLE_RATE, ctx->ai_name);

   return ctx;
}

void voice_processing_cleanup(voice_ctx_t *ctx) {
   if (!ctx)
      return;

#ifdef HAVE_VAD_SILERO
   if (ctx->vad) {
      vad_silero_cleanup(ctx->vad);
   }
   if (ctx->sentence_buf) {
      sentence_buffer_free(ctx->sentence_buf);
   }
#endif

#ifdef HAVE_ASR_ENGINE
   if (ctx->asr) {
      asr_engine_cleanup(ctx->asr);
   }
#elif defined(HAVE_ASR_WHISPER)
   if (ctx->asr) {
      asr_whisper_cleanup(ctx->asr);
   }
#endif

#ifdef HAVE_TTS_PIPER
   if (ctx->tts) {
      tts_piper_cleanup(ctx->tts);
   }
#endif

   free(ctx->audio_buffer);

   /* Destroy thread synchronization */
   pthread_mutex_destroy(&ctx->response_mutex);

   free(ctx);

   LOG_INFO("Voice processing cleaned up");
}

voice_state_t voice_processing_get_state(const voice_ctx_t *ctx) {
   return ctx ? ctx->state : VOICE_STATE_SILENCE;
}

void voice_processing_stop(voice_ctx_t *ctx) {
   if (ctx) {
      ctx->running = false;
   }
}

void voice_processing_speak_greeting(voice_ctx_t *ctx, satellite_ctx_t *sat_ctx) {
   if (!ctx || !sat_ctx)
      return;

#ifdef HAVE_TTS_PIPER
   if (!ctx->tts) {
      LOG_ERROR("TTS not loaded, cannot speak greeting");
      return;
   }

   const char *greeting = time_of_day_greeting();
   LOG_INFO("Speaking greeting: %s", greeting);

   int16_t *audio = NULL;
   size_t audio_len = 0;
   tts_piper_result_t tts_result;

   if (tts_piper_synthesize(ctx->tts, greeting, &audio, &audio_len, &tts_result) == 0 && audio &&
       audio_len > 0) {
      audio_playback_t *playback = (audio_playback_t *)sat_ctx->audio_playback;
      int sample_rate = tts_piper_get_sample_rate(ctx->tts);
      volatile int stop_flag = 0;
      audio_playback_play(playback, audio, audio_len, sample_rate, &stop_flag);
      free(audio);
      LOG_INFO("Greeting playback complete");
   } else {
      LOG_ERROR("Failed to synthesize greeting");
   }
#else
   (void)ctx;
   (void)sat_ctx;
   LOG_INFO("TTS not available, skipping greeting");
#endif
}

void voice_processing_speak_offline(voice_ctx_t *ctx, satellite_ctx_t *sat_ctx) {
   if (!ctx || !sat_ctx)
      return;

#ifdef HAVE_TTS_PIPER
   if (!ctx->tts) {
      LOG_ERROR("TTS not loaded, cannot speak offline message");
      return;
   }

   LOG_INFO("Speaking offline message: %s", offline_message);

   int16_t *audio = NULL;
   size_t audio_len = 0;
   tts_piper_result_t tts_result;

   if (tts_piper_synthesize(ctx->tts, offline_message, &audio, &audio_len, &tts_result) == 0 &&
       audio && audio_len > 0) {
      audio_playback_t *playback = (audio_playback_t *)sat_ctx->audio_playback;
      int sample_rate = tts_piper_get_sample_rate(ctx->tts);
      volatile int stop_flag = 0;
      audio_playback_play(playback, audio, audio_len, sample_rate, &stop_flag);
      free(audio);
   } else {
      LOG_ERROR("Failed to synthesize offline message");
   }
#else
   (void)ctx;
   (void)sat_ctx;
   LOG_INFO("TTS not available, cannot speak offline message");
#endif
}

int voice_processing_loop(voice_ctx_t *ctx,
                          satellite_ctx_t *sat_ctx,
                          ws_client_t *ws,
                          const satellite_config_t *config) {
   if (!ctx || !sat_ctx || !ws || !config) {
      LOG_ERROR("Invalid parameters");
      return 1;
   }

   audio_capture_t *capture = (audio_capture_t *)sat_ctx->audio_capture;
   audio_playback_t *playback = (audio_playback_t *)sat_ctx->audio_playback;

   /* Store playback handle for sentence-level TTS callback */
   ctx->playback = playback;
   ctx->tts_stop_flag = 0;

   /* Set up WebSocket callbacks */
   ws_client_set_stream_callback(ws, on_stream_callback, ctx);
   ws_client_set_state_callback(ws, on_state_callback, ctx);

   LOG_INFO("Starting voice processing loop (say '%s' or '%s' to activate)", ctx->wakeWords[0],
            ctx->wakeWords[3]); /* e.g., "hello friday" or "hey friday" */

   int16_t frame_buffer[VAD_FRAME_SAMPLES];
   int debug_frame_count = 0;

   LOG_INFO("Entering main loop...");
   LOG_INFO("capture=%p, playback=%p", (void *)capture, (void *)playback);

   /* Initial debug: check ring buffer state */
   size_t initial_bytes = audio_capture_bytes_available(capture);
   LOG_INFO("Initial ring buffer state: %zu bytes available", initial_bytes);

   while (ctx->running && ws_client_is_connected(ws)) {
      /* Debug: confirm loop is running */
      debug_frame_count++;

      struct timespec t0, t1, t2;
      clock_gettime(CLOCK_MONOTONIC, &t0);

      /* NOTE: WebSocket servicing moved to background thread (ws_client handles internally)
       * We no longer call ws_client_service() here - callbacks happen asynchronously */

      clock_gettime(CLOCK_MONOTONIC, &t1);

      switch (ctx->state) {
         case VOICE_STATE_SILENCE:
         case VOICE_STATE_WAKEWORD_LISTEN:
         case VOICE_STATE_COMMAND_RECORDING: {
            /* Wait for audio data (blocking with timeout - same pattern as daemon) */
            size_t available = audio_capture_wait_for_data(capture, VAD_FRAME_SAMPLES,
                                                           AUDIO_WAIT_TIMEOUT_MS);

            clock_gettime(CLOCK_MONOTONIC, &t2);

            /* Timing debug every 100 iterations (~3 seconds) */
            if (debug_frame_count % 100 == 0) {
               long ws_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
               long wait_ms = (t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000;
               LOG_INFO("Loop %d: ws=%ldms, wait=%ldms, avail=%zu", debug_frame_count, ws_ms,
                        wait_ms, available);
            }

            if (available < VAD_FRAME_SAMPLES) {
               /* Timeout - continue to service WebSocket and check running flag */
               continue;
            }

            /* Read the audio frame */
            ssize_t samples = audio_capture_read(capture, frame_buffer, VAD_FRAME_SAMPLES);
            if (samples <= 0) {
               LOG_ERROR("audio_capture_read returned %zd after wait reported %zu available",
                         samples, available);
               continue;
            }

            /* Run VAD */
            float speech_prob = 0.0f;
#ifdef HAVE_VAD_SILERO
            if (ctx->vad) {
               speech_prob = vad_silero_process(ctx->vad, frame_buffer, samples);
            }
#endif

            bool is_speech = speech_prob >= ctx->vad_threshold;

            /* Debug: print VAD status periodically (~3 seconds) */
            if (debug_frame_count % 100 == 0) {
               LOG_INFO("VAD: prob=%.2f %s", speech_prob, is_speech ? "SPEECH" : "silence");
            }

            /* State machine transitions */
            if (ctx->state == VOICE_STATE_SILENCE) {
               /* Always store audio in pre-roll buffer (circular) */
               size_t to_copy = (size_t)samples;
               if (ctx->preroll_write_pos + to_copy > PREROLL_SAMPLES) {
                  /* Wrap around */
                  size_t first = PREROLL_SAMPLES - ctx->preroll_write_pos;
                  memcpy(ctx->preroll_buffer + ctx->preroll_write_pos, frame_buffer,
                         first * sizeof(int16_t));
                  memcpy(ctx->preroll_buffer, frame_buffer + first,
                         (to_copy - first) * sizeof(int16_t));
                  ctx->preroll_write_pos = to_copy - first;
               } else {
                  memcpy(ctx->preroll_buffer + ctx->preroll_write_pos, frame_buffer,
                         to_copy * sizeof(int16_t));
                  ctx->preroll_write_pos += to_copy;
                  if (ctx->preroll_write_pos >= PREROLL_SAMPLES) {
                     ctx->preroll_write_pos = 0;
                  }
               }
               if (ctx->preroll_valid < PREROLL_SAMPLES) {
                  ctx->preroll_valid += to_copy;
                  if (ctx->preroll_valid > PREROLL_SAMPLES) {
                     ctx->preroll_valid = PREROLL_SAMPLES;
                  }
               }

               if (is_speech) {
                  ctx->speech_frame_count++;
                  if (ctx->speech_frame_count >= SPEECH_START_FRAMES) {
                     LOG_INFO("Speech detected, listening for wake word...");
                     ctx->state = VOICE_STATE_WAKEWORD_LISTEN;
                     ctx->silence_frame_count = 0;

                     /* Copy pre-roll buffer to audio buffer (preserves audio before VAD trigger) */
                     ctx->audio_buffer_len = 0;
                     if (ctx->preroll_valid > 0) {
                        if (ctx->preroll_valid < PREROLL_SAMPLES) {
                           /* Buffer not full - copy from start */
                           memcpy(ctx->audio_buffer, ctx->preroll_buffer,
                                  ctx->preroll_valid * sizeof(int16_t));
                           ctx->audio_buffer_len = ctx->preroll_valid;
                        } else {
                           /* Buffer full - copy in order: write_pos to end, then start to write_pos
                            */
                           size_t first = PREROLL_SAMPLES - ctx->preroll_write_pos;
                           memcpy(ctx->audio_buffer, ctx->preroll_buffer + ctx->preroll_write_pos,
                                  first * sizeof(int16_t));
                           memcpy(ctx->audio_buffer + first, ctx->preroll_buffer,
                                  ctx->preroll_write_pos * sizeof(int16_t));
                           ctx->audio_buffer_len = PREROLL_SAMPLES;
                        }
                        LOG_INFO("Pre-roll: copied %zu samples (%.2fs) to audio buffer",
                                 ctx->audio_buffer_len, ctx->audio_buffer_len / (float)SAMPLE_RATE);
                     }
                     /* Reset pre-roll for next time */
                     ctx->preroll_write_pos = 0;
                     ctx->preroll_valid = 0;

#ifdef HAVE_ASR_ENGINE
                     /* For streaming engines (Vosk): feed pre-roll buffer so it starts decoding */
                     if (ctx->asr_loaded && ctx->asr &&
                         asr_engine_get_type(ctx->asr) == ASR_ENGINE_VOSK &&
                         ctx->audio_buffer_len > 0) {
                        asr_result_t *preroll_partial = asr_engine_process(ctx->asr,
                                                                           ctx->audio_buffer,
                                                                           ctx->audio_buffer_len);
                        if (preroll_partial)
                           asr_engine_result_free(preroll_partial);
                     }
#endif
                  }
               } else {
                  ctx->speech_frame_count = 0;
               }
            } else if (ctx->state == VOICE_STATE_WAKEWORD_LISTEN ||
                       ctx->state == VOICE_STATE_COMMAND_RECORDING) {
               /* Buffer audio */
               if (ctx->audio_buffer_len + samples <= ctx->audio_buffer_capacity) {
                  memcpy(ctx->audio_buffer + ctx->audio_buffer_len, frame_buffer,
                         samples * sizeof(int16_t));
                  ctx->audio_buffer_len += samples;
               }

#ifdef HAVE_ASR_ENGINE
               /* For streaming engines (Vosk): feed each frame for incremental decoding */
               if (ctx->asr_loaded && ctx->asr &&
                   asr_engine_get_type(ctx->asr) == ASR_ENGINE_VOSK) {
                  asr_result_t *partial = asr_engine_process(ctx->asr, frame_buffer, samples);
                  if (partial)
                     asr_engine_result_free(partial);
               }
#endif

               /* Check for end of speech */
               if (!is_speech) {
                  ctx->silence_frame_count++;
                  if (ctx->silence_frame_count >= ctx->silence_end_frames) {
                     if (ctx->state == VOICE_STATE_WAKEWORD_LISTEN) {
                        /* Check for wake word */
                        LOG_INFO("Silence detected, checking for wake word (buffer=%zu samples, "
                                 "%.2fs)...",
                                 ctx->audio_buffer_len, ctx->audio_buffer_len / (float)SAMPLE_RATE);
                        ctx->state = VOICE_STATE_PROCESSING;

                        if (ctx->audio_buffer_len < SAMPLE_RATE / 4) { /* Less than 250ms */
                           LOG_INFO("Audio buffer too short, returning to silence");
                           ctx->state = VOICE_STATE_SILENCE;
                           ctx->speech_frame_count = 0;
                           ctx->silence_frame_count = 0;
                           continue;
                        }

                        if (!ensure_asr_loaded(ctx)) {
                           LOG_ERROR("ASR not available");
                           ctx->state = VOICE_STATE_SILENCE;
                           continue;
                        }

#if defined(HAVE_ASR_ENGINE) || defined(HAVE_ASR_WHISPER)
                        /* Feed audio and get transcription */
                        LOG_INFO("Running ASR on %zu samples...", ctx->audio_buffer_len);
#ifdef HAVE_ASR_ENGINE
                        /* For Whisper: feed entire buffer (Vosk already got it incrementally) */
                        if (asr_engine_get_type(ctx->asr) == ASR_ENGINE_WHISPER) {
                           asr_engine_process(ctx->asr, ctx->audio_buffer, ctx->audio_buffer_len);
                        }
                        asr_result_t *result = asr_engine_finalize(ctx->asr);
#else
                        asr_whisper_process(ctx->asr, ctx->audio_buffer, ctx->audio_buffer_len);
                        asr_whisper_result_t *result = asr_whisper_finalize(ctx->asr);
#endif
                        LOG_INFO("ASR complete: result=%p, text=%s", (void *)result,
                                 result && result->text ? result->text : "(null)");
                        if (result && result->text && result->text[0]) {
                           printf("\n>>> Heard: %s\n\n", result->text);
                           fflush(stdout);

                           char *command_text = NULL;
                           if (check_wake_word(ctx, result->text, &command_text)) {
                              LOG_INFO("Wake word detected!");

                              if (command_text && command_text[0]) {
                                 /* Command was in the same phrase as wake word */
                                 printf("\n>>> Command: %s\n\n", command_text);
                                 fflush(stdout);

                                 /* Reset for new response (lock for buffer access) */
                                 pthread_mutex_lock(&ctx->response_mutex);
                                 ctx->response_buffer[0] = '\0';
                                 ctx->response_len = 0;
                                 pthread_mutex_unlock(&ctx->response_mutex);
                                 atomic_store(&ctx->response_complete, false);
#ifdef HAVE_VAD_SILERO
                                 if (ctx->sentence_buf) {
                                    sentence_buffer_clear(ctx->sentence_buf);
                                 }
#endif
                                 /* Send query */
                                 ctx->state = VOICE_STATE_WAITING;
                                 ws_client_send_query(ws, command_text);
                                 free(command_text);
                              } else {
                                 /* Wake word only - wait for command */
                                 LOG_INFO("Waiting for command...");
                                 ctx->state = VOICE_STATE_COMMAND_RECORDING;
                                 ctx->audio_buffer_len = 0;
                                 ctx->silence_frame_count = 0;
                              }
                           } else {
                              LOG_DEBUG("No wake word, returning to silence");
                              ctx->state = VOICE_STATE_SILENCE;
                           }
#ifdef HAVE_ASR_ENGINE
                           asr_engine_result_free(result);
#else
                           asr_whisper_result_free(result);
#endif
                        } else {
                           ctx->state = VOICE_STATE_SILENCE;
                           if (result)
#ifdef HAVE_ASR_ENGINE
                              asr_engine_result_free(result);
#else
                              asr_whisper_result_free(result);
#endif
                        }
#ifdef HAVE_ASR_ENGINE
                        asr_engine_reset(ctx->asr);
#else
                        asr_whisper_reset(ctx->asr);
#endif
#else
                        ctx->state = VOICE_STATE_SILENCE;
#endif
                     } else {
                        /* Command recording complete - transcribe and send */
                        LOG_INFO("Command complete, transcribing...");
                        ctx->state = VOICE_STATE_PROCESSING;

#if defined(HAVE_ASR_ENGINE) || defined(HAVE_ASR_WHISPER)
                        /* Feed audio and get transcription */
#ifdef HAVE_ASR_ENGINE
                        if (asr_engine_get_type(ctx->asr) == ASR_ENGINE_WHISPER) {
                           asr_engine_process(ctx->asr, ctx->audio_buffer, ctx->audio_buffer_len);
                        }
                        asr_result_t *cmd_result = asr_engine_finalize(ctx->asr);
#else
                        asr_whisper_process(ctx->asr, ctx->audio_buffer, ctx->audio_buffer_len);
                        asr_whisper_result_t *cmd_result = asr_whisper_finalize(ctx->asr);
#endif
                        if (cmd_result && cmd_result->text && cmd_result->text[0]) {
                           printf("\n>>> Command: %s\n\n", cmd_result->text);
                           fflush(stdout);

                           /* Reset for new response (lock for buffer access) */
                           pthread_mutex_lock(&ctx->response_mutex);
                           ctx->response_buffer[0] = '\0';
                           ctx->response_len = 0;
                           pthread_mutex_unlock(&ctx->response_mutex);
                           atomic_store(&ctx->response_complete, false);
#ifdef HAVE_VAD_SILERO
                           if (ctx->sentence_buf) {
                              sentence_buffer_clear(ctx->sentence_buf);
                           }
#endif
                           /* Send query */
                           ctx->state = VOICE_STATE_WAITING;
                           ws_client_send_query(ws, cmd_result->text);
#ifdef HAVE_ASR_ENGINE
                           asr_engine_result_free(cmd_result);
#else
                           asr_whisper_result_free(cmd_result);
#endif
                        } else {
                           LOG_INFO("Empty transcription, returning to silence");
                           ctx->state = VOICE_STATE_SILENCE;
                           if (cmd_result)
#ifdef HAVE_ASR_ENGINE
                              asr_engine_result_free(cmd_result);
#else
                              asr_whisper_result_free(cmd_result);
#endif
                        }
#ifdef HAVE_ASR_ENGINE
                        asr_engine_reset(ctx->asr);
#else
                        asr_whisper_reset(ctx->asr);
#endif
#else
                        ctx->state = VOICE_STATE_SILENCE;
#endif
                     }
                  }
               } else {
                  ctx->silence_frame_count = 0;
               }
            }
            break;
         }

         case VOICE_STATE_WAITING:
            /* Wait for response - TTS happens via sentence callback as chunks stream in */
            if (atomic_load(&ctx->response_complete)) {
               /* Log response (protected access to buffer) */
               pthread_mutex_lock(&ctx->response_mutex);
               LOG_INFO("Response complete: %.100s%s", ctx->response_buffer,
                        ctx->response_len > 100 ? "..." : "");
               pthread_mutex_unlock(&ctx->response_mutex);

               /* TTS already handled by on_sentence_complete callback during streaming */
               /* Just log and return to silence */
               ctx->state = VOICE_STATE_SILENCE;
               ctx->speech_frame_count = 0;
               ctx->silence_frame_count = 0;
               ctx->tts_stop_flag = 0; /* Reset for next interaction */
            } else {
               /* Still waiting - small sleep to avoid busy loop */
               usleep(10000); /* 10ms */
            }
            break;

         case VOICE_STATE_SPEAKING:
            /* Playing TTS - check for barge-in */
            /* TODO: Implement non-blocking playback with barge-in detection */
            ctx->state = VOICE_STATE_SILENCE;
            break;

         default:
            ctx->state = VOICE_STATE_SILENCE;
            break;
      }
      /* Note: No manual timing - audio_capture_wait_for_data() provides pacing */
   }

   LOG_INFO("Voice processing loop ended");
   return 0;
}
