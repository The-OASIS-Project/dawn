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
 * Messaging engine — inbound messages waiting for a held conversation.
 *
 * One worker thread answers every channel.  A turn on a conversation another
 * session's turn holds (the user typing into the channel's conversation from
 * the WebUI) can't run yet; waiting for it on the worker would hold up every
 * other channel.  Such a message waits on this list instead, and the worker
 * looks again every MESSAGING_PARKED_RECHECK_MS.  Only the worker thread
 * touches the list.
 */
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include <sodium.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/conv_turn_lease.h"
#include "logging.h"
#include "messaging/messaging_engine_internal.h"

static inbound_item_t *s_parked_head;
static inbound_item_t *s_parked_tail;
static size_t s_parked_count;

void inbound_item_free(inbound_item_t *item) {
   if (!item) {
      return;
   }
   if (item->kind == INBOUND_CODE && item->body) {
      sodium_memzero(item->body, strlen(item->body)); /* a reply code */
   }
   free(item->body);
   free(item);
}

size_t inbound_parked_count(void) {
   return s_parked_count;
}

/* The last waiting item of @p channel_id, and how many of its items wait. */
static const inbound_item_t *channel_parked(int64_t channel_id, size_t *count) {
   const inbound_item_t *last = NULL;
   size_t n = 0;
   for (const inbound_item_t *it = s_parked_head; it; it = it->next) {
      if (it->ref.channel_id == channel_id) {
         last = it;
         n++;
      }
   }
   *count = n;
   return last;
}

static void park(inbound_item_t *item, size_t channel_count) {
   if (channel_count >= MESSAGING_PARKED_PER_CHANNEL_MAX ||
       s_parked_count >= MESSAGING_PARKED_MAX) {
      OLOG_WARNING("messaging: %zu messages of channel %lld (%zu in all) already wait for "
                   "conversation %lld; dropping a %s message",
                   channel_count, (long long)item->ref.channel_id, s_parked_count,
                   (long long)item->busy_conv, item->provider);
      inbound_item_free(item);
      return;
   }
   item->next = NULL;
   if (item->parked_at.tv_sec == 0) {
      clock_gettime(CLOCK_MONOTONIC, &item->parked_at);
   }
   if (s_parked_tail) {
      s_parked_tail->next = item;
   } else {
      s_parked_head = item;
   }
   s_parked_tail = item;
   s_parked_count++;
   uint32_t holder = 0;
   (void)conv_turn_lease_holder(item->busy_conv, &holder, NULL, NULL);
   OLOG_INFO("messaging: channel %lld's message waits for conversation %lld (held by session "
             "%u; %zu of the channel's waiting)",
             (long long)item->ref.channel_id, (long long)item->busy_conv, holder,
             channel_count + 1);
}

void inbound_parked_submit(inbound_item_t *item, inbound_run_fn run) {
   if (!item) {
      return;
   }
   size_t waiting = 0;
   const inbound_item_t *last = channel_parked(item->ref.channel_id, &waiting);
   if (last && item->kind != INBOUND_STOP) {
      item->busy_conv = last->busy_conv;
      park(item, waiting);
      return;
   }
   if (run(item) == INBOUND_PARKED) {
      park(item, waiting);
      return;
   }
   inbound_item_free(item);
}

static bool channel_listed(const int64_t *ids, size_t n, int64_t id) {
   for (size_t i = 0; i < n; i++) {
      if (ids[i] == id) {
         return true;
      }
   }
   return false;
}

void inbound_parked_retry(inbound_run_fn run) {
   /* Channels whose head still waits: the rest of their items stay behind it. */
   int64_t blocked[MESSAGING_PARKED_MAX];
   size_t nblocked = 0;
   inbound_item_t *prev = NULL;
   inbound_item_t *cur = s_parked_head;
   while (cur) {
      inbound_item_t *next = cur->next;
      const int64_t channel = cur->ref.channel_id;
      bool ran = false;
      if (!channel_listed(blocked, nblocked, channel) &&
          !conv_turn_lease_holder(cur->busy_conv, NULL, NULL, NULL)) {
         /* Still on the list while it runs: nothing else touches the list. */
         ran = run(cur) == INBOUND_RAN;
      }
      if (!ran) {
         if (!channel_listed(blocked, nblocked, channel) && nblocked < MESSAGING_PARKED_MAX) {
            blocked[nblocked++] = channel;
         }
         prev = cur;
         cur = next;
         continue;
      }
      if (prev) {
         prev->next = next;
      } else {
         s_parked_head = next;
      }
      if (s_parked_tail == cur) {
         s_parked_tail = prev;
      }
      s_parked_count--;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      const long waited_ms = (long)(now.tv_sec - cur->parked_at.tv_sec) * 1000L +
                             (now.tv_nsec - cur->parked_at.tv_nsec) / 1000000L;
      OLOG_INFO("messaging: channel %lld's message ran after waiting %ld ms for conversation %lld",
                (long long)channel, waited_ms, (long long)cur->busy_conv);
      inbound_item_free(cur);
      cur = next;
   }
}

void inbound_parked_drop_all(void) {
   const size_t n = s_parked_count;
   while (s_parked_head) {
      inbound_item_t *next = s_parked_head->next;
      inbound_item_free(s_parked_head);
      s_parked_head = next;
   }
   s_parked_tail = NULL;
   s_parked_count = 0;
   if (n > 0) {
      OLOG_WARNING("messaging: %zu inbound messages not answered at shutdown: their "
                   "conversations were held",
                   n);
   }
}
