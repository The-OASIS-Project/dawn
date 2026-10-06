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
 * Which calls go to an Anthropic Messages endpoint, and what each one gets.
 * See llm_claude_route.h.
 */

#include "llm/llm_claude_route.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "llm/llm_model_family.h"

/* Whether @p base_url's host is exactly @p host. */
static bool host_is(const char *base_url, const char *host) {
   if (!base_url) {
      return false;
   }
   CURLU *url = curl_url();
   char *got = NULL;
   bool same = false;
   if (url && curl_url_set(url, CURLUPART_URL, base_url, 0) == CURLUE_OK &&
       curl_url_get(url, CURLUPART_HOST, &got, 0) == CURLUE_OK && got) {
      same = strcasecmp(got, host) == 0;
   }
   curl_free(got);
   curl_url_cleanup(url);
   return same;
}

llm_claude_route_t llm_claude_route(const char *base_url) {
   llm_claude_route_t route = { .provider = CLOUD_PROVIDER_CLAUDE };
   if (host_is(base_url, "api.anthropic.com")) {
      route.first_party = true;
   } else if (host_is(base_url, "openrouter.ai")) {
      route.provider = CLOUD_PROVIDER_OPENROUTER;
   }
   return route;
}

bool llm_uses_anthropic_messages(llm_type_t type,
                                 cloud_provider_t provider,
                                 const char *model,
                                 const char *base_url) {
   if (type != LLM_CLOUD) {
      return false;
   }
   if (provider == CLOUD_PROVIDER_CLAUDE) {
      return true;
   }
   if (provider != CLOUD_PROVIDER_OPENROUTER || !model || !host_is(base_url, "openrouter.ai")) {
      return false;
   }
   char id[LLM_MODEL_NAME_MAX];
   return llm_model_route(type, provider, model, id, sizeof(id)) == LLM_FAMILY_ANTHROPIC;
}
