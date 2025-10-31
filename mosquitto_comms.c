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

#define _GNU_SOURCE
#include <dirent.h>
#include <fnmatch.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* JSON-C */
#include <json-c/json.h>

/* OpenSSL */
#include <openssl/buffer.h>
#include <openssl/evp.h>

/* Local */
#include "dawn.h"
#include "flac_playback.h"
#include "openai.h"
#include "logging.h"
#include "mic_passthrough.h"
#include "mosquitto_comms.h"
#include "text_to_speech.h"
#include "word_to_number.h"

#define MAX_FILENAME_LENGTH 1024
#define MAX_PLAYLIST_LENGTH 100

/**
 * Array of device callbacks associating device types with their respective handling functions.
 * This facilitates dynamic invocation of actions based on the device type, enhancing the application's
 * modularity and scalability.
 *
 * FIXME:
 *    1. The static returns are not threadsafe or safe in general. Have the caller pass a pointer to fill.
 *    2. I am not currently using should_respond correctly. This needs fixing.
 */
static deviceCallback deviceCallbackArray[] = {
   {AUDIO_PLAYBACK_DEVICE, setPcmPlaybackDevice},
   {AUDIO_CAPTURE_DEVICE, setPcmCaptureDevice},
   {TEXT_TO_SPEECH, textToSpeechCallback},
   {DATE, dateCallback},
   {TIME, timeCallback},
   {MUSIC, musicCallback},
   {VOICE_AMPLIFIER, voiceAmplifierCallback},
   {SHUTDOWN, shutdownCallback},
   {VIEWING, viewingCallback},
   {VOLUME, volumeCallback},
   {LOCAL_LLM_SWITCH, localLLMCallback},
   {CLOUD_LLM_SWITCH, cloudLLMCallback}
};

static pthread_t music_thread = -1;
static pthread_t voice_thread = -1;
static char *pending_command_result = NULL;

/**
 * Retrieves the current user's home directory.
 *
 * @return A pointer to a string containing the path to the home directory. This string
 *         should not be modified or freed by the caller, as it points to an environment variable.
 */
const char* getUserHomeDirectory() {
   const char* homeDir = getenv("HOME");
   if (!homeDir) {
      LOG_ERROR("Error: HOME environment variable not set.");
      return NULL;
   }

   return homeDir;
}

/**
 * Appends a specified subdirectory to the user's home directory to construct a path.
 *
 * @param subdirectory The subdirectory to append to the home directory.
 * @return A dynamically allocated string containing the full path. The caller is responsible
 *         for freeing this memory using free().
 */
char* constructPathWithSubdirectory(const char* subdirectory) {
   const char* homeDir = getUserHomeDirectory();
   if (!homeDir) {
      // getUserHomeDirectory already prints an error message if needed.

      return NULL;
   }

   // Calculate the size needed for the full path, including null terminator
   size_t fullPathSize = strlen(homeDir) + strlen(subdirectory) + 1;

   // Allocate memory for the full path
   char* fullPath = (char*)malloc(fullPathSize);
   if (!fullPath) {
      LOG_ERROR("Error: Memory allocation failed for full path.");

      return NULL;
   }

   // Construct the full path
   snprintf(fullPath, fullPathSize, "%s%s", homeDir, subdirectory);

   return fullPath;
}

/**
 * @struct  Playlist
 * @brief   Structure to hold the list of matching filenames.
 */
typedef struct {
    char filenames[MAX_PLAYLIST_LENGTH][MAX_FILENAME_LENGTH]; /**< Array of matching filenames */
    int count; /**< Number of matching filenames */
} Playlist;

static Playlist playlist = { .count = 0 };
static int current_track = 0;

/**
 * @brief Recursively searches a directory for files matching a pattern and adds them to a playlist.
 *
 * This function searches through the given root directory and all its subdirectories
 * for files that match the provided filename pattern. It adds matching files to the playlist.
 *
 * @param rootDir The root directory where the search begins.
 * @param pattern The pattern to match filenames (supports wildcards and case folding).
 * @param playlist Pointer to the Playlist structure where matching files will be added.
 *
 * @note The function stops adding files if the playlist reaches its maximum length.
 * @note Uses the `fnmatch()` function to match filenames.
 */
void searchDirectory(const char *rootDir, const char *pattern, Playlist *playlist) {
   DIR *dir = opendir(rootDir);
   if (!dir) {
      LOG_ERROR("Error opening directory: %s", rootDir);
      return;
   }

   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL) {
      // Check if playlist is full before processing
      if (playlist->count >= MAX_PLAYLIST_LENGTH) {
         LOG_WARNING("Playlist is full.");
         closedir(dir);
         return;
      }

      if (entry->d_type == DT_REG) {
         char filePath[MAX_FILENAME_LENGTH];
         int written = snprintf(filePath, sizeof(filePath), "%s/%s", rootDir, entry->d_name);
         
         // Check if path was truncated
         if (written >= MAX_FILENAME_LENGTH) {
            LOG_WARNING("Path too long, skipping: %s/%s", rootDir, entry->d_name);
            continue;
         }

         if (fnmatch(pattern, entry->d_name, FNM_CASEFOLD) == 0) {
            strncpy(playlist->filenames[playlist->count], filePath, MAX_FILENAME_LENGTH - 1);
            playlist->filenames[playlist->count][MAX_FILENAME_LENGTH - 1] = '\0';
            playlist->count++;
         }
      } else if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && 
                 strcmp(entry->d_name, "..") != 0) {
         char subPath[MAX_FILENAME_LENGTH];
         int written = snprintf(subPath, sizeof(subPath), "%s/%s", rootDir, entry->d_name);
         
         // Check if path was truncated
         if (written >= MAX_FILENAME_LENGTH) {
            LOG_WARNING("Path too long, skipping directory: %s/%s", rootDir, entry->d_name);
            continue;
         }
         
         searchDirectory(subPath, pattern, playlist);
      }
   }

   closedir(dir);
}

#define GPT_RESPONSE_BUFFER_SIZE 512

void parseJsonCommandandExecute(const char *input)
{
   struct json_object *parsedJson = NULL;
   struct json_object *deviceObject = NULL;
   struct json_object *actionObject = NULL;
   struct json_object *valueObject = NULL;

   extern struct json_object *conversation_history;
   char *response_text = NULL;
   char gpt_response[GPT_RESPONSE_BUFFER_SIZE];

   const char *deviceName = NULL;
   const char *actionName = NULL;
   const char *value = NULL;

   char *callback_result = NULL;
   int should_respond = 0;

   int i = 0;

   // Parse the JSON data
   parsedJson = json_tokener_parse(input);
   if (parsedJson == NULL) {
      LOG_ERROR("Error: Unable to process mqtt command.");

      return;
   }

   // Get the "device" object from the JSON
   if (json_object_object_get_ex(parsedJson, "device", &deviceObject)) {
      // Extract the text value as a C string
      deviceName = json_object_get_string(deviceObject);
      if (deviceName == NULL) {
         LOG_ERROR("Error: Unable to get device name from json command.");
         json_object_put(parsedJson);
         return;
      }
   } else {
      LOG_ERROR("Error: 'device' field not found in JSON.");
      json_object_put(parsedJson);

      return;
   }

   // Get the "action" object from the JSON
   if (json_object_object_get_ex(parsedJson, "action", &actionObject)) {
      // Extract the text value as a C string
      actionName = json_object_get_string(actionObject);
      if (actionName == NULL) {
         LOG_ERROR("Error: Unable to get action name from json command.");
         json_object_put(parsedJson);
         return;
      }
   } else {
      LOG_ERROR("Error: 'action' field not found in JSON.");
      json_object_put(parsedJson);

      return;
   }

   // Get the "value" object from the JSON, not required for all commands
   if (json_object_object_get_ex(parsedJson, "value", &valueObject)) {
      // Extract the text value as a C string
      value = json_object_get_string(valueObject);
      if (value == NULL) {
         LOG_WARNING("Notice: Unable to get value name from json command.");
      }
   } else {
      LOG_WARNING("Notice: 'value' field not found in JSON.");
   }

   /* Before we process, make sure nothing's left over. */
   if (pending_command_result != NULL) {
      free(pending_command_result);
      pending_command_result = NULL;
   }

   /* Loop through device names for device types. */
   for (i = 0; i < MAX_DEVICE_TYPES; i++) {
      if (strcmp(deviceName, deviceTypeStrings[i]) == 0) {
         if (deviceCallbackArray[i].callback != NULL)
         {
            callback_result = deviceCallbackArray[i].callback(actionName,
                  (char *)value, &should_respond);

            // If in AI mode and callback returned data, store it for AI response
            if (callback_result != NULL && should_respond &&
                (command_processing_mode == CMD_MODE_LLM_ONLY ||
                 command_processing_mode == CMD_MODE_DIRECT_FIRST)) {
               size_t dest_len = (pending_command_result == NULL) ? 0 :
                                 strlen(pending_command_result);
               size_t src_len = strlen(callback_result);

               // Resize memory to fit both strings plus space and null terminator
               char *temp = pending_command_result = realloc(pending_command_result, dest_len + src_len + 2);
               if (temp == NULL) {
                  free(pending_command_result);
                  pending_command_result = NULL;
                  continue;
               }
               pending_command_result = temp;

               // Copy the new string to the end
               strcpy(pending_command_result + dest_len, " ");
               strcpy(pending_command_result + dest_len + 1, callback_result);
            }
         } else {
            LOG_WARNING("Skipping callback, value NULL.");
         }
      }
   }

   LOG_INFO("Command result for AI: %s", pending_command_result ? pending_command_result : "(null)");
   if (pending_command_result == NULL) {
      LOG_WARNING("pending_command_result is NULL. That probably shouldn't happen.");
      json_object_put(parsedJson);
      return;
   }
   snprintf(gpt_response, sizeof(gpt_response), "{\"response\": \"%s\"}", pending_command_result);

   response_text = getGptResponse(conversation_history, gpt_response, NULL, 0);
   if (response_text != NULL) {
      // AI returned successfully, vocalize response.
      LOG_INFO("AI: %s\n", response_text);
      char *match = NULL;
      if ((match = strstr(response_text, "<end_of_turn>")) != NULL) {
         *match = '\0';
         LOG_INFO("AI: %s\n", response_text);
      }
      /* FIXME: This is a quick workaround for null responses. */
      if (response_text[0] != '{') {
         text_to_speech(response_text);

         // Add the successful AI response to the conversation.
         struct json_object *ai_message = json_object_new_object();
         json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
         json_object_object_add(ai_message, "content", json_object_new_string(response_text));
         json_object_array_add(conversation_history, ai_message);
      }

      free(response_text);
      response_text = NULL;
   } else {
      // Error on AI response
      LOG_ERROR("GPT error.\n");
      text_to_speech("I'm sorry but I'm currently unavailable boss.");
   }
   free(pending_command_result);
   pending_command_result = NULL;

   // Cleanup: Release the parsed_json object
   json_object_put(parsedJson);
}

/* Mosquitto */
/* Callback called when the client receives a CONNACK message from the broker. */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code)
{
   int rc;

   LOG_INFO("MQTT Connecting.");

   if(reason_code != 0){
      LOG_WARNING("MQTT disconnecting?");
      mosquitto_disconnect(mosq);
      return;
   }

   // Subscribe in the on_connect callback
   rc = mosquitto_subscribe(mosq, NULL, APPLICATION_NAME, 0);
   if(rc != MOSQ_ERR_SUCCESS) {
      LOG_ERROR("Error on mosquitto_subscribe(): %s", mosquitto_strerror(rc));
   } else {
      LOG_INFO("Subscribed to \"%s\" MQTT.", APPLICATION_NAME);
   }
}

/* Callback called when the broker sends a SUBACK in response to a SUBSCRIBE. */
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos)
{
	int i;
	bool have_subscription = false;

   LOG_INFO("MQTT subscribed.");

	for(i=0; i<qos_count; i++){
		if(granted_qos[i] <= 2){
			have_subscription = true;
		}
	}
	if(have_subscription == false){
		LOG_ERROR("Error: All subscriptions rejected.");
		mosquitto_disconnect(mosq);
	}
}

/* Callback called when the client receives a message. */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
	LOG_INFO("%s %d %s", msg->topic, msg->qos, (char *)msg->payload);

   parseJsonCommandandExecute((char *)msg->payload);
}

char* dateCallback(const char *actionName, char *value, int *should_respond) {
   time_t current_time;
   struct tm *time_info;
   char buffer[80];
   static char return_buffer[384];
   int choice;

   *should_respond = 1;  // Default to responding

   time(&current_time);
   time_info = localtime(&current_time);

   // Format the date data
   strftime(buffer, sizeof(buffer), "%A, %B %d, %Y", time_info);

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      // Direct mode: use text-to-speech with personality
      srand(time(NULL));
      choice = rand() % 3;

      switch (choice) {
         case 0:
            snprintf(return_buffer, sizeof(return_buffer),
                     "Today's date, dear Sir, is %s. You're welcome.", buffer);
            break;
         case 1:
            snprintf(return_buffer, sizeof(return_buffer),
                     "In case you've forgotten, Sir, it's %s today.", buffer);
            break;
         case 2:
            snprintf(return_buffer, sizeof(return_buffer),
                     "The current date is %s.", buffer);
            break;
      }

      int local_should_respond = 0;
      textToSpeechCallback(NULL, return_buffer, &local_should_respond);
      return NULL;  // Already handled
   } else {
      // AI modes: return the raw data for AI to process
      snprintf(return_buffer, sizeof(return_buffer),
               "The current date is %s", buffer);
      return return_buffer;
   }
}

char* timeCallback(const char *actionName, char *value, int *should_respond) {
   time_t current_time;
   struct tm *time_info;
   char buffer[80];
   static char return_buffer[384];
   int choice;

   *should_respond = 1;

   time(&current_time);
   time_info = localtime(&current_time);

   // Format the time data
   strftime(buffer, sizeof(buffer), "%I:%M %p", time_info);

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      // Direct mode: use text-to-speech with personality
      srand(time(NULL));
      choice = rand() % 4;

      switch (choice) {
         case 0:
            snprintf(return_buffer, sizeof(return_buffer),
                     "The current time, in case your wristwatch has failed you, is %s.", buffer);
            break;
         case 1:
            snprintf(return_buffer, sizeof(return_buffer),
                     "I trust you have something important planned, Sir? It's %s.", buffer);
            break;
         case 2:
            snprintf(return_buffer, sizeof(return_buffer),
                     "Oh, you want to know the time again? It's %s, not that I'm keeping track.", buffer);
            break;
         case 3:
            snprintf(return_buffer, sizeof(return_buffer),
                     "The time is %s.", buffer);
            break;
      }

      int local_should_respond = 0;
      textToSpeechCallback(NULL, return_buffer, &local_should_respond);
      return NULL;
   } else {
      // AI modes: return the raw data
      snprintf(return_buffer, sizeof(return_buffer),
               "The time is %s.", buffer);
      return return_buffer;
   }
}

// Custom comparison function for qsort
int compare(const void *p1, const void *p2) {
    return strcmp((char *)p1, (char *)p2);
}

#define MUSIC_CALLBACK_BUFFER_SIZE 512

char* musicCallback(const char *actionName, char *value, int *should_respond) {
   PlaybackArgs args;
   char strWildcards[MAX_FILENAME_LENGTH];
   static char return_buffer[MUSIC_CALLBACK_BUFFER_SIZE];
   int i = 0;

   *should_respond = 1;  // Default to responding

   if (strcmp(actionName, "play") == 0) {
      if (music_thread != -1) {
         setMusicPlay(0);
         int join_result = pthread_join(music_thread, NULL);
         if (join_result == 0) {
            music_thread = -1;
         }
      }

      playlist.count = 0;
      current_track = 0;

      if ((strlen(value) + 8) > MAX_FILENAME_LENGTH) {
         LOG_ERROR("\"%s\" is too long to search for.", value);

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            snprintf(return_buffer, sizeof(return_buffer),
                     "Search term '%s' is too long", value);
            return return_buffer;
         }
      }

      // Construct the full path to the user's music directory
      char* musicDir = constructPathWithSubdirectory(MUSIC_DIR);
      if (!musicDir) {
         LOG_ERROR("Error constructing music path.");

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            strcpy(return_buffer, "Failed to access music directory");
            return return_buffer;
         }
      }

      strWildcards[0] = '*';
      for (i = 0; value[i] != '\0'; i++) {
         if (value[i] == ' ') {
            strWildcards[i+1] = '*';
         } else {
            strWildcards[i+1] = value[i];
         }
      }
      strWildcards[i+1] = '*';
      strWildcards[i+2] = '\0';
      strcat(strWildcards, ".flac");

      searchDirectory(musicDir, strWildcards, &playlist);

      free(musicDir); // free the allocated memory

      // Sort the array using qsort
      qsort(playlist.filenames, playlist.count, MAX_FILENAME_LENGTH, compare);

      LOG_INFO("New playlist:");
      for (i = 0; i < playlist.count; i++) {
         LOG_INFO("\t%s", playlist.filenames[i]);
      }

      if (playlist.count > 0) {
         args.sink_name = getPcmPlaybackDevice();
         args.file_name = playlist.filenames[current_track];
         args.start_time = 0;       /* For now set to zero. We may support other modes later. */

         LOG_INFO("Playing: %s %s %d", args.sink_name, args.file_name, args.start_time);

         // Create the playback thread
         if (pthread_create(&music_thread, NULL, playFlacAudio, &args)) {
            LOG_ERROR("Error creating thread");

            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               strcpy(return_buffer, "Failed to start music playback");
               return return_buffer;
            }
         }

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;  // Music starts playing, no verbal response needed
            return NULL;
         } else {
            *should_respond = 0;
            snprintf(return_buffer, sizeof(return_buffer),
                     "Playing %s - found %d matching tracks", value, playlist.count);
            return return_buffer;
         }
      } else {
         LOG_WARNING("No music matching that description was found.");

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            snprintf(return_buffer, sizeof(return_buffer),
                     "No music found matching '%s'", value);
            return return_buffer;
         }
      }
   } else if (strcmp(actionName, "stop") == 0) {
      LOG_INFO("Stopping music playback.");
      setMusicPlay(0);

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         *should_respond = 0;
         return NULL;
      } else {
         strcpy(return_buffer, "Music playback stopped");
         return return_buffer;
      }
   } else if (strcmp(actionName, "next") == 0) {
      if (music_thread != -1) {
         setMusicPlay(0);
         int join_result = pthread_join(music_thread, NULL);
         if (join_result == 0) {
            music_thread = -1;
         }
      }

      if (playlist.count > 0) {
         current_track++;
         if (current_track >= playlist.count) {
            current_track = 0;
         }

         args.sink_name = getPcmPlaybackDevice();
         args.file_name = playlist.filenames[current_track];
         args.start_time = 0;       /* For now set to zero. We may support other modes later. */

         LOG_INFO("Playing: %s %s %d", args.sink_name, args.file_name, args.start_time);

         // Create the playback thread
         if (pthread_create(&music_thread, NULL, playFlacAudio, &args)) {
            LOG_ERROR("Error creating music thread");

            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               strcpy(return_buffer, "Failed to play next track");
               return return_buffer;
            }
         }

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            // Extract just the filename from the full path
            const char *filename = strrchr(playlist.filenames[current_track], '/');
            if (filename) {
               filename++; // Skip the '/'
            } else {
               filename = playlist.filenames[current_track];
            }
            snprintf(return_buffer, sizeof(return_buffer),
                     "Playing next track: %s", filename);
            return return_buffer;
         }
      } else {
         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            strcpy(return_buffer, "No playlist available");
            return return_buffer;
         }
      }
   } else if (strcmp(actionName, "previous") == 0) {
      if (music_thread != -1) {
         setMusicPlay(0);
         int join_result = pthread_join(music_thread, NULL);
         if (join_result == 0) {
            music_thread = -1;
         }
      }

      if (playlist.count > 0) {
         current_track--;
         if (current_track < 0) {
            current_track = playlist.count - 1;
         }

         args.sink_name = getPcmPlaybackDevice();
         args.file_name = playlist.filenames[current_track];
         args.start_time = 0;       /* For now set to zero. We may support other modes later. */

         LOG_INFO("Playing: %s %s %d", args.sink_name, args.file_name, args.start_time);

         // Create the playback thread
         if (pthread_create(&music_thread, NULL, playFlacAudio, &args)) {
            LOG_ERROR("Error creating music thread");

            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               strcpy(return_buffer, "Failed to play previous track");
               return return_buffer;
            }
         }

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            // Extract just the filename from the full path
            const char *filename = strrchr(playlist.filenames[current_track], '/');
            if (filename) {
               filename++; // Skip the '/'
            } else {
               filename = playlist.filenames[current_track];
            }
            snprintf(return_buffer, sizeof(return_buffer),
                     "Playing previous track: %s", filename);
            return return_buffer;
         }
      } else {
         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            strcpy(return_buffer, "No playlist available");
            return return_buffer;
         }
      }
   }

   return NULL;
}

char* voiceAmplifierCallback(const char *actionName, char *value, int *should_respond) {
   static char return_buffer[256];

   *should_respond = 1;

   if (strcmp(actionName, "enable") == 0) {
      if ((voice_thread != -1) && (pthread_kill(voice_thread, 0) == 0)) {
         LOG_WARNING("Voice amplification thread already running.");

         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            strcpy(return_buffer, "Voice amplifier is already enabled");
            return return_buffer;
         }
         *should_respond = 0;
         return NULL;
      }

      // Create the playback thread
      if (pthread_create(&voice_thread, NULL, voiceAmplificationThread, NULL)) {
         LOG_ERROR("Error creating voice thread");

         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            strcpy(return_buffer, "Failed to enable voice amplifier");
            return return_buffer;
         }
         *should_respond = 0;
         return NULL;
      }

      if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
         strcpy(return_buffer, "Voice amplifier enabled");
         return return_buffer;
      }
      *should_respond = 0;
      return NULL;

   } else if (strcmp(actionName, "disable") == 0) {
      if ((voice_thread != -1) && (pthread_kill(voice_thread, 0) == 0)) {
         setStopVA();

         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            strcpy(return_buffer, "Voice amplifier disabled");
            return return_buffer;
         }
      } else {
         LOG_WARNING("Voice amplification thread not running.");

         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            strcpy(return_buffer, "Voice amplifier was not running");
            return return_buffer;
         }
      }
      *should_respond = 0;
      return NULL;
   }

   return NULL;
}

// Shutdown callback
char* shutdownCallback(const char *actionName, char *value, int *should_respond) {
   static char return_buffer[256];

   *should_respond = 1;

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      system("sudo shutdown -h now");
      int local_should_respond = 0;
      textToSpeechCallback(NULL, "Emergency shutdown initiated.", &local_should_respond);
      return NULL;
   } else {
      // In AI modes, confirm before shutting down
      strcpy(return_buffer, "Shutdown command received. Initiating emergency shutdown.");
      // Still execute the shutdown
      system("sudo shutdown -h now");
      return return_buffer;
   }
}

/**
 * Reads the entire contents of a file into memory.
 *
 * @param filename The path to the file to be read.
 * @param length Pointer to a size_t variable where the size of the file will be stored.
 * @return A pointer to the allocated memory containing the file's contents. The caller
 *         is responsible for freeing this memory. Returns NULL on failure.
 */
unsigned char *read_file(const char *filename, size_t *length) {
   *length = 0; // Ensure length is set to 0 initially
   FILE *file = fopen(filename, "rb");
   if (!file) {
      LOG_ERROR("File opening failed: %s", filename);
      return NULL;
   }

   fseek(file, 0, SEEK_END);
   long size = ftell(file);
   if (size == -1) {
      LOG_ERROR("Failed to determine file size: %s", filename);
      fclose(file);
      return NULL;
   }
   *length = (size_t)size;
   LOG_INFO("The image file is %ld bytes.\n", *length);
   fseek(file, 0, SEEK_SET);

   unsigned char *content = malloc(*length);
   if (!content) {
      LOG_ERROR("Memory allocation failed");
      fclose(file);
      return NULL;
   }

   size_t read_length = fread(content, 1, *length, file);
   if (*length != read_length) {
      LOG_ERROR("Failed to read the total size. Expected: %ld, Read: %ld", *length, read_length);
      free(content);
      fclose(file);
      return NULL;
   }

   fclose(file);
   return content;
}

/**
 * Encodes data using Base64 encoding.
 *
 * @param buffer Pointer to the data to be encoded.
 * @param length Length of the data to encode.
 * @return A pointer to the null-terminated Base64 encoded string, or NULL if an error occurred.
 *         The caller is responsible for freeing this memory.
 */
char *base64_encode(const unsigned char *buffer, size_t length) {
   if (buffer == NULL || length <= 0) {
      LOG_ERROR("Invalid input to base64_encode.");
      return NULL;
   }

   BIO *bio = NULL, *b64 = NULL;
   BUF_MEM *bufferPtr = NULL;

   // Create a new BIO for Base64 encoding.
   b64 = BIO_new(BIO_f_base64());
   if (b64 == NULL) {
      LOG_ERROR("Failed to create Base64 BIO.");
      return NULL;
   }

   // Create a new BIO that holds data in memory.
   bio = BIO_new(BIO_s_mem());
   if (bio == NULL) {
      LOG_ERROR("Failed to create memory BIO.");
      BIO_free_all(b64); // Ensure cleanup
      return NULL;
   }

   // Chain the base64 BIO onto the memory BIO.
   // This means that data written to 'bio' will first be Base64 encoded, then stored in memory.
   bio = BIO_push(b64, bio);

   // Set the flag to not use newlines as BIO_FLAGS_BASE64_NO_NL implies.
   // This affects the Base64 encoding to output all data in one continuous line.
   BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

   // Write the input buffer into the BIO chain.
   // This data gets Base64 encoded by 'b64', then stored in the memory managed by 'bio'.
   if (BIO_write(bio, buffer, length) <= 0) {
      LOG_ERROR("Failed to write data to BIO.");
      BIO_free_all(bio); // Also frees 'b64' since it's pushed onto 'bio'
      return NULL;
   }

   // Ensure all data is flushed through the BIO chain and any encoding is completed.
   if (BIO_flush(bio) <= 0) {
      LOG_ERROR("Failed to flush BIO.");
      BIO_free_all(bio);
      return NULL;
   }

   // Retrieve a pointer to the memory BIO's data.
   // This does not remove the data from the BIO, but allows access to it.
   BIO_get_mem_ptr(bio, &bufferPtr);
   if (bufferPtr == NULL || bufferPtr->data == NULL) {
      LOG_ERROR("Failed to get pointer to BIO memory.");
      BIO_free_all(bio);
      return NULL;
   }

   // Allocate memory for the output text and copy the Base64 encoded data.
   // Note: 'bufferPtr->length' contains the length of the Base64 encoded data.
   char *b64text = malloc(bufferPtr->length + 1);
   if (b64text == NULL) {
      LOG_ERROR("Memory allocation failed for Base64 text.");
      BIO_free_all(bio);
      return NULL;
   }

   memcpy(b64text, bufferPtr->data, bufferPtr->length);
   b64text[bufferPtr->length] = '\0'; // Null-terminate the Base64 encoded string.

   // Free the entire BIO chain, automatically freeing both 'b64' and 'bio'.
   BIO_free_all(bio);

   return b64text;
}

char* viewingCallback(const char *actionName, char *value, int *should_respond) {
   size_t image_size = 0;
   static char return_buffer[256];

   *should_respond = 1;  // Always respond for viewing

   LOG_INFO("Viewing image received: %s", value);

   // Read the image file into memory.
   unsigned char *image_content = read_file(value, &image_size);
   if (image_content != NULL) {
      // Encode the image content into Base64.
      char *base64_image = base64_encode(image_content, image_size);
      if (base64_image) {
         // Process the Base64-encoded image for vision AI tasks.
         process_vision_ai(base64_image, strlen(base64_image) + 1);

         free(base64_image);
      }
      free(image_content);

      if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
         strcpy(return_buffer, "Image captured and ready for vision processing");
         return return_buffer;
      }
   } else {
      LOG_ERROR("Error reading image file.");

      if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
         snprintf(return_buffer, sizeof(return_buffer),
                  "Failed to read image file: %s", value);
         return return_buffer;
      }
   }

   *should_respond = 0;  // In direct mode, no response needed
   return NULL;
}

char* volumeCallback(const char *actionName, char *value, int *should_respond) {
   static char return_buffer[256];
   float floatVol = wordToNumber(value);

   LOG_INFO("Music volume: %s/%0.2f", value, floatVol);

   if (floatVol >= 0 && floatVol <= 2.0) {
      setMusicVolume(floatVol);

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         *should_respond = 0;  // No response in direct mode for volume
         return NULL;
      } else {
         // AI modes: return confirmation
         snprintf(return_buffer, sizeof(return_buffer),
                  "Music volume set to %.1f", floatVol);
         *should_respond = 1;
         return return_buffer;
      }
   } else {
      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         int local_should_respond = 0;
         textToSpeechCallback(NULL, "Invalid volume level requested.", &local_should_respond);
         *should_respond = 0;
         return NULL;
      } else {
         snprintf(return_buffer, sizeof(return_buffer),
                  "Invalid volume level %.1f requested. Volume must be between 0 and 2.", floatVol);
         *should_respond = 1;
         return return_buffer;
      }
   }
}

char* localLLMCallback(const char *actionName, char *value, int *should_respond) {
   static char return_buffer[256];

   LOG_INFO("Setting AI to local LLM.");
   setLLM(LOCAL_LLM);

   // Always return string for AI modes (ignored in DIRECT_ONLY)
   strcpy(return_buffer, "AI switched to local LLM");
   *should_respond = 1;
   return return_buffer;
}

char* cloudLLMCallback(const char *actionName, char *value, int *should_respond) {
   static char return_buffer[256];

   LOG_INFO("Setting AI to cloud LLM.");
   setLLM(CLOUD_LLM);

   // Always return string for AI modes (ignored in DIRECT_ONLY)
   strcpy(return_buffer, "AI switched to cloud LLM");
   *should_respond = 1;
   return return_buffer;
}
/* End Mosquitto Stuff */

