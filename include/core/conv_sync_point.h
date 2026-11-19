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
 * A conversation's sync point: the stored state a reload of it is built
 * from.  A session records the point its live history was built at; when a
 * turn begins it reads the point again, and what changed says whether the
 * history is current, only lacks rows written since (appended), or must be
 * built again (anything else changes what a reload builds).
 */

#ifndef CORE_CONV_SYNC_POINT_H
#define CORE_CONV_SYNC_POINT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/hash_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a reload of a conversation is built from. */
typedef struct {
   int64_t max_id;        /**< its newest row's id (0 = no rows) */
   int64_t watermark;     /**< context_watermark_msg_id: rows up to it are summarized */
   int64_t floor;         /**< reasoning_floor_msg_id: rows up to it replay no blocks */
   int64_t floor_pending; /**< reasoning_floor_pending: no blocks replay while set */
   char prefix_hash[DAWN_SHA256_HEX_LEN];   /**< the frozen system prompt ("" = none) */
   char tools_hash[DAWN_SHA256_HEX_LEN];    /**< the frozen tool set ("" = none) */
   char in_force_hash[DAWN_SHA256_HEX_LEN]; /**< what is in force ("" = none) */
} conv_sync_point_t;

/** How a history built at one point stands at a later one. */
typedef enum {
   CONV_SYNC_SAME,   /**< nothing a reload reads changed */
   CONV_SYNC_TAIL,   /**< only rows were added (and what is in force moved) */
   CONV_SYNC_REBUILD /**< anything else: the history must be built again */
} conv_sync_change_t;

/** @brief Whether two points are the same stored state */
static inline bool conv_sync_point_equal(const conv_sync_point_t *a, const conv_sync_point_t *b) {
   return a->max_id == b->max_id && a->watermark == b->watermark && a->floor == b->floor &&
          a->floor_pending == b->floor_pending && strcmp(a->prefix_hash, b->prefix_hash) == 0 &&
          strcmp(a->tools_hash, b->tools_hash) == 0 &&
          strcmp(a->in_force_hash, b->in_force_hash) == 0;
}

/**
 * @brief What changed from @p was to @p now
 *
 * Rows only ever join a conversation at its end (ids grow), so rows added
 * since are a tail; a compaction, a reasoning floor, a new frozen prompt or
 * tool set, or a newest row taken back changes rows a reload already has.
 */
static inline conv_sync_change_t conv_sync_point_compare(const conv_sync_point_t *was,
                                                         const conv_sync_point_t *now) {
   if (was->watermark != now->watermark || was->floor != now->floor ||
       was->floor_pending != now->floor_pending ||
       strcmp(was->prefix_hash, now->prefix_hash) != 0 ||
       strcmp(was->tools_hash, now->tools_hash) != 0 || now->max_id < was->max_id) {
      return CONV_SYNC_REBUILD;
   }
   if (now->max_id == was->max_id && strcmp(was->in_force_hash, now->in_force_hash) == 0) {
      return CONV_SYNC_SAME;
   }
   return CONV_SYNC_TAIL;
}

#ifdef __cplusplus
}
#endif

#endif /* CORE_CONV_SYNC_POINT_H */
