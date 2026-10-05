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
 * Unit tests for Home Assistant name matching: never pick among equals, and
 * a lock or cover only by a strong match.
 */

#include <stdio.h>
#include <string.h>

#include "core/str_fuzzy.h"
#include "tools/homeassistant_match.h"
#include "unity.h"

static ha_entity_list_t s_list; /* large: static, not on the stack */

static void add(const char *entity_id, const char *name, ha_domain_t domain) {
   ha_entity_t *e = &s_list.entities[s_list.count++];
   memset(e, 0, sizeof(*e));
   snprintf(e->entity_id, sizeof(e->entity_id), "%s", entity_id);
   snprintf(e->friendly_name, sizeof(e->friendly_name), "%s", name);
   str_fuzzy_tolower(e->friendly_name_lower, name, sizeof(e->friendly_name_lower));
   e->domain = domain;
}

void setUp(void) {
   s_list.count = 0;
   add("light.kitchen_light", "Kitchen Light", HA_DOMAIN_LIGHT);
   add("light.kitchen_light_2", "Kitchen Light 2", HA_DOMAIN_LIGHT);
   add("light.desk_lamp", "Desk Lamp", HA_DOMAIN_LIGHT);
   add("light.floor_lamp", "Floor Lamp", HA_DOMAIN_LIGHT);
   add("light.office", "Office Light", HA_DOMAIN_LIGHT);
   add("lock.front_door", "Front Door", HA_DOMAIN_LOCK);
   add("cover.garage_main", "Garage Main Door", HA_DOMAIN_COVER);
   add("switch.gate_relay", "Side Gate", HA_DOMAIN_SWITCH);
   add("switch.porch_plug", "Porch Lamp", HA_DOMAIN_SWITCH); /* made into the light below */
   add("light.porch_plug", "Porch Lamp", HA_DOMAIN_LIGHT);
   add("cover.den_blinds", "Den Blinds", HA_DOMAIN_COVER);
   add("scene.indoor_lights", "Indoor Lights", HA_DOMAIN_SCENE);
   add("script.open_garage", "Open Garage", HA_DOMAIN_SCRIPT);
}

void tearDown(void) {
}

static ha_error_t match(const char *name, ha_domain_t domain, int *idx, char *cands) {
   return homeassistant_match(&s_list, name, domain, idx, cands, 640);
}

/* The whole name wins even when another name contains it. */
static void test_exact_wins(void) {
   int idx = -1;
   char cands[640];
   TEST_ASSERT_EQUAL_INT(HA_OK, match("kitchen light", HA_DOMAIN_UNKNOWN, &idx, cands));
   TEST_ASSERT_EQUAL_STRING("light.kitchen_light", s_list.entities[idx].entity_id);
   TEST_ASSERT_EQUAL_INT(HA_OK, match("Front Door", HA_DOMAIN_LOCK, &idx, cands));
   TEST_ASSERT_EQUAL_STRING("lock.front_door", s_list.entities[idx].entity_id);
}

/* Two entities tied for the best match: which one, never the first. */
static void test_tie_asks(void) {
   int idx = -1;
   char cands[640];
   TEST_ASSERT_EQUAL_INT(HA_ERR_AMBIGUOUS, match("lamp", HA_DOMAIN_UNKNOWN, &idx, cands));
   TEST_ASSERT_NOT_NULL(strstr(cands, "Desk Lamp (light.desk_lamp)"));
   TEST_ASSERT_NOT_NULL(strstr(cands, "Floor Lamp (light.floor_lamp)"));
   TEST_ASSERT_NULL(strstr(cands, "Office"));
}

/* A light may match on shared words; a lock or cover may not. */
static void test_strong_match_for_doors(void) {
   int idx = -1;
   char cands[640];
   TEST_ASSERT_EQUAL_INT(HA_OK, match("office ceiling light", HA_DOMAIN_LIGHT, &idx, cands));
   TEST_ASSERT_EQUAL_STRING("light.office", s_list.entities[idx].entity_id);
   TEST_ASSERT_EQUAL_INT(HA_ERR_AMBIGUOUS, match("garage side door", HA_DOMAIN_COVER, &idx, cands));
   TEST_ASSERT_NOT_NULL(strstr(cands, "Garage Main Door (cover.garage_main)"));
   /* Through on/off/toggle (any domain) too. */
   TEST_ASSERT_EQUAL_INT(HA_ERR_AMBIGUOUS,
                         match("garage side door", HA_DOMAIN_UNKNOWN, &idx, cands));
   TEST_ASSERT_EQUAL_INT(HA_OK, match("garage", HA_DOMAIN_COVER, &idx, cands));
   TEST_ASSERT_EQUAL_STRING("cover.garage_main", s_list.entities[idx].entity_id);
}

/* Shared words can't reach a door, however many; a gate relay is a door too. */
static void test_doors_need_the_name(void) {
   int idx = -1;
   char cands[640];
   TEST_ASSERT_EQUAL_INT(HA_ERR_AMBIGUOUS,
                         match("front door lock main", HA_DOMAIN_LOCK, &idx, cands));
   TEST_ASSERT_EQUAL_INT(HA_ERR_AMBIGUOUS, match("side yard gate", HA_DOMAIN_UNKNOWN, &idx, cands));
   TEST_ASSERT_NOT_NULL(strstr(cands, "switch.gate_relay"));
   TEST_ASSERT_EQUAL_INT(HA_OK, match("side gate", HA_DOMAIN_UNKNOWN, &idx, cands));
}

/* A switch made into a light keeps its switch under the same name: one choice. */
static void test_shadowed_switch(void) {
   int idx = -1;
   char cands[640];
   TEST_ASSERT_EQUAL_INT(HA_OK, match("porch lamp", HA_DOMAIN_UNKNOWN, &idx, cands));
   TEST_ASSERT_EQUAL_STRING("light.porch_plug", s_list.entities[idx].entity_id);
}

/* What opens a door waits for a yes; blinds and "indoor" don't. */
static void test_opens_door(void) {
   int idx = -1;
   char cands[640];
   TEST_ASSERT_EQUAL_INT(HA_OK, match("front door", HA_DOMAIN_LOCK, &idx, cands));
   TEST_ASSERT_TRUE(homeassistant_opens_door(&s_list.entities[idx]));
   TEST_ASSERT_EQUAL_INT(HA_OK, match("den blinds", HA_DOMAIN_COVER, &idx, cands));
   TEST_ASSERT_FALSE(homeassistant_opens_door(&s_list.entities[idx]));
   TEST_ASSERT_EQUAL_INT(HA_OK, match("indoor lights", HA_DOMAIN_SCENE, &idx, cands));
   TEST_ASSERT_FALSE(homeassistant_opens_door(&s_list.entities[idx]));
   TEST_ASSERT_EQUAL_INT(HA_OK, match("open garage", HA_DOMAIN_SCRIPT, &idx, cands));
   TEST_ASSERT_TRUE(homeassistant_opens_door(&s_list.entities[idx]));
}

static void test_not_found(void) {
   int idx = -1;
   char cands[640];
   TEST_ASSERT_EQUAL_INT(HA_ERR_ENTITY_NOT_FOUND,
                         match("bathroom", HA_DOMAIN_UNKNOWN, &idx, cands));
   TEST_ASSERT_EQUAL_INT(HA_ERR_ENTITY_NOT_FOUND,
                         match("kitchen light", HA_DOMAIN_LOCK, &idx, cands));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_exact_wins);
   RUN_TEST(test_tie_asks);
   RUN_TEST(test_strong_match_for_doors);
   RUN_TEST(test_doors_need_the_name);
   RUN_TEST(test_shadowed_switch);
   RUN_TEST(test_opens_door);
   RUN_TEST(test_not_found);
   return UNITY_END();
}
