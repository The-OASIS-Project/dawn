/**
 * DAWN Living Tool Pills
 * Renders tool-call / tool-result activity as compact, always-visible "pills"
 * inline in the transcript, replacing the old hidden `.transcript-entry.debug`
 * tool entries. One pill per tool invocation, correlated by tool_call_id; a
 * `tool_call` opens a pending pill and its `tool_result` resolves it in place.
 * Tools from one text-turn iteration form a GROUP (collapsed to a summary when
 * many). Fed by three paths, all keyed on tool_call_id:
 *   - LIVE (origin + bystander): DawnStreaming.handleToolStep -> toolCall/toolResult
 *   - RELOAD: DawnHistory reload -> renderReloadGroup
 * See SERVER_AUTHORITATIVE_PERSISTENCE_DESIGN §living-dawn-toolpills.
 *
 * Untrusted content (tool name/args/result) is ALWAYS set via textContent —
 * never innerHTML — and detail is capped. The only markup emitted is the static
 * SVG caret/glyph, which carry no external data.
 *
 * Usage:
 *   DawnToolPills.toolCall(id, name, argsJson, iter)    // live: open pending pill (iter optional)
 *   DawnToolPills.toolResult(id, resultText, isError)   // live: resolve it (isError → red)
 *   DawnToolPills.closeGroup()                          // live: seal current group (per iteration)
 *   DawnToolPills.reset()                               // conversation switch / clear
 *   DawnToolPills.renderReloadGroup([{id,name,args,result}])  // reload: one terminal group
 */
(function (global) {
   'use strict';

   var CARET =
      '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path d="M4 2l4 4-4 4"/></svg>';
   var GLYPH =
      '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true">' +
      '<rect x="2" y="2" width="5" height="5" rx="1"/><rect x="9" y="2" width="5" height="5" rx="1"/>' +
      '<rect x="2" y="9" width="5" height="5" rx="1"/><rect x="9" y="9" width="5" height="5" rx="1"/></svg>';

   var DETAIL_CAP = 4096; // matches the daemon-side event_payload cap intent
   var COLLAPSE_AT = 3; // groups of >= this collapse to a summary on close
   var uid = 0;

   function cap(s) {
      s = s == null ? '' : String(s);
      return s.length > DETAIL_CAP ? s.slice(0, DETAIL_CAP) + '\n… (truncated)' : s;
   }

   function transcriptEl() {
      return typeof DawnElements !== 'undefined' ? DawnElements.transcript : null;
   }

   function scrollToBottom() {
      var t = transcriptEl();
      if (t) t.scrollTop = t.scrollHeight;
   }

   // Pretty client-measured duration (the daemon does not send tool timing).
   function fmtDuration(ms) {
      if (ms == null) return '';
      return ms < 1000 ? Math.round(ms) + ' ms' : (ms / 1000).toFixed(1) + ' s';
   }

   // ---- DOM factories --------------------------------------------------------

   function ioBlock(kind, label, value) {
      var wrap = document.createElement('div');
      wrap.className = 'dawn-toolpill-io ' + kind;
      var lb = document.createElement('div');
      lb.className = 'dawn-toolpill-io-label';
      lb.textContent = label;
      var pre = document.createElement('pre');
      pre.className = 'dawn-toolpill-io-body';
      pre.textContent = cap(value); // UNTRUSTED — textContent only
      // The body scrolls (max-height); make it keyboard-focusable so a keyboard-only user can
      // scroll a long args/result (WCAG 2.1.1).
      pre.tabIndex = 0;
      pre.setAttribute('role', 'group');
      pre.setAttribute('aria-label', label + ' content');
      wrap.append(lb, pre);
      return wrap;
   }

   // Build one pill node in the `running` state; caller fills args + later resolves.
   function createPill(id, name) {
      var safeName = (name || 'tool').slice(0, 128); // bound the name length (defensive)
      var domId = 'tp-' + ++uid;
      var pill = document.createElement('div');
      pill.className = 'dawn-toolpill running';
      pill.setAttribute('role', 'listitem');
      pill.dataset.toolCallId = id || '';

      var head = document.createElement('button');
      head.type = 'button';
      head.className = 'dawn-toolpill-head';
      head.setAttribute('aria-expanded', 'false');
      head.setAttribute('aria-controls', domId);

      var dot = document.createElement('span');
      dot.className = 'dawn-status-dot running';
      dot.setAttribute('aria-hidden', 'true');
      var nm = document.createElement('span');
      nm.className = 'dawn-toolpill-name';
      nm.textContent = safeName; // UNTRUSTED — textContent only
      var meta = document.createElement('span');
      meta.className = 'dawn-toolpill-meta';
      meta.textContent = 'running…';
      var car = document.createElement('span');
      car.className = 'dawn-toolpill-caret';
      car.innerHTML = CARET;

      head.append(dot, nm, meta, car);

      var detail = document.createElement('div');
      detail.className = 'dawn-toolpill-detail';
      detail.id = domId;
      detail.setAttribute('role', 'region');
      detail.setAttribute('aria-label', safeName + ' details');
      var inner = document.createElement('div');
      inner.className = 'dawn-toolpill-detail-inner';
      detail.appendChild(inner);

      head.addEventListener('click', function () {
         var open = pill.classList.toggle('expanded');
         head.setAttribute('aria-expanded', open ? 'true' : 'false');
      });

      pill.append(head, detail);
      pill._dot = dot;
      pill._meta = meta;
      pill._inner = inner;
      pill._startedAt = Date.now();
      return pill;
   }

   function setPillArgs(pill, args) {
      if (args != null && args !== '') pill._inner.appendChild(ioBlock('args', 'Arguments', args));
   }

   // Resolve a running pill to its terminal state. RED-ONLY design: state==='error' lights
   // the red rail/dot for a daemon-CONFIRMED failure (`error:true` on the tool_result payload);
   // everything else — success OR unknown — settles to a NEUTRAL "done" tone (deliberately NO
   // green, so colour marks only the exception that wants attention, and a missing signal degrades
   // to neutral rather than a false claim). The daemon's is_error is set at execute time, never
   // parsed from result text. Live-only v1: reload (renderReloadGroup) passes no state → neutral,
   // even for a historically-failed tool (persisting error→reload is a deferred follow-up). The
   // 'success' branch is retained but unused — reserved if an explicit green is ever wanted.
   function resolvePill(pill, meta, result, state) {
      pill.classList.remove('running');
      var cls = state === 'success' || state === 'error' ? ' ' + state : '';
      if (cls) pill.classList.add(state);
      pill._dot.className = 'dawn-status-dot' + cls;
      pill._meta.textContent = meta || '';
      pill._inner.appendChild(ioBlock('result', 'Result', result));
      if (pill._groupDot) pill._groupDot.className = 'dawn-status-dot' + cls;
   }

   // Build a group container. Always carries a (hidden-while-inline) summary chip;
   // closeGroup() promotes it to a collapsible summary when the group is large.
   function buildGroup() {
      var g = document.createElement('div');
      g.className = 'dawn-toolgroup inline expanded';

      var sum = document.createElement('button');
      sum.type = 'button';
      sum.className = 'dawn-toolgroup-summary';
      sum.setAttribute('aria-expanded', 'true');
      var listId = 'tg-' + ++uid;
      sum.setAttribute('aria-controls', listId);
      var glyph = document.createElement('span');
      glyph.className = 'dawn-toolgroup-glyph';
      glyph.innerHTML = GLYPH;
      var lab = document.createElement('span');
      lab.className = 'dawn-toolgroup-label';
      lab.textContent = '0 tools';
      var states = document.createElement('span');
      states.className = 'dawn-toolgroup-states';
      states.setAttribute('aria-hidden', 'true');
      var car = document.createElement('span');
      car.className = 'dawn-toolpill-caret';
      car.innerHTML = CARET;
      sum.append(glyph, lab, states, car);

      var list = document.createElement('div');
      list.className = 'dawn-toolgroup-pills';
      list.id = listId;
      list.setAttribute('role', 'list');

      sum.addEventListener('click', function () {
         var open = g.classList.toggle('expanded');
         g.classList.toggle('collapsed', !open);
         sum.setAttribute('aria-expanded', open ? 'true' : 'false');
         g._userToggled = true;
      });

      g.append(sum, list);
      g._list = list;
      g._label = lab;
      g._states = states;
      g._summary = sum;
      g._pills = [];
      return g;
   }

   function groupAppendPill(g, pill) {
      g._pills.push(pill);
      g._list.appendChild(pill);
      var d = document.createElement('span');
      d.className = 'dawn-status-dot running';
      pill._groupDot = d;
      g._states.appendChild(d);
      g._label.textContent = g._pills.length + (g._pills.length === 1 ? ' tool' : ' tools');
      refreshGroupAria(g);
   }

   function refreshGroupAria(g) {
      var errs = g._pills.filter(function (p) {
         return p.classList.contains('error');
      }).length;
      var running = g._pills.some(function (p) {
         return p.classList.contains('running');
      });
      g._summary.setAttribute(
         'aria-label',
         g._pills.length +
            (g._pills.length === 1 ? ' tool' : ' tools') +
            (errs ? ', ' + errs + ' failed' : '') +
            (running ? ', running' : '')
      );
   }

   // Finalize a group: reveal the summary + collapse when large (unless the user
   // already toggled it), otherwise leave it inline (bare pills, no chrome).
   function finalizeGroup(g) {
      if (!g) return;
      refreshGroupAria(g);
      if (g._pills.length >= COLLAPSE_AT && !g._userToggled) {
         g.classList.remove('inline', 'expanded');
         g.classList.add('collapsible', 'collapsed');
         g._summary.setAttribute('aria-expanded', 'false');
      }
   }

   // ---- Live API -------------------------------------------------------------

   var currentGroup = null;
   var lastIter = null; // last-seen tool-loop iteration index (per-iteration group sealing)

   function ensureGroup() {
      // Self-heal if the transcript was cleared (conversation switch) out from under a
      // still-open live group — never append into a detached node.
      if (currentGroup && !currentGroup.isConnected) currentGroup = null;
      if (!currentGroup) {
         var t = transcriptEl();
         if (!t) return null;
         currentGroup = buildGroup();
         t.appendChild(currentGroup);
      }
      return currentGroup;
   }

   // `iter` (optional) is the daemon's 0-based tool-loop iteration index. When it CHANGES from the
   // last-seen value while a group is still open, this is an iteration boundary the stream_start
   // seal missed (a tool-only iteration renders no text bubble) — seal here so its tools start a
   // fresh group, matching reload's per-assistant-message grouping. Keyed on "differs" (not
   // "increments") so a per-turn reset (…3 -> 0) also seals. Absent iter -> stream_start seal only.
   function toolCall(id, name, args, iter) {
      if (typeof iter === 'number') {
         if (currentGroup && iter !== lastIter) closeGroup();
         lastIter = iter;
      }
      var g = ensureGroup();
      if (!g) return;
      var pill = createPill(id, name);
      setPillArgs(pill, args);
      groupAppendPill(g, pill);
      scrollToBottom();
   }

   function findPill(g, id) {
      if (!g || !id) return null;
      for (var i = 0; i < g._pills.length; i++) {
         if (g._pills[i].dataset.toolCallId === id) return g._pills[i];
      }
      return null;
   }

   // Oldest still-running pill in the group — the FIFO fallback for an id-less result.
   function oldestRunning(g) {
      if (!g) return null;
      for (var i = 0; i < g._pills.length; i++) {
         if (g._pills[i].classList.contains('running')) return g._pills[i];
      }
      return null;
   }

   // Resolve a running pill for `id`. Pair by tool_call_id; fall back to the oldest running pill
   // when the id is empty/uncorrelated (native tool-calling always carries an id — this covers
   // id-less legacy tools + mild out-of-order), and only as a last resort open a lone pill so
   // nothing is dropped. RED-ONLY: `isError` reds the pill's rail/dot AND its group-state dot (via
   // resolvePill), so a failure contained in a collapsed group stays visible in the summary;
   // success/unknown settle to a neutral "done" (no green). `isError` is the daemon's explicit
   // confirmed-failure flag, never inferred from `result` text.
   function toolResult(id, result, isError) {
      var pill = findPill(currentGroup, id) || oldestRunning(currentGroup);
      if (!pill) {
         var g = ensureGroup();
         if (!g) return;
         pill = createPill(id, 'tool');
         groupAppendPill(g, pill);
      }
      var meta = pill._startedAt ? fmtDuration(Date.now() - pill._startedAt) : '';
      resolvePill(pill, meta, result, isError ? 'error' : undefined);
      refreshGroupAria(currentGroup);
      scrollToBottom();
   }

   function closeGroup() {
      if (currentGroup) {
         finalizeGroup(currentGroup);
         currentGroup = null;
      }
   }

   function reset() {
      currentGroup = null;
      lastIter = null;
   }

   // ---- Reload API -----------------------------------------------------------

   // Build ONE terminal group from reload data. items: [{ id, name, args, result }]
   // (already paired by tool_call_id in the caller). Appends at the current
   // transcript position — call once per assistant tool_calls message so live and
   // reload split into the same per-iteration groups.
   function renderReloadGroup(items) {
      var t = transcriptEl();
      if (!t || !items || !items.length) return;
      var g = buildGroup();
      t.appendChild(g);
      items.forEach(function (it) {
         var pill = createPill(it.id, it.name);
         setPillArgs(pill, it.args);
         groupAppendPill(g, pill);
         resolvePill(pill, '', it.result);
      });
      finalizeGroup(g);
   }

   global.DawnToolPills = {
      toolCall: toolCall,
      toolResult: toolResult,
      closeGroup: closeGroup,
      reset: reset,
      renderReloadGroup: renderReloadGroup,
   };
})(window);
