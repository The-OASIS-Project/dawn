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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include "text_to_speech.h"
#include "secrets.h"
#include "dawn.h"

/**
 * @brief A structure to manage dynamic memory as a buffer.
 *
 * This is commonly used to collect data chunks received by CURL's write callback functions.
 * The structure is designed to be expandable to accommodate data of unknown size, such as
 * data received from a network operation.
 *
 * @see https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
 */
struct MemoryStruct {
   char *memory; /**< Pointer to the dynamically allocated buffer */
   size_t size;  /**< Current size of the buffer */
};

/**
 * @brief Callback for writing data to a MemoryStruct buffer.
 *
 * This function is intended to be used with libcurl's CURLOPT_WRITEFUNCTION option.
 * Whenever libcurl receives data that is to be saved, this function is called. It
 * reallocates the MemoryStruct's memory block to fit the new piece of data, ensuring
 * that the data is concatenated properly within the buffer.
 *
 * @param contents Pointer to the data libcurl has ready for us.
 * @param size The size of the data in the block, always 1.
 * @param nmemb Number of blocks to write, each of size 'size'.
 * @param userp Pointer to a MemoryStruct structure where the data should be stored.
 *
 * @return The number of bytes actually taken care of. If that amount differs from the
 * amount passed to your function, it'll signal an error to the libcurl. Returning 0
 * will signal an out-of-memory error.
 */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
   size_t realsize = size * nmemb;
   struct MemoryStruct *mem = (struct MemoryStruct *)userp;

   mem->memory = realloc(mem->memory, mem->size + realsize + 1);
   if (mem->memory == NULL) {
      printf("Not enough memory (realloc returned NULL)\n");
      return 0;
   }

   memcpy(&(mem->memory[mem->size]), contents, realsize);
   mem->size += realsize;
   mem->memory[mem->size] = 0;

   return realsize;
}

/**
 * @brief Check internet connection with timeout.
 *
 * This function checks the availability of an internet connection by attempting to
 * establish a connection to the specified URL within a given timeout period.
 *
 * @param url The URL to check for connection.
 * @param timeout_seconds The timeout value in seconds for the connection attempt.
 * @return 1 if a connection is successfully established within the timeout, 0 otherwise.
 *
 * @note This function uses non-blocking socket techniques and the select() function
 *       to provide a timeout mechanism for the connection attempt.
 */
int checkInternetConnectionWithTimeout(const char *url, int timeout_seconds) {
    struct hostent *host = gethostbyname(url);
    if (host == NULL) {
        return 0;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        return 0;
    }

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(80);
    server.sin_addr = *((struct in_addr *)host->h_addr);

    // Set socket as non-blocking
    fcntl(sock, F_SETFL, O_NONBLOCK);

    int result = connect(sock, (struct sockaddr *)&server, sizeof(server));
    if (result == -1) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);

        struct timeval timeout;
        timeout.tv_sec = timeout_seconds;
        timeout.tv_usec = 0;

        result = select(sock + 1, NULL, &write_fds, NULL, &timeout);
        if (result == 1) {
            int error;
            socklen_t error_len = sizeof(error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_len);
            if (error == 0) {
                result = 0;  // Connection successful
            }
        }
    }

    close(sock);

    return (result == 0);
}

/**
 * @brief Get a response using the GPT-based conversational model.
 *
 * This function takes the conversation history and input text, and generates a response
 * using the GPT-based conversational model. It does not modify the provided inputs.
 *
 * @param conversation_history A JSON object representing the conversation history.
 * @param input_text The input text for which a response is to be generated.
 * @return A pointer to the response text generated by the model. The memory for the response
 *         text is allocated by the function and must be freed by the caller.
 */
char *getGptResponse(struct json_object *conversation_history, char *input_text,
                     char *vision_ai_image, size_t vision_ai_image_size)
{
   CURL *curl_handle = NULL;  /* Handle for curl library. */
   CURLcode res = -1;
   struct curl_slist *headers = NULL;

   struct MemoryStruct chunk;

   const char *payload = NULL;
   char *response = NULL;
   int total_tokens = 0;

   json_object *root = NULL;

   json_object *user_message = NULL;

   json_object *parsed_json = NULL;
   json_object *choices = NULL;
   json_object *first_choice = NULL;
   json_object *message = NULL;
   json_object *content = NULL;
   json_object *finish_details = NULL;
   json_object *finish_reason = NULL;
   json_object *type = NULL;
   json_object *usage_obj = NULL;
   json_object *total_tokens_obj = NULL;

   // Root JSON Container
   root = json_object_new_object();

   // ChatGPT Model
   json_object_object_add(root, "model", json_object_new_string(OPENAI_MODEL));

   // Messages
   user_message = json_object_new_object();
   json_object_object_add(user_message, "role", json_object_new_string("user"));

#ifdef OPENAI_VISION
   json_object *content_array = json_object_new_array();
   json_object *text_obj = json_object_new_object();
   json_object_object_add(text_obj, "type", json_object_new_string("text"));
   json_object_object_add(text_obj, "text", json_object_new_string(input_text));
   json_object_array_add(content_array, text_obj);

   json_object *image_obj = json_object_new_object();
   json_object *image_url_obj = json_object_new_object();
   if (vision_ai_image != NULL && vision_ai_image_size > 0) {
      json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

      // Construct the full data URI for the image
      char *data_uri_prefix = "data:image/jpeg;base64,";
      size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_ai_image) + 1; // +1 for the null terminator
      char *data_uri = malloc(data_uri_length);

      if (data_uri != NULL) {
         snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_ai_image);

         // Now use data_uri in the JSON object construction
         json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));

         // Free the data_uri after it's no longer needed
         free(data_uri);
      } else {
         // Handle memory allocation failure
         fprintf(stderr, "Failed to allocate memory for data URI.\n");
      }

      json_object_object_add(image_obj, "image_url", image_url_obj);
      json_object_array_add(content_array, image_obj);
   }

   json_object_object_add(user_message, "content", content_array);
#else
   json_object_object_add(user_message, "content", json_object_new_string(input_text));
#endif
   json_object_array_add(conversation_history, user_message);
   json_object_object_add(root, "messages", conversation_history);

   // Max Tokens
   json_object_object_add(root, "max_tokens", json_object_new_int(GPT_MAX_TOKENS));

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
   printf("JSON Payload (PLAIN):\n%s\n", payload);

   chunk.memory = malloc(1);
   if (chunk.memory == NULL) {
      printf("Error allocating memory!\n");

      return NULL;
   }
   chunk.size = 0;

   if (checkInternetConnectionWithTimeout("https://api.openai.com", 4)) {
      printf("URL did not return. Unavailable.\n");

      return NULL;
   }

   curl_handle = curl_easy_init();
   if (curl_handle) {
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, OPENAI_HEADER);

      curl_easy_setopt(curl_handle, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

      res = curl_easy_perform(curl_handle);
      if (res != CURLE_OK) {
         fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
      }

      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
   }

   printf("Raw receive from ChatGPT: %s\n", (char *) chunk.memory);

   parsed_json = json_tokener_parse(chunk.memory);
   if (!parsed_json) {
      fprintf(stderr, "Failed to parse JSON response.\n");
      free(chunk.memory);
      return NULL;
   }

   if (!json_object_object_get_ex(parsed_json, "choices", &choices) ||
      json_object_get_type(choices) != json_type_array ||
      json_object_array_length(choices) < 1) {
      fprintf(stderr, "Error in parsing response: 'choices' missing or invalid.\n");
      json_object_put(parsed_json); // Correctly free JSON object
      free(chunk.memory);
      return NULL;
   }

   first_choice = json_object_array_get_idx(choices, 0);
   if (!first_choice) {
      fprintf(stderr, "Error: 'choices' array is empty.\n");
      json_object_put(parsed_json);
      free(chunk.memory);
      return NULL;
   }

   if (!json_object_object_get_ex(first_choice, "message", &message) ||
      !json_object_object_get_ex(message, "content", &content)) {
      fprintf(stderr, "Error: 'message' or 'content' field missing.\n");
      json_object_put(parsed_json);
      free(chunk.memory);
      return NULL;
   }

   // Optional: Safely access 'finish_reason'
   json_object_object_get_ex(first_choice, "finish_reason", &finish_reason);

   if (!json_object_object_get_ex(parsed_json, "usage", &usage_obj) ||
      !json_object_object_get_ex(usage_obj, "total_tokens", &total_tokens_obj)) {
      fprintf(stderr, "Error: 'usage' or 'total_tokens' field missing.\n");
      json_object_put(parsed_json);
      free(chunk.memory);
      return NULL;
   }

   total_tokens = json_object_get_int(total_tokens_obj);
   printf("Total tokens: %d\n", total_tokens);

   // Duplicate the response content string safely
   const char* content_str = json_object_get_string(content);
   if (!content_str) {
      fprintf(stderr, "Error: 'content' field is empty or not a string.\n");
      json_object_put(parsed_json);
      free(chunk.memory);
      return NULL;
   }
   response = strdup(content_str);

   if ((finish_reason != NULL) && (strcmp(json_object_get_string(finish_reason), "stop") != 0))
   {
#if 0 /* They change this API, so for now, remove the reason. */
      if (strcmp(json_object_get_string(type), "length") == 0)
      {
         printf("GPT returned prematurely due to token length.\n");
      } else {
         printf("GPT returned prematurely due to \"%s\".\n",
                json_object_get_string(type));
      }
#endif
   } else {
      printf("Response finished properly.\n");
   }

   json_object_put(parsed_json);
   free(chunk.memory);

   return response;
}
