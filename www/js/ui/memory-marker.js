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
 * DAWN Memory: the conversation marker (DawnMemoryMarker)
 *
 * What memory learned from a conversation, shown in it: one collapsed line
 * per batch, after the message the batch read up to, opening to the same op
 * rows (and Undo, Keep, Forget) as the Memory panel's Learned tab.  Placed
 * after the greatest rendered message id at or below the batch's
 * after_message_id; a batch whose messages aren't rendered shows only in the
 * Learned tab.  A batch pushed live may arrive before the reply it follows has
 * its message id, so it waits briefly for that, then goes at the end.
 */
(function () {
   'use strict';

   /* How long a live batch waits for the messages it follows to get their ids. */
   const HOLD_MS = 3000;
   const RETRY_MS = 300;
   /* Batches one page asks for, and the pages one conversation's load follows. */
   const LIST_LIMIT = 20;
   const LIST_PAGES_MAX = 5;

   const held = new Map(); /* batch_id → {batch, until, timer} */
   let pages = 0; /* pages asked for since the conversation loaded */

   function transcript() {
      return typeof DawnElements !== 'undefined' ? DawnElements.transcript : null;
   }

   /* Whether @p id is the conversation shown. */
   function shown(id) {
      const active =
         typeof DawnHistory !== 'undefined' && DawnHistory.getActiveConversationId
            ? DawnHistory.getActiveConversationId()
            : null;
      return id != null && active != null && String(id) === String(active);
   }

   function clearHeld() {
      held.forEach((h) => clearTimeout(h.timer));
      held.clear();
   }

   /* The last element of the greatest rendered message id at or below @p after,
    * and whether a message entry after it has no id yet (a reply not adopted). */
   function anchorFor(t, after) {
      let best = -1;
      let anchor = null;
      let unadopted = false;
      for (const el of t.children) {
         if (el.classList.contains('memory-marker')) continue;
         const raw = el.getAttribute('data-message-id');
         const id = raw == null ? NaN : Number(raw);
         if (Number.isFinite(id) && id <= after && id >= best) {
            best = id;
            anchor = el;
            unadopted = false;
         } else if (anchor && raw == null && el.classList.contains('transcript-entry')) {
            unadopted = true;
         }
      }
      return { anchor: anchor, unadopted: unadopted };
   }

   function build(batch) {
      const marker = document.createElement('details');
      marker.className = 'memory-marker';
      marker.dataset.batchId = String(batch.batch_id);
      const summary = document.createElement('summary');
      summary.className = 'memory-marker-summary';
      const B = window.DawnMemoryBatches;
      const content = B ? B.renderBatch(batch, { where: 'marker' }) : null;
      summary.textContent = 'Memory: ' + (B ? B.summaryText(batch) : 'updated');
      if (batch.outside) {
         /* Said where it shows when closed, too. */
         const text = B ? B.outsideText(batch) : 'Read outside text';
         const b = document.createElement('span');
         b.className = 'dawn-badge warning soft';
         b.textContent = text;
         b.title = 'This conversation included text someone else wrote';
         b.setAttribute('aria-hidden', 'true');
         summary.appendChild(document.createTextNode(' '));
         summary.appendChild(b);
         const sr = document.createElement('span');
         sr.className = 'sr-only';
         sr.textContent = ', ' + text.charAt(0).toLowerCase() + text.slice(1);
         summary.appendChild(sr);
      }
      marker.appendChild(summary);
      if (content) marker.appendChild(content);
      return marker;
   }

   /* After @p anchor, and after any marker of an earlier batch already there. */
   function insertAfter(t, anchor, marker, batchId) {
      let at = anchor;
      while (
         at.nextElementSibling &&
         at.nextElementSibling.classList.contains('memory-marker') &&
         Number(at.nextElementSibling.dataset.batchId) < batchId
      ) {
         at = at.nextElementSibling;
      }
      t.insertBefore(marker, at.nextElementSibling);
   }

   /* Draw @p batch in the conversation shown; a live one waits for its reply's
    * id up to HOLD_MS, then goes at the end.  Whether it was placed. */
   function place(batch, live) {
      const t = transcript();
      if (!t || !shown(batch.conversation_id) || !(batch.after_message_id > 0)) {
         return true; /* nowhere to show it here: the Learned tab has it */
      }
      const found = anchorFor(t, batch.after_message_id);
      const h = held.get(batch.batch_id);
      const waited = h && Date.now() >= h.until;
      if (live && (!found.anchor || found.unadopted) && !waited) {
         return false;
      }
      if (!found.anchor && !live) {
         return true; /* its messages aren't loaded */
      }
      const old = t.querySelector(`.memory-marker[data-batch-id="${batch.batch_id}"]`);
      const marker = build(batch);
      if (old) {
         /* Redrawn in place: open stays open, the keyboard stays near. */
         marker.open = old.open;
         if (window.DawnMemoryBatches) DawnMemoryBatches.replaceKeepingFocus(old, marker);
         else old.replaceWith(marker);
      } else if (found.anchor) {
         insertAfter(t, found.anchor, marker, batch.batch_id);
      } else {
         t.appendChild(marker); /* waited long enough, nothing to follow: the end */
      }
      return true;
   }

   function hold(batch) {
      const prev = held.get(batch.batch_id);
      if (prev) clearTimeout(prev.timer);
      const entry = { batch: batch, until: prev ? prev.until : Date.now() + HOLD_MS, timer: 0 };
      held.set(batch.batch_id, entry);
      const retry = () => {
         if (!shown(entry.batch.conversation_id)) {
            held.delete(entry.batch.batch_id);
            return;
         }
         if (place(entry.batch, true)) {
            held.delete(entry.batch.batch_id);
         } else {
            entry.timer = setTimeout(retry, RETRY_MS);
         }
      };
      entry.timer = setTimeout(retry, RETRY_MS);
   }

   /** A conversation finished loading: its batches are asked for. */
   function ask(id, before) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      const payload = { conversation_id: id, limit: LIST_LIMIT };
      if (before) payload.before_batch_id = before;
      pages++;
      DawnWS.send({ type: 'list_memory_batches', payload: payload });
   }

   function conversationLoaded(id) {
      clearHeld();
      pages = 0;
      if (!window.DawnMemoryBatches || !DawnMemoryBatches.isEnabled() || id == null) return;
      ask(id, null);
   }

   /** list_memory_batches_response for a conversation. */
   function handleList(payload) {
      if (!payload || !shown(payload.conversation_id) || payload.error) return;
      const batches = Array.isArray(payload.batches) ? payload.batches.slice() : [];
      /* Oldest first, so markers sharing a message read in order. */
      batches.sort((a, b) => a.batch_id - b.batch_id);
      batches.forEach((b) => place(b, false));
      /* Older batches of a long conversation: the next pages, up to a few. */
      if (payload.has_more && pages < LIST_PAGES_MAX) {
         ask(payload.conversation_id, payload.next_before_batch_id);
      }
   }

   /** memory_batch: recorded or changed.  Whether it is new to this view. */
   function handleBatch(batch) {
      if (!shown(batch.conversation_id)) return false;
      const t = transcript();
      const known =
         held.has(batch.batch_id) ||
         !!(t && t.querySelector(`.memory-marker[data-batch-id="${batch.batch_id}"]`));
      if (!place(batch, true)) hold(batch);
      return !known;
   }

   window.DawnMemoryMarker = {
      conversationLoaded,
      handleList,
      handleBatch,
   };
})();
