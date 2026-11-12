/*
 * DAWN WebUI — Shared tablist binding helper
 *
 * One place to wire WAI-ARIA roving-tabindex + arrow-key navigation
 * onto any `role="tablist"` strip across the WebUI.  Replaces three
 * near-identical implementations in memory.js / scheduler-queue.js /
 * music.js and unifies behavior for memory-import-tabs and the
 * CalDAV auth-type-selector.
 *
 * Usage:
 *   const tl = DawnTablist.bind({
 *      tabs: el.querySelectorAll('.my-tab'),
 *      getActive: () => state.activeTab,
 *      onActivate: (name) => switchTab(name),
 *      // attr: 'tab',  // optional, default reads tab.dataset.tab
 *   });
 *   tl.sync();      // sync DOM to initial getActive() — call after bind
 *                   // and inside switchTab after updating state.
 *   tl.destroy();   // remove listeners (modal teardown, etc.)
 *
 * Contract:
 *   - Each tab button must have a `data-<attr>="<name>"` identifier
 *     and SHOULD be a `<button>` element (or use a `role="button"`
 *     ancestor that accepts Enter/Space natively).  We don't wire a
 *     separate Enter/Space handler — native <button> semantics turn
 *     those keys into click events, and our click handler already
 *     calls onActivate.
 *   - getActive() returns the currently-active tab name (string).
 *   - onActivate(name) is called when the user clicks a tab or
 *     navigates via arrow / Home / End.  Consumer EITHER updates
 *     state + calls tl.sync() inline within onActivate (compact
 *     form, used by calendar-accounts), OR delegates to a
 *     switchTab-style function that does state + sync (memory,
 *     music, scheduler).  Both shapes work.
 *   - The helper applies `.active` class, `aria-selected`, and a
 *     roving `tabindex` on sync().  Only the active tab gets
 *     tabindex="0"; inactive tabs get "-1" so Tab moves PAST the
 *     strip rather than stepping through each tab one-by-one.
 *   - tl.destroy() is OPTIONAL for persistent tablists (header
 *     popovers whose tab elements never leave the DOM).  Call it
 *     when the tab strip itself is being removed (modal teardown,
 *     dynamic insertion) to avoid leaked listeners.  Failure to
 *     destroy on removal leaves orphaned listeners on detached DOM
 *     nodes, which the GC collects with the nodes themselves so it's
 *     not catastrophic — just untidy.
 *
 * Keys: ←/→ cycle (wrapping), Home/End jump to first/last; a tab with the
 * `hidden` attribute is skipped.
 * Automatic-activation pattern — focus + selection move together
 * (no separate Space/Enter step).
 */
(function (global) {
   'use strict';

   function bind(opts) {
      if (!opts || !opts.tabs || !opts.getActive || !opts.onActivate) {
         throw new Error('DawnTablist.bind: tabs, getActive, onActivate are required');
      }
      const tabs = Array.from(opts.tabs);
      const getActive = opts.getActive;
      const onActivate = opts.onActivate;
      /* dataset key that holds the tab identifier.  e.g. attr='tab'
       * reads tab.dataset.tab (HTML: data-tab="...").  Defaults to
       * 'tab' which covers the three header popovers; memory-import
       * passes 'source' (data-source) and calendar auth passes
       * 'authType' (data-auth-type). */
      const attr = opts.attr || 'tab';

      function nameOf(tab) {
         return tab.dataset[attr];
      }

      function sync() {
         const active = getActive();
         tabs.forEach((tab) => {
            const isActive = nameOf(tab) === active;
            tab.classList.toggle('active', isActive);
            tab.setAttribute('aria-selected', isActive ? 'true' : 'false');
            tab.setAttribute('tabindex', isActive ? '0' : '-1');
         });
      }

      function onClick(e) {
         const tab = e.currentTarget;
         const name = nameOf(tab);
         if (name) onActivate(name);
      }

      function onKeydown(e) {
         const key = e.key;
         if (key !== 'ArrowLeft' && key !== 'ArrowRight' && key !== 'Home' && key !== 'End') {
            return;
         }
         /* A hidden tab (a feature the server lacks) is skipped. */
         const shown = tabs.filter((t) => !t.hidden);
         if (shown.length === 0) return;
         const active = getActive();
         const currentIdx = shown.findIndex((t) => nameOf(t) === active);
         let nextIdx;
         if (key === 'Home') {
            nextIdx = 0;
         } else if (key === 'End') {
            nextIdx = shown.length - 1;
         } else if (key === 'ArrowLeft') {
            nextIdx = currentIdx <= 0 ? shown.length - 1 : currentIdx - 1;
         } else {
            nextIdx = currentIdx >= shown.length - 1 ? 0 : currentIdx + 1;
         }
         const nextTab = shown[nextIdx];
         if (!nextTab) return;
         e.preventDefault();
         const name = nameOf(nextTab);
         if (name) onActivate(name);
         nextTab.focus();
      }

      tabs.forEach((tab) => {
         tab.addEventListener('click', onClick);
         tab.addEventListener('keydown', onKeydown);
      });

      function destroy() {
         tabs.forEach((tab) => {
            tab.removeEventListener('click', onClick);
            tab.removeEventListener('keydown', onKeydown);
         });
      }

      return { sync: sync, destroy: destroy };
   }

   global.DawnTablist = { bind: bind };
})(window);
