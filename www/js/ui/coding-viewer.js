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
 * A coding job's work, read straight from the agent gateway's mirror: its
 * changed files and commits, and one file at a time as a diff or as it now
 * reads.  Nothing here reaches the model.  It is where the user checks the work
 * before publishing it, so every character is shown as it is: invisible and
 * direction-changing ones appear as visible markers, and code reads left to
 * right whatever it contains.
 */
(function (global) {
   'use strict';

   /* Every control, format (bidi controls, zero-width, tags), separator and
    * default-ignorable code point (variation selectors, Hangul fillers), the
    * spaces that look like a plain one (no-break, wide, thin, braille blank),
    * and a carriage return inside a line: each would make the text read
    * differently from what it is.  Tab is kept, and an emoji's own variation
    * selector. */
   const HIDDEN = new RegExp(
      '[\\p{Cc}\\p{Cf}\\p{Zl}\\p{Zp}\\p{Default_Ignorable_Code_Point}\\u00A0\\u2000-\\u200A' +
         '\\u202F\\u205F\\u2800\\u3000\\r]',
      'gu'
   );
   const PICTO_END = new RegExp('\\p{Extended_Pictographic}$', 'u');
   const HUNK = /^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@/;
   const NARROW = '(max-width: 759px)';

   let dlg = null;
   let ui = {};
   let job = 0;
   let opener = null;
   let openerRoot = null; /* where the opener lives: a rebuilt one is looked for there */
   let escToken = null;
   let cache = new Map(); /* 'path|view' -> the gateway's answer, for this open */
   let nonceSeq = 0;
   let latest = ''; /* only the newest request's answer renders */
   let pending = null; /* the file request in flight: {nonce, path, view} */
   let downOnBackdrop = false;
   let current = null; /* the file shown: {path, added, removed, protected} */
   let view = 'diff';

   function elem(tag, cls, text) {
      const e = document.createElement(tag);
      if (cls) e.className = cls;
      if (text != null) e.textContent = String(text);
      return e;
   }

   /* @p text into @p parent as plain text, hidden characters as markers.
    * True when it had any. */
   function appendShown(parent, text) {
      let last = 0;
      let found = false;
      text.replace(HIDDEN, function (ch, at) {
         if (ch === '\t' || (ch === '\uFE0F' && PICTO_END.test(text.slice(0, at)))) {
            return ch; /* a tab; an emoji's presentation selector */
         }
         if (at > last) parent.appendChild(document.createTextNode(text.slice(last, at)));
         const code = ch.codePointAt(0).toString(16).toUpperCase().padStart(4, '0');
         parent.appendChild(elem('span', 'cv-hidden', '<U+' + code + '>'));
         last = at + ch.length;
         found = true;
         return ch;
      });
      if (last < text.length) parent.appendChild(document.createTextNode(text.slice(last)));
      return found;
   }

   function shownPath(cls, path) {
      const e = elem('span', cls);
      e.setAttribute('dir', 'ltr');
      appendShown(e, path);
      return e;
   }

   function say(text) {
      if (ui.status) ui.status.textContent = text;
   }

   function note(text, extra) {
      const box = elem('div', 'cv-note', text);
      if (extra) box.appendChild(extra);
      ui.pane.replaceChildren(box);
      ui.pane.removeAttribute('aria-busy');
   }

   function send(what, path) {
      latest = ''; /* whatever was asked before is for something else now */
      pending = null;
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         if (what === 'changes') {
            ui.list.replaceChildren(elem('div', 'cv-note', 'Not connected.'));
         } else {
            note('Not connected.');
         }
         return;
      }
      latest = 'v' + ++nonceSeq;
      pending = what === 'file' ? { nonce: latest, path: path, view: view } : null;
      const payload = { job: job, what: what, nonce: latest };
      if (what === 'file') {
         payload.path = path;
         payload.view = view;
      }
      DawnWS.send({ type: 'coding_view', payload: payload });
   }

   /* ---- the dialog ------------------------------------------------------- */

   function build() {
      dlg = elem('dialog', 'dawn-dialog coding-viewer');
      dlg.setAttribute('aria-labelledby', 'cv-title');
      const head = elem('header', 'dawn-dialog-head');
      ui.back = elem('button', 'cv-back', '\u2190 Files');
      ui.back.type = 'button';
      ui.back.setAttribute('aria-label', 'Back to files');
      ui.title = elem('h2', 'dawn-dialog-title');
      ui.title.id = 'cv-title';
      ui.title.setAttribute('dir', 'auto');
      ui.head = elem('span', 'cv-headref');
      const closeBtn = elem('button', 'dawn-dialog-close', '×');
      closeBtn.type = 'button';
      closeBtn.setAttribute('aria-label', 'Close');
      head.append(ui.back, ui.title, ui.head, closeBtn);
      ui.body = elem('div', 'cv-body');
      ui.list = elem('div', 'cv-list');
      const side = elem('section', 'cv-file');
      ui.bar = elem('div', 'cv-bar');
      ui.banner = elem('div', 'cv-banner');
      ui.banner.hidden = true;
      ui.pane = elem('div', 'cv-pane');
      ui.pane.setAttribute('role', 'region');
      ui.pane.setAttribute('aria-label', 'File');
      ui.pane.tabIndex = 0;
      side.append(ui.bar, ui.banner, ui.pane);
      ui.body.append(ui.list, side);
      ui.status = elem('div', 'sr-only');
      ui.status.setAttribute('role', 'status');
      dlg.append(head, ui.body, ui.status);
      document.body.appendChild(dlg);
      closeBtn.addEventListener('click', close);
      ui.back.addEventListener('click', backToFiles);
      dlg.addEventListener('cancel', function (e) {
         e.preventDefault(); /* one way out: it unregisters Escape too */
         dismiss();
      });
      /* The backdrop closes only when pressed and released on it, so a text
       * selection that ends outside doesn't. */
      dlg.addEventListener('pointerdown', function (e) {
         downOnBackdrop = e.target === dlg;
      });
      dlg.addEventListener('click', function (e) {
         if (downOnBackdrop && e.target === dlg) close();
         downOnBackdrop = false;
      });
   }

   function backToFiles() {
      dlg.classList.remove('show-file');
      const sel = ui.list.querySelector('[aria-current="true"]');
      if (sel) sel.focus();
   }

   /* Escape or a phone's back: from a file on a phone, back to the list. */
   function dismiss() {
      if (dlg.classList.contains('show-file') && window.matchMedia(NARROW).matches) {
         backToFiles();
      } else {
         close();
      }
   }

   function open(jobId, name, from) {
      if (!dlg) build();
      if (dlg.open) close();
      job = Number(jobId) || 0;
      opener = from || document.activeElement;
      openerRoot =
         opener && opener.closest ? opener.closest('#jobs-popover, #llm-controls-grid') : null;
      cache = new Map();
      current = null;
      view = 'diff';
      ui.title.replaceChildren();
      appendShown(ui.title, 'Job #' + job + (name ? ': ' + name : ''));
      ui.head.textContent = '';
      dlg.classList.remove('show-file');
      ui.list.replaceChildren(elem('div', 'cv-note', 'Loading…'));
      ui.bar.replaceChildren();
      ui.banner.hidden = true;
      note('Choose a file to see what changed.');
      dlg.showModal();
      if (typeof DawnEscStack !== 'undefined') {
         escToken = DawnEscStack.register(function () {
            dismiss();
            return true;
         });
      }
      send('changes');
   }

   function close() {
      if (!dlg || !dlg.open) return;
      dlg.close();
      if (escToken !== null && typeof DawnEscStack !== 'undefined') {
         DawnEscStack.unregister(escToken);
      }
      escToken = null;
      cache = new Map();
      latest = '';
      pending = null;
      /* The button that opened it, or its row's rebuilt one. */
      const again =
         opener && opener.isConnected
            ? opener
            : (openerRoot || document).querySelector(
                 '[data-action="changes"][data-job-id="' + String(job) + '"]'
              );
      if (again) again.focus();
   }

   /* ---- the listing ------------------------------------------------------ */

   function counts(f) {
      const box = elem('span', 'cv-counts');
      if (f.added == null) {
         box.appendChild(elem('span', 'cv-bin', 'binary'));
         return box;
      }
      box.append(elem('span', 'cv-add', '+' + f.added), elem('span', 'cv-del', '−' + f.removed));
      return box;
   }

   function fileButton(f) {
      const b = elem('button', 'cv-filebtn');
      b.type = 'button';
      const shown = f.path.split(' -> ').pop();
      const slash = shown.lastIndexOf('/');
      const top = elem('span', 'cv-fileline');
      top.append(shownPath('cv-name', slash >= 0 ? shown.slice(slash + 1) : shown), counts(f));
      if (f.protected) top.appendChild(elem('span', 'dawn-badge warning', 'Protected'));
      b.appendChild(top);
      if (slash >= 0) b.appendChild(shownPath('cv-dir', shown.slice(0, slash)));
      if (f.path.indexOf(' -> ') >= 0) {
         b.appendChild(shownPath('cv-dir', 'renamed from ' + f.path.split(' -> ')[0]));
      }
      const added = f.added == null ? 'binary' : f.added + ' added, ' + f.removed + ' removed';
      b.setAttribute('aria-label', f.path + ', ' + added + (f.protected ? ', protected' : ''));
      b.addEventListener('click', function () {
         choose(f, b);
      });
      return b;
   }

   function renderChanges(r) {
      ui.head.textContent = r.head ? String(r.head).slice(0, 12) : '';
      const files = Array.isArray(r.files) ? r.files : [];
      const commits = Array.isArray(r.commits) ? r.commits : [];
      ui.list.replaceChildren();
      if (!files.length) {
         ui.list.appendChild(
            elem('div', 'cv-note', 'No committed changes yet; work is collected when a run stops.')
         );
         return;
      }
      ui.list.appendChild(
         elem('h3', 'cv-section', 'Files (' + (r.files_total || files.length) + ')')
      );
      const ul = elem('ul', 'cv-files');
      files.forEach(function (f) {
         const li = elem('li');
         li.appendChild(fileButton(f));
         ul.appendChild(li);
      });
      ui.list.appendChild(ul);
      if (r.files_total > files.length) {
         ui.list.appendChild(
            elem('div', 'cv-note', r.files_total - files.length + ' more files not listed.')
         );
      }
      ui.list.appendChild(elem('h3', 'cv-section', 'Commits (' + commits.length + ')'));
      const ol = elem('ol', 'cv-commits');
      commits.forEach(function (c) {
         const li = elem('li');
         li.appendChild(elem('span', 'cv-hash', String(c.hash || '').slice(0, 8)));
         const s = elem('span', 'cv-subject');
         s.setAttribute('dir', 'auto');
         appendShown(s, String(c.subject || ''));
         li.appendChild(s);
         ol.appendChild(li);
      });
      ui.list.appendChild(ol);
      ui.taskId = r.task_id || '';
      const first = ui.list.querySelector('.cv-filebtn');
      if (first) first.focus();
   }

   /* ---- one file --------------------------------------------------------- */

   function viewButton(name, label) {
      const b = elem('button', 'cv-viewbtn', label);
      b.type = 'button';
      b.setAttribute('aria-pressed', view === name ? 'true' : 'false');
      b.addEventListener('click', function () {
         if (view === name || !current) return;
         view = name;
         show(current);
      });
      return b;
   }

   function choose(f, btn) {
      ui.list.querySelectorAll('.cv-filebtn').forEach(function (b) {
         b.removeAttribute('aria-current');
      });
      btn.setAttribute('aria-current', 'true');
      view = 'diff';
      current = f;
      show(f);
      if (window.matchMedia(NARROW).matches) {
         dlg.classList.add('show-file');
         ui.back.focus();
      }
   }

   function show(f) {
      const group = elem('div', 'cv-views');
      group.setAttribute('role', 'group');
      group.setAttribute('aria-label', 'View');
      group.append(viewButton('diff', 'Diff'), viewButton('new', 'New'));
      ui.bar.replaceChildren(shownPath('cv-barpath', f.path), counts(f), group);
      ui.pane.setAttribute(
         'aria-label',
         (view === 'diff' ? 'Diff of ' : 'New version of ') + f.path
      );
      ui.banner.hidden = true;
      const key = f.path + '|' + view;
      if (cache.has(key)) {
         latest = ''; /* an answer still on its way is for something else now */
         pending = null;
         renderFile(cache.get(key));
         return;
      }
      ui.pane.setAttribute('aria-busy', 'true');
      ui.pane.replaceChildren(elem('div', 'cv-note', 'Loading…'));
      send('file', f.path);
   }

   function row(cls, o, n, sign, text) {
      const r = elem('div', 'cv-row ' + cls);
      const oc = elem('span', 'cv-o');
      const nc = elem('span', 'cv-n');
      if (o) oc.dataset.o = o;
      if (n) nc.dataset.n = n;
      const code = elem('span', 'cv-c');
      code.setAttribute('dir', 'ltr');
      const hidden = appendShown(code, text);
      r.append(oc, nc, elem('span', 'cv-s', sign), code);
      return { node: r, hidden: hidden };
   }

   /* A new block every BLOCK_ROWS rows (and per hunk), each sized for
    * content-visibility so off-screen rows cost little. */
   const BLOCK_ROWS = 200;

   function renderText(text) {
      const frag = document.createDocumentFragment();
      const lines = text.split('\n');
      if (lines.length && lines[lines.length - 1] === '') lines.pop();
      let block = null;
      let o = 0;
      let n = 0;
      let oLeft = 0; /* lines of the hunk still to come, old and new side */
      let nLeft = 0;
      let hidden = false;
      const diff = view === 'diff';
      function put(r) {
         if (!block || block.childElementCount >= BLOCK_ROWS) {
            block = elem('div', 'cv-hunk');
            frag.appendChild(block);
         }
         block.appendChild(r.node);
         hidden = hidden || r.hidden;
      }
      lines.forEach(function (raw) {
         const line = raw.endsWith('\r') ? raw.slice(0, -1) : raw; /* CRLF isn't hidden */
         if (!diff) {
            put(row('cv-ctx', '', ++n, '', line));
            return;
         }
         const m = HUNK.exec(line);
         if (m) {
            o = Number(m[1]);
            n = Number(m[2]);
            const c = /^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))?/.exec(line);
            oLeft = c[1] === undefined ? 1 : Number(c[1]);
            nLeft = c[2] === undefined ? 1 : Number(c[2]);
            block = null; /* a hunk starts its own block */
            put(row('cv-at', '', '', '', line));
            return;
         }
         const inHunk = oLeft > 0 || nLeft > 0;
         if (inHunk && line[0] === '+') {
            nLeft--;
            put(row('cv-plus', '', n++, '+', line.slice(1)));
         } else if (inHunk && line[0] === '-') {
            oLeft--;
            put(row('cv-minus', o++, '', '−', line.slice(1)));
         } else if (inHunk && (line[0] === ' ' || line === '')) {
            oLeft--;
            nLeft--;
            put(row('cv-ctx', o++, n++, '', line.slice(1)));
         } else {
            put(row('cv-meta', '', '', '', line)); /* headers, "\ No newline", ... */
         }
      });
      frag.querySelectorAll('.cv-hunk').forEach(function (b) {
         b.style.containIntrinsicSize = 'auto ' + (b.childElementCount * 1.45).toFixed(1) + 'em';
      });
      return { frag: frag, hidden: hidden, count: lines.length };
   }

   function renderFile(r) {
      ui.pane.removeAttribute('aria-busy');
      const name = current ? current.path : '';
      if (r.too_large) {
         const how = elem('code', 'cv-how');
         how.textContent = ui.taskId ? 'refs/agent/' + ui.taskId : 'refs/agent/<task id>';
         const extra = elem('div', 'cv-note-sub');
         extra.append(
            document.createTextNode('To look at it locally, fetch '),
            how,
            document.createTextNode(
               " from the gateway's mirror (the agent gateway's README: " +
                  'Looking at the work yourself).'
            )
         );
         note('Too large to show here (' + Math.ceil(r.too_large / 1024) + ' KB).', extra);
         say(name + ': too large to show');
         return;
      }
      if (r.binary) {
         note('Binary file.');
         say(name + ': binary');
         return;
      }
      if (r.missing) {
         note('Not in this version (the job deleted it).');
         say(name + ': not in this version');
         return;
      }
      const text = String(r.text || '');
      if (!text) {
         const msg = view === 'diff' ? 'No changes to this file.' : 'Empty file.';
         note(msg);
         say(name + ': ' + msg);
         return;
      }
      const out = renderText(text);
      ui.pane.replaceChildren(out.frag);
      ui.pane.scrollTop = 0;
      ui.banner.hidden = !out.hidden;
      ui.banner.textContent = out.hidden
         ? 'This file contains hidden or direction-changing characters, shown as <U+…>.'
         : '';
      say(
         name +
            ': ' +
            (view === 'diff' ? 'diff' : 'new version') +
            ', ' +
            out.count +
            ' lines' +
            (out.hidden ? ', has hidden characters' : '')
      );
   }

   /* ---- answers ---------------------------------------------------------- */

   function handleResponse(p) {
      if (!dlg || !dlg.open || !p || p.nonce !== latest || Number(p.job) !== job) {
         return; /* closed, or an older request */
      }
      if (!p.ok) {
         const msg = p.error || 'The coding agent didn’t answer.';
         if (p.what === 'changes') {
            ui.list.replaceChildren(elem('div', 'cv-note', msg));
         } else {
            note(msg);
         }
         say(msg);
         return;
      }
      const r = p.result || {};
      if (p.what === 'changes') {
         renderChanges(r);
         return;
      }
      if (!pending || pending.nonce !== p.nonce) return;
      cache.set(pending.path + '|' + pending.view, r);
      const shown = current && current.path === pending.path && view === pending.view;
      pending = null;
      if (shown) renderFile(r); /* only under the file and view it is for */
   }

   global.DawnCodingViewer = {
      open: open,
      handleResponse: handleResponse,
   };
})(window);
