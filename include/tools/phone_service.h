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
 *
 * Phone Service — call state machine, event handling, contact resolution,
 * TTS announcements, HUD MQTT dispatch.
 */

#ifndef PHONE_SERVICE_H
#define PHONE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tools/phone_audio_config.h" /* phone_audio_config_t for the audio-config setter */

/* Phone states.
 *
 * PHONE_STATE_ANSWERING is the interim "a surface has claimed the ring and is
 * answering it" state: phone_service_answer() flips RINGING_IN -> ANSWERING
 * atomically so a second surface can't race the same answer, and the ECHO
 * call_connected event then advances ANSWERING -> ACTIVE.
 *
 * NOTE: the ordered state-name table in phone_tool.c's status handler is
 * indexed by these numeric values, so keep the two in lockstep when editing. */
typedef enum {
   PHONE_STATE_IDLE = 0,
   PHONE_STATE_DIALING,
   PHONE_STATE_RINGING_IN,
   PHONE_STATE_ACTIVE,
   PHONE_STATE_ANSWERING,
} phone_state_t;

/* Default modem USB audio port for the local-handset PCM bridge. */
#define PHONE_PCM_PORT_DEFAULT "/dev/ttyUSB4"

/* Phone service configuration */
typedef struct {
   bool enabled;
   bool confirm_outbound;
   char audio_device[64]; /* legacy — unused (was the USB-sound-card device) */
   char pcm_port[64];     /* legacy — unused; pcm_port now lives in phone_audio_config_t */
   int user_id;           /* owner of the modem — contacts/logs scoped to this user */
   int sms_retention_days;
   int call_log_retention_days;
   int rate_limit_sms_per_min;
   int rate_limit_calls_per_min;
   int rate_limit_sms_per_day;
} phone_service_config_t;

/* Forward decl so this Layer-3 header doesn't pull in json-c. */
struct json_object;

/* Lifecycle of a browser incoming-call banner. */
typedef enum {
   PHONE_CALL_NOTIF_RINGING = 0, /* show the banner (Answer/Reject) */
   PHONE_CALL_NOTIF_ACTIVE,      /* call answered — clear the ring banner */
   PHONE_CALL_NOTIF_ENDED,       /* call ended/dropped — clear the banner */
} phone_call_notif_status_t;

/**
 * @brief Push an incoming-call notification to the owner's WebUI browser sessions.
 *
 * Weak-symbol bridge (Layer 3 -> Layer 4), mirroring the scheduler broadcast
 * pattern: the strong override lives in src/webui/webui_phone.c and links only
 * in ENABLE_WEBUI builds, while phone_service.c supplies a no-op stub for
 * WebUI-less builds so the phone service links standalone.  Called on the MQTT
 * event thread from phone_service_handle_event().
 *
 * @param user_id      Modem owner — the notification is filtered to this user's sessions.
 * @param status       ringing / active / ended (drives the banner lifecycle).
 * @param number       Caller number (may be empty).
 * @param contact_name Reverse-looked-up name (may be empty).
 * @param call_id      Call-log id for client-side correlation.
 * @param elapsed_sec  Seconds the call has been active (0 at connect / for non-active
 *                     statuses); lets the browser show a reconnect-accurate timer.
 * @param photo        Borrowed contact-photo json ({data,mime}) or NULL; read
 *                     synchronously, never retained.
 */
void webui_broadcast_phone_call(int user_id,
                                phone_call_notif_status_t status,
                                const char *number,
                                const char *contact_name,
                                int64_t call_id,
                                int elapsed_sec,
                                struct json_object *photo);

/**
 * @brief Snapshot the current live call (for WebUI reconnect rehydration).
 *
 * Reports a call that is ringing (RINGING_IN / ANSWERING → status RINGING) or
 * connected (ACTIVE → status ACTIVE); returns false when idle/dialing.
 *
 * @param status_out    Filled with RINGING or ACTIVE (optional, may be NULL).
 * @param number_out    Filled with the call number (may be empty).
 * @param number_size   Size of number_out.
 * @param name_out      Filled with the contact name (may be empty).
 * @param name_size     Size of name_out.
 * @param call_id_out   Filled with the call-log id (optional, may be NULL).
 * @param elapsed_sec_out Seconds since connect for ACTIVE, else 0 (optional).
 * @return true if a call is currently ringing or active, false otherwise.
 */
bool phone_service_get_call_snapshot(phone_call_notif_status_t *status_out,
                                     char *number_out,
                                     size_t number_size,
                                     char *name_out,
                                     size_t name_size,
                                     int64_t *call_id_out,
                                     int *elapsed_sec_out);

/**
 * @brief Initialize the phone service.
 *
 * Subscribes to echo/events and echo/response via DAWN's MQTT connection.
 *
 * @return 0 on success, 1 on failure.
 */
int phone_service_init(void);

/**
 * @brief Configure call-audio processing (from the [phone] config section).
 *
 * Stores the modem PCM port, near-end uplink APM knobs, and downlink gain used
 * when the audio bridge is started on `pcm_ready`.  Resolved TOML -> struct in
 * phone_tool and forwarded here; if never called, the bridge uses built-in
 * defaults.  Call at startup, before any call.
 *
 * @param audio Full call-audio config (port + uplink + downlink + gains). Copied.
 */
void phone_service_set_audio_config(const phone_audio_config_t *audio);

/**
 * @brief Shutdown the phone service.
 */
void phone_service_shutdown(void);

/**
 * @brief Check if the phone service is available (ECHO daemon online).
 */
bool phone_service_available(void);

/**
 * @brief Initiate an outbound call.
 *
 * Resolves contact name to phone number, publishes dial to echo/cmd.
 *
 * @param user_id        User ID for contact lookup.
 * @param name_or_number Contact name or phone number.
 * @param result_buf     Output: human-readable result for the LLM.
 * @param buf_size       Size of result buffer.
 * @return 0 on success, 1 on error.
 */
int phone_service_call(int user_id, const char *name_or_number, char *result_buf, size_t buf_size);

/**
 * @brief Answer an incoming call.
 */
int phone_service_answer(int user_id, char *result_buf, size_t buf_size);

/**
 * @brief Hang up the current call.
 */
int phone_service_hangup(int user_id, char *result_buf, size_t buf_size);

/**
 * @brief Send an SMS.
 *
 * Resolves contact name, publishes send_sms to echo/cmd.
 */
int phone_service_send_sms(int user_id,
                           const char *name_or_number,
                           const char *body,
                           char *result_buf,
                           size_t buf_size);

/**
 * @brief Send a code text (a link or reply code), recording `log_body` in the
 *        user's SMS log instead of `body` (the log is readable through the
 *        phone tool)
 *
 * Not held to the per-minute and per-day SMS limits: its senders cap codes
 * themselves, and a dropped code strands what waits for it.
 */
int phone_service_send_code_sms(int user_id,
                                const char *name_or_number,
                                const char *body,
                                const char *log_body,
                                char *result_buf,
                                size_t buf_size);

/**
 * @brief Get the current phone state.
 */
phone_state_t phone_service_get_state(void);

/**
 * @brief Handle an event from echo/events (called from MQTT on_message).
 *
 * Parses the event JSON and updates call state, inserts DB records,
 * sends TTS announcements and HUD MQTT notifications.
 *
 * @param payload     JSON payload from echo/events.
 * @param payload_len Length of payload.
 */
void phone_service_handle_event(const char *payload, int payload_len);

/**
 * @brief Get the phone service configuration (for tool layer).
 */
const phone_service_config_t *phone_service_get_config(void);

#endif /* PHONE_SERVICE_H */
