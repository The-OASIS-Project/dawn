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
 * component_status: HUD presence and re-subscribing on every MQTT connect.  libmosquitto and the
 * HUD side effects are stubs here.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/component_status.h"
#include "unity.h"

/* ── stubs ──────────────────────────────────────────────────────────────── */

static int s_subscribed_hud;
static int s_published_online;
static int s_tools_refreshed;
static int s_discovery_requested;

int mosquitto_subscribe(struct mosquitto *mosq, int *mid, const char *sub, int qos) {
   (void)mosq;
   (void)mid;
   (void)qos;
   if (strcmp(sub, STATUS_TOPIC_HUD) == 0) {
      s_subscribed_hud++;
   }
   return MOSQ_ERR_SUCCESS;
}

int mosquitto_publish(struct mosquitto *mosq,
                      int *mid,
                      const char *topic,
                      int payloadlen,
                      const void *payload,
                      int qos,
                      bool retain) {
   (void)mosq;
   (void)mid;
   (void)qos;
   (void)retain;
   (void)payloadlen; /* the payload is a C string here */
   if (strcmp(topic, STATUS_TOPIC_DAWN) == 0 && strstr((const char *)payload, "\"online\"")) {
      s_published_online++;
   }
   return MOSQ_ERR_SUCCESS;
}

int mosquitto_will_set(struct mosquitto *mosq,
                       const char *topic,
                       int payloadlen,
                       const void *payload,
                       int qos,
                       bool retain) {
   (void)mosq;
   (void)topic;
   (void)payloadlen;
   (void)payload;
   (void)qos;
   (void)retain;
   return MOSQ_ERR_SUCCESS;
}

const char *mosquitto_strerror(int mosq_errno) {
   (void)mosq_errno;
   return "stub";
}

int64_t ocp_get_timestamp_ms(void) {
   return 1;
}

void llm_tools_refresh(void) {
   s_tools_refreshed++;
}

void llm_tools_invalidate_cache(void) {
}

void invalidate_system_instructions(void) {
}

void hud_discovery_request_update(struct mosquitto *mosq) {
   (void)mosq;
   s_discovery_requested++;
}

/* ── helpers ────────────────────────────────────────────────────────────── */

static struct mosquitto *const s_mosq = (struct mosquitto *)0x1;

static void deliver(const char *topic, const char *status) {
   char payload[128];
   snprintf(payload, sizeof(payload),
            "{\"device\":\"x\",\"msg_type\":\"status\",\"status\":\"%s\",\"timestamp\":5}", status);
   component_status_handle_message(topic, payload, (int)strlen(payload));
}

void setUp(void) {
   s_subscribed_hud = 0;
   s_published_online = 0;
   s_tools_refreshed = 0;
   s_discovery_requested = 0;
}

void tearDown(void) {
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_every_connect_subscribes_and_says_online(void) {
   /* main() made the first connect; a reconnect must do it all again (the
    * client's session is clean, so the broker forgot the subscriptions). */
   TEST_ASSERT_EQUAL_INT(0, component_status_init(s_mosq));
   TEST_ASSERT_EQUAL_INT(1, s_subscribed_hud);
   TEST_ASSERT_EQUAL_INT(1, s_published_online);
}

static void test_hud_change_refreshes_tools_once(void) {
   deliver(STATUS_TOPIC_HUD, "online");
   TEST_ASSERT_TRUE(component_status_is_hud_online());
   TEST_ASSERT_EQUAL_INT(1, s_tools_refreshed);
   TEST_ASSERT_EQUAL_INT(1, s_discovery_requested);

   deliver(STATUS_TOPIC_HUD, "online"); /* a heartbeat: no change */
   TEST_ASSERT_EQUAL_INT(1, s_tools_refreshed);

   deliver(STATUS_TOPIC_HUD, "offline");
   TEST_ASSERT_FALSE(component_status_is_hud_online());
   TEST_ASSERT_EQUAL_INT(2, s_tools_refreshed);
   TEST_ASSERT_EQUAL_INT(1, s_discovery_requested);
}

static void test_bad_messages_ignored(void) {
   component_status_handle_message("other/status", "{\"status\":\"online\"}", 19);
   component_status_handle_message(STATUS_TOPIC_HUD, "not json", 8);
   component_status_handle_message(STATUS_TOPIC_HUD,
                                   "{\"msg_type\":\"event\",\"status\":\"online\"}", 38);
   TEST_ASSERT_FALSE(component_status_is_hud_online());
}

int main(void) {
   UNITY_BEGIN();
   /* The first connect starts the heartbeat thread; the tests are reconnects. */
   component_status_init(s_mosq);
   RUN_TEST(test_every_connect_subscribes_and_says_online);
   RUN_TEST(test_hud_change_refreshes_tools_once);
   RUN_TEST(test_bad_messages_ignored);
   component_status_shutdown();
   return UNITY_END();
}
