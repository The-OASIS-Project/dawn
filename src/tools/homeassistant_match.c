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
 * Which Home Assistant entity a name means: fuzzy matching that never picks
 * among equals.
 */

#include "tools/homeassistant_match.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "core/str_fuzzy.h"

/* Lowest score a name match counts at (two shared words). */
#define HA_MATCH_MIN_SCORE 40
#define HA_MATCH_MAX_CANDIDATES 5

/* @p word as a whole word of @p text_lower ("door", not "indoor"). */
static bool has_word(const char *text_lower, const char *word) {
   const size_t n = strlen(word);
   for (const char *p = strstr(text_lower, word); p; p = strstr(p + 1, word)) {
      const bool start = p == text_lower || !isalnum((unsigned char)p[-1]);
      if (start && !isalnum((unsigned char)p[n]))
         return true;
   }
   return false;
}

/* A name that says it opens a door: garage, gate, door or unlock, as a word of
 * its name or entity_id. */
static bool names_a_door(const ha_entity_t *ent) {
   static const char *const words[] = { "garage", "gate", "door", "unlock" };
   char eid_lower[HA_MAX_ENTITY_ID];
   str_fuzzy_tolower(eid_lower, ent->entity_id, sizeof(eid_lower));
   for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
      if (has_word(ent->friendly_name_lower, words[i]) || has_word(eid_lower, words[i]))
         return true;
   }
   return false;
}

bool homeassistant_opens_door(const ha_entity_t *ent) {
   switch (ent->domain) {
      case HA_DOMAIN_LOCK:
         return true;
      case HA_DOMAIN_COVER: /* a garage door or gate, not blinds or an awning */
         return strcmp(ent->device_class, "garage") == 0 ||
                strcmp(ent->device_class, "gate") == 0 || strcmp(ent->device_class, "door") == 0 ||
                names_a_door(ent);
      case HA_DOMAIN_SWITCH:
      case HA_DOMAIN_SCENE:
      case HA_DOMAIN_SCRIPT:
      case HA_DOMAIN_AUTOMATION:
         return names_a_door(ent);
      default:
         return false;
   }
}

/* The entity's name (or entity_id) contains @p needle_lower whole. */
static bool name_contains(const ha_entity_t *ent, const char *needle_lower) {
   char eid_lower[HA_MAX_ENTITY_ID];
   str_fuzzy_tolower(eid_lower, ent->entity_id, sizeof(eid_lower));
   return strstr(ent->friendly_name_lower, needle_lower) || strstr(eid_lower, needle_lower);
}

/* A device made into a light, fan or cover ("change device type") keeps its
 * original switch, under the same name: that pair isn't two choices. */
static void drop_shadowed_switches(const ha_entity_list_t *list,
                                   int count,
                                   int *scores,
                                   int score,
                                   int *best,
                                   int *ties) {
   for (int i = 0; i<count && * ties> 1; i++) {
      if (scores[i] != score || list->entities[i].domain != HA_DOMAIN_SWITCH)
         continue;
      for (int j = 0; j < count; j++) {
         if (j != i && scores[j] == score && list->entities[j].domain != HA_DOMAIN_SWITCH &&
             strcmp(list->entities[i].friendly_name_lower, list->entities[j].friendly_name_lower) ==
                 0) {
            scores[i] = 0;
            (*ties)--;
            if (*best == i)
               *best = j;
            break;
         }
      }
   }
}

static void list_candidates(const ha_entity_list_t *list,
                            int count,
                            const int *scores,
                            int score,
                            char *out,
                            size_t out_len) {
   size_t len = 0;
   for (int i = 0, n = 0; i < count && n < HA_MATCH_MAX_CANDIDATES && len < out_len; i++) {
      if (scores[i] != score)
         continue;
      int w = snprintf(out + len, out_len - len, "%s%s (%s)", n ? "; " : "",
                       list->entities[i].friendly_name, list->entities[i].entity_id);
      if (w < 0 || (size_t)w >= out_len - len) {
         out[len] = '\0'; /* no half an entry */
         break;
      }
      len += (size_t)w;
      n++;
   }
}

ha_error_t homeassistant_match(const ha_entity_list_t *list,
                               const char *name,
                               ha_domain_t domain_hint,
                               int *index_out,
                               char *candidates,
                               size_t candidates_len) {
   if (candidates && candidates_len > 0)
      candidates[0] = '\0';
   if (!list || !name || !index_out)
      return HA_ERR_INVALID_PARAM;

   char needle_lower[256];
   str_fuzzy_tolower(needle_lower, name, sizeof(needle_lower));

   int scores[HA_MAX_ENTITIES];
   int best_score = 0, best = -1, ties = 0;
   const int count = list->count < HA_MAX_ENTITIES ? list->count : HA_MAX_ENTITIES;
   for (int i = 0; i < count; i++) {
      const ha_entity_t *ent = &list->entities[i];
      scores[i] = 0;
      if (domain_hint != HA_DOMAIN_UNKNOWN && ent->domain != domain_hint)
         continue;

      /* Score against friendly_name (pre-lowered) and entity_id */
      int score = str_fuzzy_score(ent->friendly_name_lower, needle_lower);
      char eid_lower[HA_MAX_ENTITY_ID];
      str_fuzzy_tolower(eid_lower, ent->entity_id, sizeof(eid_lower));
      int eid_score = str_fuzzy_score(eid_lower, needle_lower);
      if (eid_score > score)
         score = eid_score;

      scores[i] = score;
      if (score > best_score) {
         best_score = score;
         best = i;
         ties = 1;
      } else if (score == best_score && score > 0) {
         ties++;
      }
   }

   if (best < 0 || best_score < HA_MATCH_MIN_SCORE)
      return HA_ERR_ENTITY_NOT_FOUND;
   drop_shadowed_switches(list, count, scores, best_score, &best, &ties);
   if (ties > 1 || (homeassistant_opens_door(&list->entities[best]) &&
                    !name_contains(&list->entities[best], needle_lower))) {
      if (candidates)
         list_candidates(list, count, scores, best_score, candidates, candidates_len);
      return HA_ERR_AMBIGUOUS;
   }
   *index_out = best;
   return HA_OK;
}
