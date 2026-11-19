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
   const SHORT_HEAD = 12; /* characters of a commit hash shown */
   const SINCE_PUSH = 'Since last push';
   const PUSHED = 'Everything is pushed; showing all changes.';

   let dlg = null;
   let ui = {};
   let job = 0;
   let opener = null;
   let openerRoot = null; /* where the opener lives: a rebuilt one is looked for there */
   let escToken = null;
   let cache = new Map(); /* 'path|view' -> the gateway's answer, for this open */
   let nonceSeq = 0;
   let latest = ''; /* only the newest file request's answer renders */
   let pending = null; /* the file request in flight: {nonce, seq, path, view} */
   let changesNonce = ''; /* the changes request in flight (apart from a file's) */
   let changesSeq = 0;
   let listed = false; /* the open's first listing has come */
   let loadedHead = ''; /* the full head of the work listed */
   let acts = null; /* publish and close (coding-viewer-actions.js) */
   let downOnBackdrop = false;
   let current = null; /* the file shown: {path, added, removed, to_check, why} */
   let view = 'diff';
   let scope = 'all'; /* what the list shows: all, since_push or a commit's hash */
   let baseName = 'the base'; /* the task's base branch, for the notes */
   let loadedScope = ''; /* the scope the listing is for */

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

   /* A file to check before publishing (an older gateway called it protected):
    * flagged, never blocked. */
   function isCheck(f) {
      return Boolean(f && (f.to_check || f.protected));
   }

   function checkTip(f) {
      return 'Look at this before you publish' + (f.why ? ': ' + f.why : '');
   }

   function checkBadge(f) {
      const b = elem('span', 'dawn-badge warning', 'check');
      b.title = checkTip(f);
      return b;
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

   /* A changes request and a file request each have their own nonce: asking
    * for the job's facts again doesn't drop the file being loaded. */
   function send(what, path) {
      const connected = typeof DawnWS !== 'undefined' && DawnWS.isConnected();
      const nonce = 'v' + ++nonceSeq;
      if (what === 'changes') {
         changesNonce = connected ? nonce : '';
         changesSeq = nonceSeq;
      } else {
         latest = connected ? nonce : ''; /* whatever was asked before is for something else now */
         pending = connected
            ? { nonce: nonce, seq: nonceSeq, path: path, view: view, scope: scope }
            : null;
      }
      if (!connected) {
         if (what === 'file') {
            note('Not connected.');
         } else if (!listed) {
            ui.list.replaceChildren(elem('div', 'cv-note', 'Not connected.'));
         }
         ui.list.removeAttribute('aria-busy');
         return;
      }
      const payload = { job: job, what: what, nonce: nonce };
      if (scope !== 'all') payload.scope = scope;
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
      ui.action = elem('section', 'cv-action');
      ui.action.setAttribute('aria-labelledby', 'cv-action-title');
      /* A group, not a <footer>: inside the dialog that would be a second page footer. */
      ui.actions = elem('div', 'cv-actions');
      ui.actions.setAttribute('role', 'group');
      ui.actions.setAttribute('aria-label', 'Job actions');
      ui.actions.hidden = true;
      ui.status = elem('div', 'sr-only');
      ui.status.setAttribute('role', 'status');
      dlg.append(head, ui.body, ui.action, ui.actions, ui.status);
      if (typeof DawnCodingViewerActions !== 'undefined') {
         acts = DawnCodingViewerActions.create({
            dlg: dlg,
            panel: ui.action,
            footer: ui.actions,
            elem: elem,
            appendShown: appendShown,
            shownPath: shownPath,
            isCheck: isCheck,
            checkBadge: checkBadge,
            say: say,
            shortHead: SHORT_HEAD,
            job: function () {
               return job;
            },
            head: function () {
               return loadedHead;
            },
            isOpen: function () {
               return !!dlg && dlg.open;
            },
            refresh: function () {
               send('changes');
            },
            home: home,
            close: close,
         });
      }
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
      const sel = ui.list.querySelector('.cv-filebtn[aria-current="true"]');
      if (sel) sel.focus();
   }

   /* Where focus goes when the footer has nothing to take it. */
   function home() {
      const sel =
         ui.list.querySelector('.cv-filebtn[aria-current="true"]') ||
         ui.list.querySelector('.cv-filebtn') ||
         ui.list.querySelector('button');
      /* An empty pane says nothing: the dialog's close button then. */
      const empty = !ui.pane.childNodes.length;
      (sel || (empty ? dlg.querySelector('.dawn-dialog-close') : ui.pane)).focus();
   }

   /* Escape or a phone's back: the publish or close panel first, then from a
    * file on a phone back to the list, then the dialog. */
   function dismiss() {
      if (acts && acts.dismiss()) return;
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
      scope = 'all';
      loadedScope = '';
      latest = '';
      pending = null;
      listed = false;
      loadedHead = '';
      if (acts) acts.reset();
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
      changesNonce = '';
      if (acts) acts.reset(); /* an answer still on its way becomes a toast */
      /* The button that opened it, or its row's rebuilt one. */
      const again =
         opener && opener.isConnected
            ? opener
            : (openerRoot || document).querySelector(
                 '[data-action="changes"][data-job-id="' + String(job) + '"]'
              );
      if (again) {
         again.focus();
      } else {
         /* Its row went with the close: the jobs button, always there signed in. */
         const btn = document.getElementById('jobs-btn');
         if (btn) btn.focus();
      }
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
      if (isCheck(f)) top.appendChild(checkBadge(f));
      b.appendChild(top);
      if (slash >= 0) b.appendChild(shownPath('cv-dir', shown.slice(0, slash)));
      if (f.path.indexOf(' -> ') >= 0) {
         b.appendChild(shownPath('cv-dir', 'renamed from ' + f.path.split(' -> ')[0]));
      }
      if (f.includes_main)
         b.appendChild(elem('span', 'cv-dir', 'includes ' + baseName + '’s changes'));
      if (isCheck(f) && f.why) b.appendChild(elem('span', 'cv-dir', 'Check: ' + f.why));
      const added = f.added == null ? 'binary' : f.added + ' added, ' + f.removed + ' removed';
      b.setAttribute(
         'aria-label',
         f.path +
            ', ' +
            added +
            (isCheck(f) ? ', ' + checkTip(f) : '') +
            (f.includes_main ? ', includes ' + baseName + '’s changes' : '')
      );
      b.addEventListener('click', function () {
         choose(f, b);
      });
      return b;
   }

   /* @p first: the open's own listing (focus goes to it); later ones only
    * update the job's facts, and the listing when the work changed. */
   /* Forget the file shown and every file read: they may read differently now. */
   function dropFiles() {
      cache = new Map();
      current = null;
      latest = '';
      pending = null;
      dlg.classList.remove('show-file');
      ui.bar.replaceChildren();
      ui.banner.hidden = true;
   }

   function renderChanges(r, first) {
      const head = r.head ? String(r.head) : '';
      ui.list.removeAttribute('aria-busy');
      if (acts) acts.render(r.job_state);
      if (r.job_state && r.job_state.closed) {
         /* Its clone and branch are gone: nothing left to list (the footer says Closed). */
         listed = true;
         loadedHead = '';
         dropFiles();
         ui.head.textContent = '';
         ui.list.replaceChildren(elem('div', 'cv-note', 'Closed: its clone and branch are gone.'));
         ui.pane.replaceChildren();
         ui.pane.removeAttribute('aria-busy');
         return;
      }
      const applied = String(r.scope || 'all');
      baseName = r.base_name || 'the base';
      const files = Array.isArray(r.files) ? r.files : [];
      const commits = Array.isArray(r.commits) ? r.commits : [];
      /* Since last push with nothing since the push: the gateway showed all. */
      const pushed = !!r.nothing_since_push && scope === 'since_push';
      if (pushed) {
         scope = 'all';
         loadedScope = '';
      }
      if (!first && head === loadedHead && applied === loadedScope) {
         /* A publish since: Since last push comes or goes, the files stay. */
         const had = !!ui.list.querySelector('.cv-scopebtn[data-scope="since_push"]');
         const old = ui.list.querySelector('.cv-scopes');
         if (old && had !== !!r.since_push_available) old.replaceWith(scopeList(r, commits));
         return;
      }
      const moved = !first && head !== loadedHead;
      const rescoped = !first && !moved; /* only the scope changed: focus stays on it */
      listed = true;
      loadedHead = head;
      loadedScope = applied;
      if (!first) dropFiles();
      if (moved) {
         note('The work changed since you opened it.');
         say('The work changed since you opened it.');
      } else if (pushed) {
         note(PUSHED);
         say(PUSHED);
      } else if (rescoped) {
         note('Choose a file to see what changed.');
      }
      ui.head.textContent = head.slice(0, SHORT_HEAD);
      ui.list.replaceChildren();
      /* No commits at all: nothing to choose.  Commits with no net change (one
       * undoing another) still get the scopes, to look at each. */
      if (!files.length && applied === 'all' && !commits.length) {
         ui.list.appendChild(
            elem('div', 'cv-note', 'No committed changes yet; work is collected when a run stops.')
         );
         return;
      }
      ui.list.append(elem('h3', 'cv-section', 'Show'), scopeList(r, commits));
      /* The files, under the scope they are for (the scope button's label). */
      const sec = elem('div', 'cv-filesec');
      const title = elem('h3', 'cv-section', 'Files (' + (r.files_total || files.length) + ')');
      if (applied === 'since_push') {
         title.append(' · ' + SINCE_PUSH.toLowerCase());
      } else if (applied !== 'all') {
         title.append(' · commit ', elem('span', 'cv-hash', applied.slice(0, 8)));
      }
      sec.appendChild(title);
      if (!files.length) sec.appendChild(elem('div', 'cv-note', 'No files in this view.'));
      if (r.scope_merge) {
         sec.appendChild(
            elem(
               'div',
               'cv-note',
               r.merge_recorded
                  ? 'A merge: only files with conflicts are shown; the rest came from ' +
                       baseName +
                       '.'
                  : 'A merge: only files the merge changed beyond both sides are shown.'
            )
         );
      }
      const ul = elem('ul', 'cv-files');
      files.forEach(function (f) {
         const li = elem('li');
         li.appendChild(fileButton(f));
         ul.appendChild(li);
      });
      sec.appendChild(ul);
      if (r.files_total > files.length) {
         sec.appendChild(
            elem('div', 'cv-note', r.files_total - files.length + ' more files not listed.')
         );
      }
      ui.list.appendChild(sec);
      ui.taskId = r.task_id || '';
      const btn = ui.list.querySelector('.cv-filebtn');
      if (btn && first) btn.focus();
      const sel = ui.list.querySelector('.cv-scopebtn[aria-current="true"]');
      if (sel && rescoped) {
         sel.focus();
         if (!pushed) say(sel.getAttribute('aria-label') + ': ' + files.length + ' files');
      }
   }

   /* ---- the scope: the whole job, since the last push, or one commit ------ */

   function scopeButton(name, label, sub) {
      const b = elem('button', 'cv-scopebtn');
      b.type = 'button';
      if (sub) {
         b.append(elem('span', 'cv-hash', name.slice(0, 8)), sub);
      } else {
         b.textContent = label;
      }
      b.setAttribute('aria-label', label);
      b.dataset.scope = name;
      if (scope === name) b.setAttribute('aria-current', 'true');
      b.addEventListener('click', function () {
         if (scope === name) return;
         scope = name;
         ui.list.querySelectorAll('.cv-scopebtn').forEach(function (x) {
            x.removeAttribute('aria-current');
         });
         b.setAttribute('aria-current', 'true');
         dropFiles();
         loadedScope = ''; /* the listing is gone: the next answer redraws it */
         note('Loading…');
         /* No stale files under the new scope while it loads. */
         const sec = ui.list.querySelector('.cv-filesec');
         if (sec) sec.replaceChildren(elem('div', 'cv-note', 'Loading…'));
         ui.list.setAttribute('aria-busy', 'true');
         send('changes');
      });
      return b;
   }

   /* View only: Publish and Close act on the whole job whatever is shown. */
   function scopeList(r, commits) {
      const ol = elem('ol', 'cv-scopes');
      ol.setAttribute('aria-label', 'Show');
      const add = function (list, b) {
         const li = elem('li');
         li.appendChild(b);
         list.appendChild(li);
      };
      add(ol, scopeButton('all', 'All changes'));
      if (r.since_push_available) add(ol, scopeButton('since_push', SINCE_PUSH));
      if (!commits.length) return ol;
      const label = commits.length + (commits.length === 1 ? ' commit' : ' commits');
      const sub = elem('ol', 'cv-commits');
      sub.setAttribute('aria-label', label);
      const group = elem('li');
      group.append(elem('div', 'cv-group', label), sub);
      ol.appendChild(group);
      /* Since the last push: the commits it covers carry the selected look, as
       * the one commit does when it is the view. */
      const covered =
         r.scope === 'since_push' && Array.isArray(r.scope_commits)
            ? new Set(r.scope_commits.map(String))
            : null;
      commits.forEach(function (c) {
         const hash = String(c.hash || '');
         const inView = covered !== null && covered.has(hash);
         const s = elem('span', 'cv-subject');
         s.setAttribute('dir', 'auto');
         appendShown(s, String(c.subject || ''));
         if (c.merge) s.appendChild(elem('span', 'cv-dir', 'merge of ' + baseName));
         const merge = c.merge ? ', merge of ' + baseName : '';
         const b = scopeButton(
            hash,
            'Commit ' +
               hash.slice(0, 8) +
               ': ' +
               c.subject +
               merge +
               (inView ? ', in this view' : ''),
            s
         );
         if (inView) b.classList.add('cv-inview');
         add(sub, b);
      });
      return ol;
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
      if (isCheck(f)) group.before(checkBadge(f));
      ui.pane.setAttribute(
         'aria-label',
         (view === 'diff' ? 'Diff of ' : 'New version of ') + f.path
      );
      ui.banner.hidden = true;
      const key = scope + '|' + f.path + '|' + view;
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

   /* The gateway serves one request of a user's at a time, and a newer one
    * replaces one still waiting, so when one of this tab's requests is
    * answered, an earlier one of the other kind still unanswered was replaced:
    * it is asked again. */
   function handleChanges(p) {
      changesNonce = '';
      const first = !listed;
      if (!p.ok && scope !== 'all') {
         /* That scope is gone (the work moved): back to all of it. */
         scope = 'all';
         loadedScope = ''; /* relisted, marked as all */
         send('changes');
         const msg = p.error || 'The coding agent didn’t answer.';
         note(msg);
         say(msg);
         return;
      }
      if (!p.ok) {
         const msg = p.error || 'The coding agent didn’t answer.';
         ui.list.removeAttribute('aria-busy');
         if (first) ui.list.replaceChildren(elem('div', 'cv-note', msg));
         say(msg);
      } else {
         renderChanges(p.result || {}, first);
      }
      if (pending && pending.seq < changesSeq && current) show(current);
   }

   function handleResponse(p) {
      if (!dlg || !dlg.open || !p || Number(p.job) !== job) return; /* closed */
      if (p.what === 'changes') {
         if (p.nonce === changesNonce) handleChanges(p);
         return;
      }
      if (p.nonce !== latest || !pending || pending.nonce !== p.nonce) return; /* an older one */
      const asked = pending;
      pending = null;
      if (changesNonce && changesSeq < asked.seq) send('changes');
      if (!p.ok) {
         const msg = p.error || 'The coding agent didn’t answer.';
         note(msg);
         say(msg);
         return;
      }
      const r = p.result || {};
      cache.set(asked.scope + '|' + asked.path + '|' + asked.view, r);
      if (current && current.path === asked.path && view === asked.view && scope === asked.scope) {
         renderFile(r); /* only under the file and view it is for */
      }
   }

   /* Publish and close answers, to this tab only. */
   function handleActionResponse(type, p) {
      if (acts) acts.handle(type, p);
   }

   /* The socket came back: whatever was waiting may never be answered. */
   function reconnected() {
      if (!dlg || !dlg.open) return;
      if (acts) acts.reconnected();
      send('changes');
      if (pending && current) show(current);
   }

   /* A job_update row: the open job's status changed, so ask for its facts. */
   function onJobUpdate(row) {
      if (!dlg || !dlg.open || !row || Number(row.conversation_id) !== job) return;
      if (!acts || row.status !== acts.status()) send('changes');
   }

   global.DawnCodingViewer = {
      open: open,
      handleResponse: handleResponse,
      handleActionResponse: handleActionResponse,
      reconnected: reconnected,
      onJobUpdate: onJobUpdate,
   };
})(window);
