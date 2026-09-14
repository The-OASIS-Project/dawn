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
 * Generic rate limiter with multi-IP tracking and LRU eviction.
 */

#include "core/rate_limiter.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "utils/string_utils.h"

void rate_limiter_init(rate_limiter_t *limiter,
                       rate_limit_entry_t *entries,
                       const rate_limiter_config_t *config) {
   if (!limiter || !entries || !config) {
      return;
   }

   limiter->entries = entries;
   limiter->config = *config;
   pthread_mutex_init(&limiter->mutex, NULL);
}

bool rate_limiter_check(rate_limiter_t *limiter, const char *ip) {
   if (!limiter || !ip || !limiter->entries) {
      return false;
   }

   /* Fail-closed on truncation: if the input would not fit, the internal
    * strncpy would store a prefix that collides with any other IP sharing
    * that prefix, breaking the per-slot strcmp identity check and letting
    * a colliding caller evict a legitimate burned-budget state.  Treat
    * over-long inputs as rate-limited.  strnlen with a small bound avoids
    * scanning an unterminated buffer. */
   if (strnlen(ip, RATE_LIMIT_IP_SIZE) >= RATE_LIMIT_IP_SIZE) {
      return true;
   }

   pthread_mutex_lock(&limiter->mutex);

   time_t now = time(NULL);
   rate_limit_entry_t *entry = NULL;
   rate_limit_entry_t *lru_entry = NULL;
   time_t oldest_access = now + 1;

   /* Search for existing entry or find LRU slot */
   for (int i = 0; i < limiter->config.slot_count; i++) {
      rate_limit_entry_t *e = &limiter->entries[i];

      /* Empty slot */
      if (e->ip[0] == '\0') {
         if (!lru_entry) {
            lru_entry = e;
         }
         continue;
      }

      /* Expired entry - treat as empty */
      if ((now - e->window_start) >= limiter->config.window_sec) {
         e->ip[0] = '\0';
         if (!lru_entry || e->last_access < oldest_access) {
            lru_entry = e;
            oldest_access = e->last_access;
         }
         continue;
      }

      /* Found matching IP */
      if (strcmp(e->ip, ip) == 0) {
         entry = e;
         break;
      }

      /* Track LRU for potential eviction */
      if (e->last_access < oldest_access) {
         oldest_access = e->last_access;
         lru_entry = e;
      }
   }

   /* No existing entry - use empty or LRU slot */
   if (!entry) {
      entry = lru_entry;
      if (!entry) {
         /* All slots full and active - use random eviction to prevent targeting */
         entry = &limiter->entries[rand() % limiter->config.slot_count];
      }
      safe_strscpy(entry->ip, ip);
      entry->count = 1;
      entry->window_start = now;
      entry->last_access = now;
      pthread_mutex_unlock(&limiter->mutex);
      return false;
   }

   /* Update last access time */
   entry->last_access = now;

   /* Check limit */
   if (entry->count >= limiter->config.max_count) {
      pthread_mutex_unlock(&limiter->mutex);
      return true; /* Rate limited */
   }

   entry->count++;
   pthread_mutex_unlock(&limiter->mutex);
   return false;
}

void rate_limiter_reset(rate_limiter_t *limiter, const char *ip) {
   if (!limiter || !ip || !limiter->entries) {
      return;
   }

   pthread_mutex_lock(&limiter->mutex);

   for (int i = 0; i < limiter->config.slot_count; i++) {
      rate_limit_entry_t *e = &limiter->entries[i];
      if (e->ip[0] != '\0' && strcmp(e->ip, ip) == 0) {
         e->ip[0] = '\0'; /* Clear entry on reset */
         break;
      }
   }

   pthread_mutex_unlock(&limiter->mutex);
}

void rate_limiter_clear_all(rate_limiter_t *limiter) {
   if (!limiter || !limiter->entries) {
      return;
   }

   pthread_mutex_lock(&limiter->mutex);

   for (int i = 0; i < limiter->config.slot_count; i++) {
      limiter->entries[i].ip[0] = '\0';
   }

   pthread_mutex_unlock(&limiter->mutex);
}

void rate_limiter_normalize_ip(const char *ip, char *out, size_t out_len) {
   if (!ip || !out || out_len == 0) {
      if (out && out_len > 0) {
         out[0] = '\0';
      }
      return;
   }

   struct in6_addr addr6;
   if (inet_pton(AF_INET6, ip, &addr6) == 1) {
      /* Zero the interface identifier (lower 64 bits) for /64 prefix */
      memset(&addr6.s6_addr[8], 0, 8);
      inet_ntop(AF_INET6, &addr6, out, out_len);
   } else {
      /* IPv4 or other - use as-is */
      safe_strncpy(out, ip, out_len);
   }
}
