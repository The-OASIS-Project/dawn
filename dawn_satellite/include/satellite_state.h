/*
 * DAWN Satellite - State Machine
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

#ifndef SATELLITE_STATE_H
#define SATELLITE_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Satellite states */
typedef enum {
   STATE_IDLE,       /* Waiting for button press */
   STATE_RECORDING,  /* Recording audio */
   STATE_CONNECTING, /* Connecting to DAWN server */
   STATE_SENDING,    /* Sending audio to server */
   STATE_WAITING,    /* Waiting for AI response */
   STATE_RECEIVING,  /* Receiving response from server */
   STATE_PLAYING,    /* Playing response audio */
   STATE_ERROR       /* Error state */
} satellite_state_t;

/* Events that trigger state transitions */
typedef enum {
   EVENT_BUTTON_PRESS,    /* Button pressed */
   EVENT_BUTTON_RELEASE,  /* Button released */
   EVENT_RECORD_COMPLETE, /* Recording finished */
   EVENT_CONNECT_SUCCESS, /* Connected to server */
   EVENT_CONNECT_FAIL,    /* Connection failed */
   EVENT_SEND_COMPLETE,   /* Audio sent successfully */
   EVENT_SEND_FAIL,       /* Send failed */
   EVENT_RESPONSE_READY,  /* Response received */
   EVENT_RESPONSE_FAIL,   /* Response failed */
   EVENT_PLAYBACK_DONE,   /* Playback finished */
   EVENT_ERROR,           /* Error occurred */
   EVENT_TIMEOUT          /* Operation timeout */
} satellite_event_t;

/* Forward declarations */
struct satellite_ctx;

/**
 * State handler function type
 *
 * @param ctx Satellite context
 * @param event Event to process
 * @return Next state
 */
typedef satellite_state_t (*state_handler_t)(struct satellite_ctx *ctx, satellite_event_t event);

/**
 * Satellite context - holds all subsystem contexts
 */
typedef struct satellite_ctx {
   satellite_state_t state;      /* Current state */
   satellite_state_t prev_state; /* Previous state */

   /* Audio buffers */
   int16_t *audio_buffer;    /* Recording buffer */
   size_t audio_buffer_size; /* Buffer size in samples */
   size_t recorded_samples;  /* Number of recorded samples */

   /* Response buffers */
   uint8_t *response_buffer; /* Server response (WAV) */
   size_t response_size;     /* Response size in bytes */

   /* Configuration */
   char server_ip[64];       /* DAWN server IP */
   uint16_t server_port;     /* DAWN server port */
   char capture_device[64];  /* ALSA capture device */
   char playback_device[64]; /* ALSA playback device */

   /* Subsystem contexts (opaque pointers) */
   void *dap_client;     /* DAP client context */
   void *audio_capture;  /* Audio capture context */
   void *audio_playback; /* Audio playback context */
   void *display;        /* Display context */
   void *gpio;           /* GPIO context */

   /* Control flags */
   volatile int stop_recording; /* Stop recording flag */
   volatile int stop_playback;  /* Stop playback flag */
   volatile int running;        /* Main loop running */

   /* Error handling */
   char error_msg[256]; /* Last error message */
   int error_code;      /* Last error code */
} satellite_ctx_t;

/**
 * Initialize satellite context with default configuration
 *
 * @param ctx Pointer to satellite context
 * @return 0 on success, -1 on error
 */
int satellite_init(satellite_ctx_t *ctx);

/**
 * Clean up satellite context
 *
 * @param ctx Pointer to satellite context
 */
void satellite_cleanup(satellite_ctx_t *ctx);

/**
 * Set server configuration
 *
 * @param ctx Pointer to satellite context
 * @param ip Server IP address
 * @param port Server port (0 for default)
 */
void satellite_set_server(satellite_ctx_t *ctx, const char *ip, uint16_t port);

/**
 * Set audio device configuration
 *
 * @param ctx Pointer to satellite context
 * @param capture_device ALSA capture device (NULL for default)
 * @param playback_device ALSA playback device (NULL for default)
 */
void satellite_set_audio_devices(satellite_ctx_t *ctx,
                                 const char *capture_device,
                                 const char *playback_device);

/**
 * Process an event and transition state
 *
 * @param ctx Pointer to satellite context
 * @param event Event to process
 * @return New state after transition
 */
satellite_state_t satellite_process_event(satellite_ctx_t *ctx, satellite_event_t event);

/**
 * Get current state
 *
 * @param ctx Pointer to satellite context
 * @return Current state
 */
satellite_state_t satellite_get_state(satellite_ctx_t *ctx);

/**
 * Get state name string
 *
 * @param state State value
 * @return State name string
 */
const char *satellite_state_name(satellite_state_t state);

/**
 * Get event name string
 *
 * @param event Event value
 * @return Event name string
 */
const char *satellite_event_name(satellite_event_t event);

/**
 * Set error message
 *
 * @param ctx Pointer to satellite context
 * @param code Error code
 * @param fmt Format string
 * @param ... Format arguments
 */
void satellite_set_error(satellite_ctx_t *ctx, int code, const char *fmt, ...);

/**
 * Update display with current state
 *
 * @param ctx Pointer to satellite context
 */
void satellite_update_display(satellite_ctx_t *ctx);

/**
 * Update LED state based on current state
 *
 * @param ctx Pointer to satellite context
 */
void satellite_update_leds(satellite_ctx_t *ctx);

#endif /* SATELLITE_STATE_H */
