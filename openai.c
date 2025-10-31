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

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <curl/curl.h>
#include <json-c/json.h>

#include "dawn.h"
#include "logging.h"
#include "openai.h"
#include "secrets.h"
#include "text_to_speech.h"

extern command_processing_mode_t command_processing_mode;

/**
 * @brief A static buffer to store the URL of the selected LLM service.
 *
 * This buffer stores the URL for the currently selected LLM,
 * either cloud or local. Its maximum length is 2048 characters.
 */
static char llm_url[2048];

void setLLM(llm_t type) {
   switch (type) {
      case CLOUD_LLM:
         snprintf(llm_url, 2048, "%s", CLOUDAI_URL);
         break;
      case LOCAL_LLM:
         snprintf(llm_url, 2048, "%s", LOCALAI_URL);
         break;
      default:
         LOG_ERROR("Unknown LLM type requested");
   }
}

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
      LOG_ERROR("Not enough memory (realloc returned NULL)");
      return 0;
   }

   memcpy(&(mem->memory[mem->size]), contents, realsize);
   mem->size += realsize;
   mem->memory[mem->size] = 0;

   return realsize;
}

/**
 * @brief Extracts the host and port from a URL, removing protocol and paths.
 *
 * This function handles stripping protocols (http, https) and extracts the host and port from URLs.
 * If no port is provided, it defaults to port 80 for http and 443 for https.
 *
 * @param url The input URL string.
 * @param host Output buffer to store the extracted host (must be pre-allocated).
 * @param port Output buffer to store the extracted port (must be large enough for the port number).
 * @return int Returns 0 on success, -1 on failure.
 */
int extract_host_and_port(const char *url, char *host, char *port) {
   // Validate the input arguments
   if (url == NULL || host == NULL || port == NULL) {
      LOG_ERROR("Error: NULL argument passed to extract_host_and_port.");
      return -1;
   }

   if (strlen(url) == 0) {
      LOG_ERROR("Error: Empty URL provided.");
      return -1;
   }

   const char *start = url;

   // Determine protocol and set default port
   if (strncmp(url, "http://", 7) == 0) {
      start = url + 7;  // Skip "http://"
      strcpy(port, "80");  // Default port for http
   } else if (strncmp(url, "https://", 8) == 0) {
      start = url + 8;  // Skip "https://"
      strcpy(port, "443");  // Default port for https
   } else {
      // If no recognizable protocol, assume http and continue
      strcpy(port, "80");
   }

   // Find the end of the host part (either ':' for port or '/' for path)
   const char *end = strpbrk(start, ":/");
   if (end == NULL) {
      // No port or path, the host is the entire remaining string
      strcpy(host, start);
   } else if (*end == ':') {
      // Extract the host and port
      strncpy(host, start, end - start);
      host[end - start] = '\0';  // Null-terminate the host
      strcpy(port, end + 1);     // Port starts after ':'
   } else {
      // Extract the host only (no port, but has a path)
      strncpy(host, start, end - start);
      host[end - start] = '\0';  // Null-terminate the host
   }

   return 0;
}

int checkInternetConnectionWithTimeout(const char *url, int timeout_seconds) {
   char host[2048];
   char port[6];

   // Extract host from the URL (ignores path and protocol)
   if (extract_host_and_port(url, host, port) == -1) {
       LOG_ERROR("Error: Invalid URL format");
       return 0;
   }

   // Set up address resolution hints
   struct addrinfo hints, *res;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;        // Use IPv4
   hints.ai_socktype = SOCK_STREAM;  // TCP stream sockets

   // Resolve host (works for both hostnames and IP addresses)
   int status = getaddrinfo(host, port, &hints, &res);
   if (status != 0) {
      LOG_ERROR("getaddrinfo: %s", gai_strerror(status));
      return 0;
   }

   // Create a socket
   int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (sock == -1) {
      LOG_ERROR("socket: %s", strerror(errno));
      freeaddrinfo(res);
      return 0;
   }

   // Set socket as non-blocking
   fcntl(sock, F_SETFL, O_NONBLOCK);

   // Attempt to connect
   int result = connect(sock, res->ai_addr, res->ai_addrlen);
   if (result == -1 && errno != EINPROGRESS) {
      LOG_ERROR("connect: %s", strerror(errno));
      close(sock);
      freeaddrinfo(res);
      return 0;
   }

   // Set up the file descriptor set for select()
   fd_set write_fds;
   FD_ZERO(&write_fds);
   FD_SET(sock, &write_fds);

   // Set the timeout value
   struct timeval timeout;
   timeout.tv_sec = timeout_seconds;
   timeout.tv_usec = 0;

   // Wait for the socket to become writable within the timeout
   result = select(sock + 1, NULL, &write_fds, NULL, &timeout);
   if (result == 1) {
      // Socket is writable, check for connection success
      int error;
      socklen_t error_len = sizeof(error);
      if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0) {
         result = 1;  // Connection successful
      } else {
         result = 0;  // Connection failed
      }
   } else {
      result = 0;  // Timeout or error
   }

   // Clean up
   close(sock);
   freeaddrinfo(res);

   return result;
}

char *getGptResponse(struct json_object *conversation_history, const char *input_text,
                     char *vision_ai_image, size_t vision_ai_image_size)
{
   CURL *curl_handle = NULL;  /* Handle for curl library. */
   CURLcode res = -1;
   struct curl_slist *headers = NULL;
   char full_url[2048 + 20] = "";

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

#if defined(OPENAI_VISION)
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
         LOG_ERROR("Failed to allocate memory for data URI.");
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
   printf("JSON Payload (PLAIN): %s", payload);
   printf("\n");

   chunk.memory = malloc(1);
   if (chunk.memory == NULL) {
      LOG_ERROR("Error allocating memory!");

      return NULL;
   }
   chunk.size = 0;

   if (!checkInternetConnectionWithTimeout(llm_url, 4)) {
      LOG_ERROR("URL did not return. Unavailable.");

      if (strcmp(CLOUDAI_URL, llm_url) == 0) {
         LOG_WARNING("Falling back to local LLM.");

         text_to_speech("Unable to contact cloud LLM.");

         setLLM(LOCAL_LLM);
         text_to_speech("Setting to AI to local LLM.");
      } else {
         return NULL;
      }
   }

   curl_handle = curl_easy_init();
   if (curl_handle) {
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, OPENAI_HEADER);

      snprintf(full_url, 2048 + 20, "%s%s", llm_url, "/v1/chat/completions");
      curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

      res = curl_easy_perform(curl_handle);
      if (res != CURLE_OK) {
         LOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
      }

      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
   }

   printf("Raw receive from ChatGPT: %s", (char *) chunk.memory);
   printf("\n");

   parsed_json = json_tokener_parse(chunk.memory);
   if (!parsed_json) {
      LOG_ERROR("Failed to parse JSON response.");
      free(chunk.memory);
      return NULL;
   }

   if (!json_object_object_get_ex(parsed_json, "choices", &choices) ||
      json_object_get_type(choices) != json_type_array ||
      json_object_array_length(choices) < 1) {
      LOG_ERROR("Error in parsing response: 'choices' missing or invalid.");
      json_object_put(parsed_json); // Correctly free JSON object
      free(chunk.memory);
      return NULL;
   }

   first_choice = json_object_array_get_idx(choices, 0);
   if (!first_choice) {
      LOG_ERROR("Error: 'choices' array is empty.");
      json_object_put(parsed_json);
      free(chunk.memory);
      return NULL;
   }

   if (!json_object_object_get_ex(first_choice, "message", &message) ||
      !json_object_object_get_ex(message, "content", &content)) {
      LOG_ERROR("Error: 'message' or 'content' field missing.");
      json_object_put(parsed_json);
      free(chunk.memory);
      return NULL;
   }

   // Optional: Safely access 'finish_reason'
   json_object_object_get_ex(first_choice, "finish_reason", &finish_reason);

   if (!json_object_object_get_ex(parsed_json, "usage", &usage_obj) ||
      !json_object_object_get_ex(usage_obj, "total_tokens", &total_tokens_obj)) {
      LOG_ERROR("Error: 'usage' or 'total_tokens' field missing.");
      json_object_put(parsed_json);
      free(chunk.memory);
      return NULL;
   }

   total_tokens = json_object_get_int(total_tokens_obj);
   LOG_WARNING("Total tokens: %d", total_tokens);

   // Duplicate the response content string safely
   const char* content_str = json_object_get_string(content);
   if (!content_str) {
      LOG_ERROR("Error: 'content' field is empty or not a string.");
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
         LOG_ERROR("GPT returned prematurely due to token length.");
      } else {
         LOG_ERROR("GPT returned prematurely due to \"%s\".",
                json_object_get_string(type));
      }
#endif
   } else {
      LOG_INFO("Response finished properly.");
   }

   json_object_put(parsed_json);
   free(chunk.memory);

   return response;
}
