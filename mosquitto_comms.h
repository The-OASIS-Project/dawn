#ifndef MOSQUITTO_COMMS_H
#define MOSQUITTO_COMMS_H

typedef enum {
   AUDIO_PLAYBACK_DEVICE,
   AUDIO_CAPTURE_DEVICE,
   TEXT_TO_SPEECH,
   DATE,
   TIME,
   MUSIC,
   VOICE_AMPLIFIER,
   SHUTDOWN,
   MAX_DEVICE_TYPES
} deviceType; /**< These are the device types we handle in this application. */

static const char *deviceTypeStrings[] = {
   "audio playback device",
   "audio capture device",
   "text to speech",
   "date",
   "time",
   "music",
   "voice amplifier",
   "shutdown alpha bravo charlie"
};

typedef struct {
   deviceType device;
   char action[20];
   union Value {
      int i;
      double d;
      char str[20];
   } value;
} deviceCommand;

typedef struct {
   deviceType device;
   void (*callback)(const char *, char *);
} deviceCallback;

void on_connect(struct mosquitto *mosq, void *obj, int reason_code);
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos);
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);
void dateCallback(const char *actionName, char *value);
void timeCallback(const char *actionName, char *value);
void musicCallback(const char *actionName, char *value);
void voiceAmplifierCallback(const char *actionName, char *value);
void shutdownCallback(const char *actionName, char *value);

#endif // MOSQUITTO_COMMS_H

