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
 * Per-turn context-injection surface (Phase 1g-ii of Dynamic Context Injection).
 *
 * Renders the daemon's context_injection broadcast as a trust-tiered transcript
 * block:
 *  - internal items collapsed by default behind a count summary
 *  - external items expose a one-line preview with disclosure for full text
 *  - user-content items use the same shape as external but rendered monospace
 *
 * Filter-match rejections render as a separate dawn-badge.warning chip outside
 * the block so the source category stays visible regardless of the user's
 * disclosure state.  Persistence semantics mirror DawnSilentObserve: the chip
 * stays dismissed until a NEWER rejection arrives.
 *
 * Wire format (flat at root, NOT under msg.payload):
 *
 *   @typedef {Object} ContextInjectionMsg
 *   @property {"context_injection"} type
 *   @property {number} user_id
 *   @property {number} conversation_id
 *   @property {number} turn_id
 *   @property {ContextInjectionItem[]} items
 *   @property {{source_id:string,count:number}[]} filter_rejections
 *
 *   @typedef {Object} ContextInjectionItem
 *   @property {string} source_id
 *   @property {"internal"|"external"|"user-content"} source_type
 *   @property {string} text
 *   @property {number} score
 *   @property {{semantic:number,recency:number,importance:number,source:number}}
 *            score_breakdown
 *   @property {number} applied_source_weight
 *   @property {"new"|"changed"|"in_context"|"referenced"|"left_out"} [state]
 *                                          -- sent this turn (new / changed), already
 *                                             shown by an earlier turn (named again, or
 *                                             not), or due to be sent but left out
 *   @property {number} [item_timestamp]    -- unix seconds the item was learned, saved or
 *                                             happens; omitted when it has none
 *   @property {{conversation_id:number,msg_id_start:number,msg_id_end:number}}
 *            [provenance]                  -- omitted entirely when unavailable
 */
(function () {
   'use strict';

   const FILTER_MATCH_LOOKBACK_MS = 60 * 60 * 1000;
   /* Block cap (H3) — bound transcript DOM growth on long sessions.  Older
    * blocks are removed FIFO when this cap is exceeded.  Empty-state lines
    * count toward this cap as well (no special-case). */
   const MAX_BLOCKS = 50;
   /* Defense-in-depth length cap (M11) — long source_id strings get truncated
    * to bound aria-label / title attribute payloads in case a future tool
    * ships an unusually long identifier. */
   const SOURCE_ID_MAX = 64;

   /* Dismiss-tracking for the warning chip — re-arms only when a NEWER
    * rejection arrives, matching silent-observe.js semantics. */
   let lastFilterRejectionTs = 0;
   let chipDismissedAt = 0;

   /* Per-conversation last-rendered turn (M1).  Map<conv_id, turn_id> —
    * short-circuits replay-burst and equal-or-lower turn_ids that arrive
    * out of order during reconnect. */
   const lastTurnByConv = new Map();

   /* Bounded list of appended blocks for FIFO eviction (H3). */
   const appendedBlocks = [];

   /* Modal trigger restoration (focus return on close). */
   let modalTrigger = null;
   let ciEscToken = null; // DawnEscStack registration while the info modal is open

   /* =============================================================================
    * Sanitization
    * ============================================================================= */

   /** Sanitize untrusted strings — DOMPurify with empty allowlists, falling
    *  back to manual entity escaping if DOMPurify is unavailable.  Numerics
    *  go through Number() coercion at use sites; never call this on a
    *  numeric field. */
   function sanitizeText(value) {
      if (typeof value !== 'string') return '';
      try {
         return window.DOMPurify
            ? window.DOMPurify.sanitize(value, { ALLOWED_TAGS: [], ALLOWED_ATTR: [] })
            : value.replace(
                 /[<>&"']/g,
                 (c) =>
                    ({
                       '<': '&lt;',
                       '>': '&gt;',
                       '&': '&amp;',
                       '"': '&quot;',
                       "'": '&#39;',
                    })[c]
              );
      } catch (_e) {
         return '';
      }
   }

   /** Sanitize + length-clamp for source identifiers used in aria/title
    *  attributes.  Bounds screen-reader DoS payloads (M11). */
   function sanitizeSourceId(value) {
      const s = sanitizeText(value);
      return s.length > SOURCE_ID_MAX ? s.slice(0, SOURCE_ID_MAX - 1) + '…' : s;
   }

   /* =============================================================================
    * Defense-in-depth: only render events whose conversation_id matches the
    * client's view of the active conversation.  Server already filters by
    * authenticated user_id and session-side active conversation, but the
    * client's view of the active conversation may have drifted from the
    * session view (recent switch in another tab, race on conversation
    * create).  Drop events that don't match — caller is unauthenticated
    * in this view.
    * ============================================================================= */

   function eventTargetsActiveView(msg) {
      if (typeof DawnHistory === 'undefined' || !DawnHistory.getActiveConversationId) {
         return true;
      }
      const active = DawnHistory.getActiveConversationId();
      if (!active) return true;
      return Number(msg.conversation_id) === Number(active);
   }

   /* =============================================================================
    * Item / cluster builders
    * ============================================================================= */

   function trustTierLabel(sourceType) {
      switch (sourceType) {
         case 'internal':
            return 'Internal';
         case 'external':
            return 'External';
         case 'user-content':
            return 'User content';
         default:
            return 'Item';
      }
   }

   /** What happened to the item this turn: sent, or already in context.
    *  Allowlisted — the wire value never reaches the DOM as-is. */
   function stateLabel(state) {
      switch (state) {
         case 'new':
            return 'Sent';
         case 'changed':
            return 'Sent again (changed)';
         case 'referenced':
            return 'Already in context, named';
         case 'in_context':
            return 'Already in context';
         case 'left_out':
            return 'Not sent (left out)';
         default:
            return '';
      }
   }

   function fmtScore(n) {
      const v = Number(n);
      if (!Number.isFinite(v)) return '0.00';
      return v.toFixed(2);
   }

   function previewLine(text) {
      if (!text) return '(empty)';
      const newlineIdx = text.indexOf('\n');
      const single = newlineIdx >= 0 ? text.slice(0, newlineIdx) : text;
      if (single.length <= 140) return single;
      return single.slice(0, 137) + '…';
   }

   /** Build the score-tooltip string ONCE per item (M4).  All values flow
    *  through fmtScore() → only numerics reach the title attribute, so this
    *  remains injection-safe even when interpolated unescaped. */
   function buildScoreTitle(item) {
      const b = (item && item.score_breakdown) || {};
      return (
         `Score ${fmtScore(item && item.score)} ` +
         `(semantic ${fmtScore(b.semantic)} · recency ${fmtScore(b.recency)} · ` +
         `importance ${fmtScore(b.importance)} · source ${fmtScore(b.source)})`
      );
   }

   /** The item's date as the model sees it (YYYY-MM-DD, local), or '' without
    *  a valid timestamp. */
   function itemDate(ts) {
      if (typeof ts !== 'number' || !Number.isFinite(ts) || ts <= 0) return '';
      const d = new Date(ts * 1000);
      if (Number.isNaN(d.getTime())) return '';
      const pad = (n) => String(n).padStart(2, '0');
      return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
   }

   /** Cache derived strings on the item so we don't re-sanitize/re-format on
    *  every render path (M2 + M3). */
   function decorate(item) {
      if (!item || typeof item !== 'object') return item;
      if (item._decorated) return item;
      item._safeSourceId = sanitizeSourceId(item.source_id || 'unknown');
      item._date = itemDate(item.item_timestamp);
      item._safeText = sanitizeText(item.text || '');
      item._previewLine = previewLine(item._safeText);
      item._scoreTitle = buildScoreTitle(item);
      item._decorated = true;
      return item;
   }

   function buildItemRow(item, idx) {
      decorate(item);
      const row = document.createElement('div');
      row.className = 'dawn-context-injection-item';
      const trust =
         item.source_type === 'internal' ||
         item.source_type === 'external' ||
         item.source_type === 'user-content'
            ? item.source_type
            : 'external';
      row.setAttribute('data-source-trust', trust);

      const header = document.createElement('div');
      header.className = 'dawn-context-injection-item-header';

      const sourceLabel = document.createElement('span');
      sourceLabel.className = 'dawn-context-injection-source';
      /* M9: bracket the identifier so it reads as a key, not as content. */
      sourceLabel.textContent = item._date
         ? `[${item._safeSourceId} ${item._date}]`
         : `[${item._safeSourceId}]`;
      header.appendChild(sourceLabel);

      /* M6: trust-tier label is a plain span — dawn-badge defaults conflict
       * with the contextual styling and the override would amount to
       * reskinning a primitive.  Plain span is the simpler fix. */
      const trustLabel = document.createElement('span');
      trustLabel.className = 'dawn-context-injection-trust';
      trustLabel.textContent = trustTierLabel(trust);
      header.appendChild(trustLabel);

      const state = stateLabel(item.state);
      if (state) {
         const stateEl = document.createElement('span');
         stateEl.className = 'dawn-context-injection-trust';
         stateEl.textContent = state;
         header.appendChild(stateEl);
      }

      /* H6: drop the per-row raw score number (no comparative context — score
       * breakdown moved entirely into the modal).  Tooltip remains on the
       * Tell-me-more affordance for keyboard/mouse hover. */
      const moreBtn = document.createElement('button');
      moreBtn.type = 'button';
      moreBtn.className = 'dawn-context-injection-more';
      moreBtn.textContent = 'Tell me more';
      /* SECURITY NOTE (M10): only numeric-derived strings (fmtScore output)
       * may flow into the title attribute.  buildScoreTitle() already enforces
       * this — every interpolation is fmtScore(...).  Do NOT add any string
       * field (source_id, text, etc.) to the tooltip without re-sanitizing,
       * even though title attributes are not parsed as HTML — they are read
       * verbatim by screen readers. */
      moreBtn.setAttribute('title', item._scoreTitle);
      moreBtn.setAttribute(
         'aria-label',
         `Show score breakdown for ${item._safeSourceId} ${idx + 1}`
      );
      moreBtn.addEventListener('click', (ev) => {
         ev.preventDefault();
         ev.stopPropagation();
         openInfoModal(item, moreBtn);
      });
      header.appendChild(moreBtn);

      row.appendChild(header);

      const safeText = item._safeText;
      const preview = item._previewLine;

      if (trust === 'internal') {
         /* For internal items rendered standalone (when no internal cluster is
          * present), still expose the text under a disclosure so the user can
          * audit.  When wrapped in the internal cluster, the cluster shows
          * count + expands rows. */
         const details = document.createElement('details');
         details.className = 'dawn-context-injection-disclosure';
         const summary = document.createElement('summary');
         summary.textContent = preview;
         details.appendChild(summary);
         const body = document.createElement('div');
         body.className = 'dawn-context-injection-text';
         body.textContent = safeText;
         details.appendChild(body);
         row.appendChild(details);
      } else {
         const previewEl = document.createElement('div');
         previewEl.className = 'dawn-context-injection-preview';
         previewEl.textContent = preview;
         row.appendChild(previewEl);

         if (safeText && safeText.length > preview.length) {
            const details = document.createElement('details');
            details.className = 'dawn-context-injection-disclosure';
            const summary = document.createElement('summary');
            summary.textContent = 'Show more';
            details.appendChild(summary);
            const body = document.createElement('div');
            body.className = 'dawn-context-injection-text';
            body.textContent = safeText;
            details.appendChild(body);
            row.appendChild(details);
         }
      }

      return row;
   }

   /** M5: native <details><summary> for the internal cluster — saves the
    *  manual aria-expanded toggle and gets keyboard / screen-reader
    *  semantics from the browser. */
   function buildInternalCluster(internalItems) {
      const details = document.createElement('details');
      details.className = 'dawn-context-injection-internal-cluster';
      details.setAttribute('data-source-trust', 'internal');

      const summary = document.createElement('summary');
      summary.className = 'dawn-context-injection-cluster-summary';
      const count = internalItems.length;
      summary.textContent = `${count} internal item${count === 1 ? '' : 's'}`;
      details.appendChild(summary);

      const list = document.createElement('div');
      list.className = 'dawn-context-injection-cluster-list';
      internalItems.forEach((item, idx) => list.appendChild(buildItemRow(item, idx)));
      details.appendChild(list);

      return details;
   }

   /** Build the per-turn block.  Empty turns get the muted single-line
    *  treatment (H4); non-empty turns get the full block. */
   function buildBlock(msg) {
      const items = Array.isArray(msg.items) ? msg.items : [];

      if (items.length === 0) {
         /* H4: muted single-line empty state — no border, no chrome. */
         const line = document.createElement('div');
         line.className = 'dawn-context-injection-empty-line';
         line.setAttribute('data-turn-id', String(Number(msg.turn_id) || 0));
         line.setAttribute('role', 'note');
         line.textContent = 'No additional context for this turn.';
         return line;
      }

      const block = document.createElement('div');
      block.className = 'dawn-context-injection-block';
      block.setAttribute('data-turn-id', String(Number(msg.turn_id) || 0));
      block.setAttribute('role', 'region');
      block.setAttribute('aria-label', 'Context injected for this turn');

      const header = document.createElement('div');
      header.className = 'dawn-context-injection-header';
      const title = document.createElement('span');
      title.className = 'dawn-context-injection-title';
      title.textContent = 'Context for this turn';
      header.appendChild(title);
      block.appendChild(header);

      const internalItems = [];
      const otherItems = [];
      for (const it of items) {
         if (it && it.source_type === 'internal') internalItems.push(it);
         else if (it) otherItems.push(it);
      }

      if (internalItems.length > 0) {
         block.appendChild(buildInternalCluster(internalItems));
      }
      otherItems.forEach((item, idx) => block.appendChild(buildItemRow(item, idx)));

      return block;
   }

   /* =============================================================================
    * Filter chip
    * ============================================================================= */

   function buildFilterChip(rejections) {
      const valid = rejections.filter(
         (r) => r && typeof r.source_id === 'string' && Number(r.count) > 0
      );
      if (valid.length === 0) return null;
      const head = valid[0];
      const cat = sanitizeSourceId(head.source_id);

      const chip = document.createElement('button');
      chip.type = 'button';
      chip.className = 'dawn-badge warning soft dawn-context-injection-warning-chip';
      chip.setAttribute('role', 'button');
      chip.setAttribute('tabindex', '0');
      chip.setAttribute(
         'aria-label',
         `Filtered: blocked-pattern match in ${cat}. Click to dismiss`
      );
      chip.textContent = `Filtered: blocked-pattern match in ${cat} (click to dismiss)`;

      const dismiss = (ev) => {
         ev.preventDefault();
         ev.stopPropagation();
         chipDismissedAt = lastFilterRejectionTs;
         chip.classList.add('hidden');
      };
      chip.addEventListener('click', dismiss);
      chip.addEventListener('keydown', (ev) => {
         if (ev.key === 'Enter' || ev.key === ' ') dismiss(ev);
      });

      return chip;
   }

   function renderRejectionChip(rejections) {
      /* H5: anchor is statically mounted in index.html.  We do NOT lazily
       * inject — the static mount keeps CSP-friendly markup and shares the
       * warning-rail visual hierarchy with silent-observe's chip. */
      const anchor = document.getElementById('context-injection-warning-chip-anchor');
      if (!anchor) return;
      while (anchor.firstChild) anchor.removeChild(anchor.firstChild);
      if (lastFilterRejectionTs <= chipDismissedAt) return;
      const chip = buildFilterChip(rejections);
      if (chip) anchor.appendChild(chip);
   }

   /* =============================================================================
    * Block append + transcript-cap eviction (H3)
    * ============================================================================= */

   function appendBlock(msg) {
      const transcript =
         (window.DawnElements && window.DawnElements.transcript) ||
         document.getElementById('transcript');
      if (!transcript) return;

      const placeholder = transcript.querySelector('.transcript-placeholder');
      if (placeholder) placeholder.remove();

      const block = buildBlock(msg);
      transcript.appendChild(block);

      /* H3: FIFO eviction.  Removed blocks may already be detached from the
       * DOM (e.g., transcript cleared by a conversation switch) — check
       * isConnected before remove. */
      appendedBlocks.push(block);
      while (appendedBlocks.length > MAX_BLOCKS) {
         const old = appendedBlocks.shift();
         if (old && old.isConnected && old.parentNode) {
            old.parentNode.removeChild(old);
         }
      }

      const rejections = Array.isArray(msg.filter_rejections) ? msg.filter_rejections : [];
      const hasRejection = rejections.some((r) => r && Number(r.count) > 0);
      if (hasRejection) {
         lastFilterRejectionTs = Date.now();
         renderRejectionChip(rejections);
      }

      transcript.scrollTop = transcript.scrollHeight;
   }

   /* =============================================================================
    * Score-breakdown / provenance modal
    * ============================================================================= */

   function isModalOpen() {
      const modal = document.getElementById('context-injection-modal');
      return modal && !modal.classList.contains('hidden');
   }

   function openInfoModal(item, trigger) {
      decorate(item);
      const modal = document.getElementById('context-injection-modal');
      const body = document.getElementById('context-injection-modal-body');
      const title = document.getElementById('context-injection-modal-title');
      if (!modal || !body) return;

      modalTrigger = trigger || document.activeElement;
      title.textContent = `${item._safeSourceId} — score breakdown`;

      while (body.firstChild) body.removeChild(body.firstChild);

      const summary = document.createElement('p');
      summary.className = 'dawn-context-injection-modal-summary';
      summary.textContent = `Final score ${fmtScore(item.score)} · applied source weight ${fmtScore(
         item.applied_source_weight
      )}`;
      body.appendChild(summary);

      body.appendChild(buildBreakdownChart(item.score_breakdown));

      if (item.provenance && Number(item.provenance.conversation_id) > 0) {
         body.appendChild(buildProvenanceSection(item.provenance));
      } else {
         const noProv = document.createElement('p');
         noProv.className = 'dawn-context-injection-modal-noprov';
         noProv.textContent = 'No source conversation available for this item.';
         body.appendChild(noProv);
      }

      modal.classList.remove('hidden');
      if (ciEscToken === null) {
         ciEscToken = DawnEscStack.register(() => {
            closeInfoModal();
            return true;
         });
      }
      const closeBtn = document.getElementById('context-injection-modal-close');
      if (closeBtn) closeBtn.focus();
   }

   function closeInfoModal() {
      const modal = document.getElementById('context-injection-modal');
      if (modal) modal.classList.add('hidden');
      if (ciEscToken !== null) {
         DawnEscStack.unregister(ciEscToken);
         ciEscToken = null;
      }
      /* M13: trigger element may have been removed by a conversation switch
       * or transcript clear while the modal was open — the focus call
       * would otherwise throw, leaving us in an inconsistent state. */
      if (modalTrigger && typeof modalTrigger.focus === 'function') {
         try {
            modalTrigger.focus();
         } catch (_e) {
            /* Trigger gone — let focus fall back to <body> naturally. */
         }
      }
      modalTrigger = null;
   }

   /** M8: per-dimension bar colors for the breakdown chart. */
   function buildBreakdownChart(breakdown) {
      const wrap = document.createElement('div');
      wrap.className = 'dawn-context-injection-breakdown';
      const fields = ['semantic', 'recency', 'importance', 'source'];
      const safe = breakdown && typeof breakdown === 'object' ? breakdown : {};
      const max = Math.max(...fields.map((f) => Math.max(0, Number(safe[f]) || 0)), 0.0001);

      fields.forEach((f) => {
         const v = Math.max(0, Number(safe[f]) || 0);
         const row = document.createElement('div');
         row.className = 'dawn-context-injection-bar-row';

         const label = document.createElement('span');
         label.className = 'dawn-context-injection-bar-label';
         label.textContent = f;
         row.appendChild(label);

         const track = document.createElement('span');
         track.className = 'dawn-context-injection-bar-track';
         const fill = document.createElement('span');
         fill.className = 'dawn-context-injection-bar-fill';
         fill.setAttribute('data-dim', f);
         const pct = (v / max) * 100;
         fill.style.width = `${Math.max(0, Math.min(100, pct)).toFixed(1)}%`;
         track.appendChild(fill);
         row.appendChild(track);

         const num = document.createElement('span');
         num.className = 'dawn-context-injection-bar-value';
         num.textContent = v.toFixed(3);
         row.appendChild(num);

         wrap.appendChild(row);
      });
      return wrap;
   }

   function buildProvenanceSection(prov) {
      const wrap = document.createElement('div');
      wrap.className = 'dawn-context-injection-provenance';

      const heading = document.createElement('h4');
      heading.className = 'dawn-context-injection-provenance-heading';
      heading.textContent = 'Source';
      wrap.appendChild(heading);

      const meta = document.createElement('p');
      meta.className = 'dawn-context-injection-provenance-meta';
      meta.textContent = `Conversation ${Number(prov.conversation_id)} · messages ${Number(
         prov.msg_id_start
      )}–${Number(prov.msg_id_end)}`;
      wrap.appendChild(meta);

      /* H1: canonical export is DawnHistory.loadConversation (history.js:1796).
       * Defensively guard so the affordance is dropped (not silently broken)
       * if the API name ever changes. */
      if (
         typeof DawnHistory !== 'undefined' &&
         typeof DawnHistory.loadConversation === 'function'
      ) {
         const btn = document.createElement('button');
         btn.type = 'button';
         btn.className = 'btn-link dawn-context-injection-provenance-link';
         btn.textContent = 'Open source conversation';
         btn.addEventListener('click', () => {
            DawnHistory.loadConversation(Number(prov.conversation_id));
            closeInfoModal();
         });
         wrap.appendChild(btn);
      }

      return wrap;
   }

   /* =============================================================================
    * Modal focus trap (C1) — Tab cycles within the modal subtree, Esc closes.
    * Esc is bound at document level so it fires regardless of focus location.
    * ============================================================================= */

   function getFocusableInModal(modal) {
      if (!modal) return [];
      const sel =
         'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), ' +
         'textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';
      return Array.from(modal.querySelectorAll(sel)).filter(
         (el) => !el.classList.contains('hidden') && el.offsetParent !== null
      );
   }

   function handleModalTabTrap(modal, ev) {
      if (ev.key !== 'Tab') return;
      const focusables = getFocusableInModal(modal);
      if (focusables.length === 0) {
         ev.preventDefault();
         return;
      }
      const first = focusables[0];
      const last = focusables[focusables.length - 1];
      if (ev.shiftKey && document.activeElement === first) {
         ev.preventDefault();
         last.focus();
      } else if (!ev.shiftKey && document.activeElement === last) {
         ev.preventDefault();
         first.focus();
      }
   }

   /* =============================================================================
    * Init
    * ============================================================================= */

   function init() {
      const closeBtn = document.getElementById('context-injection-modal-close');
      if (closeBtn) closeBtn.addEventListener('click', closeInfoModal);

      const modal = document.getElementById('context-injection-modal');
      if (modal) {
         modal.addEventListener('click', (ev) => {
            if (ev.target === modal) closeInfoModal();
         });
         /* C1: focus trap — keep Tab inside the modal while it's open. */
         modal.addEventListener('keydown', (ev) => handleModalTabTrap(modal, ev));
      }

      /* Escape close is handled via DawnEscStack (register-on-open in
       * openInfoModal / unregister in closeInfoModal), so it stacks under any
       * layer opened above it. */
   }

   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   /* =============================================================================
    * Public surface — invoked by dawn.js's WebSocket dispatch.
    * ============================================================================= */

   window.DawnContextInjection = {
      /**
       * Render a context_injection event.  The argument is the FLAT message
       * (not msg.payload) — context_injection events are wrapped at the
       * dispatch level, not the payload level.  See
       * src/webui/webui_server.c::webui_broadcast_context_injection.
       *
       * @param {ContextInjectionMsg} msg
       */
      handleEvent(msg) {
         if (!msg || typeof msg !== 'object') return;
         if (msg.type !== 'context_injection') return;
         if (!eventTargetsActiveView(msg)) {
            console.debug(
               'DawnContextInjection: dropping event for non-active conversation',
               msg.conversation_id
            );
            return;
         }

         /* M1: turn-id replay dedup.  Reconnect bursts can replay the latest
          * event; equal-or-lower turn_ids than the highest already rendered
          * for this conversation are dropped. */
         const convKey = String(Number(msg.conversation_id) || 0);
         const turn = Number(msg.turn_id) || 0;
         const lastTurn = lastTurnByConv.get(convKey) || 0;
         if (turn > 0 && turn <= lastTurn) {
            console.debug(
               'DawnContextInjection: dropping replay turn',
               turn,
               'last seen',
               lastTurn
            );
            return;
         }
         if (turn > 0) lastTurnByConv.set(convKey, turn);

         /* Auto-expire stale dismissals (lookback window matches silent-
          * observe semantics) — if the most recent rejection is older than
          * the window, reset the dismiss timestamp so the next chip arms
          * cleanly. */
         if (chipDismissedAt > 0 && Date.now() - chipDismissedAt > FILTER_MATCH_LOOKBACK_MS) {
            chipDismissedAt = 0;
         }

         appendBlock(msg);
      },

      reset() {
         lastFilterRejectionTs = 0;
         chipDismissedAt = 0;
         lastTurnByConv.clear();
         appendedBlocks.length = 0;
         const anchor = document.getElementById('context-injection-warning-chip-anchor');
         if (anchor) {
            while (anchor.firstChild) anchor.removeChild(anchor.firstChild);
         }
         /* M12: only close the modal if it's actually open — prevents
          * yanking focus from another flow that happens to share state. */
         if (isModalOpen()) {
            closeInfoModal();
         }
      },
   };
})();
