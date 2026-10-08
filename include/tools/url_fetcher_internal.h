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
 * URL Fetcher internal helpers — shared between url_fetcher.c and the
 * pluggable fallback dispatcher in url_fetcher_fallback.c. Same shape as
 * llm_openai_internal.h: isolates pluggable-provider routing into a single
 * TOCTOU-safe entry point and preserves a single source of truth for the
 * FlareSolverr fetch path used by both the legacy fallback and the new
 * dispatcher.
 *
 * NOT a public API. Tools outside src/tools/ MUST use tools/url_fetcher.h.
 */

#ifndef URL_FETCHER_INTERNAL_H
#define URL_FETCHER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Run the full FlareSolverr fallback path: fetch via FlareSolverr,
 *        extract markdown, run content-quality / Chromium-error-page detection.
 *
 * Returns URL_FETCH_SUCCESS with allocated markdown in *out_content, or an
 * URL_FETCH_* error code. Caller frees on success.
 */
int flaresolverr_fallback_fetch(const char *url,
                                const char *base_url,
                                char **out_content,
                                size_t *out_size);

/**
 * @brief Cached check: is FlareSolverr enabled in config AND reachable?
 *        Used by the fallback dispatcher to decide whether to cascade to
 *        FlareSolverr when the primary fallback (Tavily) fails.
 */
bool flaresolverr_is_enabled_and_available(void);

/**
 * @brief Parse an IPv4 whitelist entry in CIDR notation ("10.0.0.0/8").
 *
 * The prefix must be a whole number 0-32: "10.0.0.0/", "/abc" or "/33" is not
 * a CIDR (the entry is then taken as a host name).
 *
 * @return 1 with the network (masked) and netmask in host byte order, or 0.
 */
int url_fetcher_parse_cidr(const char *cidr, unsigned int *network, unsigned int *netmask);

/**
 * @brief Pluggable-provider entry point invoked from the 5 fallback sites
 *        in url_fetcher.c. Reads `g_config.url_fetcher.fallback` ONCE (TOCTOU
 *        guard) and dispatches to FlareSolverr or Tavily. On Tavily failure,
 *        cascades to FlareSolverr when both are configured.
 *
 *        Defined in url_fetcher_fallback.c.
 */
int url_fetcher_try_fallback(const char *url,
                             const char *base_url,
                             char **out_content,
                             size_t *out_size);

/* Detector helpers used by the FlareSolverr error-page gate live in
 * url_fetcher_detect.{c,h} so the unit-test suite can link against just
 * those pure functions without pulling in the full url_fetcher.c chain. */

#endif /* URL_FETCHER_INTERNAL_H */
