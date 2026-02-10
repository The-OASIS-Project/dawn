/*
 * DAWN Satellite - Main Application
 *
 * Voice satellite for DAWN voice assistant server.
 * Supports two protocols:
 *   - DAP (Tier 2): Push-to-talk, audio streaming to server
 *   - DAP2 (Tier 1): Local ASR/TTS, text-only to server
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "audio_capture.h"
#include "audio_playback.h"
#include "logging.h"
#include "satellite_config.h"
#include "satellite_state.h"
#include "voice_processing.h"

#ifdef ENABLE_DAP2
#include "ws_client.h"
#else
#include "dap_client.h"
#endif

#ifdef ENABLE_DISPLAY
#include "display.h"
#endif

#ifdef ENABLE_SDL_UI
#include "sdl_ui.h"
#endif

#ifdef HAVE_OPUS
#include "music_playback.h"
#include "music_stream.h"
#endif

#ifdef ENABLE_NEOPIXEL
#include "neopixel.h"
#endif

#ifdef HAVE_GPIOD
#include "gpio_control.h"
#endif

#define VERSION "2.0.0"

/* Global context for signal handler */
static satellite_ctx_t *g_ctx = NULL;
static voice_ctx_t *g_voice_ctx = NULL;
#ifdef ENABLE_SDL_UI
static sdl_ui_t *g_sdl_ui = NULL;
#endif

#ifdef ENABLE_NEOPIXEL
static neopixel_t *g_neopixel = NULL;

/* Helper to update NeoPixels based on satellite state */
static void update_neopixel_for_state(satellite_state_t state) {
   if (!g_neopixel || !g_neopixel->initialized)
      return;

   switch (state) {
      case STATE_IDLE:
      case STATE_SILENCE:
         neopixel_set_mode(g_neopixel, NEO_IDLE_CYCLING);
         break;
      case STATE_RECORDING:
      case STATE_WAKEWORD_LISTEN:
      case STATE_COMMAND_RECORDING:
         neopixel_set_mode(g_neopixel, NEO_RECORDING);
         break;
      case STATE_CONNECTING:
      case STATE_SENDING:
      case STATE_WAITING:
      case STATE_RECEIVING:
      case STATE_PROCESSING:
         neopixel_set_mode(g_neopixel, NEO_WAITING);
         break;
      case STATE_PLAYING:
      case STATE_SPEAKING:
         neopixel_set_mode(g_neopixel, NEO_PLAYING);
         break;
      case STATE_ERROR:
         neopixel_set_mode(g_neopixel, NEO_ERROR);
         break;
   }
}
#endif

/* Signal handler for clean shutdown */
static void signal_handler(int sig) {
   (void)sig;
   if (g_ctx) {
      g_ctx->running = 0;
      g_ctx->stop_recording = 1;
      g_ctx->stop_playback = 1;
   }
   if (g_voice_ctx) {
      voice_processing_stop(g_voice_ctx);
   }
#ifdef ENABLE_SDL_UI
   if (g_sdl_ui) {
      sdl_ui_stop(g_sdl_ui);
   }
#endif
}

/* Print usage */
static void print_usage(const char *prog) {
   printf("DAWN Satellite v%s - Voice satellite for DAWN server\n", VERSION);
#ifdef ENABLE_DAP2
   printf("Protocol: DAP2 (Tier 1, text-based)\n\n");
#else
   printf("Protocol: DAP (Tier 2, audio streaming)\n\n");
#endif
   printf("Usage: %s [options]\n\n", prog);
   printf("Options:\n");
   printf("  -C, --config FILE    Configuration file (default: auto-detect)\n");
   printf("  -s, --server IP      DAWN server IP/hostname (default: localhost)\n");
#ifdef ENABLE_DAP2
   printf("  -p, --port PORT      WebUI port (default: 8080)\n");
   printf("  -S, --ssl            Use secure WebSocket (wss://)\n");
   printf("  -I, --no-ssl-verify  Disable SSL certificate verification (dev only!)\n");
   printf("  -N, --name NAME      Satellite name (default: Satellite)\n");
   printf("  -L, --location LOC   Satellite location (default: unset)\n");
#else
   printf("  -p, --port PORT      DAP server port (default: 5000)\n");
#endif
   printf("  -c, --capture DEV    ALSA capture device (default: plughw:0,0)\n");
   printf("  -o, --playback DEV   ALSA playback device (default: plughw:0,0)\n");
#ifdef ENABLE_DAP2
   printf("  -k, --keyboard       Use keyboard input for testing (bypasses VAD)\n");
#else
   printf("  -k, --keyboard       Use keyboard instead of GPIO button\n");
   printf("  -n, --num-leds N     Number of NeoPixel LEDs (default: 3)\n");
#endif
   printf("  -d, --no-display     Disable framebuffer display\n");
   printf("  -v, --verbose        Enable verbose logging\n");
#ifdef ENABLE_DAP2
   printf("  -V, --voice          Enable voice-activated mode (VAD + wake word + ASR + TTS)\n");
#endif
   printf("  -h, --help           Show this help message\n");
   printf("\n");
   printf("Operation:\n");
#ifdef ENABLE_DAP2
   printf("  Production (-V): Wake word (\"Hey Friday\") activates, VAD detects end-of-speech\n");
   printf("  Testing (-k): Type text at prompt to simulate transcribed speech\n");
#else
   printf("  GPIO button or SPACE: Press and hold to record, release to send\n");
#endif
   printf("  Ctrl+C: Exit\n");
   printf("\n");
}

#ifndef ENABLE_DAP2
/* Check if key is pressed (non-blocking) - used by DAP mode only */
static int kbhit(void) {
   struct termios oldt, newt;
   int ch;
   int oldf;

   tcgetattr(STDIN_FILENO, &oldt);
   newt = oldt;
   newt.c_lflag &= ~(ICANON | ECHO);
   tcsetattr(STDIN_FILENO, TCSANOW, &newt);
   oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
   fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

   ch = getchar();

   tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
   fcntl(STDIN_FILENO, F_SETFL, oldf);

   if (ch != EOF) {
      ungetc(ch, stdin);
      return 1;
   }

   return 0;
}

/* Get pressed key - used by DAP mode only */
static int getch(void) {
   struct termios oldt, newt;
   int ch;

   tcgetattr(STDIN_FILENO, &oldt);
   newt = oldt;
   newt.c_lflag &= ~(ICANON | ECHO);
   tcsetattr(STDIN_FILENO, TCSANOW, &newt);
   ch = getchar();
   tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

   return ch;
}
#endif /* !ENABLE_DAP2 */

#ifdef ENABLE_DAP2
/* =============================================================================
 * DAP2 Mode (Tier 1) - Text-based protocol
 * ============================================================================= */

/* Response accumulator for streaming */
static char g_response_buffer[8192];
static size_t g_response_len = 0;
static int g_response_complete = 0;

static void on_stream_callback(const char *text, bool is_end, void *user_data) {
   (void)user_data;

   if (text && text[0]) {
      /* Append to buffer */
      size_t len = strlen(text);
      if (g_response_len + len < sizeof(g_response_buffer) - 1) {
         memcpy(g_response_buffer + g_response_len, text, len);
         g_response_len += len;
         g_response_buffer[g_response_len] = '\0';
      }

      /* Print streamed text */
      printf("%s", text);
      fflush(stdout);
   }

   if (is_end) {
      g_response_complete = 1;
      printf("\n");
   }
}

static void on_state_callback(const char *state, void *user_data) {
   satellite_ctx_t *ctx = (satellite_ctx_t *)user_data;
   (void)ctx;

   printf("[State: %s]\n", state);

#ifdef ENABLE_NEOPIXEL
   if (strcmp(state, "thinking") == 0) {
      update_neopixel_for_state(STATE_WAITING);
   } else if (strcmp(state, "idle") == 0) {
      update_neopixel_for_state(STATE_IDLE);
   }
#endif
}

/* Main loop for DAP2 mode */
static int dap2_main_loop(satellite_ctx_t *ctx,
                          ws_client_t *ws,
                          int use_keyboard,
                          satellite_config_t *config,
                          voice_ctx_t *voice_ctx,
                          bool first_connect) {
   /* Build identity from config */
   ws_identity_t identity;
   memset(&identity, 0, sizeof(identity));

   /* Use UUID from config (already ensured) */
   snprintf(identity.uuid, sizeof(identity.uuid), "%s", config->identity.uuid);
   snprintf(identity.name, sizeof(identity.name), "%s", config->identity.name);
   snprintf(identity.location, sizeof(identity.location), "%s", config->identity.location);

   /* Load reconnect_secret from config (if we have one from previous session) */
   if (config->identity.reconnect_secret[0]) {
      snprintf(identity.reconnect_secret, sizeof(identity.reconnect_secret), "%s",
               config->identity.reconnect_secret);
      printf("Attempting reconnection with saved session secret...\n");
   }

   ws_capabilities_t caps = { .local_asr = true, /* Will have local ASR */
                              .local_tts = true, /* Will have local TTS */
                              .wake_word = true /* Will have wake word */ };

   printf("Registering satellite '%s'...\n", identity.name);
   if (ws_client_register(ws, &identity, &caps) != 0) {
      fprintf(stderr, "Failed to register: %s\n", ws_client_get_error(ws));
      return 1;
   }

   /* Save the reconnect_secret received from server for future reconnections */
   const char *new_secret = ws_client_get_reconnect_secret(ws);
   if (new_secret && new_secret[0]) {
      satellite_config_set_reconnect_secret(config, new_secret);
      printf("Session secret saved for future reconnections\n");
   }

#ifdef HAVE_OPUS
   /* Create music playback + streaming after successful registration */
   music_playback_t *music_pb = NULL;
   music_stream_t *music_ws = NULL;
   if (ctx->audio_playback && ((audio_playback_t *)ctx->audio_playback)->initialized) {
      const char *stok = ws_client_get_session_token(ws);
      if (stok) {
         music_pb = music_playback_create((audio_playback_t *)ctx->audio_playback);
         if (music_pb) {
            music_ws = music_stream_create(config->server.host, config->server.port,
                                           config->server.ssl, config->server.ssl_verify, stok,
                                           music_pb);
            if (music_ws) {
               if (music_stream_connect(music_ws) == 0) {
                  printf("Music streaming connected\n");
               } else {
                  printf("Music streaming connection failed (will retry)\n");
               }
            }
#ifdef ENABLE_SDL_UI
            if (g_sdl_ui && music_pb) {
               sdl_ui_set_music_playback(g_sdl_ui, music_pb);
            }
#endif
            /* Wire music into voice processing for TTS arbitration */
            if (voice_ctx && music_pb) {
               voice_processing_set_music_playback(voice_ctx, music_pb);
            }
         }
      } else {
         printf("No session token received — music streaming disabled\n");
      }
   }
#endif

   printf("\n=== DAWN Satellite Ready (DAP2 Mode) ===\n");
   printf("UUID: %s\n", identity.uuid);
   printf("Name: %s\n", identity.name);
   if (identity.location[0]) {
      printf("Location: %s\n", identity.location);
   }
   printf("\n");

   /* Check processing mode - voice mode uses VAD, doesn't need GPIO */
   if (config->processing.mode == PROCESSING_MODE_VOICE_ACTIVATED && voice_ctx) {
      printf("Say '%s' to activate\n", config->wake_word.word);
      printf("Press Ctrl+C to exit\n\n");

      /* Set global for signal handler */
      g_voice_ctx = voice_ctx;

      /* Speak welcome greeting on first connect only */
      if (first_connect) {
         voice_processing_speak_greeting(voice_ctx, ctx);
      }

      /* Run voice-activated loop */
      int result = voice_processing_loop(voice_ctx, ctx, ws, config);

      /* Check if loop exited due to disconnection (not user-requested stop) */
      if (!ws_client_is_connected(ws) && ctx->running) {
         /* Connection lost - speak offline message */
         voice_processing_speak_offline(voice_ctx, ctx);
      }

#ifdef HAVE_OPUS
      if (music_ws)
         music_stream_destroy(music_ws);
      if (music_pb)
         music_playback_destroy(music_pb);
#endif
      return result;
   }

   if (use_keyboard) {
      printf("Type a message and press Enter to send\n");
      printf("Type 'quit' or press Ctrl+C to exit\n\n");
      printf("> ");
      fflush(stdout);
   } else {
      printf("Voice mode not active. Use -V flag or set mode = \"voice_activated\" in config\n");
      printf("Press Ctrl+C to exit\n\n");
   }

   /* Set up callbacks */
   ws_client_set_stream_callback(ws, on_stream_callback, ctx);
   ws_client_set_state_callback(ws, on_state_callback, ctx);

   char input_buffer[1024] = { 0 };

   while (ctx->running && ws_client_is_connected(ws)) {
      /* Check for keyboard input using select() for non-blocking check */
      if (use_keyboard) {
         fd_set fds;
         struct timeval tv = { 0, 50000 }; /* 50ms timeout */

         FD_ZERO(&fds);
         FD_SET(STDIN_FILENO, &fds);

         int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

         if (ready > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            if (fgets(input_buffer, sizeof(input_buffer), stdin)) {
               /* Remove newline */
               size_t len = strlen(input_buffer);
               if (len > 0 && input_buffer[len - 1] == '\n') {
                  input_buffer[len - 1] = '\0';
                  len--;
               }

               /* Check for quit command */
               if (strcmp(input_buffer, "quit") == 0 || strcmp(input_buffer, "exit") == 0) {
                  ctx->running = 0;
                  break;
               }

               if (len > 0) {
#ifdef ENABLE_NEOPIXEL
                  update_neopixel_for_state(STATE_SENDING);
#endif

                  /* Reset response buffer */
                  g_response_buffer[0] = '\0';
                  g_response_len = 0;
                  g_response_complete = 0;

                  /* Send query */
                  if (ws_client_send_query(ws, input_buffer) != 0) {
                     printf("Failed to send query: %s\n", ws_client_get_error(ws));
                  } else {
                     printf("[Response]: ");
                     fflush(stdout);

                     /* Wait for response
                      * NOTE: Don't call ws_client_service() - background thread handles it */
                     while (!g_response_complete && ctx->running && ws_client_is_connected(ws)) {
                        usleep(50000); /* 50ms */
                     }

                     /* In real implementation, we would:
                      * 1. Run local TTS on g_response_buffer
                      * 2. Play audio */
                     printf("\n[TTS would speak]: %.200s%s\n", g_response_buffer,
                            g_response_len > 200 ? "..." : "");

#ifdef ENABLE_NEOPIXEL
                     update_neopixel_for_state(STATE_IDLE);
#endif
                  }

                  printf("\n> ");
                  fflush(stdout);
               }
            }
         }
         /* No keyboard input - background thread handles WebSocket */
      }
      /* WebSocket servicing handled by background thread in ws_client */

#ifdef HAVE_GPIOD
      /* TODO: Handle GPIO button for real voice input */
#endif

#ifdef ENABLE_NEOPIXEL
      neopixel_update(g_neopixel);
#endif

      usleep(10000); /* 10ms */
   }

#ifdef HAVE_OPUS
   if (music_ws)
      music_stream_destroy(music_ws);
   if (music_pb)
      music_playback_destroy(music_pb);
#endif
   return 0;
}

#else /* !ENABLE_DAP2 */
/* =============================================================================
 * DAP Mode (Tier 2) - Audio streaming protocol
 * ============================================================================= */

/* Main recording and transaction loop */
static int do_recording_transaction(satellite_ctx_t *ctx) {
   audio_capture_t *capture = (audio_capture_t *)ctx->audio_capture;
   audio_playback_t *playback = (audio_playback_t *)ctx->audio_playback;
   dap_client_t *client = (dap_client_t *)ctx->dap_client;

   /* Start recording */
   printf("[MAIN] Recording... (release button to stop)\n");
#ifdef ENABLE_NEOPIXEL
   update_neopixel_for_state(STATE_RECORDING);
#endif
   ctx->stop_recording = 0;
   ctx->recorded_samples = 0;

   ssize_t samples = audio_capture_record(capture, ctx->audio_buffer, ctx->audio_buffer_size,
                                          &ctx->stop_recording);

   if (samples <= 0) {
#ifdef ENABLE_NEOPIXEL
      update_neopixel_for_state(STATE_ERROR);
#endif
      satellite_set_error(ctx, -1, "Recording failed");
      return -1;
   }

   ctx->recorded_samples = samples;
   printf("[MAIN] Recorded %zu samples (%.2f seconds)\n", ctx->recorded_samples,
          (float)ctx->recorded_samples / AUDIO_SAMPLE_RATE);

   /* Create WAV from recorded audio */
   uint8_t *wav_data = NULL;
   size_t wav_size = 0;

   if (audio_create_wav(ctx->audio_buffer, ctx->recorded_samples, &wav_data, &wav_size) != 0) {
#ifdef ENABLE_NEOPIXEL
      update_neopixel_for_state(STATE_ERROR);
#endif
      satellite_set_error(ctx, -1, "Failed to create WAV");
      return -1;
   }

   /* Connect to server */
   printf("[MAIN] Connecting to %s:%u...\n", ctx->server_ip, ctx->server_port);
#ifdef ENABLE_NEOPIXEL
   update_neopixel_for_state(STATE_CONNECTING);
#endif
   satellite_process_event(ctx, EVENT_CONNECT_SUCCESS);

   if (dap_client_init(client, ctx->server_ip, ctx->server_port) != DAP_SUCCESS) {
      free(wav_data);
#ifdef ENABLE_NEOPIXEL
      update_neopixel_for_state(STATE_ERROR);
#endif
      satellite_set_error(ctx, -1, "Failed to init DAP client");
      return -1;
   }

   if (dap_client_connect(client) != DAP_SUCCESS) {
      free(wav_data);
      dap_client_cleanup(client);
#ifdef ENABLE_NEOPIXEL
      update_neopixel_for_state(STATE_ERROR);
#endif
      satellite_set_error(ctx, -1, "Connection failed");
      return -1;
   }

   /* Send audio and get response */
   printf("[MAIN] Sending audio...\n");
#ifdef ENABLE_NEOPIXEL
   update_neopixel_for_state(STATE_SENDING);
#endif
   satellite_process_event(ctx, EVENT_SEND_COMPLETE);

   uint8_t *response = NULL;
   size_t response_size = 0;

   int result = dap_client_transact(client, wav_data, wav_size, &response, &response_size);

   free(wav_data);
   dap_client_disconnect(client);

   if (result != DAP_SUCCESS || !response || response_size == 0) {
      if (response)
         free(response);
#ifdef ENABLE_NEOPIXEL
      update_neopixel_for_state(STATE_ERROR);
#endif
      satellite_set_error(ctx, -1, "Transaction failed");
      return -1;
   }

   printf("[MAIN] Received %zu bytes response\n", response_size);

   /* Store response for playback */
   if (ctx->response_buffer) {
      free(ctx->response_buffer);
   }
   ctx->response_buffer = response;
   ctx->response_size = response_size;

   /* Play response */
   printf("[MAIN] Playing response...\n");
#ifdef ENABLE_NEOPIXEL
   update_neopixel_for_state(STATE_PLAYING);
#endif
   satellite_process_event(ctx, EVENT_RESPONSE_READY);

   ctx->stop_playback = 0;
   result = audio_playback_play_wav(playback, ctx->response_buffer, ctx->response_size,
                                    &ctx->stop_playback);

   if (result != 0) {
#ifdef ENABLE_NEOPIXEL
      update_neopixel_for_state(STATE_ERROR);
#endif
      satellite_set_error(ctx, -1, "Playback failed");
      return -1;
   }

   printf("[MAIN] Playback complete\n");
#ifdef ENABLE_NEOPIXEL
   update_neopixel_for_state(STATE_IDLE);
#endif
   satellite_process_event(ctx, EVENT_PLAYBACK_DONE);

   return 0;
}

/* Main loop for DAP mode */
static int dap_main_loop(satellite_ctx_t *ctx, int use_keyboard) {
   int button_was_pressed = 0;

   while (ctx->running) {
      int button_pressed = 0;

      if (use_keyboard) {
         if (kbhit()) {
            int ch = getch();
            if (ch == ' ') {
               button_pressed = 1;
            } else if (ch == 'q' || ch == 'Q') {
               ctx->running = 0;
               break;
            }
         }
      }
#ifdef HAVE_GPIOD
      else {
         gpio_control_t *gpio = (gpio_control_t *)ctx->gpio;
         if (gpio->initialized) {
            button_pressed = gpio_button_read(gpio);
         }
      }
#endif

      if (button_pressed && !button_was_pressed) {
         printf("[MAIN] Button pressed - starting recording\n");
         satellite_process_event(ctx, EVENT_BUTTON_PRESS);
      } else if (!button_pressed && button_was_pressed) {
         printf("[MAIN] Button released - processing\n");
         ctx->stop_recording = 1;
         satellite_process_event(ctx, EVENT_BUTTON_RELEASE);

         if (do_recording_transaction(ctx) != 0) {
            printf("[MAIN] Transaction failed: %s\n", ctx->error_msg);
            satellite_process_event(ctx, EVENT_ERROR);
            sleep(2);
            satellite_process_event(ctx, EVENT_TIMEOUT);
         }
      }

      button_was_pressed = button_pressed;

#ifdef ENABLE_NEOPIXEL
      neopixel_update(g_neopixel);
#endif

      usleep(10000);
   }

   return 0;
}

#endif /* ENABLE_DAP2 */

int main(int argc, char *argv[]) {
   satellite_ctx_t ctx;
   satellite_config_t config;
   int use_keyboard = 0;
   int voice_mode = 0; /* -V flag overrides config processing mode */
#ifdef ENABLE_DISPLAY
   int no_display = 0;
#endif
   int verbose = 0;

   /* Command-line override values (NULL/0/-1 = not specified) */
   const char *config_file = NULL;
   const char *cli_server = NULL;
   uint16_t cli_port = 0;
   int cli_ssl = -1;
   int cli_ssl_verify = -1; /* -1=default, 0=disable, 1=enable */
   const char *cli_capture = NULL;
   const char *cli_playback = NULL;
   const char *cli_name = NULL;
   const char *cli_location = NULL;
   int cli_num_leds = 0;

#ifdef ENABLE_NEOPIXEL
   neopixel_t neopixel_ctx;
   memset(&neopixel_ctx, 0, sizeof(neopixel_ctx));
#endif

   /* Parse command line options */
   static struct option long_options[] = { { "config", required_argument, 0, 'C' },
                                           { "server", required_argument, 0, 's' },
                                           { "port", required_argument, 0, 'p' },
                                           { "capture", required_argument, 0, 'c' },
                                           { "playback", required_argument, 0, 'o' },
                                           { "keyboard", no_argument, 0, 'k' },
                                           { "no-display", no_argument, 0, 'd' },
                                           { "num-leds", required_argument, 0, 'n' },
                                           { "verbose", no_argument, 0, 'v' },
                                           { "help", no_argument, 0, 'h' },
#ifdef ENABLE_DAP2
                                           { "ssl", no_argument, 0, 'S' },
                                           { "no-ssl-verify", no_argument, 0, 'I' },
                                           { "name", required_argument, 0, 'N' },
                                           { "location", required_argument, 0, 'L' },
                                           { "voice", no_argument, 0, 'V' },
#endif
                                           { 0, 0, 0, 0 } };

#ifdef ENABLE_DAP2
   const char *optstring = "C:s:p:c:o:kdn:vhSIN:L:V";
#else
   const char *optstring = "C:s:p:c:o:kdn:vh";
#endif

   int opt;
   while ((opt = getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
      switch (opt) {
         case 'C':
            config_file = optarg;
            break;
         case 's':
            cli_server = optarg;
            break;
         case 'p':
            cli_port = (uint16_t)atoi(optarg);
            break;
         case 'c':
            cli_capture = optarg;
            break;
         case 'o':
            cli_playback = optarg;
            break;
         case 'k':
            use_keyboard = 1;
            break;
         case 'd':
#ifdef ENABLE_DISPLAY
            no_display = 1;
#endif
            break;
         case 'n':
            cli_num_leds = atoi(optarg);
            if (cli_num_leds < 1)
               cli_num_leds = 1;
            if (cli_num_leds > 16)
               cli_num_leds = 16;
            break;
         case 'v':
            verbose = 1;
            break;
         case 'h':
            print_usage(argv[0]);
            return 0;
#ifdef ENABLE_DAP2
         case 'S':
            cli_ssl = 1;
            break;
         case 'I':
            cli_ssl_verify = 0; /* Disable SSL verification (for development only) */
            break;
         case 'N':
            cli_name = optarg;
            break;
         case 'L':
            cli_location = optarg;
            break;
         case 'V':
            voice_mode = 1;
            break;
#endif
         default:
            print_usage(argv[0]);
            return 1;
      }
   }

   /* Initialize logging (console mode, bridges DAWN_LOG_* from common library) */
   init_logging(NULL, LOG_TO_CONSOLE);

   /* Initialize config with defaults */
   satellite_config_init_defaults(&config);

   /* Load config file (auto-detect if not specified) */
   int config_result = satellite_config_load(&config, config_file);
   if (config_file && config_result != 0) {
      fprintf(stderr, "Failed to load config file: %s\n", config_file);
      return 1;
   }

   /* Apply command-line overrides */
   satellite_config_apply_overrides(&config, cli_server, cli_port, cli_ssl, cli_ssl_verify,
                                    cli_name, cli_location, cli_capture, cli_playback, cli_num_leds,
                                    use_keyboard);

   /* Ensure we have a UUID */
   satellite_config_ensure_uuid(&config);

#ifdef ENABLE_DAP2
   /* -V flag overrides config processing mode */
   if (voice_mode) {
      config.processing.mode = PROCESSING_MODE_VOICE_ACTIVATED;
   }

   /* Validate model paths (warns if missing, disables features gracefully) */
   satellite_config_validate_paths(&config);
#endif

   if (verbose) {
#ifdef ENABLE_DAP2
      setenv("WS_DEBUG", "1", 1);
#else
      setenv("DAP_DEBUG", "1", 1);
#endif
      satellite_config_print(&config);
   }

   printf("DAWN Satellite v%s starting...\n", VERSION);
#ifdef ENABLE_DAP2
   printf("Protocol: DAP2 (Tier 1)\n");
   printf("Server: %s://%s:%u\n", config.server.ssl ? "wss" : "ws", config.server.host,
          config.server.port);
#else
   printf("Protocol: DAP (Tier 2)\n");
   printf("Server: %s:%u\n", config.server.host, config.server.port);
#endif
   printf("Identity: %s @ %s\n", config.identity.name,
          config.identity.location[0] ? config.identity.location : "(no location)");

   /* Initialize satellite context */
   if (satellite_init(&ctx) != 0) {
      fprintf(stderr, "Failed to initialize satellite\n");
      return 1;
   }

   g_ctx = &ctx;

   /* Configure satellite from config */
   satellite_set_server(&ctx, config.server.host, config.server.port);
   satellite_set_audio_devices(&ctx, config.audio.capture_device, config.audio.playback_device);

   /* Set up signal handlers */
   signal(SIGINT, signal_handler);
   signal(SIGTERM, signal_handler);

   /* Initialize audio only for voice-activated mode */
   if (config.processing.mode == PROCESSING_MODE_VOICE_ACTIVATED) {
      printf("Initializing audio capture...\n");
      audio_capture_t *capture = NULL;
      if (audio_capture_init(&capture, ctx.capture_device) != 0) {
         fprintf(stderr, "Failed to initialize audio capture\n");
         satellite_cleanup(&ctx);
         return 1;
      }
      ctx.audio_capture = capture;

      printf("Initializing audio playback...\n");
      if (audio_playback_init((audio_playback_t *)ctx.audio_playback, ctx.playback_device) != 0) {
         fprintf(stderr, "Failed to initialize audio playback\n");
         satellite_cleanup(&ctx);
         return 1;
      }
   } else {
      printf("Text-only mode: audio devices not initialized\n");
   }

#ifdef ENABLE_DISPLAY
   if (config.display.enabled && !no_display) {
      printf("Initializing display...\n");
      if (display_init((display_t *)ctx.display, config.display.device) != 0) {
         printf("Display not available, continuing without\n");
      }
   }
#endif

#ifdef HAVE_GPIOD
   if (config.gpio.enabled && !use_keyboard) {
      printf("Initializing GPIO...\n");
      if (gpio_init((gpio_control_t *)ctx.gpio, config.gpio.chip) != 0) {
         printf("GPIO not available, falling back to keyboard\n");
         use_keyboard = 1;
      }
   } else {
      use_keyboard = 1;
   }
#else
   use_keyboard = 1;
#endif

#ifdef ENABLE_NEOPIXEL
   if (config.neopixel.enabled) {
      printf("Initializing NeoPixels (%d LEDs)...\n", config.neopixel.num_leds);
      if (neopixel_init(&neopixel_ctx, config.neopixel.spi_device, config.neopixel.num_leds) != 0) {
         printf("NeoPixel init failed, continuing without LEDs\n");
      } else {
         g_neopixel = &neopixel_ctx;
         neopixel_set_brightness(&neopixel_ctx, config.neopixel.brightness);
         neopixel_set_mode(&neopixel_ctx, NEO_IDLE_CYCLING);
      }
   }
#endif

   /* Update display with initial state */
   satellite_update_display(&ctx);
   satellite_update_leds(&ctx);

   int result = 0;

   /* Load voice models BEFORE connecting to daemon.
    * Models can take several seconds to load (especially on Pi).
    * We don't want the satellite appearing "ready" while still loading. */
   voice_ctx_t *voice_ctx = NULL;
   if (config.processing.mode == PROCESSING_MODE_VOICE_ACTIVATED) {
      printf("Loading voice models...\n");
      voice_ctx = voice_processing_init(&config);
      if (!voice_ctx) {
         fprintf(stderr, "Failed to initialize voice processing\n");
         satellite_cleanup(&ctx);
         return 1;
      }
      g_voice_ctx = voice_ctx;
      printf("Voice models loaded\n");

      /* Check if Ctrl+C was pressed during model loading */
      if (!ctx.running) {
         printf("\nInterrupted during model loading\n");
         voice_processing_cleanup(voice_ctx);
         satellite_cleanup(&ctx);
         close_logging();
         return 0;
      }
   }

#ifdef ENABLE_DAP2
#ifdef ENABLE_SDL_UI
   /* Start SDL2 UI BEFORE connection loop — persists across reconnections */
   sdl_ui_t *sdl_ui = NULL;
   if (config.sdl_ui.enabled && voice_ctx) {
      sdl_ui_config_t ui_config = {
         .width = config.sdl_ui.width,
         .height = config.sdl_ui.height,
         .font_dir = config.sdl_ui.font_dir,
         .ai_name = config.general.ai_name,
         .voice_ctx = voice_ctx,
         .satellite_name = config.identity.name,
         .satellite_location = config.identity.location,
      };
      sdl_ui = sdl_ui_init(&ui_config);
      if (sdl_ui) {
         g_sdl_ui = sdl_ui;
         sdl_ui_start(sdl_ui);
      } else {
         printf("SDL UI init failed, continuing headless\n");
      }
   }
#endif

   /* Connection loop with automatic reconnection */
   {
      uint32_t delay_ms = config.server.reconnect_delay_ms;
      uint32_t max_attempts = config.server.max_reconnect_attempts;
      uint32_t attempt = 0;
      bool first_connect = true;

      while (ctx.running) {
         /* Create WebSocket client */
         ws_client_t *ws = ws_client_create(config.server.host, config.server.port,
                                            config.server.ssl, config.server.ssl_verify);
         if (!ws) {
            fprintf(stderr, "Failed to create WebSocket client\n");
            break;
         }

         /* Attempt connection */
         printf("Connecting to daemon at %s:%d...\n", config.server.host, config.server.port);
         if (ws_client_connect(ws) != 0) {
            fprintf(stderr, "Connection failed: %s\n", ws_client_get_error(ws));
            ws_client_destroy(ws);

            attempt++;
            if (max_attempts > 0 && attempt >= max_attempts) {
               fprintf(stderr, "Max reconnect attempts (%u) reached\n", max_attempts);
               break;
            }

            /* Exponential backoff: delay doubles each attempt, caps at 60s */
            uint32_t backoff = delay_ms * (1u << (attempt < 5 ? attempt : 5));
            if (backoff > 60000)
               backoff = 60000;
            printf("Retrying in %u ms (attempt %u)...\n", backoff, attempt);
            usleep(backoff * 1000);
            continue;
         }

         /* Check if interrupted during connection */
         if (!ctx.running) {
            ws_client_destroy(ws);
            break;
         }

         /* Connected — reset attempt counter */
         attempt = 0;

#ifdef ENABLE_SDL_UI
         /* Wire WS client to SDL UI for music controls before registration
          * (so music_subscribe auto-fires in ws_client_register) */
         if (sdl_ui) {
            sdl_ui_set_ws_client(sdl_ui, ws);
         }
#endif

         /* Run main loop (returns when disconnected or user exits) */
         result = dap2_main_loop(&ctx, ws, use_keyboard, &config, voice_ctx, first_connect);
         first_connect = false;
         ws_client_destroy(ws);

         /* If user requested exit, don't reconnect */
         if (!ctx.running) {
            break;
         }

         /* Connection lost — retry */
         printf("Connection lost, reconnecting in %u ms...\n", delay_ms);
         usleep(delay_ms * 1000);
      }
   }

#ifdef ENABLE_SDL_UI
   if (sdl_ui) {
      sdl_ui_stop(sdl_ui);
      sdl_ui_cleanup(sdl_ui);
      g_sdl_ui = NULL;
   }
#endif

   if (voice_ctx)
      voice_processing_cleanup(voice_ctx);
#else
   printf("\n=== DAWN Satellite Ready (DAP Mode) ===\n");
   if (use_keyboard) {
      printf("Press SPACE to start recording, release to send\n");
   } else {
      printf("Press GPIO button to start recording, release to send\n");
   }
   printf("Press Ctrl+C to exit\n\n");

   result = dap_main_loop(&ctx, use_keyboard);
#endif

   printf("\nShutting down...\n");

#ifdef ENABLE_NEOPIXEL
   neopixel_cleanup(&neopixel_ctx);
#endif

   satellite_cleanup(&ctx);
   close_logging();
   printf("Goodbye!\n");

   return result;
}
