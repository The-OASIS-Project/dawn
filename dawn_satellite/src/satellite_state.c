/*
 * DAWN Satellite - State Machine Implementation
 *
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
 */

#include "satellite_state.h"

#include "audio_capture.h"
#include "audio_playback.h"

#ifdef ENABLE_DAP2
#include "ws_client.h"
#else
#include "dap_client.h"
#endif

#ifdef ENABLE_DISPLAY
#include "display.h"
#endif

#ifdef HAVE_GPIOD
#include "gpio_control.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_INFO(fmt, ...) fprintf(stdout, "[STATE] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[STATE ERROR] " fmt "\n", ##__VA_ARGS__)

/* State names for logging */
static const char *state_names[] = { "IDLE",    "RECORDING", "CONNECTING", "SENDING",
                                     "WAITING", "RECEIVING", "PLAYING",    "ERROR" };

/* Event names for logging */
static const char *event_names[] = { "BUTTON_PRESS",    "BUTTON_RELEASE", "RECORD_COMPLETE",
                                     "CONNECT_SUCCESS", "CONNECT_FAIL",   "SEND_COMPLETE",
                                     "SEND_FAIL",       "RESPONSE_READY", "RESPONSE_FAIL",
                                     "PLAYBACK_DONE",   "ERROR",          "TIMEOUT" };

const char *satellite_state_name(satellite_state_t state) {
   if (state >= 0 && state < sizeof(state_names) / sizeof(state_names[0])) {
      return state_names[state];
   }
   return "UNKNOWN";
}

const char *satellite_event_name(satellite_event_t event) {
   if (event >= 0 && event < sizeof(event_names) / sizeof(event_names[0])) {
      return event_names[event];
   }
   return "UNKNOWN";
}

void satellite_set_error(satellite_ctx_t *ctx, int code, const char *fmt, ...) {
   if (!ctx)
      return;

   ctx->error_code = code;

   va_list args;
   va_start(args, fmt);
   vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, args);
   va_end(args);

   LOG_ERROR("Error %d: %s", code, ctx->error_msg);
}

int satellite_init(satellite_ctx_t *ctx) {
   if (!ctx)
      return -1;

   memset(ctx, 0, sizeof(satellite_ctx_t));

   /* Default configuration */
   strcpy(ctx->server_ip, "192.168.1.100");
   ctx->server_port = 5000;
   strcpy(ctx->capture_device, AUDIO_DEFAULT_CAPTURE_DEVICE);
   strcpy(ctx->playback_device, AUDIO_DEFAULT_PLAYBACK_DEVICE);

   /* Allocate audio buffer (30 seconds at 16kHz) */
   ctx->audio_buffer_size = AUDIO_SAMPLE_RATE * AUDIO_MAX_RECORD_TIME;
   ctx->audio_buffer = malloc(ctx->audio_buffer_size * sizeof(int16_t));
   if (!ctx->audio_buffer) {
      satellite_set_error(ctx, -1, "Failed to allocate audio buffer");
      return -1;
   }

   /* Allocate subsystem contexts */
#ifdef ENABLE_DAP2
   /* DAP2: WebSocket client is managed separately in main.c */
   ctx->dap_client = NULL;
#else
   ctx->dap_client = malloc(sizeof(dap_client_t));
   if (!ctx->dap_client) {
      satellite_set_error(ctx, -1, "Failed to allocate DAP client context");
      satellite_cleanup(ctx);
      return -1;
   }
   memset(ctx->dap_client, 0, sizeof(dap_client_t));
#endif

   ctx->audio_capture = malloc(sizeof(audio_capture_t));
   ctx->audio_playback = malloc(sizeof(audio_playback_t));

   if (!ctx->audio_capture || !ctx->audio_playback) {
      satellite_set_error(ctx, -1, "Failed to allocate subsystem contexts");
      satellite_cleanup(ctx);
      return -1;
   }

   memset(ctx->audio_capture, 0, sizeof(audio_capture_t));
   memset(ctx->audio_playback, 0, sizeof(audio_playback_t));

#ifdef ENABLE_DISPLAY
   ctx->display = malloc(sizeof(display_t));
   if (ctx->display) {
      memset(ctx->display, 0, sizeof(display_t));
   }
#endif

#ifdef HAVE_GPIOD
   ctx->gpio = malloc(sizeof(gpio_control_t));
   if (ctx->gpio) {
      memset(ctx->gpio, 0, sizeof(gpio_control_t));
   }
#endif

   ctx->state = STATE_IDLE;
   ctx->running = 1;

   LOG_INFO("Satellite context initialized");
   return 0;
}

void satellite_cleanup(satellite_ctx_t *ctx) {
   if (!ctx)
      return;

   ctx->running = 0;

   /* Clean up subsystems */
#ifndef ENABLE_DAP2
   if (ctx->dap_client) {
      dap_client_cleanup((dap_client_t *)ctx->dap_client);
      free(ctx->dap_client);
   }
#endif

   if (ctx->audio_capture) {
      audio_capture_cleanup((audio_capture_t *)ctx->audio_capture);
      free(ctx->audio_capture);
   }

   if (ctx->audio_playback) {
      audio_playback_cleanup((audio_playback_t *)ctx->audio_playback);
      free(ctx->audio_playback);
   }

#ifdef ENABLE_DISPLAY
   if (ctx->display) {
      display_cleanup((display_t *)ctx->display);
      free(ctx->display);
   }
#endif

#ifdef HAVE_GPIOD
   if (ctx->gpio) {
      gpio_cleanup((gpio_control_t *)ctx->gpio);
      free(ctx->gpio);
   }
#endif

   /* Free buffers */
   if (ctx->audio_buffer) {
      free(ctx->audio_buffer);
   }

   if (ctx->response_buffer) {
      free(ctx->response_buffer);
   }

   LOG_INFO("Satellite context cleaned up");
}

void satellite_set_server(satellite_ctx_t *ctx, const char *ip, uint16_t port) {
   if (!ctx)
      return;

   if (ip) {
      strncpy(ctx->server_ip, ip, sizeof(ctx->server_ip) - 1);
   }
   if (port > 0) {
      ctx->server_port = port;
   }

   LOG_INFO("Server set to %s:%u", ctx->server_ip, ctx->server_port);
}

void satellite_set_audio_devices(satellite_ctx_t *ctx,
                                 const char *capture_device,
                                 const char *playback_device) {
   if (!ctx)
      return;

   if (capture_device) {
      strncpy(ctx->capture_device, capture_device, sizeof(ctx->capture_device) - 1);
   }
   if (playback_device) {
      strncpy(ctx->playback_device, playback_device, sizeof(ctx->playback_device) - 1);
   }

   LOG_INFO("Audio devices: capture=%s, playback=%s", ctx->capture_device, ctx->playback_device);
}

satellite_state_t satellite_get_state(satellite_ctx_t *ctx) {
   return ctx ? ctx->state : STATE_ERROR;
}

/* State transition logic */
satellite_state_t satellite_process_event(satellite_ctx_t *ctx, satellite_event_t event) {
   if (!ctx)
      return STATE_ERROR;

   satellite_state_t old_state = ctx->state;
   satellite_state_t new_state = old_state;

   LOG_INFO("Event %s in state %s", satellite_event_name(event), satellite_state_name(old_state));

   switch (old_state) {
      case STATE_IDLE:
         if (event == EVENT_BUTTON_PRESS) {
            new_state = STATE_RECORDING;
         }
         break;

      case STATE_RECORDING:
         if (event == EVENT_BUTTON_RELEASE || event == EVENT_RECORD_COMPLETE) {
            new_state = STATE_CONNECTING;
         } else if (event == EVENT_ERROR) {
            new_state = STATE_ERROR;
         }
         break;

      case STATE_CONNECTING:
         if (event == EVENT_CONNECT_SUCCESS) {
            new_state = STATE_SENDING;
         } else if (event == EVENT_CONNECT_FAIL || event == EVENT_TIMEOUT) {
            new_state = STATE_ERROR;
         }
         break;

      case STATE_SENDING:
         if (event == EVENT_SEND_COMPLETE) {
            new_state = STATE_WAITING;
         } else if (event == EVENT_SEND_FAIL) {
            new_state = STATE_ERROR;
         }
         break;

      case STATE_WAITING:
         if (event == EVENT_RESPONSE_READY) {
            new_state = STATE_PLAYING;
         } else if (event == EVENT_RESPONSE_FAIL || event == EVENT_TIMEOUT) {
            new_state = STATE_ERROR;
         }
         break;

      case STATE_RECEIVING:
         /* DAP2 doesn't use RECEIVING state (text is immediate) */
         /* For DAP, handle like WAITING */
         if (event == EVENT_RESPONSE_READY) {
            new_state = STATE_PLAYING;
         } else if (event == EVENT_RESPONSE_FAIL || event == EVENT_TIMEOUT) {
            new_state = STATE_ERROR;
         }
         break;

      case STATE_PLAYING:
         if (event == EVENT_PLAYBACK_DONE) {
            new_state = STATE_IDLE;
         } else if (event == EVENT_BUTTON_PRESS) {
            /* Barge-in: stop playback and start recording */
            ctx->stop_playback = 1;
            new_state = STATE_RECORDING;
         }
         break;

      case STATE_ERROR:
         /* Any button press returns to idle */
         if (event == EVENT_BUTTON_PRESS) {
            new_state = STATE_IDLE;
         }
         /* Auto-return to idle after timeout */
         if (event == EVENT_TIMEOUT) {
            new_state = STATE_IDLE;
         }
         break;
   }

   if (new_state != old_state) {
      LOG_INFO("State transition: %s -> %s", satellite_state_name(old_state),
               satellite_state_name(new_state));
      ctx->prev_state = old_state;
      ctx->state = new_state;

      /* Update UI elements */
      satellite_update_display(ctx);
      satellite_update_leds(ctx);
   }

   return new_state;
}

void satellite_update_display(satellite_ctx_t *ctx) {
#ifdef ENABLE_DISPLAY
   if (!ctx || !ctx->display)
      return;

   display_t *disp = (display_t *)ctx->display;
   if (!disp->initialized)
      return;

   /* Clear screen */
   display_clear(disp, COLOR_BLACK);

   /* Draw header */
   display_text(disp, 4, 4, "DAWN Satellite", COLOR_CYAN, COLOR_BLACK, 2);

   /* Draw state */
   const char *state_str = satellite_state_name(ctx->state);
   uint16_t state_color = COLOR_WHITE;

   switch (ctx->state) {
      case STATE_IDLE:
         state_color = COLOR_GREEN;
         break;
      case STATE_RECORDING:
         state_color = COLOR_BLUE;
         break;
      case STATE_CONNECTING:
      case STATE_SENDING:
      case STATE_WAITING:
         state_color = COLOR_YELLOW;
         break;
      case STATE_PLAYING:
         state_color = COLOR_GREEN;
         break;
      case STATE_ERROR:
         state_color = COLOR_RED;
         break;
      default:
         state_color = COLOR_WHITE;
         break;
   }

   display_text(disp, 4, 40, state_str, state_color, COLOR_BLACK, 2);

   /* Draw error message if in error state */
   if (ctx->state == STATE_ERROR && ctx->error_msg[0]) {
      display_text(disp, 4, 70, ctx->error_msg, COLOR_RED, COLOR_BLACK, 1);
   }

   /* Draw server info */
   char info[64];
   snprintf(info, sizeof(info), "%s:%u", ctx->server_ip, ctx->server_port);
   display_text(disp, 4, 100, info, COLOR_WHITE, COLOR_BLACK, 1);
#else
   (void)ctx;
#endif
}

void satellite_update_leds(satellite_ctx_t *ctx) {
#ifdef HAVE_GPIOD
   if (!ctx || !ctx->gpio)
      return;

   gpio_control_t *gpio = (gpio_control_t *)ctx->gpio;
   if (!gpio->initialized)
      return;

   led_state_t led_state = LED_STATE_OFF;

   switch (ctx->state) {
      case STATE_IDLE:
         led_state = LED_STATE_IDLE;
         break;
      case STATE_RECORDING:
         led_state = LED_STATE_RECORDING;
         break;
      case STATE_CONNECTING:
      case STATE_SENDING:
      case STATE_WAITING:
      case STATE_RECEIVING:
         led_state = LED_STATE_PROCESSING;
         break;
      case STATE_PLAYING:
         led_state = LED_STATE_PLAYING;
         break;
      case STATE_ERROR:
         led_state = LED_STATE_ERROR;
         break;
   }

   gpio_led_set_state(gpio, led_state);
#else
   (void)ctx;
#endif
}
