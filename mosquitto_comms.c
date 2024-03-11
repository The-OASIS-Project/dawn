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
 * All contributions to this project are agreed to be licensed under the
 * GPLv3 or any later version. Contributions are understood to be
 * any modifications, enhancements, or additions to the project
 * and become the property of the original author Kris Kersey.
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

/* Mosquitto */
#include <mosquitto.h>

/* JSON-C */
#include <json-c/json.h>

/* OpenSSL */
#include <openssl/buffer.h>
#include <openssl/evp.h>

/* Local */
#include "dawn.h"
#include "flac_playback.h"
#include "mic_passthrough.h"
#include "mosquitto_comms.h"

#define MAX_FILENAME_LENGTH 1024
#define MAX_PLAYLIST_LENGTH 100

/**
 * Array of device callbacks associating device types with their respective handling functions.
 * This facilitates dynamic invocation of actions based on the device type, enhancing the application's
 * modularity and scalability.
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
   {VIEWING, viewingCallback}
};

static pthread_t music_thread = -1;
static pthread_t voice_thread = -1;

/**
 * Retrieves the current user's home directory.
 *
 * @return A pointer to a string containing the path to the home directory. This string
 *         should not be modified or freed by the caller, as it points to an environment variable.
 */
const char* getUserHomeDirectory() {
   const char* homeDir = getenv("HOME");
   if (!homeDir) {
      fprintf(stderr, "Error: HOME environment variable not set.\n");
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
      fprintf(stderr, "Error: Memory allocation failed for full path.\n");

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

void searchDirectory(const char *rootDir, const char *pattern, Playlist *playlist) {
   DIR *dir = opendir(rootDir);
   if (!dir) {
      perror("Error opening directory");
      return;
   }

   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL) {
      if (entry->d_type == DT_REG) { // Regular file
         if (playlist->count >= MAX_PLAYLIST_LENGTH) {
            fprintf(stderr, "Playlist is full\n");
            closedir(dir);
            return;
         }

         char filePath[MAX_FILENAME_LENGTH];
         snprintf(filePath, sizeof(filePath), "%s/%s", rootDir, entry->d_name);

         if (fnmatch(pattern, entry->d_name, FNM_CASEFOLD) == 0) {
            strcpy(playlist->filenames[playlist->count], filePath);
            playlist->count++;
         }
      } else if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) { // Directory
         char subPath[MAX_FILENAME_LENGTH];
         snprintf(subPath, sizeof(subPath), "%s/%s", rootDir, entry->d_name);
         searchDirectory(subPath, pattern, playlist);
      }
   }

   closedir(dir);
}

void parseJsonCommandandExecute(const char *input)
{
   struct json_object *parsedJson = NULL;
   struct json_object *deviceObject = NULL;
   struct json_object *actionObject = NULL;
   struct json_object *valueObject = NULL;

   const char *deviceName = NULL;
   const char *actionName = NULL;
   const char *value = NULL;

   int i = 0;

   // Parse the JSON data
   parsedJson = json_tokener_parse(input);
   if (parsedJson == NULL) {
      fprintf(stderr, "Error: Unable to process mqtt command.\n");

      return;
   }

   // Get the "device" object from the JSON
   if (json_object_object_get_ex(parsedJson, "device", &deviceObject)) {
      // Extract the text value as a C string
      deviceName = json_object_get_string(deviceObject);
      if (deviceName == NULL) {
         fprintf(stderr, "Error: Unable to get device name from json command.\n");
         json_object_put(parsedJson);
         return;
      }
   } else {
      fprintf(stderr, "Error: 'device' field not found in JSON.\n");
      json_object_put(parsedJson);

      return;
   }

   // Get the "action" object from the JSON
   if (json_object_object_get_ex(parsedJson, "action", &actionObject)) {
      // Extract the text value as a C string
      actionName = json_object_get_string(actionObject);
      if (actionName == NULL) {
         fprintf(stderr, "Error: Unable to get action name from json command.\n");
         json_object_put(parsedJson);
         return;
      }
   } else {
      fprintf(stderr, "Error: 'action' field not found in JSON.\n");
      json_object_put(parsedJson);

      return;
   }

   // Get the "value" object from the JSON, not required for all commands
   if (json_object_object_get_ex(parsedJson, "value", &valueObject)) {
      // Extract the text value as a C string
      value = json_object_get_string(valueObject);
      if (value == NULL) {
         fprintf(stderr, "Notice: Unable to get value name from json command.\n");
      }
   } else {
      fprintf(stderr, "Notice: 'value' field not found in JSON.\n");
   }

   /* Loop through device names for device types. */
   for (i = 0; i < MAX_DEVICE_TYPES; i++) {
      if (strcmp(deviceName, deviceTypeStrings[i]) == 0) {
         if (deviceCallbackArray[i].callback != NULL)
         {
            deviceCallbackArray[i].callback(actionName, (void *) value);
         } else {
            printf("Skipping callback, value NULL.\n");
         }
      }
   }

   // Cleanup: Release the parsed_json object
   json_object_put(parsedJson);
}

/* Mosquitto STUFF */
/* Callback called when the client receives a CONNACK message from the broker. */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code)
{
   int rc;

   printf("MQTT Connecting.\n");

   if(reason_code != 0){
      printf("MQTT disconnecting?\n");
      mosquitto_disconnect(mosq);
      return;
   }
}

/* Callback called when the broker sends a SUBACK in response to a SUBSCRIBE. */
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos)
{
	int i;
	bool have_subscription = false;

   printf("MQTT subscribed.\n");

	for(i=0; i<qos_count; i++){
		if(granted_qos[i] <= 2){
			have_subscription = true;
		}
	}
	if(have_subscription == false){
		fprintf(stderr, "Error: All subscriptions rejected.\n");
		mosquitto_disconnect(mosq);
	}
}

/* Callback called when the client receives a message. */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
	printf("%s %d %s\n", msg->topic, msg->qos, (char *)msg->payload);

   parseJsonCommandandExecute((char *)msg->payload);
}

void dateCallback(const char *actionName, char *value) {
   time_t current_time;
   struct tm *time_info;
   char buffer[80];
   int choice;

   time(&current_time);
   time_info = localtime(&current_time);
   srand(time(NULL));
   choice = rand() % 3;

   switch (choice) {
      case 0:
         strftime(buffer, sizeof(buffer), "Today's date, dear Sir, is %A, %B %d, %Y. You're welcome.", time_info);
         break;
      case 1:
         strftime(buffer, sizeof(buffer), "In case you've forgotten, Sir, it's %A, %B %d, %Y today.", time_info);
         break;
      case 2:
         strftime(buffer, sizeof(buffer), "The current date is %A, %B %d, %Y.", time_info);
         break;
   }

   textToSpeechCallback(NULL, buffer);
}

void timeCallback(const char *actionName, char *value) {
   time_t current_time;
   struct tm *time_info;
   char buffer[80];
   int choice;

   time(&current_time);
   time_info = localtime(&current_time);
   srand(time(NULL));
   choice = rand() % 4;

   switch (choice) {
      case 0:
         strftime(buffer, sizeof(buffer), "The current time, in case your wristwatch has failed you, is %I:%M %p.", time_info);
         break;
      case 1:
         strftime(buffer, sizeof(buffer), "I trust you have something important planned, Sir? It's %I:%M %p.", time_info);
         break;
      case 2:
         strftime(buffer, sizeof(buffer), "Oh, you want to know the time again? It's %I:%M %p, not that I'm keeping track.", time_info);
         break;
      case 3:
         strftime(buffer, sizeof(buffer), "The time is %I:%M %p.", time_info);
         break;
   }

   textToSpeechCallback(NULL, buffer);
}

// Custom comparison function for qsort
int compare(const void *p1, const void *p2) {
    printf("Comparing %s to %s\n", (char *)p1, (char *)p2);
    return strcmp((char *)p1, (char *)p2);
}

void musicCallback(const char *actionName, char *value) {
   PlaybackArgs args;
   char strWildcards[MAX_FILENAME_LENGTH];
   int i = 0;

   if (strcmp(actionName, "play") == 0) {
      if ((music_thread != -1) && (pthread_kill(music_thread, 0) == 0)) {
         setMusicPlay(0);
         pthread_join(music_thread, NULL);
      }

      playlist.count = 0;
      current_track = 0;

      if ((strlen(value) + 8) > MAX_FILENAME_LENGTH) {
         fprintf(stderr, "\"%s\" is too long to search for.\n", value);

         return;
      }

      // Construct the full path to the user's music directory
      char* musicDir = constructPathWithSubdirectory(MUSIC_DIR);
      if (!musicDir) {
         fprintf(stderr, "Error constructing music path.\n");

         return;
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

      printf("Original pattern: \"%s\", New pattern: \"%s\"\n", value, strWildcards);
      searchDirectory(musicDir, strWildcards, &playlist);

      free(musicDir); // free the allocated memory

      // Sort the array using qsort
      qsort(playlist.filenames, playlist.count, MAX_FILENAME_LENGTH, compare);

      printf("New playlist:\n");
      for (i = 0; i < playlist.count; i++) {
         printf("%s\n", playlist.filenames[i]);
      }

      if (playlist.count > 0) {
         args.sink_name = getPcmPlaybackDevice();
         args.file_name = playlist.filenames[current_track];
         args.start_time = 0;       /* For now set to zero. We may support other modes later. */

         printf("Playing: %s %s %d\n", args.sink_name, args.file_name, args.start_time);

         // Create the playback thread
         if (pthread_create(&music_thread, NULL, playFlacAudio, &args)) {
            fprintf(stderr, "Error creating thread\n");
            return;
         }
      } else {
         printf("No music matching that description was found.\n");
      }
   } else if (strcmp(actionName, "stop") == 0) {
      printf("Stopping music playback.\n");
      setMusicPlay(0);
   } else if (strcmp(actionName, "next") == 0) {
      if ((music_thread != -1) && (pthread_kill(music_thread, 0) == 0)) {
         setMusicPlay(0);
         pthread_join(music_thread, NULL);
      }

      if (playlist.count > 0) {
         current_track++;
         if (current_track >= playlist.count) {
            current_track = 0;
         }

         args.sink_name = getPcmPlaybackDevice();
         args.file_name = playlist.filenames[current_track];
         args.start_time = 0;       /* For now set to zero. We may support other modes later. */

         printf("Playing: %s %s %d\n", args.sink_name, args.file_name, args.start_time);

         // Create the playback thread
         if (pthread_create(&music_thread, NULL, playFlacAudio, &args)) {
            fprintf(stderr, "Error creating music thread\n");
            return;
         }
      }
   } else if (strcmp(actionName, "previous") == 0) {
      if ((music_thread != -1) && (pthread_kill(music_thread, 0)) == 0) {
         setMusicPlay(0);
         pthread_join(music_thread, NULL);
      }

      if (playlist.count > 0) {
         current_track--;
         if (current_track < 0) {
            current_track = playlist.count - 1;
         }

         args.sink_name = getPcmPlaybackDevice();
         args.file_name = playlist.filenames[current_track];
         args.start_time = 0;       /* For now set to zero. We may support other modes later. */

         printf("Playing: %s %s %d\n", args.sink_name, args.file_name, args.start_time);

         // Create the playback thread
         if (pthread_create(&music_thread, NULL, playFlacAudio, &args)) {
            fprintf(stderr, "Error creating music thread\n");
            return;
         }
      }
   }
}

void voiceAmplifierCallback(const char *actionName, char *value) {
   if (strcmp(actionName, "enable") == 0) {
      if ((voice_thread != -1) && (pthread_kill(voice_thread, 0) == 0)) {
         printf("Voice amplificiation thread already running.\n");
         return;
      }

      // Create the playback thread
      if (pthread_create(&voice_thread, NULL, voiceAmplificationThread, NULL)) {
         fprintf(stderr, "Error creating voice thread\n");
         return;
      }
   } else if (strcmp(actionName, "disable") == 0) {
      if ((voice_thread != -1) && (pthread_kill(voice_thread, 0) == 0)) {
         setStopVA();
      } else {
         printf("Voice amplificiation thread not running.\n");
      }
   }
}

void shutdownCallback(const char *actionName, char *value) {
   system("sudo shutdown -h now");
   textToSpeechCallback(NULL, "Emergency shutdown initiated.");
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
      perror("File opening failed");
      return NULL;
   }

   fseek(file, 0, SEEK_END);
   long size = ftell(file);
   if (size == -1) {
      perror("Failed to determine file size");
      fclose(file);
      return NULL;
   }
   *length = (size_t)size;
   printf("The image file is %ld bytes.\n", *length);
   fseek(file, 0, SEEK_SET);

   unsigned char *content = malloc(*length);
   if (!content) {
      perror("Memory allocation failed");
      fclose(file);
      return NULL;
   }

   size_t read_length = fread(content, 1, *length, file);
   if (*length != read_length) {
      fprintf(stderr, "Failed to read the total size. Expected: %ld, Read: %ld\n", *length, read_length);
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
      fprintf(stderr, "Invalid input to base64_encode.\n");
      return NULL;
   }

   BIO *bio = NULL, *b64 = NULL;
   BUF_MEM *bufferPtr = NULL;

   // Create a new BIO for Base64 encoding.
   b64 = BIO_new(BIO_f_base64());
   if (b64 == NULL) {
      fprintf(stderr, "Failed to create Base64 BIO.\n");
      return NULL;
   }

   // Create a new BIO that holds data in memory.
   bio = BIO_new(BIO_s_mem());
   if (bio == NULL) {
      fprintf(stderr, "Failed to create memory BIO.\n");
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
      fprintf(stderr, "Failed to write data to BIO.\n");
      BIO_free_all(bio); // Also frees 'b64' since it's pushed onto 'bio'
      return NULL;
   }

   // Ensure all data is flushed through the BIO chain and any encoding is completed.
   if (BIO_flush(bio) <= 0) {
      fprintf(stderr, "Failed to flush BIO.\n");
      BIO_free_all(bio);
      return NULL;
   }

   // Retrieve a pointer to the memory BIO's data.
   // This does not remove the data from the BIO, but allows access to it.
   BIO_get_mem_ptr(bio, &bufferPtr);
   if (bufferPtr == NULL || bufferPtr->data == NULL) {
      fprintf(stderr, "Failed to get pointer to BIO memory.\n");
      BIO_free_all(bio);
      return NULL;
   }

   // Allocate memory for the output text and copy the Base64 encoded data.
   // Note: 'bufferPtr->length' contains the length of the Base64 encoded data.
   char *b64text = malloc(bufferPtr->length + 1);
   if (b64text == NULL) {
      fprintf(stderr, "Memory allocation failed for Base64 text.\n");
      BIO_free_all(bio);
      return NULL;
   }

   memcpy(b64text, bufferPtr->data, bufferPtr->length);
   b64text[bufferPtr->length] = '\0'; // Null-terminate the Base64 encoded string.

   // Free the entire BIO chain, automatically freeing both 'b64' and 'bio'.
   BIO_free_all(bio);

   return b64text;
}

/**
 * Callback function to handle the viewing of an image. It reads the specified image file,
 * encodes its content into Base64, and passes the encoded data for vision AI processing.
 *
 * @param actionName The name of the action triggering this callback. Not used in this function,
 *                   but included to match expected callback signature.
 * @param value The file path to the image to be viewed and processed.
 */
void viewingCallback(const char *actionName, char *value) {
   size_t image_size = 0;

   printf("Viewing image received: %s\n", value);

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
   } else {
      printf("Error reading image file.\n");
   }
}
/* End Mosquitto Stuff */

