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
 * the project author(s).
 *
 * SMS messaging driver — outbound delegates to phone_service_send_sms;
 * inbound is driven by phone_service.c calling
 * messaging_engine_handle_sms_inbound on every received SMS, so this
 * driver has no listener thread.  See
 * docs/MESSAGING_CHANNELS_DESIGN.md §7.
 */
#include "messaging/messaging_sms.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_driver.h"
#include "messaging/messaging_engine.h"
#include "tools/phone_service.h"

/* Fallback user_id used only when the caller doesn't supply one
 * (defensive — current contract requires user_id > 0).  Matches the
 * existing phone_tool's default. */
#define SMS_FALLBACK_USER_ID 1

/* Strict E.164: leading '+', then 8-15 decimal digits.  Tighter than
 * what arbitrary modems sometimes report; relax only if real-world
 * traffic surfaces edge cases (short codes, unformatted senders).
 * Shared between validate_address and build_address_json so the public
 * driver surface emits well-formed JSON even if a future caller skipped
 * validate_address.  Matches the Slack driver's defense-in-depth
 * pattern (sk_build_address_json calls is_valid_slack_channel). */
static bool is_valid_e164(const char *s) {
   if (!s || s[0] != '+') {
      return false;
   }
   size_t digits = 0;
   for (size_t i = 1; s[i] != '\0'; i++) {
      if (!isdigit((unsigned char)s[i])) {
         return false;
      }
      digits++;
   }
   return (digits >= 8 && digits <= 15);
}

static int sms_extract_phone_e164(const char *address_json, char *out, size_t out_size) {
   if (!address_json || !out || out_size == 0) {
      return FAILURE;
   }
   struct json_object *obj = json_tokener_parse(address_json);
   if (!obj) {
      return FAILURE;
   }
   struct json_object *p = NULL;
   int rc = FAILURE;
   if (json_object_object_get_ex(obj, "phone_e164", &p) && p) {
      const char *s = json_object_get_string(p);
      if (s && s[0]) {
         snprintf(out, out_size, "%s", s);
         rc = SUCCESS;
      }
   }
   json_object_put(obj);
   return rc;
}

static int sms_init(const char *credentials_json) {
   (void)credentials_json; /* SMS has no per-driver credentials — uses ECHO modem */
   return SUCCESS;
}

static void sms_shutdown_impl(void) {
   /* No listener thread, no persistent connection — nothing to tear
    * down. */
}

/* Send @p text: an ordinary text when @p log_text is NULL, else a code text
 * logged as @p log_text. */
static int sms_send_text_logged_as(int user_id,
                                   const char *provider_address,
                                   const char *address_json,
                                   const char *text,
                                   const char *log_text) {
   char e164[32];
   e164[0] = '\0';
   /* Prefer typed provider_address (the E.164 string); fall back to
    * parsing address_json for callers that haven't migrated to the
    * new contract. */
   if (provider_address && provider_address[0] != '\0') {
      snprintf(e164, sizeof(e164), "%s", provider_address);
   } else if (address_json) {
      if (sms_extract_phone_e164(address_json, e164, sizeof(e164)) != SUCCESS) {
         OLOG_WARNING("sms: send_text — no phone_e164 in provider_address or address_json");
         return FAILURE;
      }
   } else {
      OLOG_WARNING("sms: send_text — no provider_address or address_json");
      return FAILURE;
   }

   /* Use the supplied user_id so phone_service_send_sms scopes its
    * audit log + per-user rate buckets correctly.  Falls back to the
    * single-admin default if the caller passed 0 (older code path or
    * a context where user_id wasn't available — should not happen on
    * the messaging-engine path, so log loudly if it does so a future
    * misconfiguration doesn't silently misroute audit entries). */
   int effective_user_id;
   if (user_id > 0) {
      effective_user_id = user_id;
   } else {
      OLOG_WARNING("sms: send_text with user_id=%d; falling back to user %d (caller bug)", user_id,
                   SMS_FALLBACK_USER_ID);
      effective_user_id = SMS_FALLBACK_USER_ID;
   }
   char result[256] = { 0 };
   int rc = log_text
                ? phone_service_send_code_sms(effective_user_id, e164, text, log_text, result,
                                              sizeof(result))
                : phone_service_send_sms(effective_user_id, e164, text, result, sizeof(result));
   if (rc != SUCCESS) {
      OLOG_WARNING("sms: phone_service_send_sms failed (rc=%d): %s", rc, result);
      return FAILURE;
   }
   return SUCCESS;
}

static int sms_send_text(int user_id,
                         const char *provider_address,
                         const char *address_json,
                         const char *text) {
   return sms_send_text_logged_as(user_id, provider_address, address_json, text, NULL);
}

/* Code texts (link and reply codes): their content isn't kept (the user's SMS
 * log, readable through the phone tool, gets `log_text`), and they aren't held
 * to the SMS rate limits (phone_service_send_code_sms). */
static int sms_send_text_unlogged(int user_id,
                                  const char *provider_address,
                                  const char *address_json,
                                  const char *text,
                                  const char *log_text) {
   return sms_send_text_logged_as(user_id, provider_address, address_json, text,
                                  log_text ? log_text : "(not kept)");
}

static void sms_build_address_json(const char *provider_address, char *buf, size_t buf_size) {
   if (!buf || buf_size == 0) {
      return;
   }
   /* Re-validate even though all current callers pre-validate.  Defense
    * in depth on the driver's public surface — a future caller bypassing
    * validate_address would otherwise emit malformed JSON via snprintf
    * below.  is_valid_e164 rejects '"' and '\\' (digits-only after '+')
    * so the snprintf can't be tricked. */
   if (!provider_address || !is_valid_e164(provider_address)) {
      snprintf(buf, buf_size, "{}");
      return;
   }
   snprintf(buf, buf_size, "{\"phone_e164\":\"%s\"}", provider_address);
}

static int sms_register_inbound_cb(messaging_inbound_fn cb) {
   /* The SMS inbound path is driven externally by phone_service.c
    * (which calls messaging_engine_handle_sms_inbound directly), so
    * this driver never invokes the registered callback.  Accepting
    * the registration keeps the contract uniform across drivers. */
   (void)cb;
   return SUCCESS;
}

static int sms_validate_address(const char *address_json) {
   char e164[32];
   if (sms_extract_phone_e164(address_json, e164, sizeof(e164)) != SUCCESS) {
      return FAILURE;
   }
   return is_valid_e164(e164) ? SUCCESS : FAILURE;
}

static int sms_is_connected(void) {
   /* The modem connection is owned by ECHO; from this driver's
    * perspective we always claim connected.  Real failures surface as
    * phone_service_send_sms returning non-zero. */
   return 1;
}

static int sms_reconnect(void) {
   return SUCCESS;
}

static const messaging_driver_t s_sms_driver = {
   .name = "sms",
   .authenticates_sender = false, /* a sender number can be forged */
   .out_format = MSG_FMT_PLAIN,
   .init = sms_init,
   .shutdown = sms_shutdown_impl,
   .send_text = sms_send_text,
   .send_text_unlogged = sms_send_text_unlogged,
   .build_address_json = sms_build_address_json,
   .register_inbound_cb = sms_register_inbound_cb,
   .validate_address = sms_validate_address,
   .is_connected = sms_is_connected,
   .reconnect = sms_reconnect,
};

int messaging_sms_register(void) {
   /* Init BEFORE engine registration to match the canonical order shared
    * by Telegram/Discord/Slack — init failure shouldn't leave a dead
    * driver in the engine's registry.  sms_init is a no-op today, but
    * the symmetric shape is a trap-avoidance pattern for whoever adds
    * real init work to the SMS driver later. */
   if (sms_init(NULL) != SUCCESS) {
      OLOG_ERROR("sms: driver init failed");
      return FAILURE;
   }
   if (messaging_engine_register_driver(&s_sms_driver) != MESSAGING_SUCCESS) {
      OLOG_ERROR("sms: registration with engine failed");
      sms_shutdown_impl();
      return FAILURE;
   }
   OLOG_INFO("sms: registered (outbound via phone_service, inbound via "
             "messaging_engine_handle_sms_inbound)");
   return SUCCESS;
}

void messaging_sms_shutdown(void) {
   sms_shutdown_impl();
}
