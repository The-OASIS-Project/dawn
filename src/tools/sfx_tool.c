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
 * SFX Tool - Sound effect playback with concurrent slot management
 *
 * Plays short sound effects (up to SFX_MAX_CONCURRENT simultaneously).
 * Each slot has its own stop flag, completely independent from the global
 * music_play flag used by the music tool. This ensures music and effects
 * never interfere with each other.
 *
 * Sound effects are received via MQTT from MIRAGE, which forwards audio
 * commands from the SPARK/AURA hardware path.
 */

#include "tools/sfx_tool.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "audio/audio_backend.h"
#include "audio/audio_converter.h"
#include "audio/audio_decoder.h"
#include "audio/audio_utils.h"
#include "audio/flac_playback.h"
#include "dawn.h"
#include "logging.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define SFX_MAX_CONCURRENT 4
#define SFX_MIN_INTERVAL_MS 50
#define SFX_READ_BUFFER_FRAMES 4096
#define SFX_MAX_CHANNELS 2
#define SFX_THREAD_STACK_SIZE (256 * 1024) /* 256KB per playback thread */

/* =============================================================================
 * Slot Management
 * ============================================================================= */

typedef struct {
   pthread_t thread;
   char filename[512];
   _Atomic int stop;
   _Atomic int active;
} sfx_slot_t;

/*
 * Slot synchronization contract:
 *   - sfx_mutex protects slot allocation/deallocation and filename writes
 *   - _Atomic stop/active flags allow lock-free checking from playback threads
 *   - Playback threads set active=0 without the mutex (benign race: stop on
 *     a finishing thread is harmless)
 */
static sfx_slot_t sfx_slots[SFX_MAX_CONCURRENT];
static pthread_mutex_t sfx_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t sfx_last_play_ms;

/* Tool config */
typedef struct {
   char sound_path[PATH_MAX];
} sfx_config_t;

static sfx_config_t sfx_config = { .sound_path = "sound_assets/" };

/* =============================================================================
 * Helpers
 * ============================================================================= */

static uint32_t get_time_ms(void) {
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/**
 * @brief Validate a sound effect filename for safety
 *
 * Rejects path traversal, absolute paths, and unsupported extensions.
 * Only bare filenames with allowed audio extensions are accepted.
 *
 * @param filename The filename to validate
 * @return true if safe, false if rejected
 */
static bool validate_sfx_filename(const char *filename) {
   if (!filename || filename[0] == '\0')
      return false;

   /* Reject path separators and traversal */
   if (strchr(filename, '/') || strchr(filename, '\\'))
      return false;
   if (strstr(filename, ".."))
      return false;

   /* Require supported audio extension */
   const char *dot = strrchr(filename, '.');
   if (!dot)
      return false;

   const char *allowed[] = { ".ogg", ".wav", ".flac", ".mp3", NULL };
   for (int i = 0; allowed[i]; i++) {
      if (strcasecmp(dot, allowed[i]) == 0)
         return true;
   }

   return false;
}

/* =============================================================================
 * Playback Thread
 * ============================================================================= */

typedef struct {
   int slot_index;
   char filepath[PATH_MAX];
   const char *sink_name;
} sfx_thread_args_t;

static void *sfx_playback_thread(void *arg) {
   sfx_thread_args_t *targs = (sfx_thread_args_t *)arg;
   int slot_idx = targs->slot_index;
   sfx_slot_t *slot = &sfx_slots[slot_idx];

   audio_decoder_t *decoder = NULL;
   audio_stream_playback_handle_t *playback_handle = NULL;
   audio_converter_t *converter = NULL;
   int16_t *read_buffer = NULL;
   int16_t *output_buffer = NULL;
   size_t output_buffer_frames = 0;

   OLOG_INFO("SFX: Playing %s (slot %d)", targs->filepath, slot_idx);

   if (audio_backend_get_type() == AUDIO_BACKEND_NONE) {
      OLOG_ERROR("SFX: Audio backend not initialized");
      goto cleanup;
   }

   decoder = audio_decoder_open(targs->filepath);
   if (!decoder) {
      OLOG_ERROR("SFX: Failed to open: %s", targs->filepath);
      goto cleanup;
   }

   audio_decoder_info_t info;
   if (audio_decoder_get_info(decoder, &info) != AUDIO_DECODER_SUCCESS) {
      OLOG_ERROR("SFX: Failed to get audio info");
      goto cleanup;
   }

   unsigned int output_rate = audio_conv_get_output_rate();
   unsigned int output_channels = audio_conv_get_output_channels();

   audio_stream_params_t params = { .sample_rate = output_rate,
                                    .channels = output_channels,
                                    .format = AUDIO_FORMAT_S16_LE,
                                    .period_frames = 1024,
                                    .buffer_frames = 4096 };
   audio_hw_params_t hw_params;

   playback_handle = audio_stream_playback_open(targs->sink_name, &params, &hw_params);
   if (!playback_handle) {
      OLOG_ERROR("SFX: Failed to open playback device");
      goto cleanup;
   }

   unsigned int actual_rate = hw_params.sample_rate;
   unsigned int actual_channels = hw_params.channels;

   read_buffer = malloc(SFX_READ_BUFFER_FRAMES * SFX_MAX_CHANNELS * sizeof(int16_t));
   if (!read_buffer) {
      OLOG_ERROR("SFX: Failed to allocate read buffer");
      goto cleanup;
   }

   audio_converter_params_t conv_params = { .sample_rate = info.sample_rate,
                                            .channels = info.channels };
   if (audio_converter_needed_ex(&conv_params, actual_rate, actual_channels)) {
      converter = audio_converter_create_ex(&conv_params, actual_rate, actual_channels);
      if (!converter) {
         OLOG_ERROR("SFX: Failed to create converter");
         goto cleanup;
      }

      output_buffer_frames = audio_converter_max_output_frames(converter, SFX_READ_BUFFER_FRAMES);
      output_buffer = malloc(output_buffer_frames * actual_channels * sizeof(int16_t));
      if (!output_buffer) {
         OLOG_ERROR("SFX: Failed to allocate output buffer");
         goto cleanup;
      }
   }

   /* Playback loop - checks per-slot stop flag, NOT global music_play */
   while (!atomic_load(&slot->stop)) {
      ssize_t frames_read = audio_decoder_read(decoder, read_buffer, SFX_READ_BUFFER_FRAMES);

      if (frames_read < 0) {
         OLOG_ERROR("SFX: Decode error in slot %d", slot_idx);
         break;
      }
      if (frames_read == 0) {
         break; /* EOF */
      }

      /* Apply global volume */
      float vol = getMusicVolume();
      audio_apply_volume(read_buffer, (size_t)frames_read, info.channels, vol);

      ssize_t frames_written;
      if (converter) {
         size_t converted = 0;
         if (audio_converter_process(converter, read_buffer, (size_t)frames_read, output_buffer,
                                     output_buffer_frames, &converted) != 0) {
            OLOG_ERROR("SFX: Conversion failed in slot %d", slot_idx);
            break;
         }
         frames_written = audio_stream_playback_write(playback_handle, output_buffer, converted);
      } else {
         frames_written = audio_stream_playback_write(playback_handle, read_buffer,
                                                      (size_t)frames_read);
      }

      if (frames_written < 0) {
         int err = (int)(-frames_written);
         if (err == AUDIO_ERR_UNDERRUN) {
            audio_stream_playback_recover(playback_handle, err);
         } else {
            OLOG_ERROR("SFX: Write error in slot %d", slot_idx);
            break;
         }
      }
   }

cleanup:
   free(output_buffer);
   audio_converter_destroy(converter);
   free(read_buffer);

   if (decoder) {
      audio_decoder_close(decoder);
   }

   if (playback_handle) {
      if (atomic_load(&slot->stop)) {
         audio_stream_playback_drop(playback_handle);
      } else {
         audio_stream_playback_drain(playback_handle);
      }
      audio_stream_playback_close(playback_handle);
   }

   OLOG_INFO("SFX: Slot %d finished (%s)", slot_idx, targs->filepath);

   atomic_store(&slot->active, 0);
   free(targs);
   return NULL;
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

static char *sfx_callback(const char *action, char *value, int *should_respond) {
   /* Default to responding — silent NULL on failure leaves the LLM
    * unable to tell the user what went wrong.  Specific error branches
    * still override *should_respond explicitly if needed. */
   *should_respond = 1;

   if (!action) {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: sfx tool called without an action. Valid: play, stop, list.");
   }

   if (strcmp(action, "list") == 0) {
      /* List available sound effect files */
      *should_respond = 1;

      char resolved_base[PATH_MAX];
      if (!realpath(sfx_config.sound_path, resolved_base)) {
         return strdup(TOOL_RESULT_ERROR_MARK "Sound effects directory not found.");
      }

      DIR *dir = opendir(resolved_base);
      if (!dir) {
         return strdup(TOOL_RESULT_ERROR_MARK "Cannot open sound effects directory.");
      }

      /* Build response with available files */
      char *result = malloc(2048);
      if (!result) {
         closedir(dir);
         return strdup(TOOL_RESULT_ERROR_MARK "Memory error.");
      }

      int offset = snprintf(result, 2048, "Available sound effects:\n");
      int count = 0;
      struct dirent *entry;

      while ((entry = readdir(dir)) != NULL && offset < 1900) {
         if (entry->d_type != DT_REG)
            continue;

         /* Only list files with supported extensions */
         const char *dot = strrchr(entry->d_name, '.');
         if (!dot)
            continue;

         const char *allowed[] = { ".ogg", ".wav", ".flac", ".mp3", NULL };
         bool valid = false;
         for (int i = 0; allowed[i]; i++) {
            if (strcasecmp(dot, allowed[i]) == 0) {
               valid = true;
               break;
            }
         }
         if (!valid)
            continue;

         offset += snprintf(result + offset, 2048 - offset, "- %s\n", entry->d_name);
         count++;
      }

      closedir(dir);

      if (count == 0) {
         snprintf(result, 2048, "No sound effect files found in %.1980s", sfx_config.sound_path);
      }

      return result;
   }

   if (!value) {
      return strdup("Error: sfx play/stop requires a filename. Call action='list' to see "
                    "available files.");
   }

   if (strcmp(action, "play") == 0) {
      /* Rate limit */
      uint32_t now = get_time_ms();
      if (now - atomic_load(&sfx_last_play_ms) < SFX_MIN_INTERVAL_MS) {
         OLOG_WARNING("SFX: Rate limited (<%dms since last play)", SFX_MIN_INTERVAL_MS);
         char *msg = malloc(160);
         if (msg)
            snprintf(msg, 160,
                     TOOL_RESULT_ERROR_MARK
                     "Error: sfx rate-limited (less than %d ms since last play). Wait and "
                     "retry.",
                     SFX_MIN_INTERVAL_MS);
         return msg ? msg : strdup(TOOL_RESULT_ERROR_MARK "Error: sfx rate-limited.");
      }

      /* Validate filename */
      if (!validate_sfx_filename(value)) {
         OLOG_ERROR("SFX: Invalid filename rejected: %s", value);
         char *msg = malloc(256);
         if (msg)
            snprintf(msg, 256,
                     TOOL_RESULT_ERROR_MARK
                     "Error: invalid sfx filename '%s'. Bare filename only (no path), "
                     "extension .ogg/.wav/.flac/.mp3. Call action='list' to see available "
                     "files.",
                     value);
         return msg ? msg : strdup(TOOL_RESULT_ERROR_MARK "Error: invalid sfx filename.");
      }

      /* Build full path */
      char filepath[PATH_MAX];
      int n = snprintf(filepath, sizeof(filepath), "%s%s", sfx_config.sound_path, value);
      if (n < 0 || (size_t)n >= sizeof(filepath)) {
         OLOG_ERROR("SFX: Path too long");
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: sfx file path too long (internal limit exceeded).");
      }

      /* Verify realpath stays within sound_path.
       * Note: resolved path (not original filepath) is passed to the playback
       * thread, mitigating TOCTOU. sound_assets/ is assumed root-owned. */
      char resolved[PATH_MAX];
      char resolved_base[PATH_MAX];
      if (!realpath(filepath, resolved)) {
         OLOG_ERROR("SFX: File not found: %s", filepath);
         char *msg = malloc(256);
         if (msg)
            snprintf(msg, 256,
                     TOOL_RESULT_ERROR_MARK
                     "Error: sfx file '%s' not found. Call action='list' to see available "
                     "files.",
                     value);
         return msg ? msg : strdup(TOOL_RESULT_ERROR_MARK "Error: sfx file not found.");
      }
      if (!realpath(sfx_config.sound_path, resolved_base)) {
         OLOG_ERROR("SFX: Sound path not found: %s", sfx_config.sound_path);
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: sfx sound directory unreachable (server config issue).");
      }
      if (strncmp(resolved, resolved_base, strlen(resolved_base)) != 0) {
         OLOG_ERROR("SFX: Path traversal blocked: %s", filepath);
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: sfx filename rejected (path traversal blocked).");
      }

      pthread_mutex_lock(&sfx_mutex);

      /* Find a free slot */
      int slot_idx = -1;
      for (int i = 0; i < SFX_MAX_CONCURRENT; i++) {
         if (!atomic_load(&sfx_slots[i].active)) {
            /* Reap old thread before reuse */
            if (sfx_slots[i].thread) {
               pthread_join(sfx_slots[i].thread, NULL);
               sfx_slots[i].thread = 0;
            }
            slot_idx = i;
            break;
         }
      }

      if (slot_idx < 0) {
         pthread_mutex_unlock(&sfx_mutex);
         OLOG_WARNING("SFX: All %d slots busy, dropping: %s", SFX_MAX_CONCURRENT, value);
         char *msg = malloc(160);
         if (msg)
            snprintf(msg, 160,
                     TOOL_RESULT_ERROR_MARK
                     "Error: all %d sfx playback slots busy. Stop a current sound with "
                     "action='stop' before starting a new one.",
                     SFX_MAX_CONCURRENT);
         return msg ? msg : strdup(TOOL_RESULT_ERROR_MARK "Error: all sfx slots busy.");
      }

      sfx_slot_t *slot = &sfx_slots[slot_idx];
      atomic_store(&slot->stop, 0);
      atomic_store(&slot->active, 1);
      snprintf(slot->filename, sizeof(slot->filename), "%s", value);

      sfx_thread_args_t *targs = malloc(sizeof(sfx_thread_args_t));
      if (!targs) {
         atomic_store(&slot->active, 0);
         pthread_mutex_unlock(&sfx_mutex);
         OLOG_ERROR("SFX: Failed to allocate thread args");
         return strdup(TOOL_RESULT_ERROR_MARK "Error: sfx playback failed (memory allocation).");
      }

      targs->slot_index = slot_idx;
      snprintf(targs->filepath, sizeof(targs->filepath), "%s", resolved);
      targs->sink_name = getPcmPlaybackDevice();

      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, SFX_THREAD_STACK_SIZE);

      int rc = pthread_create(&slot->thread, &attr, sfx_playback_thread, targs);
      pthread_attr_destroy(&attr);

      if (rc != 0) {
         atomic_store(&slot->active, 0);
         free(targs);
         pthread_mutex_unlock(&sfx_mutex);
         OLOG_ERROR("SFX: Failed to create thread: %d", rc);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: sfx playback failed (thread spawn failed).");
      }

      /* Detach not needed - we join on reuse or shutdown */
      atomic_store(&sfx_last_play_ms, now);
      pthread_mutex_unlock(&sfx_mutex);

      char *msg = malloc(96);
      if (msg)
         snprintf(msg, 96, "Playing sfx '%s'.", value);
      return msg ? msg : strdup("Playing sfx.");

   } else if (strcmp(action, "stop") == 0) {
      pthread_mutex_lock(&sfx_mutex);

      int stopped = 0;
      for (int i = 0; i < SFX_MAX_CONCURRENT; i++) {
         if (atomic_load(&sfx_slots[i].active) && strcmp(sfx_slots[i].filename, value) == 0) {
            atomic_store(&sfx_slots[i].stop, 1);
            OLOG_INFO("SFX: Stopping slot %d (%s)", i, value);
            stopped++;
         }
      }

      pthread_mutex_unlock(&sfx_mutex);

      char *msg = malloc(96);
      if (msg) {
         if (stopped > 0)
            snprintf(msg, 96, "Stopped sfx '%s' (%d slot%s).", value, stopped,
                     stopped == 1 ? "" : "s");
         else
            snprintf(msg, 96, "No active sfx slot matched '%s'.", value);
      }
      return msg ? msg : strdup("Stop request processed.");
   }

   return strdup(TOOL_RESULT_ERROR_MARK "Error: unknown sfx action. Valid: play, stop, list.");
}

/* =============================================================================
 * Config Parser
 * ============================================================================= */

static void sfx_config_parser(toml_table_t *table, void *config) {
   sfx_config_t *cfg = (sfx_config_t *)config;

   if (!table)
      return;

   const char *raw = toml_raw_in(table, "sound_path");
   if (raw) {
      char *val = NULL;
      if (toml_rtos(raw, &val) == 0 && val) {
         snprintf(cfg->sound_path, sizeof(cfg->sound_path), "%s", val);
         /* Ensure trailing slash */
         size_t len = strlen(cfg->sound_path);
         if (len > 0 && len < sizeof(cfg->sound_path) - 1 && cfg->sound_path[len - 1] != '/') {
            cfg->sound_path[len] = '/';
            cfg->sound_path[len + 1] = '\0';
         }
         free(val);
      }
   }
}

/* =============================================================================
 * Shutdown
 * ============================================================================= */

static void sfx_shutdown(void) {
   OLOG_INFO("SFX: Shutting down...");

   /* Signal all active slots to stop */
   for (int i = 0; i < SFX_MAX_CONCURRENT; i++) {
      if (atomic_load(&sfx_slots[i].active)) {
         atomic_store(&sfx_slots[i].stop, 1);
      }
   }

   /* Join all threads */
   for (int i = 0; i < SFX_MAX_CONCURRENT; i++) {
      if (sfx_slots[i].thread) {
         pthread_join(sfx_slots[i].thread, NULL);
         sfx_slots[i].thread = 0;
      }
   }

   OLOG_INFO("SFX: Shutdown complete");
}

/* =============================================================================
 * Tool Registration
 * ============================================================================= */

static const treg_param_t sfx_params[] = {
   {
       .name = "action",
       .description = "Action to perform",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "play", "stop", "list" },
       .enum_count = 3,
   },
   {
       .name = "filename",
       .description =
           "Sound effect filename (e.g., \"hand_rep_fire2.ogg\"). Required for play/stop. "
           "Call action='list' first to discover available files; do not invent filenames. "
           "Bare filename only (no path), with extension .ogg/.wav/.flac/.mp3.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t sfx_tool_metadata = {
   .name = "sfx",
   .device_string = "sound effect",
   .topic = "dawn",
   .description = "Play, stop, or list available sound effects",
   .params = sfx_params,
   .param_count = 2,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .config = &sfx_config,
   .config_size = sizeof(sfx_config),
   .config_parser = sfx_config_parser,
   .config_section = "sfx",
   .cleanup = sfx_shutdown,
   .callback = sfx_callback,
};

int sfx_tool_register(void) {
   return tool_registry_register(&sfx_tool_metadata);
}
