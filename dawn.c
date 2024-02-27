#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <getopt.h>

#include <mosquitto.h>
#include "mosquitto_comms.h"

#include "vosk_api.h"

#include "dawn.h"
#include "text_to_speech.h"
#include "openai.h"
#include "text_to_command_nuevo.h"

#define DEFAULT_RATE             22050
#define DEFAULT_CHANNELS         1
#define DEFAULT_CAPTURE_SECONDS  0.5f
#define DEFAULT_COMMAND_TIMEOUT  6  /**< This is how many iterations of DEFAULT_CAPTURE_SECONDS. */

#define BACKGROUND_CAPTURE_SECONDS  6

#ifdef ALSA_DEVICE
#include <alsa/asoundlib.h>

#define DEFAULT_ACCESS     SND_PCM_ACCESS_RW_INTERLEAVED
#define DEFAULT_FORMAT     SND_PCM_FORMAT_S16_LE
#define DEFAULT_FRAMES     64
#else
#include <pulse/simple.h>
#include <pulse/error.h>

#define DEFAULT_PULSE_FORMAT  PA_SAMPLE_S16LE
#endif

#define TALKING_THRESHOLD_OFFSET 0.015

static char pcm_capture_device[MAX_WORD_LENGTH] = "";
static char pcm_playback_device[MAX_WORD_LENGTH] = "";

/* Parsed audio devices. */
static audioDevices captureDevices[MAX_AUDIO_DEVICES];   /**< Audio capture devices. */
static int numAudioCaptureDevices = 0;                   /**< How many capture devices. */

static audioDevices playbackDevices[MAX_AUDIO_DEVICES];  /**< Audio playback devices. */
static int numAudioPlaybackDevices = 0;                  /**< How many playback devices. */


typedef struct {
#ifdef ALSA_DEVICE
   snd_pcm_t *handle;
   snd_pcm_uframes_t frames;
#else
   pa_simple *pa_handle;
   pa_usec_t pa_latency;
   size_t pa_framesize;
#endif

   uint32_t full_buff_size;
} audioControl;

#ifndef ALSA_DEVICE
static const pa_sample_spec sample_spec = {
   .format = DEFAULT_PULSE_FORMAT,
   .rate = DEFAULT_RATE,
   .channels = DEFAULT_CHANNELS
};
#endif

static double backgroundRMS = 3.0;

static char *wakeWords[] = {
   "hello " AI_NAME,
   "okay " AI_NAME,
   "alright " AI_NAME,
   "hey " AI_NAME,
   "hi " AI_NAME
};

static char *goodbyeWords[] = {
   "good bye",
   "goodbye",
   "good night",
   "bye",
   "quit",
   "exit"
};

const char* wakeResponses[] = {
   "Hello Sir.",
   "At your service Sir.",
   "Yes Sir?",
   "How may I assist you Sir?",
   "Listening Sir."
};

static char *ignoreWords[] = {
   "",
   "the",
   "cancel",
   "never mind",
   "nevermind",
   "ignore"
};

const char* morning_greeting = "Good morning boss.";
const char* day_greeting = "Good day Sir.";
const char* evening_greeting = "Good evening Sir.";

typedef enum { SILENCE, WAKEWORD_LISTEN, COMMAND_RECORDING, PROCESS_COMMAND } listeningState;

//actionType actions[MAX_ACTIONS];    /**< All of the available actions read from the JSON. */
//int numActions = 0;                 /**< Total actions in the actions array. */

#if 0
// WAVE file header format
typedef struct WAVE_HEADER {
   unsigned char riff[4];       // RIFF string
   uint32_t overall_size;       // overall size of file in bytes
   unsigned char wave[4];       // WAVE string
   unsigned char fmt_chunk_marker[4];   // fmt string with trailing null char
   uint32_t length_of_fmt;      // length of the format data
   uint16_t format_type;        // format type
   uint16_t channels;           // number of channels
   uint32_t sample_rate;        // sampling rate (blocks per second)
   uint32_t byterate;           // SampleRate * NumChannels * BitsPerSample/8
   uint16_t block_align;        // NumChannels * BitsPerSample/8
   uint16_t bits_per_sample;    // bits per sample, 8- 8bits, 16- 16 bits etc
   unsigned char data_chunk_header[4];  // DATA string or FLLR string
   uint32_t data_size;          // NumSamples * NumChannels * BitsPerSample/8 - size of the next chunk that will be read
} WAVE_HEADER;

void create_wave_header(WAVE_HEADER * header, uint32_t byte_count)
{
   // RIFF chunk
   memcpy(header->riff, "RIFF", 4);
   header->overall_size = byte_count + 36;      // You might need to adjust this value

   // WAVE chunk
   memcpy(header->wave, "WAVE", 4);

   // FMT sub-chunk (audio format)
   memcpy(header->fmt_chunk_marker, "fmt ", 4);
   header->length_of_fmt = 16;  // 16 for PCM
   header->format_type = 1;     // 1 for PCM
   header->channels = CHANNELS;
   header->sample_rate = RATE;
   header->byterate = RATE * CHANNELS * 2;      // SampleRate * NumChannels * BitsPerSample/8
   header->block_align = CHANNELS * 2;  // NumChannels * BitsPerSample/8
   header->bits_per_sample = 16;        // 16 bits for PCM format

   // data sub-chunk
   memcpy(header->data_chunk_header, "data", 4);
   header->data_size = byte_count;      // total size of raw audio data
}

void write_wave_file(const char *filename, WAVE_HEADER * header, char *data)
{
   FILE *f = fopen(filename, "w");
   if (f == NULL) {
      fprintf(stderr, "Failed to open file\n");
      exit(1);
   }

   fwrite(header, sizeof(WAVE_HEADER), 1, f);
   fwrite(data, header->overall_size, 1, f);

   fclose(f);
}
#endif

#if 0
// Define the function to draw the waveform using SDL
void drawWaveform(const int16_t *audioBuffer, size_t numSamples) {
    // Clear the screen
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    // Define waveform colors (e.g., green lines)
    SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);

    // Determine the dimensions and position of the waveform within the window
    int waveformPosX = 0; // Adjust as needed
    int waveformPosY = 50; // Adjust as needed
    int waveformWidth = 0; // Adjust as needed
    int waveformHeight = 0; // Adjust as needed
    SDL_GetWindowSize(win, &waveformWidth, &waveformHeight);
    waveformPosY = waveformPosY + (waveformHeight - waveformPosY*2) / 2;

    // Calculate the step size to sample audio data and draw the waveform
    int stepSize = numSamples / waveformWidth;
    if (stepSize <= 0) {
        stepSize = 1;
    }

    // Scale factor for the waveform's height
    float scaleFactor = (float)waveformHeight / INT16_MAX;

    // Draw the waveform based on the audio buffer data
    int prevX = waveformPosX;
    int prevY = waveformPosY + (int)(audioBuffer[0] * scaleFactor);
    for (size_t i = 1; i < numSamples; i += stepSize) {
        int x = waveformPosX + waveformWidth * i / numSamples;
        int y = waveformPosY + (int)(audioBuffer[i] * scaleFactor);

        SDL_RenderDrawLine(ren, prevX, prevY, x, y);

        prevX = x;
        prevY = y;
    }

    // Update the screen
    SDL_RenderPresent(ren);
}
#endif

void textToSpeechCallback(const char *actionName, char *value) {
   printf("Received text to speech command: \"%s\"\n", value);
   text_to_speech(pcm_playback_device, value);
}

char *getPcmPlaybackDevice(void) {
   return pcm_playback_device;
}

char *findAudioPlaybackDevice(char *name) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioPlaybackDevices; i++) {
      if (strcmp(playbackDevices[i].name, name) == 0) {
         return playbackDevices[i].device;
         break;
      }
   }

   return NULL;
}

void setPcmPlaybackDevice(const char *actionName, char *value) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioPlaybackDevices; i++) {
      if (strcmp(playbackDevices[i].name, value) == 0) {
         printf("Setting audio playback device to \"%s\".\n", playbackDevices[i].device);
         strncpy(pcm_playback_device, playbackDevices[i].device, MAX_WORD_LENGTH);
         snprintf(speech, MAX_COMMAND_LENGTH, "Switching playback device to %s.", value);
         text_to_speech(pcm_playback_device, speech);
         break;
      }
   }

   if (i >= numAudioPlaybackDevices) {
      fprintf(stderr, "Requested audio playback device not found.\n");
      snprintf(speech, MAX_COMMAND_LENGTH, "Sorry sir. A playback devices called %s was not found.", value);
      text_to_speech(pcm_playback_device, speech);
   }
}

char *getPcmCaptureDevice(void) {
   return pcm_capture_device;
}

void setPcmCaptureDevice(const char *actionName, char *value) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioCaptureDevices; i++) {
      if (strcmp(captureDevices[i].name, value) == 0) {
         printf("Setting audio capture device to \"%s\".\n", captureDevices[i].device);
         strncpy(pcm_capture_device, captureDevices[i].device, MAX_WORD_LENGTH);
         snprintf(speech, MAX_COMMAND_LENGTH, "Switching capture device to %s.", value);
         text_to_speech(pcm_playback_device, speech);
         break;
      }
   }

   if (i >= numAudioCaptureDevices) {
      fprintf(stderr, "Requested audio capture device not found.\n");
      snprintf(speech, MAX_COMMAND_LENGTH, "Sorry sir. A capture devices called %s was not found.", value);
      text_to_speech(pcm_playback_device, speech);
   }
}



// Function to calculate the root mean square (RMS) of an audio buffer
double calculateRMS(const int16_t *audioBuffer, size_t numSamples) {
    double sumOfSquares = 0.0;
    for (size_t i = 0; i < numSamples; ++i) {
       // Normalize the sample to the range [-1, 1] using 32768.0 as the divisor
       double normalizedSample = (double)audioBuffer[i] / 32768.0;
       sumOfSquares += normalizedSample * normalizedSample;
    }
    return sqrt(sumOfSquares / numSamples);
}

void *measureBackgroundAudio(void *audHandle)
{
   audioControl *myControl = (audioControl *) audHandle;

   // Audio Buffer
   char *buff = NULL;
   uint32_t buff_size = 0;
#ifdef ALSA_DEVICE
   uint32_t max_buff_size = DEFAULT_RATE * DEFAULT_CHANNELS * 2 * BACKGROUND_CAPTURE_SECONDS;
#else
   uint32_t max_buff_size = (myControl->pa_latency +
                            (BACKGROUND_CAPTURE_SECONDS * DEFAULT_RATE * myControl->pa_framesize)) /
                            myControl->pa_framesize;
#endif
   char *max_buff = NULL;
   int rc = 0;
   int error = 0;

   // Allocate Audio Buffers
   buff = (char *)malloc(myControl->full_buff_size);
   if (buff == NULL) {
      fprintf(stderr, "malloc() failed on buff.\n");

      return NULL;
   }

   max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      fprintf(stderr, "malloc() failed on max_buff.\n");

      return NULL;
   }

#ifdef ALSA_DEVICE
   while (1) {
      rc = snd_pcm_readi(myControl->handle, buff, myControl->frames);
      if ((rc > 0) && (buff_size + myControl->full_buff_size <= max_buff_size)) {
         memcpy(max_buff + buff_size, buff, myControl->full_buff_size);
         buff_size += myControl->full_buff_size;
      } else {
         if (rc <= 0) {
            printf("Error reading PCM.\n");
         }
         double rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));
         printf("RMS of background recording is %g.\n", rms);

         backgroundRMS = rms;

         break;
      }
   }
#else
   for (size_t i = 0; i < max_buff_size / myControl->full_buff_size; ++i) {
      if (pa_simple_read(myControl->pa_handle, buff, myControl->pa_framesize, &error) < 0) {
         printf("Could not read audio: %s\n", pa_strerror(error));
         break;
      }

      memcpy(max_buff + buff_size, buff, myControl->full_buff_size);
      buff_size += myControl->full_buff_size;
   }

   double rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));
   printf("RMS of background recording is %g.\n", rms);

   backgroundRMS = rms;
#endif

   free(buff);
   free(max_buff);

   return NULL;
}

char *getTextResponse(const char *input)
{
    struct json_object *parsed_json;
    struct json_object *text_object;
    char *return_text = NULL;

    // Parse the JSON data
    parsed_json = json_tokener_parse(input);
    if (parsed_json == NULL) {
       fprintf(stderr, "Error: Unable to process text response.\n");

       return NULL;
    }

    // Get the "text" object from the JSON
    if (json_object_object_get_ex(parsed_json, "text", &text_object)) {
        // Extract the text value as a C string
        const char *input_text = json_object_get_string(text_object);
        if (input_text == NULL) {
           fprintf(stderr, "Error: Unable to get string from input text.\n");
           json_object_put(parsed_json);
           return NULL;
        }
        return_text = malloc((strlen(input_text) + 1) * sizeof(char));
        if (return_text == NULL) {
           fprintf(stderr, "malloc() failed in getTextResponse().\n");

           return NULL;
        }

        // FIXME: sprintf(return_text, "%s ", input_text);
        sprintf(return_text, "%s", input_text);

        // Now you can use the 'input_text' C string as needed
        printf("Input Text: %s\n", return_text);
    } else {
        fprintf(stderr, "Error: 'text' field not found in JSON.\n");
        json_object_put(parsed_json);

        return NULL;
    }

    // Cleanup: Release the parsed_json object
    json_object_put(parsed_json);

    return return_text;
}

#ifdef ALSA_DEVICE
int openAlsaPcmCaptureDevice(snd_pcm_t **handle, char *pcm_device, snd_pcm_uframes_t *frames)
{
   snd_pcm_hw_params_t *params = NULL;
   unsigned int rate = DEFAULT_RATE;
   int dir = 0;
   *frames = DEFAULT_FRAMES;
   int rc = 0;

   printf("ALSA CAPTURE DRIVER\n");

   /* Open PCM device for playback. */
   rc = snd_pcm_open(handle, pcm_device, SND_PCM_STREAM_CAPTURE, 0);
   if (rc < 0) {
      fprintf(stderr, "unable to open pcm device for capture (%s): %s\n", pcm_device, snd_strerror(rc));
      return 1;
   }

   snd_pcm_hw_params_alloca(&params);
   snd_pcm_hw_params_any(*handle, params);
   snd_pcm_hw_params_set_access(*handle, params, DEFAULT_ACCESS);
   snd_pcm_hw_params_set_format(*handle, params, DEFAULT_FORMAT);
   snd_pcm_hw_params_set_channels(*handle, params, DEFAULT_CHANNELS);
   snd_pcm_hw_params_set_rate_near(*handle, params, &rate, &dir);
   printf("Capture rate set to %u\n", rate);
   snd_pcm_hw_params_set_period_size_near(*handle, params, frames, &dir);
   printf("Frames set to %lu\n", *frames);
   rc = snd_pcm_hw_params(*handle, params);
   if (rc < 0) {
      fprintf(stderr, "unable to set hw parameters: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   return 0;
}
#else
pa_simple *openPulseaudioCaptureDevice(char *pcm_capture_device)
{
   pa_simple *pa_handle = NULL;
   int rc = 0;

   printf("PULSEAUDIO CAPTURE DRIVER: %s\n", pcm_capture_device);

   /* Create a new playback stream */
   if (!(pa_handle = pa_simple_new(NULL, APPLICATION_NAME, PA_STREAM_RECORD, pcm_capture_device, "record", &sample_spec, NULL, NULL, &rc))) {
      fprintf(stderr, "Error opening PulseAudio record: %s\n", pa_strerror(rc));
      return NULL;
   }

   printf("Capture opened successfully.\n");

   return pa_handle;
}
#endif

const char* timeOfDayGreeting(void) {
   time_t t = time(NULL);
   struct tm *local_time = localtime(&t);

   int hour = local_time->tm_hour;
   if (hour >= 3 && hour < 12) {
      return morning_greeting;
   } else if (hour >= 12 && hour < 18) {
      return day_greeting;
   } else {
      return evening_greeting;
   }
}

const char* wakeWordAcknowledgment() {
   int numWakeResponses = sizeof(wakeResponses) / sizeof(wakeResponses[0]);
   int choice;

   srand(time(NULL));
   choice = rand() % numWakeResponses;

   return wakeResponses[choice];
}

void display_help(int argc, char *argv[]) {
   if (argc > 0) {
      printf("Usage: %s [options]\n", argv[0]);
   } else {
      printf("Usage: [options]\n");
   }

   printf("Options:\n");
   printf("  -c, --capture DEVICE   Specify the PCM capture device.\n");
   printf("  -d, --playback DEVICE  Specify the PCM playback device.\n");
   printf("  -h, --help             Display this help message and exit.\n");
}

int main(int argc, char *argv[])
{
   char *input_text = NULL;
   char *command_text = NULL;
   char *response_text = NULL;
   const char *vosk_output = NULL;
   struct json_object *conversation_history = NULL;
   struct json_object *system_message = NULL;
   int rc = 0;
   int opt = 0;

#ifdef ALSA_DEVICE
   // Define the ALSA parameters
   snd_pcm_t *handle = NULL;
   snd_pcm_uframes_t frames = 0;
#else
   // Define the Pulse parameters
   int error = 0;
#endif

   // Audio Buffer
   char *buff = NULL;
   uint32_t buff_size = 0;
   uint32_t full_buff_size = 0;
#ifdef ALSA_DEVICE
   float temp_buff_size = DEFAULT_RATE * DEFAULT_CHANNELS * 2 * DEFAULT_CAPTURE_SECONDS;
   uint32_t max_buff_size = (uint32_t)ceil(temp_buff_size);
#else
   float temp_buff_size = 0;
   uint32_t max_buff_size = 0;
#endif
   char *max_buff = NULL;

   // Command Configuration
   FILE *configFile = NULL;
   char buffer[10*1024];
   int bytes_read = 0;

   // Command Parsing
   actionType actions[MAX_ACTIONS];
   int numActions = 0;

   commandSearchElement commands[MAX_COMMANDS];
   int numCommands = 0;

   char *next_char_ptr = NULL;

   audioControl myAudioControls;
   pthread_t backgroundAudioDetect;

   int commandTimeout = 0;

   /* MQTT */
   struct mosquitto *mosq;

   /* Array Counts */
   int numGoodbyeWords = sizeof(goodbyeWords) / sizeof(goodbyeWords[0]);
   int numWakeWords = sizeof(wakeWords) / sizeof(wakeWords[0]);
   int numIgnoreWords = sizeof(ignoreWords) / sizeof(ignoreWords[0]);

   int i = 0;

   static struct option long_options[] = {
      {"capture", required_argument, NULL, 'c'},
      {"playback", required_argument, NULL, 'd'},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0}
   };
   int option_index = 0;

   // TODO: I'm adding this here but it will need better error clean-ups.
   curl_global_init(CURL_GLOBAL_DEFAULT);

   while ((opt = getopt_long(argc, argv, "c:d:h", long_options, &option_index)) != -1) {
      switch (opt) {
      case 'c':
         strncpy(pcm_capture_device, optarg, sizeof(pcm_capture_device));
         pcm_capture_device[sizeof(pcm_capture_device) - 1] = '\0';
         break;
      case 'd':
         strncpy(pcm_playback_device, optarg, sizeof(pcm_playback_device));
         pcm_playback_device[sizeof(pcm_playback_device) - 1] = '\0';
         break;
      case 'h':
         display_help(argc, argv);
         exit(EXIT_SUCCESS);
      case '?':
         display_help(argc, argv);
         exit(EXIT_FAILURE);
      default:
         display_help(argc, argv);
         exit(EXIT_FAILURE);
      }
   }

   if (strcmp(pcm_capture_device, "") == 0) {
      strncpy(pcm_capture_device, DEFAULT_PCM_CAPTURE_DEVICE, sizeof(pcm_capture_device));
      pcm_capture_device[sizeof(pcm_capture_device) - 1] = '\0';
   }

   if (strcmp(pcm_playback_device, "") == 0) {
      strncpy(pcm_playback_device, DEFAULT_PCM_PLAYBACK_DEVICE, sizeof(pcm_playback_device));
      pcm_playback_device[sizeof(pcm_playback_device) - 1] = '\0';
   }

   // Command Processing
   initActions(actions);

   printf("Reading json file...");
   configFile = fopen(CONFIG_FILE, "r");
   if (configFile == NULL) {
      fprintf(stderr, "Unable to open config file: %s\n", CONFIG_FILE);
      return 1;
   }

   if ((bytes_read = fread(buffer, 1, sizeof(buffer), configFile)) > 0) {
      buffer[bytes_read] = '\0';
   } else {
      fprintf(stderr, "Failed to read config file (%s): %s\n", CONFIG_FILE, strerror(bytes_read));
      fclose(configFile);
      return 1;
   }

   fclose(configFile);
   printf("Done.\n");

   if (parseCommandConfig(buffer, actions, &numActions,
                          captureDevices, &numAudioCaptureDevices,
                          playbackDevices, &numAudioPlaybackDevices)) {
      fprintf(stderr, "Error parsing json.\n");
      return 1;
   }

   printf("\n");
   printParsedData(actions, numActions);
   convertActionsToCommands(actions, &numActions, commands, &numCommands);
   printCommands(commands, numCommands);

   // JSON setup for OpenAI
   conversation_history = json_object_new_array();
   system_message = json_object_new_object();

   json_object_object_add(system_message, "role", json_object_new_string("system"));
   json_object_object_add(system_message, "content",
                          json_object_new_string
                          (AI_DESCRIPTION));
   json_object_array_add(conversation_history, system_message);

#ifdef ALSA_DEVICE
   // Open Audio Capture Device
   rc = openAlsaPcmCaptureDevice(&handle, pcm_capture_device, &frames);
   if (rc) {
      fprintf(stderr, "Error creating ALSA capture device.\n");
      return 1;
   }
   full_buff_size = frames * DEFAULT_CHANNELS * 2;

   myAudioControls.handle = handle;
   myAudioControls.frames = frames;
   myAudioControls.full_buff_size = full_buff_size;
#else
   myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
   if (myAudioControls.pa_handle == NULL) {
      fprintf(stderr, "Error creating Pulse capture device.\n");
      return 1;
   }

   myAudioControls.pa_latency = pa_simple_get_latency(myAudioControls.pa_handle, &error);
   myAudioControls.pa_framesize = pa_frame_size(&sample_spec);

   full_buff_size = myAudioControls.pa_framesize;

   myAudioControls.full_buff_size = full_buff_size;

   temp_buff_size = DEFAULT_CAPTURE_SECONDS * DEFAULT_RATE * myAudioControls.pa_framesize;
   max_buff_size = (myAudioControls.pa_latency + ceil(temp_buff_size)) / myAudioControls.pa_framesize;
#endif

   printf("max_buff_size: %u, full_buff_size: %u\n", max_buff_size, full_buff_size);

   // Allocate Audio Buffers
   buff = (char *)malloc(full_buff_size);
   if (buff == NULL) {
      fprintf(stderr, "malloc() failed on buff.\n");

#ifdef ALSA_DEVICE
      snd_pcm_close(handle);
#else
      pa_simple_free(myAudioControls.pa_handle);
#endif

      return 1;
   }

   max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      fprintf(stderr, "malloc() failed on max_buff.\n");

#ifdef ALSA_DEVICE
      snd_pcm_close(handle);
#else
      pa_simple_free(myAudioControls.pa_handle);
#endif

      return 1;
   }

   // Test background audio level
#if 0
   if (pthread_create(&backgroundAudioDetect, NULL, measureBackgroundAudio, (void *) &myAudioControls) != 0) {
      fprintf(stderr, "Error creating background audio detection thread.\n");
   }
#else
   measureBackgroundAudio((void *) &myAudioControls);
#endif

   // Vosk
   vosk_gpu_init();
   vosk_gpu_thread_init();

   VoskModel *model = vosk_model_new("model");
   if (model == NULL) {
      fprintf(stderr, "Error creating new Vosk model.\n");

      free(buff);
      free(max_buff);

#ifdef ALSA_DEVICE
      snd_pcm_close(handle);
#else
      pa_simple_free(myAudioControls.pa_handle);
#endif

      return 1;
   }
   VoskRecognizer *recognizer = vosk_recognizer_new(model, DEFAULT_RATE);
   if (recognizer == NULL) {
      fprintf(stderr, "Error creating new Vosk recognizer.\n");

      vosk_model_free(model);

      free(buff);
      free(max_buff);

#ifdef ALSA_DEVICE
      snd_pcm_close(handle);
#else
      pa_simple_free(myAudioControls.pa_handle);
#endif

      return 1;
   }

   /* MQTT Setup */
   mosquitto_lib_init();

   mosq = mosquitto_new(NULL, true, NULL);
   if (mosq == NULL){
      fprintf(stderr, "Error: Out of memory.\n");
      return 1;
   }

   /* Configure callbacks. This should be done before connecting ideally. */
   mosquitto_connect_callback_set(mosq, on_connect);
   mosquitto_subscribe_callback_set(mosq, on_subscribe);
   mosquitto_message_callback_set(mosq, on_message);

   /* Connect to local MQTT server. */
   rc = mosquitto_connect(mosq, "127.0.0.1", 1883, 60);
   if (rc != MOSQ_ERR_SUCCESS){
      mosquitto_destroy(mosq);
      fprintf(stderr, "Error on mosquitto_connect(): %s\n", mosquitto_strerror(rc));
      return 1;
   } else {
      printf("Connected to local MQTT server.\n");
   }

   rc = mosquitto_subscribe(mosq, NULL, APPLICATION_NAME, 0);
   if (rc != MOSQ_ERR_SUCCESS) {
      mosquitto_destroy(mosq);
      fprintf(stderr, "Error on mosquitto_subscribe():\"/%s\" : %s\n",
              APPLICATION_NAME, mosquitto_strerror(rc));
      return 1;
   } else {
      printf("Subscribed to \"%s\" MQTT.\n", APPLICATION_NAME);
   }

   /* Start processing MQTT events. */
   mosquitto_loop_start(mosq);
   /* End MQTT Setup */

   text_to_speech(pcm_playback_device, (char *) timeOfDayGreeting());

   // Main loop
   int quit = 0;
   int listening = 0;
   listeningState recState = SILENCE;

   printf("Listening...\n");
   listening = 1;

#ifdef ALSA_DEVICE
   snd_pcm_drop(handle);
   rc = snd_pcm_prepare(handle);
   if (rc < 0) {
   fprintf(stderr, "Cannot prepare audio interface for use (%s)\n",
      snd_strerror(rc));
      exit(1);
   }
#else
   if (pa_simple_flush(myAudioControls.pa_handle, &error) != 0) {
      printf("Unable to flush buffer: %s\n", pa_strerror(error));
   }
   pa_simple_free(myAudioControls.pa_handle);

   myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
   if (myAudioControls.pa_handle == NULL) {
      fprintf(stderr, "Error creating Pulse capture device.\n");
      return 1;
   }
#endif

   while (!quit) {
      // Record audio to buffer when 'r' key is pressed
      if (listening) {
         // Record audio to buffer
#ifdef ALSA_DEVICE
         rc = snd_pcm_readi(handle, buff, frames);
         if ((rc > 0) && (buff_size + full_buff_size <= max_buff_size)) {
            memcpy(max_buff + buff_size, buff, full_buff_size);
            buff_size += full_buff_size;
         } else {
            if (rc <= 0) {
               printf("Error reading PCM.\n");
            }
#else
         rc = pa_simple_read(myAudioControls.pa_handle, buff, myAudioControls.pa_framesize, &error);
         if ((rc == 0) && (buff_size + full_buff_size <= max_buff_size)) {
            memcpy(max_buff + buff_size, buff, myAudioControls.full_buff_size);
            buff_size += myAudioControls.full_buff_size;
         } else {
            if (rc < 0) {
               fprintf(stderr, "pa_simple_read() failed: %s\n", pa_strerror(error));
            }
#endif
            double rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));
            //printf("RMS of recording is %g.\n", rms);

            switch (recState) {
               case SILENCE:
                  if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
                     printf("SILENCE: Talking detected. Going into WAKEWORD_LISTENING.\n");
                     recState = WAKEWORD_LISTEN;

                     vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
                     vosk_output = vosk_recognizer_partial_result(recognizer);
                     if (vosk_output == NULL) {
                        fprintf(stderr, "vosk_recognizer_partial_result() returned NULL!\n");
                     } else {
                        printf("Partial Input: %s\n", vosk_output);
                     }
                  }
                  buff_size = 0;
                  break;
               case WAKEWORD_LISTEN:
                  if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
                     printf("WAKEWORD_LISTEN: Talking still in progress.\n");
                     vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
                     vosk_output = vosk_recognizer_partial_result(recognizer);
                     if (vosk_output == NULL) {
                        fprintf(stderr, "vosk_recognizer_partial_result() returned NULL!\n");
                     } else {
                        printf("Partial Input: %s\n", vosk_output);
                     }
                  } else if ((rms < (backgroundRMS + TALKING_THRESHOLD_OFFSET)) &&
                      ((commandTimeout) < DEFAULT_COMMAND_TIMEOUT)) {
                     printf(".");
                     commandTimeout++;
                  } else {
                     printf("\n");
                     commandTimeout = 0;
                     printf("WAKEWORD_LISTEN: Checking for wake word.\n");
                     vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
                     vosk_output = vosk_recognizer_final_result(recognizer);
                     if (vosk_output == NULL) {
                        fprintf(stderr, "vosk_recognizer_final_result() returned NULL!\n");
                     } else {
                        printf("Input: %s\n", vosk_output);
                        input_text = getTextResponse(vosk_output);

                        for (i = 0; i < numGoodbyeWords; i++) {
                           if (strcmp(input_text, goodbyeWords[i]) == 0) {
                              quit = 1;

                              text_to_speech(pcm_playback_device, "Goodbye sir.");
                           }
                        }

                        for (i = 0; i < numWakeWords; i++) {
                           char *found_ptr = strstr(input_text, wakeWords[i]);
                           if (found_ptr != NULL) {
                              printf("Wake word detected.\n");

                              // Calculate the length of the wake word
                              size_t wakeWordLength = strlen(wakeWords[i]);

                              // Advance the pointer to the next character after wakeWords[i]
                              next_char_ptr = found_ptr + wakeWordLength;

                              break;
                           }
                        }

                        if (i < numWakeWords) {

                           if (*next_char_ptr == '\0') {
                              printf("wakeWords[i] was found at the end of input_text.\n");
                              text_to_speech(pcm_playback_device, "Hello sir.");

#ifdef ALSA_DEVICE
                              /* Remove if we can detect immediately. */
                              snd_pcm_drop(handle);
                              if (snd_pcm_prepare(handle) < 0) {
                                 fprintf(stderr, "Cannot prepare audio interface for use (%s)\n",
                                 snd_strerror(rc));
                                 exit(1);
                              }
#else
                              if (pa_simple_flush(myAudioControls.pa_handle, &error) != 0) {
                                 printf("Unable to flush buffer: %s\n", pa_strerror(error));
                              }
                              pa_simple_free(myAudioControls.pa_handle);

                              myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
                              if (myAudioControls.pa_handle == NULL) {
                                 fprintf(stderr, "Error creating Pulse capture device.\n");
                                 return 1;
                              }
#endif

                              recState = COMMAND_RECORDING;
                           } else {
                              command_text = strdup(next_char_ptr);
                              free(input_text);

                              recState = PROCESS_COMMAND;
                           }
                        } else {
                           recState = SILENCE;
                        }
                     }
                  }
                  buff_size = 0;
                  break;
               case COMMAND_RECORDING:
                  if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
                     printf("COMMAND_RECORDING: Talking still in progress.\n");
                     vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
                     vosk_output = vosk_recognizer_partial_result(recognizer);
                     if (vosk_output == NULL) {
                        fprintf(stderr, "vosk_recognizer_partial_result() returned NULL!\n");
                     } else {
                        printf("Partial Input: %s\n", vosk_output);
                     }
                  } else if ((rms < (backgroundRMS + TALKING_THRESHOLD_OFFSET)) &&
                      ((commandTimeout) < DEFAULT_COMMAND_TIMEOUT)) {
                     printf(".");
                     commandTimeout++;
                  } else {
                     printf("\n");
                     commandTimeout = 0;
                     printf("COMMAND_RECORDING: Command processing.\n");
                     vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
                     vosk_output = vosk_recognizer_final_result(recognizer);
                     if (vosk_output == NULL) {
                        fprintf(stderr, "vosk_recognizer_final_result() returned NULL!\n");
                     } else {
                        printf("Input: %s\n", vosk_output);

                        input_text = getTextResponse(vosk_output);

                        command_text = strdup(input_text);
                        free(input_text);

                        recState = PROCESS_COMMAND;
                     }
                  }
                  buff_size = 0;
                  break;
               case PROCESS_COMMAND:
                  /* Process Commands before AI. */
                  for (i = 0; i < numCommands; i++) {
                     if (searchString(commands[i].actionWordsWildcard, command_text) == 1) {
                        char thisValue[1024];   // FIXME: These are abnormally large.
                                                // I'm in a hurry and don't want overflows.
                        char thisCommand[2048];
                        char thisSubstring[2048];
                        int strLength = 0;

                        memset(thisValue, '\0', sizeof(thisValue));
                        printf("Found command \"%s\".\n\tLooking for value in \"%s\".\n",
                               commands[i].actionWordsWildcard, commands[i].actionWordsRegex);

                        /* HERE */
                        strLength = strlen(commands[i].actionWordsRegex);
                        if ((strLength >= 2) && (commands[i].actionWordsRegex[strLength - 2] == '%') &&
                            (commands[i].actionWordsRegex[strLength - 1] == 's')) {
                           strncpy(thisSubstring, commands[i].actionWordsRegex, strLength - 2);
                           thisSubstring[strLength - 2] = '\0';
                           printf("extract_remaining_after_substring(\"%s\", %s\")\n", command_text, thisSubstring);
                           strcpy(thisValue, extract_remaining_after_substring(command_text, thisSubstring));
                        } else {
                           printf("sscanf(\"%s\", \"%s\", thisValue)\n",
                                  command_text, commands[i].actionWordsRegex);
                           int retSs = sscanf(command_text, commands[i].actionWordsRegex, thisValue);
                           printf("sscanf() returned %d and \"%s\"\n", retSs, thisValue);
                        }
                        snprintf(thisCommand, sizeof(thisCommand),
                                 commands[i].actionCommand, thisValue);
                        printf("Sending: \"%s\"\n", thisCommand);

                        rc = mosquitto_publish(mosq, NULL, commands[i].topic, strlen(thisCommand),
                                               thisCommand, 0, false);
                        if(rc != MOSQ_ERR_SUCCESS){
                           fprintf(stderr, "Error publishing: %s\n", mosquitto_strerror(rc));
                        }

                        break;
                     }
                  }

                  if (i >= numCommands) {
                     printf("Not detected as a command.\n");
#ifndef DISABLE_AI
                     for (i = 0; i < numIgnoreWords; i++) {
                        if (strcmp(command_text, ignoreWords[i]) == 0) {
                           printf("Ignore word detected.\n");
                           break;
                        }
                     }

                     if (i < numIgnoreWords) {
                        printf("Input ignored. Found in ignore list.\n");
                        recState = SILENCE;
                     } else {
                        response_text = getGptResponse(conversation_history, command_text);
                        if (response_text != NULL) {
                           printf("AI: %s\n", response_text);
                           text_to_speech(pcm_playback_device, response_text);

                           struct json_object *ai_message = json_object_new_object();
                           json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
                           json_object_object_add(ai_message, "content", json_object_new_string(response_text));
                           json_object_array_add(conversation_history, ai_message);

                           free(response_text);
                        } else {
                           fprintf(stderr, "GPT error.\n");
                           text_to_speech(pcm_playback_device, "I'm sorry but I'm currently unavailable boss.");
                        }
                     }
#endif
                  }

                  for (i = 0; i < numGoodbyeWords; i++) {
                     if (strcmp(command_text, goodbyeWords[i]) == 0) {
                        quit = 1;
                     }
                  }

                  free(command_text);

#ifdef ALSA_DEVICE
                  snd_pcm_drop(handle);
                  if (snd_pcm_prepare(handle) < 0) {
                  fprintf(stderr, "Cannot prepare audio interface for use (%s)\n",
                     snd_strerror(rc));
                     exit(1);
                  }
#else
                  if (pa_simple_flush(myAudioControls.pa_handle, &error) != 0) {
                     printf("Unable to flush buffer: %s\n", pa_strerror(error));
                  }
                  pa_simple_free(myAudioControls.pa_handle);

                  myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
                  if (myAudioControls.pa_handle == NULL) {
                     fprintf(stderr, "Error creating Pulse capture device.\n");
                     return 1;
                  }
#endif
                  recState = SILENCE;

                  break;
               default:
                  printf("I really shouldn't be here.\n");
            }
         }
      }
   }

   printf("Quit.\n");

   mosquitto_disconnect(mosq);
   mosquitto_loop_stop(mosq, false);
   mosquitto_lib_cleanup();

   json_object_put(conversation_history);

   // Cleanup
   vosk_recognizer_free(recognizer);
   vosk_model_free(model);

#ifdef ALSA_DEVICE
   snd_pcm_drop(handle);
   snd_pcm_close(handle);
#else
   pa_simple_free(myAudioControls.pa_handle);
#endif
   free(buff);
   free(max_buff);

   curl_global_cleanup();

   return 0;
}
