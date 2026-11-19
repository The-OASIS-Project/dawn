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
 * Publish and close for the changes viewer (coding-viewer.js): the footer's
 * state line and buttons, and the panel that shows DAWN's own preview before a
 * publish, or what a close would lose.  The user approves what DAWN shows here
 * with a click; no model reads it back in between.  Whether the job can be
 * published or closed comes only from the server's facts, never guessed, and
 * the server checks every click again.  The agent's words (title, description,
 * paths) are shown as text with hidden characters marked.
 */
(function (global) {
   'use strict';

   const WAIT_MS = 30000; /* no answer by then: stop waiting, ask for the job again */
   const SENT_MAX = 16; /* action answers remembered for a toast after the panel closed */
   const TOAST_MS = 5000;
   const TOAST_LINK_MS = 10000; /* a toast with a pull request link stays longer */
   /* The user's own pull request text: CODING_PR_TITLE_MAX / CODING_PR_DESC_MAX in
    * include/tools/coding_agent_card.h (bytes; the gateway checks its own limits too). */
   const PR_TITLE_MAX = 120;
   const PR_DESC_MAX = 5900;
   const CLIPPED = ' [...]'; /* the gateway's mark on a description it cut to its limit */

   function plural(n, word) {
      return n + ' ' + word + (n === 1 ? '' : 's');
   }

   /* Only an https link is made into one. */
   function safeUrl(u) {
      try {
         const x = new URL(String(u || ''));
         return x.protocol === 'https:' ? x.href : '';
      } catch (e) {
         return '';
      }
   }

   function prNumber(url) {
      const m = /\/pull\/(\d+)/.exec(url);
      return m ? '#' + m[1] : '';
   }

   function count(x) {
      return Math.max(0, Number(x) || 0);
   }

   /* @p h, from the viewer: dlg, panel, footer, elem, appendShown, shownPath, close,
    * say, shortHead, job(), head() (the full head it loaded), isOpen(), refresh(), home(). */
   function create(h) {
      const elem = h.elem;
      const SHORT_HEAD = h.shortHead; /* characters of a commit hash shown (the viewer's) */
      let state = null; /* the job's facts, from the last changes answer */
      let mode = ''; /* '', 'confirm' (the footer's close question), 'panel' */
      let panelFrom = ''; /* the footer button the panel was opened from */
      let panelBtns = null; /* builds the panel's footer buttons */
      let busy = null; /* {act, label}: the button whose request runs */
      let wantNonce = ''; /* the answer the visible panel or question waits for */
      let req = null; /* the request in flight: {nonce, timer} */
      let preview = null; /* the preview shown: {head, sha, workflows, title, desc, edit} */
      let lose = null; /* what a close would lose, as shown: {head} */
      let panelDone = null; /* what Done (and Escape) does on a result panel; null: back */
      let seq = 0;
      const kinds = new Map(); /* nonce -> 'preview' | 'publish' | 'close' | 'close-confirm' */
      /* job -> {title, desc}: what the user typed, kept until it is published, so a
       * refusal, Escape or a closed viewer loses nothing. */
      const drafts = new Map();

      /* ---- pieces ------------------------------------------------------ */

      function shownText(text) {
         const s = elem('span');
         h.appendShown(s, String(text || ''));
         return s.textContent;
      }

      function prLink(url, label) {
         const a = elem('a', null, label);
         a.href = url;
         a.target = '_blank';
         a.rel = 'noopener noreferrer';
         return a;
      }

      function para(text, tone) {
         const p = elem('p', null, text);
         if (tone) p.dataset.tone = tone;
         return p;
      }

      function heading(text) {
         const t = elem('h3', null, text);
         t.id = 'cv-action-title';
         t.tabIndex = -1;
         return t;
      }

      function button(label, variant, act, onClick) {
         const b = elem('button', 'coding-card-btn ' + variant, label);
         b.type = 'button';
         b.dataset.act = act;
         const calm = act === 'cancel' || act === 'keep' || act === 'done';
         if (busy && busy.act === act) {
            b.textContent = busy.label;
            b.setAttribute('aria-disabled', 'true');
         } else if (busy && !calm) {
            b.setAttribute('aria-disabled', 'true');
         }
         b.addEventListener('click', function () {
            if (b.getAttribute('aria-disabled') === 'true') return;
            onClick();
         });
         return b;
      }

      function cancelBtn() {
         return button('Cancel', 'secondary', 'cancel', function () {
            closePanel(true);
         });
      }

      /* ---- requests ---------------------------------------------------- */

      function request(type, extra, kind) {
         if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
            /* The toast is a live region itself: said once, by it when there is one. */
            if (typeof DawnToast !== 'undefined') DawnToast.show('Not connected.', 'error');
            else h.say('Not connected.');
            return '';
         }
         if (req) clearTimeout(req.timer);
         const nonce = 'a' + ++seq;
         DawnWS.send({ type: type, payload: Object.assign({ job: h.job(), nonce: nonce }, extra) });
         req = { nonce: nonce, timer: setTimeout(timedOut, WAIT_MS) };
         kinds.set(nonce, kind);
         if (kinds.size > SENT_MAX) kinds.delete(kinds.keys().next().value);
         wantNonce = nonce;
         return nonce;
      }

      function idle() {
         if (req) clearTimeout(req.timer);
         req = null;
         busy = null;
      }

      /* The answer the panel waits for may never come: say so there.  False
       * when the viewer is closed. */
      function stopWaiting() {
         const waited = mode === 'panel' && wantNonce !== '' && !!req && req.nonce === wantNonce;
         idle();
         if (preview && preview.edit) {
            preview.edit.title.readOnly = false; /* nothing went: they can change it again */
            preview.edit.desc.readOnly = false;
         }
         if (!h.isOpen()) return false;
         if (waited && !h.panel.querySelector('#cv-no-answer')) {
            const p = para('No answer from the coding agent yet.', 'warning');
            p.id = 'cv-no-answer';
            h.panel.appendChild(p);
         }
         drawFooter();
         return true;
      }

      function timedOut() {
         if (!stopWaiting()) return;
         h.say('No answer from the coding agent yet; checking the job again.');
         h.refresh();
      }

      /* ---- the footer --------------------------------------------------- */

      function setFooter(line, buttons) {
         const a = document.activeElement;
         const had = h.footer.contains(a) && a.dataset ? a.dataset.act : '';
         const box = elem('div');
         buttons.forEach(function (b) {
            box.appendChild(b);
         });
         h.footer.replaceChildren();
         if (line) h.footer.appendChild(line);
         h.footer.appendChild(box);
         h.footer.hidden = false;
         const again = had ? box.querySelector('[data-act="' + had + '"]') : null;
         if (again) again.focus();
      }

      function statusWord(s) {
         const w = String(s || 'unknown');
         return w.charAt(0).toUpperCase() + w.slice(1);
      }

      /* The job's state line and buttons; returns the line. */
      function drawJob() {
         const s = state;
         if (!s) {
            h.footer.hidden = true;
            h.footer.replaceChildren();
            return null;
         }
         const line = elem('p');
         const btns = [];
         const n = Number(s.unpublished_commits);
         const url = safeUrl(s.pr_url);
         let closable = true;
         if (s.closed) {
            line.textContent = 'Closed';
            closable = false;
         } else if (s.status === 'running' || s.status === 'queued') {
            line.textContent =
               (s.status === 'queued' ? 'Queued' : 'Running') + ': publish or close when it stops';
            closable = false;
         } else if (s.status !== 'done') {
            line.textContent = statusWord(s.status);
         } else if (s.published && n > 0) {
            line.textContent = plural(n, 'new commit') + ' since publishing';
            btns.push(
               button('Push ' + plural(n, 'new commit') + '…', 'primary', 'publish', startPublish)
            );
         } else if (s.published) {
            line.textContent = 'Published';
            if (url) line.append(' · ', prLink(url, ('PR ' + prNumber(url)).trim() + ' ↗'));
         } else if (n > 0) {
            line.textContent = 'Not published';
            btns.push(button('Publish…', 'primary', 'publish', startPublish));
         } else {
            line.textContent = n === 0 ? 'Nothing to publish' : 'Couldn’t tell what is unpublished';
         }
         if (closable) btns.push(button('Close job', 'danger', 'close', askClose));
         setFooter(line, btns);
         return line;
      }

      function drawConfirm() {
         const line = elem('p', null, 'Close job #' + h.job() + '? ');
         line.append('Its clone and branch go; it can’t be revised afterwards.');
         const yes = button('Close', 'danger', 'close-yes', sendClose);
         const closing = busy && busy.act === 'close-yes';
         yes.setAttribute(
            'aria-label',
            (closing ? 'Closing' : 'Close') + ' job #' + h.job() + (closing ? '…' : '')
         );
         setFooter(line, [yes, button('Keep', 'secondary', 'keep', leaveConfirm)]);
      }

      /* Out of the close question, back to the job's buttons; focus follows
       * only when it was in the footer.  Returns the state line drawn. */
      function leaveConfirm() {
         const inside = h.footer.contains(document.activeElement);
         mode = '';
         wantNonce = '';
         busy = null;
         const line = drawFooter();
         if (inside) focusFooter('close');
         return line;
      }

      /* Returns the job's state line when that is what it drew. */
      function drawFooter() {
         if (mode === 'panel' && panelBtns) {
            setFooter(null, panelBtns());
         } else if (mode === 'confirm') {
            drawConfirm();
         } else {
            return drawJob();
         }
         return null;
      }

      function focusFooter(act) {
         const b =
            h.footer.querySelector('[data-act="' + act + '"]') || h.footer.querySelector('button');
         if (b) b.focus();
         else h.home();
      }

      /* ---- the panel ---------------------------------------------------- */

      /* @p tone: 'publish', 'lose' or '' (neutral). */
      function showPanel(tone, nodes, btns) {
         panelDone = null; /* a result sets its own after this */
         if (mode !== 'panel') panelFrom = mode === 'confirm' ? 'close' : 'publish';
         mode = 'panel';
         h.dlg.classList.add('show-action');
         h.panel.className = 'cv-action' + (tone ? ' ' + tone : '');
         h.panel.replaceChildren.apply(h.panel, nodes);
         h.panel.scrollTop = 0;
         panelBtns = btns;
         drawFooter();
         const t = h.panel.querySelector('h3');
         if (t) t.focus(); /* the heading, never a button that acts */
      }

      function closePanel(refocus) {
         const from = panelFrom;
         panelDone = null;
         mode = '';
         panelFrom = '';
         panelBtns = null;
         wantNonce = '';
         busy = null;
         preview = null;
         lose = null;
         h.dlg.classList.remove('show-action');
         h.panel.className = 'cv-action';
         h.panel.replaceChildren();
         drawFooter();
         if (refocus) focusFooter(from);
      }

      /* Paths, each with its reason in words, and its check badge (checkBadge). */
      function list(paths, checkBadge) {
         const ul = elem('ul');
         paths.forEach(function (item) {
            const li = elem('li');
            const path = typeof item === 'string' ? item : String((item && item.path) || '');
            li.appendChild(h.shownPath('cv-name', path));
            if (checkBadge && h.isCheck(item)) li.append(' ', h.checkBadge(item));
            if (item && item.why)
               li.append(' ', elem('span', 'coding-muted', '(' + item.why + ')'));
            ul.appendChild(li);
         });
         return ul;
      }

      function fact(dl, name, value) {
         dl.append(elem('dt', null, name));
         const dd = elem('dd');
         dd.append.apply(dd, Array.isArray(value) ? value : [value]);
         dl.appendChild(dd);
      }

      /* ---- publish ------------------------------------------------------ */

      function startPublish() {
         preview = null;
         const nonce = request('coding_publish_preview', {}, 'preview');
         showPanel(
            'publish',
            [heading('Publish to GitHub'), para(nonce ? 'Getting the preview…' : 'Not connected.')],
            function () {
               return [cancelBtn()];
            }
         );
      }

      function renderPreview(v) {
         const head = String(v.head || '');
         const footer = String(v.pr_footer || '');
         const body = String(v.pr_body || '');
         preview = {
            head: head,
            sha: String(v.pr_body_sha256 || ''),
            workflows: !!v.workflows,
            title: String(v.pr_title || ''),
            /* What the user edits: the description without the footer the gateway adds. */
            /* (Cut to the gateway's limit, it can't be edited whole: the user writes their own.) */
            desc:
               footer && body.endsWith(footer) && !body.endsWith(CLIPPED + footer)
                  ? body.slice(0, -footer.length)
                  : '',
            footer: footer,
            edit: null /* {title, desc}: the fields, once the user chose to edit */,
         };
         const nodes = [heading('Publish to GitHub')];
         if (head && h.head() && head !== h.head()) {
            nodes.push(para('The work changed since you opened it.', 'warning'));
            h.refresh();
         }
         const url = safeUrl(v.pr_url);
         nodes.push(
            para(
               url
                  ? 'DAWN pushes the job’s new commits to its branch on GitHub.'
                  : 'DAWN pushes the job’s branch to GitHub and opens a pull request for it.'
            )
         );
         if (preview.workflows) {
            nodes.push(
               para(
                  'This work changes CI workflows, which the gateway’s token can’t push; ' +
                     'publish it yourself.',
                  'warning'
               )
            );
         }
         const dl = elem('dl');
         fact(dl, 'Repository', h.shownPath('cv-name', String(v.repo || '')));
         fact(dl, 'Branch', [
            h.shownPath('cv-name', String(v.branch || '')),
            ' → ',
            h.shownPath('cv-name', String(v.base || '')),
         ]);
         fact(dl, 'Head', elem('span', 'cv-headref', head.slice(0, SHORT_HEAD)));
         const commits = count(v.commits);
         fact(dl, 'Commits', commits + (v.commits_more ? '+' : ''));
         const files = count(v.files);
         const total = Math.max(files, Number(v.files_total) || 0);
         fact(dl, 'Files', files + (total > files ? ' (' + (total - files) + ' more)' : ''));
         nodes.push(dl);

         const check = Array.isArray(v.to_check) ? v.to_check : [];
         if (check.length) {
            const box = elem('section');
            box.dataset.tone = 'warning';
            box.append(elem('h4', null, 'Files to check before publishing'), list(check, false));
            nodes.push(box);
         }
         const hist = Array.isArray(v.history_only) ? v.history_only : [];
         if (hist.length) {
            const box = elem('section');
            box.append(
               elem('h4', null, 'History-only paths'),
               para(
                  'Changed in commits but not in the final diff: these are public in the ' +
                     'branch’s history too.'
               ),
               list(hist, true)
            );
            nodes.push(box);
         }

         const pr = elem('section');
         pr.appendChild(elem('h4', null, 'Pull request'));
         if (url) {
            const p = para('The new commits join ');
            p.append(
               prLink(url, ('pull request ' + prNumber(url)).trim() + ' ↗'),
               ' (a new one if it was merged or closed).'
            );
            pr.appendChild(p);
         }
         const titleFacts = elem('dl');
         const title = elem('span');
         title.setAttribute('dir', 'auto');
         const hiddenTitle = h.appendShown(title, String(v.pr_title || ''));
         fact(titleFacts, v.pr_title_by_agent ? 'Title (the agent’s)' : 'Title', title);
         const label = para(
            url
               ? 'Used only if a new pull request has to be opened (the current one was merged ' +
                    'or closed). Written by the agent; it would be public on GitHub.'
               : 'Description, written by the agent. It will be public on GitHub.'
         );
         label.id = 'cv-action-desc';
         const desc = elem('div', 'coding-quote');
         desc.setAttribute('dir', 'auto');
         desc.setAttribute('role', 'region');
         desc.setAttribute('aria-labelledby', 'cv-action-desc');
         desc.tabIndex = 0;
         const hiddenBody = h.appendShown(desc, String(v.pr_body || ''));
         pr.append(titleFacts, label);
         if (hiddenTitle || hiddenBody) {
            pr.appendChild(
               elem(
                  'div',
                  'cv-banner',
                  'The title or description contains hidden or direction-changing ' +
                     'characters, shown as <U+…>.'
               )
            );
         }
         pr.appendChild(desc);
         const refs = elem('div');
         /* The user's own text: for a new pull request only (an open one is edited on
          * GitHub), and with a gateway that says what it adds. */
         let editBtn = null;
         if (!url && preview.footer) {
            editBtn = button('Edit title and description', 'secondary', 'edit-pr', function () {
               startEdit(titleFacts, label, desc, refs, editBtn, true);
            });
            pr.appendChild(editBtn);
         }
         pr.appendChild(refs);
         const mentions = Array.isArray(v.mentions) ? v.mentions.map(String) : [];
         if (mentions.length) {
            const p = para('It mentions ');
            h.appendShown(p, mentions.join(', '));
            p.append(
               url
                  ? ': GitHub notifies them if a new pull request opens.'
                  : ': GitHub notifies them when the pull request opens.'
            );
            refs.appendChild(p);
         }
         const closes = Array.isArray(v.closes) ? v.closes.map(String) : [];
         if (closes.length) {
            const p = para('Merging it closes ');
            h.appendShown(p, closes.join(', '));
            p.append(' (closing keywords in the description).');
            refs.appendChild(p);
         }
         nodes.push(pr);
         if (editBtn && drafts.has(h.job())) {
            startEdit(titleFacts, label, desc, refs, editBtn, false);
         }
         showPanel('publish', nodes, function () {
            if (preview && preview.workflows) {
               return [
                  button('Done', 'primary', 'done', function () {
                     closePanel(true);
                  }),
               ];
            }
            return [button('Publish', 'primary', 'do-publish', doPublish), cancelBtn()];
         });
         h.say(
            'Publish preview: ' +
               plural(commits, 'commit') +
               ', ' +
               plural(total, 'file') +
               (check.length
                  ? ', ' + plural(check.length, 'file') + ' to check before publishing'
                  : '') +
               (preview.workflows ? '; changes CI workflows, can’t be published here' : '')
         );
      }

      /* The title and description as fields: the user's draft, else the agent's
       * text.  @p focus: moved to the title (not when a draft comes back with the
       * panel, whose heading takes it). */
      function startEdit(titleFacts, label, desc, refs, editBtn, focus) {
         if (!preview || preview.edit) return;
         /* A draft holds only the fields the user changed: the others are the agent's
          * text as it is now (a follow-up may have rewritten it). */
         const draft = drafts.get(h.job()) || {};
         const startTitle = 'title' in draft ? draft.title : preview.title;
         const startDesc = 'desc' in draft ? draft.desc : preview.desc;
         const title = elem('input', 'dawn-input');
         title.type = 'text';
         title.maxLength = PR_TITLE_MAX;
         title.value = startTitle;
         title.id = 'cv-pr-title';
         title.setAttribute('dir', 'auto');
         const titleLabel = elem('label', 'cv-pr-label', 'Title');
         titleLabel.htmlFor = 'cv-pr-title';
         const box = elem('textarea', 'dawn-input coding-answer cv-pr-desc');
         box.id = 'cv-pr-desc';
         box.rows = 12;
         box.value = startDesc;
         box.setAttribute('dir', 'auto');
         box.setAttribute('aria-describedby', 'cv-pr-footer-note');
         const descLabel = elem(
            'label',
            'cv-pr-label',
            'Description. It will be public on GitHub.'
         );
         descLabel.htmlFor = 'cv-pr-desc';
         const note = para('DAWN’s gateway adds this line at the end: ');
         note.id = 'cv-pr-footer-note';
         note.appendChild(h.shownPath('cv-name', preview.footer.trim()));
         const error = para('', 'warning');
         error.id = 'cv-pr-error';
         error.hidden = true;
         titleFacts.replaceWith(titleLabel, title);
         label.replaceWith(descLabel);
         desc.replaceWith(box, note, error);
         const extra = [];
         if (!preview.desc) {
            extra.push(
               para(
                  'The agent’s description is too long to edit here: write your own, or leave ' +
                     'this empty to publish the agent’s.'
               )
            );
         }
         /* Worked out for the agent's text; the user's own is theirs to read. */
         extra.push(para('@mentions and closing keywords in your text work as they do on GitHub.'));
         refs.replaceChildren.apply(refs, extra);
         editBtn.remove();
         /* Only what the user changed is a draft (looking isn't editing). */
         const keep = function () {
            const d = {};
            if (title.value !== preview.title) d.title = title.value;
            if (box.value !== preview.desc) d.desc = box.value;
            if (Object.keys(d).length) drafts.set(h.job(), d);
            else drafts.delete(h.job());
         };
         title.addEventListener('input', keep);
         box.addEventListener('input', keep);
         keep();
         preview.edit = { title: title, desc: box, error: error };
         if (focus) {
            title.focus();
            h.say(
               'Editing the title and description; what you type is kept until it is published.'
            );
         }
      }

      /* Why the user's own text would be refused (as the server checks it), or ''. */
      function fieldProblem(value, max, what) {
         if (value.trim() === '') return 'The ' + what + ' is empty.';
         if (new TextEncoder().encode(value).length > max) {
            return 'The ' + what + ' is over ' + max + ' bytes.';
         }
         return '';
      }

      /* @p msg under the fields ('' clears it), on @p field. */
      function setFieldError(field, msg) {
         const e = preview.edit;
         e.error.textContent = msg;
         e.error.hidden = !msg;
         [e.title, e.desc].forEach(function (f) {
            f.removeAttribute('aria-invalid');
            f.removeAttribute('aria-errormessage');
         });
         if (!field) return;
         field.setAttribute('aria-invalid', 'true');
         field.setAttribute('aria-errormessage', 'cv-pr-error');
         field.focus();
         h.say(msg);
      }

      function doPublish() {
         if (!preview || preview.workflows) return;
         const args = { head: preview.head, pr_body_sha256: preview.sha };
         const e = preview.edit;
         /* Sent when it differs from the agent's text as the gateway has it. */
         if (e && e.title.value !== preview.title) {
            const bad = fieldProblem(e.title.value, PR_TITLE_MAX, 'title');
            if (bad) return setFieldError(e.title, bad);
            args.pr_title = e.title.value;
         }
         if (e && e.desc.value !== preview.desc) {
            const bad = fieldProblem(e.desc.value, PR_DESC_MAX, 'description');
            if (bad) return setFieldError(e.desc, bad);
            args.pr_description = e.desc.value;
         }
         if (e) setFieldError(null, '');
         if (!request('coding_publish', args, 'publish')) {
            return;
         }
         if (e) {
            e.title.readOnly = true; /* typing now wouldn't be sent */
            e.desc.readOnly = true;
         }
         busy = { act: 'do-publish', label: 'Publishing…' };
         h.say('Publishing…');
         drawFooter();
      }

      /* ---- close -------------------------------------------------------- */

      function askClose() {
         mode = 'confirm';
         drawFooter();
         h.say('Close job #' + h.job() + '? Its clone and branch go.');
         focusFooter('keep');
      }

      function sendClose() {
         if (!request('coding_close', {}, 'close')) return;
         busy = { act: mode === 'panel' ? 'retry' : 'close-yes', label: 'Closing…' };
         drawFooter();
      }

      function closeAnyway() {
         if (!lose) return;
         if (!request('coding_close', { confirm: true, head: lose.head }, 'close-confirm')) return;
         busy = { act: 'close-anyway', label: 'Closing…' };
         h.say('Closing…');
         drawFooter();
      }

      function renderLose(nc) {
         const commits = Math.max(0, Number(nc.commits) || 0);
         const files = Math.max(0, Number(nc.files) || 0);
         lose = { head: typeof nc.head === 'string' ? nc.head : '' };
         const what = [];
         if (commits) what.push(plural(commits, 'unpublished commit'));
         if (files) what.push(plural(files, 'file') + ' with uncommitted changes');
         const p = para(
            'Closing deletes ' + (what.join(' and ') || 'unpublished work') + ' on branch '
         );
         p.appendChild(h.shownPath('cv-name', String(nc.branch || '')));
         if (lose.head) {
            p.append(' (at ', elem('span', 'cv-headref', lose.head.slice(0, SHORT_HEAD)), ')');
         }
         p.append('. Nothing else has them.');
         showPanel('lose', [heading('Close job #' + h.job() + '?'), p], function () {
            const btns = [button('Close anyway', 'danger', 'close-anyway', closeAnyway)];
            if (commits)
               btns.push(button('Publish instead', 'secondary', 'publish-instead', startPublish));
            btns.push(cancelBtn());
            return btns;
         });
         h.say(p.textContent);
      }

      /* ---- outcomes ----------------------------------------------------- */

      /* @p done: what Done does (default: back to the job's view). */
      function result(tone, title, p, retry, done) {
         const msg = p.message || p.error || (p.ok ? 'Done.' : 'It didn’t go through.');
         const box = para('');
         h.appendShown(box, String(msg));
         const nodes = [heading(title), box];
         const url = p.ok ? safeUrl(p.pr_url) : '';
         if (url) {
            const link = para('');
            link.appendChild(prLink(url, ('Open pull request ' + prNumber(url)).trim() + ' ↗'));
            nodes.push(link);
         }
         showPanel(tone, nodes, function () {
            if (!retry) {
               return [
                  button('Done', 'primary', 'done', function () {
                     if (panelDone) panelDone();
                     else closePanel(true);
                  }),
               ];
            }
            /* A close's retry is a plain close again: it may close at once. */
            const again = retry === startPublish ? 'Show the new preview' : 'Check again';
            return [button(again, 'primary', 'retry', retry), cancelBtn()];
         });
         panelDone = done || null; /* after showPanel: Escape does the same */
         h.say(shownText(msg));
      }

      function onAnswer(type, p, kind) {
         if (type === 'coding_publish_preview_response') {
            if (p.ok && p.preview) {
               renderPreview(p.preview);
            } else {
               result('publish', 'Can’t publish', p, startPublish);
            }
         } else if (type === 'coding_publish_response') {
            if (p.ok) {
               drafts.delete(Number(p.job));
               result('', 'Published', p, null);
            } else result('publish', 'Not published', p, startPublish);
         } else if (p.needs_confirm && !p.closed) {
            renderLose(p.needs_confirm);
         } else if (p.ok) {
            /* A closed job has nothing left to look at: Done closes the viewer. */
            result('', p.closed ? 'Closed' : 'Job #' + h.job(), p, null, p.closed ? h.close : null);
         } else {
            result('lose', 'Not closed', p, kind === 'close-confirm' ? sendClose : null);
         }
      }

      /* An answer to one of this tab's actions (the server sends them nowhere
       * else).  The panel or question waiting for it shows it; one that comes
       * after they closed is a toast, unless it was only a preview. */
      function handle(type, p) {
         if (!p || !p.nonce || !kinds.has(p.nonce)) return;
         const kind = kinds.get(p.nonce);
         kinds.delete(p.nonce);
         if (kind === 'publish' && p.ok) drafts.delete(Number(p.job)); /* here or not */
         if (req && req.nonce === p.nonce) idle();
         const here = h.isOpen() && p.nonce === wantNonce && Number(p.job) === h.job();
         if (here) {
            wantNonce = '';
            onAnswer(type, p, kind);
         } else if (kind !== 'preview' && typeof DawnToast !== 'undefined') {
            const url = p.ok ? safeUrl(p.pr_url) : '';
            const msg = p.needs_confirm
               ? 'Not closed: closing would delete unpublished work. Open its changes to decide.'
               : p.message || p.error || (p.ok ? 'Done.' : 'It didn’t go through.');
            DawnToast.show(
               'Job #' + Number(p.job) + ': ' + shownText(msg),
               p.needs_confirm ? 'info' : p.ok ? 'success' : 'error',
               url ? TOAST_LINK_MS : TOAST_MS,
               url
                  ? {
                       actions: [
                          {
                             label: 'Open pull request',
                             onClick: function () {
                                window.open(url, '_blank', 'noopener,noreferrer');
                             },
                          },
                       ],
                    }
                  : {}
            );
         }
         if (kind !== 'preview' && h.isOpen() && Number(p.job) === h.job()) {
            h.refresh(); /* published or closed: the facts come from the server */
         }
      }

      /* ---- the viewer's hooks ------------------------------------------- */

      /* A new open, or the viewer closed: nothing waits, nothing shows (the
       * footer hides with no facts). */
      function reset() {
         idle();
         state = null;
         closePanel(false);
      }

      return {
         /* The job's facts from a changes answer (null: unknown). */
         render: function (jobState) {
            const before = state ? state.status : null;
            state = jobState && typeof jobState === 'object' ? jobState : null;
            const unclosable =
               !state || state.closed || state.status === 'running' || state.status === 'queued';
            /* A close question with nothing left to ask goes. */
            const line = mode === 'confirm' && !busy && unclosable ? leaveConfirm() : drawFooter();
            if (line && state && before !== null && state.status !== before) {
               h.say(line.textContent);
            }
         },
         status: function () {
            return state ? state.status : '';
         },
         /* Escape: the panel or the close question first.  True when handled. */
         dismiss: function () {
            if (mode === 'panel' && panelDone) {
               panelDone(); /* a closed job: nothing left to go back to */
               return true;
            }
            if (mode === 'panel') {
               closePanel(true);
               return true;
            }
            if (mode === 'confirm') {
               leaveConfirm();
               return true;
            }
            return false;
         },
         reconnected: function () {
            if (req || busy) stopWaiting();
         },
         reset: reset,
         handle: handle,
      };
   }

   global.DawnCodingViewerActions = { create: create };
})(window);
