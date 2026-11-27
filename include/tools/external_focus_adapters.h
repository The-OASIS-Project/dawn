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
 * @file external_focus_adapters.h
 * @brief External focus-source adapters — Phase 1d of Dynamic Context
 *        Injection.
 *
 * Wires the L3 external adapters (document chunks, calendar events) into
 * the focus-source framework.  Adapters are registered via
 * `external_focus_adapters_register_all()`; daemon-startup wiring lands
 * in Phase 1e (this header is currently called only from tests).
 *
 * Layer: 3.  Each adapter `query` callback consumes only DB-layer
 * primitives (`document_db_*`, `calendar_db_*`) — never the
 * service layer (`document_search`, `calendar_service`) which may
 * trigger background sync or live network calls.  This is a
 * load-bearing invariant; the per-adapter file-headers list every
 * db_* call site for the network-call audit.
 *
 * Cross-user isolation: every adapter scopes its `*_db_*` calls by
 * the `user_id` parameter passed through `focus_source_query_fn` —
 * directly for chunks and for `calendar_db_account_list`, and
 * transitively through the account_id chain for occurrence queries.
 *
 * Email adapter — DEFERRED to v2.  The email subsystem has no
 * message-level cache (only `email_accounts`); DB-direct access is
 * impossible without first building an email message cache layer.
 * The cache layer is its own workstream prerequisite to the v2 email
 * focus adapter.  See `atlas/dawn/memory/DYNAMIC_CONTEXT_INJECTION.md`
 * §"Phase 1d" for the audit trail.
 */

#ifndef EXTERNAL_FOCUS_ADAPTERS_H
#define EXTERNAL_FOCUS_ADAPTERS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the document and calendar focus adapters into the
 *        focus-source framework.
 *
 * Idempotent in spirit but NOT in implementation: re-calling without
 * an intervening `focus_unregister_all()` returns FAILURE on the
 * duplicate-register path of the second adapter.  Tests call
 * `focus_unregister_all()` (test-only) in `setUp()` so each register
 * cycle starts from a clean registry.
 *
 * Production wiring (daemon startup) lands in Phase 1e.
 *
 * @return SUCCESS if both adapters register; FAILURE on the first
 *         failure (subsequent adapters are NOT registered after the
 *         first failure — caller is expected to surface the error).
 */
int external_focus_adapters_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* EXTERNAL_FOCUS_ADAPTERS_H */
