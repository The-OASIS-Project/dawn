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
 * among equals (pure; the service calls it under its cache lock).
 */

#ifndef HOMEASSISTANT_MATCH_H
#define HOMEASSISTANT_MATCH_H

#include <stddef.h>

#include "tools/homeassistant_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Room for the list of the entities a name could mean. */
#define HA_MATCH_CANDIDATES_MAX 640

/**
 * @brief The entity in @p list that @p name means
 *
 * Scores each entity's friendly name and entity_id against @p name (filtered
 * to @p domain_hint unless HA_DOMAIN_UNKNOWN).  Two entities tied for the best
 * score, or a lock or cover matched by less than its whole name (or a name
 * containing @p name), are HA_ERR_AMBIGUOUS, with the candidates written as
 * "Name (entity_id); ..." into @p candidates.
 *
 * @param index_out The entity's index in @p list (HA_OK)
 * @return HA_OK, HA_ERR_ENTITY_NOT_FOUND or HA_ERR_AMBIGUOUS
 */
ha_error_t homeassistant_match(const ha_entity_list_t *list,
                               const char *name,
                               ha_domain_t domain_hint,
                               int *index_out,
                               char *candidates,
                               size_t candidates_len);

/**
 * @brief Whether @p ent opens a door: a lock; a cover that's a garage door,
 *        gate or door (its device class or name); a switch, scene, script or
 *        automation whose name says garage, gate, door or unlock.  Matched only
 *        by its name, and acting on it waits for the user's yes.
 */
bool homeassistant_opens_door(const ha_entity_t *ent);

#ifdef __cplusplus
}
#endif

#endif /* HOMEASSISTANT_MATCH_H */
