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
 * @brief Defines device types, associated strings, and callback functions for handling device actions.
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
   AUDIO_PLAYBACK_DEVICE,  /**< Set an audio playback device. */
   AUDIO_CAPTURE_DEVICE,   /**< Set an audio capture device. */
   TEXT_TO_SPEECH,         /**< A text-to-speech action. */
   DATE,                   /**< Request for the current date. */
   TIME,                   /**< Request for the current time. */
   MUSIC,                  /**< Music playback control. */
   VOICE_AMPLIFIER,        /**< Voice amplifier control. */
   SHUTDOWN,               /**< System shutdown action. */
   VIEWING,                /**< Viewing or display actions. */
   VOLUME,                 /**< Music playback volume, */
   LOCAL_LLM_SWITCH,       /**< Local LLM switch. */
   CLOUD_LLM_SWITCH,       /**< Cloud LLM switch. */
   MAX_DEVICE_TYPES        /**< Used to determine the number of device types. */
} deviceType;

/**
 * @brief String representations for each deviceType enumeration member.
 *
 * These strings correspond to the device types and are used in MQTT JSON messages.
 * They should match the order of the `deviceType` enumeration.
 */
static const char *deviceTypeStrings[] = {
   "audio playback device",
   "audio capture device",
   "text to speech",
   "date",
   "time",
   "music",
   "voice amplifier",
   "shutdown alpha bravo charlie",
   "viewing",
   "volume",
   "local llm",
   "cloud llm"
};

/**
 * @brief Associates a device type with a callback function that processes actions for that device.
 *
 * This structure maps a `deviceType` to its corresponding callback function,
 * allowing dynamic handling of device actions.
 */
typedef struct {
   deviceType device;                        /**< The device type. */
   void (*callback)(const char *, char *);   /**< The callback function to process actions for the device. */
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
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos);

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
 */
void dateCallback(const char *actionName, char *value);

/**
 * @brief Callback function to handle time requests.
 *
 * Processes actions related to time requests, such as providing the current time.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (may be unused).
 */
void timeCallback(const char *actionName, char *value);

/**
 * @brief Callback function to handle music playback control.
 *
 * Processes actions related to music playback, such as play, pause, or stop.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (e.g., song name or control command).
 */
void musicCallback(const char *actionName, char *value);

/**
 * @brief Callback function to control the voice amplifier.
 *
 * Processes actions to enable or disable the voice amplifier functionality.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (e.g., "on" or "off").
 */
void voiceAmplifierCallback(const char *actionName, char *value);

/**
 * @brief Callback function to handle system shutdown requests.
 *
 * Processes actions to initiate a system shutdown.
 *
 * @param actionName The name of the action triggering this callback.
 * @param value      Additional value or parameters for the action (may be unused).
 */
void shutdownCallback(const char *actionName, char *value);

/**
 * @brief Callback function to handle the viewing of an image.
 *
 * Reads the specified image file, encodes its content into Base64,
 * and passes the encoded data for vision AI processing.
 *
 * @param actionName The name of the action triggering this callback. Not used in this function,
 *                   but included to match expected callback signature.
 * @param value      The file path to the image to be viewed and processed.
 */
void viewingCallback(const char *actionName, char *value);

/**
 * @brief Adjusts music volume based on user input.
 *
 * Sets the music playback volume to a value between 0.0 (silence) and 2.0 (maximum).
 *
 * @param actionName Unused but included for callback signature consistency.
 * @param value      String representing the desired volume level, converted to a float and validated.
 */
void volumeCallback(const char *actionName, char *value);

/**
 * @brief Callback function for setting the AI to use the local LLM.
 *
 * This function is triggered by an action to switch the AI to the local LLM (Large Language Model).
 *
 * @param actionName The name of the action triggering the callback.
 * @param value The value associated with the action (unused in this implementation).
 */
void localLLMCallback(const char *actionName, char *value);

/**
 * @brief Callback function for setting the AI to use the cloud LLM.
 *
 * This function is triggered by an action to switch the AI to the cloud LLM (Large Language Model).
 *
 * @param actionName The name of the action triggering the callback.
 * @param value The value associated with the action (unused in this implementation).
 */
void cloudLLMCallback(const char *actionName, char *value);

#endif // MOSQUITTO_COMMS_H

