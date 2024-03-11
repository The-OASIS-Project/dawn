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

#ifndef MOSQUITTO_COMMS_H
#define MOSQUITTO_COMMS_H

/**
 * Enumerates the types of devices or actions supported by the application.
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
   MAX_DEVICE_TYPES        /**< Used to determine the number of device types. */
} deviceType;

/**
 * Provides string representations for each deviceType enumeration member.
 * These strings are the values that should be passed in MQTT JSON messages.
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
   "viewing"
};

/**
 * Associates a device type with a callback function that processes actions for that device.
 */
typedef struct {
   deviceType device;                        /**< The device type. */
   void (*callback)(const char *, char *);   /**< The callback function to process actions for the device. */
} deviceCallback;

/* MQTT callbacks */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code);
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos);
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);

/* Device callbacks */
void dateCallback(const char *actionName, char *value);
void timeCallback(const char *actionName, char *value);
void musicCallback(const char *actionName, char *value);
void voiceAmplifierCallback(const char *actionName, char *value);
void shutdownCallback(const char *actionName, char *value);
void viewingCallback(const char *actionName, char *value);

#endif // MOSQUITTO_COMMS_H

