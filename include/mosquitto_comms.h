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
 * @file mosquitto_comms.h
 * @brief Defines device types, associated strings, and callback functions for handling device
 * actions.
 *
 * This header contains the definitions for device types supported by the application,
 * their string representations for MQTT messages, and the callback functions that process
 * actions for each device type.
 */

#ifndef MOSQUITTO_COMMS_H
#define MOSQUITTO_COMMS_H

#include <mosquitto.h>

/**
 * @brief Enumerates the types of devices or actions supported by the application.
 */
typedef enum {
   AUDIO_PLAYBACK_DEVICE, /**< Set an audio playback device. */
   AUDIO_CAPTURE_DEVICE,  /**< Set an audio capture device. */
   TEXT_TO_SPEECH,        /**< A text-to-speech action. */
   DATE,                  /**< Request for the current date. */
   TIME,                  /**< Request for the current time. */
   MUSIC,                 /**< Music playback control. */
   VOICE_AMPLIFIER,       /**< Voice amplifier control. */
   SHUTDOWN,              /**< System shutdown action. */
   VIEWING,               /**< Viewing or display actions. */
   VOLUME,                /**< Music playback volume, */
   LOCAL_LLM_SWITCH,      /**< Local LLM switch. */
   CLOUD_LLM_SWITCH,      /**< Cloud LLM switch. */
   RESET_CONVERSATION,    /**< Reset conversation context. */
   SEARCH,                /**< Web search action. */
   WEATHER,               /**< Weather information service. */
   CALCULATOR,            /**< Calculator for math expressions. */
   URL_FETCH,             /**< Fetch and extract content from a URL. */
   LLM_STATUS,            /**< Query current LLM status (local/cloud, model). */
   MAX_DEVICE_TYPES       /**< Used to determine the number of device types. */
} deviceType;

/**
 * @brief String representations for each deviceType enumeration member.
 *
 * These strings correspond to the device types and are used in MQTT JSON messages.
 * They should match the order of the `deviceType` enumeration.
 */
static const char *deviceTypeStrings[] = { "audio playback device",
                                           "audio capture device",
                                           "text to speech",
                                           "date",
                                           "time",
                                           "music",
                                           "voice amplifier",
                                           "shutdown",
                                           "viewing",
                                           "volume",
                                           "local llm",
                                           "cloud llm",
                                           "reset conversation",
                                           "search",
                                           "weather",
                                           "calculator",
                                           "url",
                                           "llm" };

/**
 * @brief Associates a device type with a callback function that processes actions for that device.
 *
 * This structure maps a `deviceType` to its corresponding callback function,
 * allowing dynamic handling of device actions. The callback can optionally return
 * data instead of directly using text-to-speech when in AI modes.
 *
 * CALLBACK RETURN VALUE CONTRACT:
 * - Return NULL if no data to report (command executed silently)
 * - Return heap-allocated string (malloc/strdup) if returning data
 * - Caller is responsible for freeing non-NULL return values
 * - Set *should_respond = 1 to send return value to LLM, 0 otherwise
 *
 * NOTE: Some legacy callbacks still use static buffers. These should be migrated
 * to heap allocation for consistency. See mosquitto_comms.c for details.
 */
typedef struct {
   deviceType device; /**< The device type. */
   char *(*callback)(const char *actionName,
                     char *value,
                     int *should_respond); /**< Callback returns heap-allocated string or NULL. */
} deviceCallback;

/* MQTT callbacks */

/**
 * @brief Callback function invoked when the client successfully connects to the MQTT broker.
 *
 * @param mosq        The Mosquitto client instance.
 * @param obj         User-defined pointer passed to the callback.
 * @param reason_code The reason code for the connection result.
 */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code);

/**
 * @brief Callback function invoked when the client successfully subscribes to a topic.
 *
 * @param mosq        The Mosquitto client instance.
 * @param obj         User-defined pointer passed to the callback.
 * @param mid         Message ID of the subscription request.
 * @param qos_count   The number of granted QoS levels.
 * @param granted_qos Array of granted QoS levels.
 */
void on_subscribe(struct mosquitto *mosq,
                  void *obj,
                  int mid,
                  int qos_count,
                  const int *granted_qos);

/**
 * @brief Callback function invoked when a message is received from the subscribed topics.
 *
 * @param mosq The Mosquitto client instance.
 * @param obj  User-defined pointer passed to the callback.
 * @param msg  The message data received.
 */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);

/* Device callbacks */

/**
 * @brief Callback function to handle date requests.
 *
 * Processes actions related to date requests, such as providing the current date.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (may be unused).
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *dateCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to handle time requests.
 *
 * Processes actions related to time requests, such as providing the current time.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (may be unused).
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *timeCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to handle music playback control.
 *
 * Processes actions related to music playback, such as play, pause, or stop.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (e.g., song name or control
 * command).
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *musicCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Sets custom music directory path.
 *
 * Sets an absolute path to the music directory. If set, this overrides
 * the default MUSIC_DIR from dawn.h. Pass NULL to use the default.
 *
 * @param path Absolute path to music directory, or NULL for default
 */
void set_music_directory(const char *path);

/**
 * @brief Callback function to control the voice amplifier.
 *
 * Processes actions to enable or disable the voice amplifier functionality.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (e.g., "on" or "off").
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *voiceAmplifierCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to handle system shutdown requests.
 *
 * Processes actions to initiate a system shutdown.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (may be unused).
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *shutdownCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to handle the viewing of an image.
 *
 * Reads the specified image file, encodes its content into Base64,
 * and passes the encoded data for vision AI processing.
 *
 * @param actionName The name of the action triggering this callback. Not used in this function,
 *                   but included to match expected callback signature.
 * @param value      The file path to the image to be viewed and processed.
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *viewingCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Adjusts music volume based on user input.
 *
 * Sets the music playback volume to a value between 0.0 (silence) and 2.0 (maximum).
 *
 * @param actionName Unused but included for callback signature consistency.
 * @param value      String representing the desired volume level, converted to a float and
 * validated.
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *volumeCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function for setting the AI to use the local LLM.
 *
 * This function is triggered by an action to switch the AI to the local LLM (Large Language Model).
 *
 * @param actionName The name of the action triggering the callback.
 * @param value The value associated with the action (unused in this implementation).
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *localLLMCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function for setting the AI to use the cloud LLM.
 *
 * This function is triggered by an action to switch the AI to the cloud LLM (Large Language Model).
 *
 * @param actionName The name of the action triggering the callback.
 * @param value The value associated with the action (unused in this implementation).
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *cloudLLMCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to reset the conversation context.
 *
 * Saves the current conversation to JSON, clears the LLM context,
 * and resets session statistics.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (may be unused).
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *resetConversationCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to perform web searches via SearXNG.
 *
 * Performs a web search using the local SearXNG instance and returns
 * formatted results for the LLM to summarize.
 *
 * @param actionName The name of the action triggering this callback (e.g., "web").
 * @param value      The search query string.
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *searchCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to get weather information.
 *
 * Fetches weather data from Open-Meteo API for the specified location
 * and returns formatted results for the LLM to present.
 *
 * @param actionName The name of the action triggering this callback (e.g., "get").
 * @param value      The location string (e.g., "Atlanta, Georgia").
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *weatherCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to evaluate mathematical expressions.
 *
 * Evaluates the given mathematical expression using TinyExpr and returns
 * the result for the LLM to present to the user.
 *
 * @param actionName The name of the action triggering this callback (e.g., "evaluate").
 * @param value      The mathematical expression to evaluate (e.g., "sqrt(144) + 2^8").
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *calculatorCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to fetch and extract content from a URL.
 *
 * Fetches the specified URL, extracts readable text content (stripping HTML),
 * and optionally summarizes if content is large. Returns the content for the
 * LLM to process.
 *
 * @param actionName The name of the action triggering this callback (e.g., "fetch").
 * @param value      The URL to fetch (e.g., "https://example.com/article").
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *urlFetchCallback(const char *actionName, char *value, int *should_respond);

/**
 * @brief Callback function to query current LLM status.
 *
 * Returns information about the currently active LLM (local or cloud),
 * including the model name and provider (for cloud).
 *
 * @param actionName The name of the action triggering this callback (e.g., "get").
 * @param value      Unused for this callback.
 * @param should_respond Should the callback return data to the AI or just handle it.
 */
char *llmStatusCallback(const char *actionName, char *value, int *should_respond);

#endif  // MOSQUITTO_COMMS_H
