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
 * DawnCodingEvents — a coding job in the WebUI: its trail (the agent_step /
 * agent_request / agent_end events DawnAgentEvents hands over) and the card
 * that answers what the job waits on.
 *
 * What a job waits on comes from the gateway (coding_pending), not from the
 * replayed events: a reload replays only a job's first events, and the jobs list
 * carries no waiting state.  The card shows DAWN's checked line (never the
 * agent's raw value); a click is re-checked by the server against the live
 * request.  One card builder serves the jobs panel row and the job's transcript.
 *
 * Everything the agent wrote (steps, progress, report, question, file names) is
 * set with textContent, isolated with dir="auto"; there are no links.
 */
(function (global) {
   'use strict';

   const pending = Object.create(null); // job id -> [{request_id, kind, line, repo, question}]
   const jobInfo = Object.create(null); // job id -> {agent, model}: who does its work
   const sending = Object.create(null); // "job:request" -> {decision, nonce} while on its way
   const drafts = Object.create(null); // "job:request" -> an answer being written
   const listeners = [];
   let tracked = ''; // the active coding jobs last asked about, joined
   let attachedJob = 0; // the coding job whose transcript is shown (0 = none)
   let lastWaiting = 0; // jobs waiting, last announced
   let nonceSeq = 0;
   const ANSWER_MAX_BYTES = 2000;
   const PATHS_SHOWN = 20;
   const PATH_OK = /^[A-Za-z0-9._/-]{1,120}$/;
   const PR_URL = /^https:\/\/github\.com\/[A-Za-z0-9-]+\/[A-Za-z0-9._-]+\/pull\/[0-9]+$/;

   function elem(tag, cls, text) {
      const e = document.createElement(tag);
      if (cls) e.className = cls;
      if (text != null) e.textContent = String(text);
      return e;
   }

   function wsReady() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   function toast(message, kind) {
      if (typeof DawnToast !== 'undefined') DawnToast.show(message, kind);
   }

   function utf8Bytes(s) {
      return new TextEncoder().encode(s).length;
   }

   function announce(text) {
      const a = document.getElementById('jobs-activity-announcer');
      if (a) a.textContent = text;
   }

   function notify() {
      const n = waitingCount();
      if (n > lastWaiting) {
         announce(n === 1 ? 'A coding job needs you.' : n + ' coding jobs need you.');
      }
      lastWaiting = n;
      listeners.forEach(function (fn) {
         try {
            fn();
         } catch (e) {
            console.error('coding-events listener:', e);
         }
      });
      renderTranscriptCard();
      applyAgentView();
   }

   /* ---- a coding job's view: DAWN's model controls don't apply ----------- */

   const AGENT_NOTE = 'To give this job more work, ask Friday to follow up on it.';
   let agentViewOn = false;
   let savedPlaceholder = null;

   function agentSelect(label, value) {
      const wrap = document.createDocumentFragment();
      wrap.appendChild(elem('span', 'llm-label', label));
      const sel = elem('select', 'llm-agent-select');
      sel.disabled = true;
      sel.setAttribute('aria-label', label + ' ' + value);
      sel.appendChild(elem('option', null, value));
      wrap.appendChild(sel);
      return wrap;
   }

   /* While a coding job's view is open, the model bar shows its agent and model
    * (greyed) and the message box is locked: typing there would reach DAWN's
    * model, not the agent. */
   function applyAgentView() {
      const info = attachedJob ? jobInfo[String(attachedJob)] : null;
      const grid = document.getElementById('llm-controls-grid');
      const input = document.getElementById('text-input');
      const on = !!info;
      if (on === agentViewOn && !on) return;
      /* Listening would put voice turns into the job's conversation, for DAWN's
       * model to answer: stop it on entering the view. */
      if (on && !agentViewOn && typeof DawnAlwaysOn !== 'undefined' && DawnAlwaysOn.isEnabled()) {
         DawnAlwaysOn.disable();
      }
      agentViewOn = on;
      if (grid) {
         let view = document.getElementById('llm-agent-view');
         if (view) view.remove();
         grid.classList.toggle('agent-mode', on);
         if (on) {
            view = elem('div');
            view.id = 'llm-agent-view';
            view.appendChild(agentSelect('Mode:', 'Agent'));
            view.appendChild(agentSelect('Provider:', info.agent + ' (agent)'));
            view.appendChild(agentSelect('Model:', info.model || info.agent + ' default'));
            const note = elem(
               'div',
               'llm-agent-note',
               'This job runs in ' + info.agent + '; DAWN’s model settings don’t apply.'
            );
            view.appendChild(note);
            if (typeof DawnCodingViewer !== 'undefined') {
               const btn = elem('button', 'jobs-open-btn llm-agent-changes', 'View changes');
               btn.setAttribute('aria-label', 'Changes made by coding job #' + attachedJob);
               btn.type = 'button';
               btn.dataset.jobId = String(attachedJob);
               btn.dataset.action = 'changes';
               const job = attachedJob;
               btn.addEventListener('click', function () {
                  DawnCodingViewer.open(job, '', btn);
               });
               view.appendChild(btn);
            }
            grid.appendChild(view);
         }
      }
      const mode = document.getElementById('llm-mini-mode');
      const model = document.getElementById('llm-mini-model');
      if (on && mode && model) {
         [mode.textContent, model.textContent] = agentMini();
      } else if (!on && window.DAWN && window.DAWN.updateLlmMiniSummary) {
         window.DAWN.updateLlmMiniSummary(); // from DAWN's live controls
      }
      if (input) {
         if (on && savedPlaceholder === null) savedPlaceholder = input.placeholder;
         input.readOnly = on;
         /* A placeholder doesn't show over a draft: say why it's locked anyway. */
         if (on) {
            input.title = AGENT_NOTE;
            input.setAttribute('aria-description', AGENT_NOTE);
         } else {
            input.removeAttribute('title');
            input.removeAttribute('aria-description');
         }
         input.placeholder = on ? AGENT_NOTE : savedPlaceholder || input.placeholder;
         input.classList.toggle('agent-locked', on);
         if (!on) savedPlaceholder = null;
      }
      ['attach-btn', 'attach-dropdown-btn', 'action-btn', 'action-dropdown-btn'].forEach(
         function (id) {
            const b = document.getElementById(id);
            if (!b) return;
            b.classList.toggle('agent-locked', on);
            if (on) b.setAttribute('aria-disabled', 'true');
            else b.removeAttribute('aria-disabled');
         }
      );
   }

   /* The message box and its buttons do nothing while it is locked (Enter would
    * still send a draft typed before the view opened; a paste or drop would queue
    * an attachment for the next chat), except Stop on a running turn. */
   function blockLocked(e) {
      if (!agentViewOn || !e.target.closest) return;
      const locked = e.target.closest('.agent-locked');
      if (!locked) return;
      if (e.type === 'keydown' && e.key !== 'Enter') return;
      if (e.type === 'click' && locked.id === 'action-btn' && typeof DawnState !== 'undefined') {
         const st = DawnState.getAppState ? DawnState.getAppState() : 'idle';
         if (st === 'processing' || st === 'speaking' || st === 'thinking') return; // Stop
      }
      e.preventDefault();
      e.stopPropagation();
   }
   ['click', 'keydown', 'mousedown', 'touchstart', 'paste', 'drop'].forEach(function (type) {
      document.addEventListener(type, blockLocked, true); // mousedown/touchstart: push-to-talk
   });

   /* The collapsed bar's text while a coding job's view is open, else null. */
   function agentMini() {
      const info = agentViewOn && attachedJob ? jobInfo[String(attachedJob)] : null;
      return info ? [info.agent, info.model || 'default'] : null;
   }

   /* ---- what the jobs wait on --------------------------------------------- */

   function request(jobIds) {
      const ids = jobIds.filter(function (id) {
         return id > 0;
      });
      if (!ids.length || !wsReady()) return false;
      DawnWS.send({ type: 'coding_pending', payload: { jobs: ids.slice(0, 16) } });
      return true;
   }

   function trackedIds() {
      return tracked ? tracked.split(',').map(Number) : [];
   }

   /* The active coding jobs changed (snapshot or update): ask about new ones. */
   function track(jobs) {
      const ids = (jobs || [])
         .filter(function (j) {
            return j && j.kind === 'coding';
         })
         .map(function (j) {
            return Number(j.conversation_id);
         })
         .sort();
      const key = ids.join(',');
      Object.keys(pending).forEach(function (id) {
         if (ids.indexOf(Number(id)) < 0 && Number(id) !== attachedJob) delete pending[id];
      });
      // Remembered only once asked, so a disconnected moment asks again later.
      if (key !== tracked && (ids.length === 0 || request(ids))) tracked = key;
   }

   /* The socket came back: what happened meanwhile isn't known, so ask again
    * and forget clicks whose answer can't arrive any more. */
   function reconnected() {
      Object.keys(sending).forEach(function (k) {
         delete sending[k];
      });
      const ids = trackedIds();
      if (attachedJob && ids.indexOf(attachedJob) < 0) ids.push(attachedJob);
      if (!request(ids)) tracked = '';
      notify();
   }

   /* The jobs panel opened: what its coding jobs wait on, fresh. */
   function refresh() {
      const ids = trackedIds();
      if (attachedJob && ids.indexOf(attachedJob) < 0) ids.push(attachedJob);
      request(ids);
   }

   function handlePending(payload) {
      const list = payload && Array.isArray(payload.jobs) ? payload.jobs : [];
      list.forEach(function (j) {
         pending[String(j.job)] = Array.isArray(j.requests) ? j.requests : [];
         jobInfo[String(j.job)] = { agent: j.agent || 'Coding agent', model: j.model || '' };
      });
      notify();
   }

   function waiting(jobId) {
      return pending[String(jobId)] || [];
   }

   function waitingCount() {
      return Object.keys(pending).filter(function (id) {
         return pending[id].length > 0;
      }).length;
   }

   /* A live event about a coding job: what it waits on may have changed. */
   function noteEvent(ev) {
      if (!ev || !ev.conversation_id) return;
      const id = Number(ev.conversation_id);
      if (ev.kind === 'agent_end') {
         pending[String(id)] = [];
         notify();
      } else if (
         ev.kind === 'agent_request' &&
         (trackedIds().indexOf(id) >= 0 || id === attachedJob)
      ) {
         request([id]); // only jobs this tab shows (the server merges the asks)
      }
   }

   /* ---- the card --------------------------------------------------------- */

   function send(jobId, req, decision, answer) {
      if (!wsReady()) {
         toast('Not connected.', 'error');
         return;
      }
      const key = jobId + ':' + req.request_id;
      if (sending[key]) return;
      const nonce = 'c' + Date.now().toString(36) + '-' + ++nonceSeq;
      sending[key] = { decision: decision, nonce: nonce };
      const payload = { job: jobId, request_id: req.request_id, decision: decision, nonce: nonce };
      if (decision === 'answer') payload.answer = answer;
      else payload.line = req.line || '';
      DawnWS.send({ type: 'coding_reply', payload: payload });
      notify();
   }

   function handleReplyResponse(p) {
      if (!p) return;
      const key = p.job + ':' + p.request_id;
      const mine = sending[key] && sending[key].nonce === p.nonce;
      if (p.ok) {
         pending[String(p.job)] = waiting(p.job).filter(function (r) {
            return r.request_id !== p.request_id;
         });
         delete drafts[key];
         // Answered (by this tab or another): this tab's own click on it is moot.
         if (!mine) delete sending[key];
      }
      if (mine) {
         delete sending[key];
         toast(p.message || (p.ok ? 'Sent.' : 'That did not work.'), p.ok ? 'success' : 'error');
         if (!p.ok) request([Number(p.job)]); // what it waits on may have changed
      }
      notify();
   }

   function button(label, cls, jobId, action, onClick, aria, describedBy) {
      const b = elem('button', 'coding-card-btn ' + cls, label);
      b.type = 'button';
      b.dataset.jobId = String(jobId);
      b.dataset.action = action;
      if (aria) {
         b.setAttribute('aria-label', aria);
         b.title = aria;
      }
      if (describedBy) b.setAttribute('aria-describedby', describedBy);
      b.addEventListener('click', function () {
         if (b.getAttribute('aria-disabled') === 'true') return;
         onClick();
      });
      return b;
   }

   /* One request's card.  `place` is 'row' (jobs panel) or 'transcript';
    * `title` names the job for screen readers. */
   function buildCard(jobId, req, place, title) {
      const key = jobId + ':' + req.request_id;
      const base = 'coding-card-' + place + '-' + jobId + '-' + req.request_id;
      const card = elem('div', 'coding-card coding-card-' + place);
      card.setAttribute('role', 'group');
      card.setAttribute(
         'aria-label',
         (req.kind === 'question' ? 'Question from ' : 'Permission for ') + (title || 'coding job')
      );
      if (place !== 'row') {
         card.appendChild(
            elem(
               'div',
               'coding-card-head',
               req.kind === 'question'
                  ? 'DAWN: the agent has a question'
                  : 'DAWN: the agent asks permission'
            )
         );
      }
      const inFlight = sending[key];
      const actions = elem('div', 'coding-card-actions');
      const lineId = base + '-line';

      if (req.kind === 'question') {
         card.appendChild(elem('div', 'coding-card-label', 'The agent asks:'));
         const q = elem('div', 'coding-quote', req.question || '');
         q.id = lineId;
         q.setAttribute('dir', 'auto');
         card.appendChild(q);
         const box = elem('textarea', 'dawn-input coding-answer');
         box.rows = 2;
         box.placeholder = 'Your answer…';
         box.value = drafts[key] || '';
         box.dataset.jobId = String(jobId);
         box.dataset.action = 'coding-answer-' + req.request_id;
         box.setAttribute('aria-label', 'Answer to the agent');
         box.setAttribute('aria-describedby', lineId);
         const sendBtn = button(
            inFlight && inFlight.decision === 'answer' ? 'Sending…' : 'Send',
            'primary',
            jobId,
            'coding-send-' + req.request_id,
            function () {
               const text = box.value.trim();
               if (!text) return;
               if (utf8Bytes(text) > ANSWER_MAX_BYTES) {
                  toast('That answer is too long (2000 bytes at most).', 'error');
                  return;
               }
               send(jobId, req, 'answer', text);
            },
            null,
            lineId
         );
         box.addEventListener('input', function () {
            drafts[key] = box.value;
         });
         box.addEventListener('keydown', function (e) {
            if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
               e.preventDefault();
               sendBtn.click();
            }
         });
         if (inFlight) box.readOnly = true;
         card.appendChild(box);
         actions.appendChild(sendBtn);
      } else if (req.line) {
         const line = elem('div', 'coding-card-line', 'It wants to ' + req.line + '.');
         line.id = lineId;
         card.appendChild(line);
         actions.appendChild(
            button(
               inFlight && inFlight.decision === 'allow' ? 'Sending…' : 'Allow for this task',
               'primary',
               jobId,
               'coding-allow-' + req.request_id,
               function () {
                  send(jobId, req, 'allow');
               },
               null,
               lineId
            )
         );
         if (req.repo) {
            actions.appendChild(
               button(
                  inFlight && inFlight.decision === 'allow_repo'
                     ? 'Sending…'
                     : 'Allow on ' + req.repo,
                  'secondary',
                  jobId,
                  'coding-allow_repo-' + req.request_id,
                  function () {
                     send(jobId, req, 'allow_repo');
                  },
                  'Allow for every task on ' + req.repo,
                  lineId
               )
            );
         }
      } else {
         const line = elem(
            'div',
            'coding-card-line',
            'It asks for something DAWN can’t show; it can only be denied.'
         );
         line.id = lineId;
         card.appendChild(line);
      }
      actions.appendChild(
         button(
            inFlight && inFlight.decision === 'deny' ? 'Sending…' : 'Deny',
            'ghost',
            jobId,
            'coding-deny-' + req.request_id,
            function () {
               send(jobId, req, 'deny');
            },
            null,
            lineId
         )
      );
      if (inFlight) {
         card.setAttribute('aria-busy', 'true');
         actions.querySelectorAll('button').forEach(function (b) {
            b.setAttribute('aria-disabled', 'true');
         });
      }
      card.appendChild(actions);
      return card;
   }

   let shownSig = '';

   /* The job's transcript: its cards after the trail (redrawn only when what it
    * shows changed, so an answer being typed isn't dropped). */
   function renderTranscriptCard() {
      const t = document.getElementById('transcript');
      if (!t) return;
      const reqs = attachedJob ? waiting(attachedJob) : [];
      const sig =
         attachedJob +
         '|' +
         reqs
            .map(function (r) {
               const s = sending[attachedJob + ':' + r.request_id];
               return r.request_id + (s ? s.decision : '');
            })
            .join(',');
      const old = t.querySelector('.coding-cards');
      if (sig === shownSig && (old !== null) === reqs.length > 0) return;
      shownSig = sig;
      const focused = old && old.contains(document.activeElement) ? document.activeElement : null;
      const focusAction = focused && focused.dataset ? focused.dataset.action : null;
      if (old) old.remove();
      if (!reqs.length) {
         if (focused) {
            // The card is answered: focus the trail's latest line instead.
            const last = t.lastElementChild;
            if (last) {
               last.setAttribute('tabindex', '-1');
               last.focus();
            }
         }
         return;
      }
      const wrap = elem('div', 'coding-cards');
      reqs.forEach(function (r) {
         wrap.appendChild(buildCard(attachedJob, r, 'transcript', ''));
      });
      const nearBottom = t.scrollHeight - t.scrollTop - t.clientHeight < 120;
      t.appendChild(wrap);
      if (nearBottom) wrap.scrollIntoView({ block: 'nearest' });
      if (focused) {
         let target = focusAction && wrap.querySelector('[data-action="' + focusAction + '"]');
         if (!target) target = wrap.querySelector('button, textarea');
         if (target) target.focus();
      }
   }

   /* The transcript of job `convId` is shown (0: something else). */
   function attached(convId) {
      attachedJob = convId > 0 ? convId : 0;
      if (attachedJob) request([attachedJob]); // the server answers only for coding jobs
      shownSig = '';
      renderTranscriptCard();
      applyAgentView();
   }

   /* ---- the trail -------------------------------------------------------- */

   function stepText(p) {
      if (p.gw === 'tool') return (p.tool || 'Tool') + ' ' + (p.brief || '');
      return p.text || '';
   }

   function requestText(p) {
      switch (p.gw) {
         case 'request':
            if (p.request_kind === 'question') return 'Asked you a question';
            return p.line ? 'Asked to ' + p.line : 'Asked for something DAWN can’t show';
         case 'reply':
            return (
               {
                  allow: 'Allowed for this task',
                  always_repo: 'Allowed on the repo',
                  always: 'Allowed everywhere',
                  deny: 'Denied',
               }[p.decision] || 'Answered'
            );
         case 'blocked':
            return 'Waiting for room to run';
         case 'input_required':
            return 'Stopped: waiting for you';
         case 'published':
            return (
               'Published' +
               (PR_URL.test(p.pr_url || '') ? ': ' + p.pr_url : p.pushed ? ': ' + p.pushed : '')
            );
         case 'merged': {
            const n = Number(p.unpublished) || 0;
            const d = Number(p.uncommitted) || 0;
            const left = [];
            if (n) left.push(n + ' newer ' + (n === 1 ? 'commit' : 'commits') + ' not published');
            if (d) left.push(d + ' changed ' + (d === 1 ? 'file' : 'files') + ' never committed');
            return 'Pull request merged; ' + (left.join(', ') || 'work left over');
         }
         case 'closed':
            return p.reason === 'merged'
               ? 'Closed: its pull request was merged'
               : p.reason === 'user'
                 ? 'Closed at your request'
                 : 'Closed: its work is gone from the gateway';
         default:
            return null;
      }
   }

   /* Changed protected paths as the chat report shows them: only plainly
    * path-shaped names, the rest counted (with the gateway's full count). */
   function protectedPaths(p) {
      const all = String(p.protected || '')
         .split('\n')
         .filter(Boolean);
      const shown = all
         .filter(function (x) {
            return PATH_OK.test(x);
         })
         .slice(0, PATHS_SHOWN);
      const total = Math.max(Number(p.protected_count) || 0, all.length);
      return { shown: shown, hidden: total - shown.length };
   }

   function endBlock(p) {
      const box = elem('details', 'coding-end');
      box.open = true;
      const ok = p.gw === 'completed';
      const head = elem(
         'summary',
         'coding-end-head ' + (ok ? 'ok' : p.gw === 'cancelled' ? 'muted' : 'err'),
         ok
            ? 'Report'
            : p.gw === 'cancelled'
              ? 'Cancelled'
              : 'Failed' + (p.error ? ': ' + p.error : '')
      );
      head.setAttribute('dir', 'auto');
      box.appendChild(head);
      if (p.summary) {
         const rep = elem('div', 'coding-report', p.summary);
         rep.setAttribute('dir', 'auto');
         box.appendChild(rep);
      }
      const facts = elem('div', 'coding-facts');
      if (p.commits) {
         const c = elem('div', 'coding-commits', p.commits);
         c.setAttribute('dir', 'auto');
         facts.appendChild(c);
      }
      if (p.diffstat) {
         const d = elem('div', 'coding-muted', p.diffstat);
         d.setAttribute('dir', 'auto');
         facts.appendChild(d);
      }
      const paths = protectedPaths(p);
      if (paths.shown.length || paths.hidden > 0) {
         const prot = elem('div', 'coding-protected');
         prot.appendChild(elem('span', 'coding-facts-label', 'Protected paths changed: '));
         paths.shown.forEach(function (path) {
            prot.appendChild(elem('span', 'coding-path', path));
         });
         if (paths.hidden > 0) {
            prot.appendChild(
               elem(
                  'span',
                  'coding-muted',
                  (paths.shown.length ? 'and ' : '') + paths.hidden + ' not shown'
               )
            );
         }
         facts.appendChild(prot);
      }
      if (p.cost_usd) {
         facts.appendChild(elem('div', 'coding-muted', 'Cost $' + Number(p.cost_usd).toFixed(2)));
      }
      if (facts.childNodes.length) box.appendChild(facts);
      return box;
   }

   /* One trail element for an agent_* event, or null. */
   function buildTrail(kind, p, live) {
      p = p || {};
      if (kind === 'agent_step') {
         const text = stepText(p);
         if (!text) return null;
         const line = elem(
            'div',
            'agent-event agent-event-line ' +
               (p.gw === 'tool' ? 'coding-step' : 'coding-note agent-event-muted'),
            text
         );
         line.setAttribute('dir', 'auto');
         return line;
      }
      if (kind === 'agent_request') {
         const text = requestText(p);
         if (!text) return null;
         const el = elem('div', 'agent-event agent-event-line agent-event-round', text);
         if (live && p.gw === 'input_required') el.setAttribute('role', 'status');
         return el;
      }
      if (kind === 'agent_end') {
         const el = endBlock(p);
         el.classList.add('agent-event', 'agent-event-line');
         if (live) el.setAttribute('role', 'status');
         return el;
      }
      return null;
   }

   /* Place a step: tool steps and the agent's notes between two requests (or
    * the end) gather in one collapsed group at the transcript's end.  True
    * when placed. */
   function placeStep(el, transcript) {
      const tool = el.classList.contains('coding-step');
      if (!tool && !el.classList.contains('coding-note')) return false;
      const cards = transcript.querySelector('.coding-cards');
      let group = cards ? cards.previousElementSibling : transcript.lastElementChild;
      if (!group || !group.classList.contains('coding-steps')) {
         group = elem('details', 'coding-steps agent-event agent-event-line');
         const head = elem('summary', 'coding-steps-head');
         head.appendChild(elem('span', 'coding-steps-count'));
         head.appendChild(elem('span', 'coding-steps-last'));
         group.appendChild(head);
         transcript.insertBefore(group, cards);
      }
      group.appendChild(el);
      const n = group.querySelectorAll('.coding-step').length;
      const head = group.firstChild;
      head.firstChild.textContent = n ? n + (n === 1 ? ' step' : ' steps') : 'Notes from the agent';
      if (tool) {
         head.lastChild.textContent = ' · last: ' + el.textContent;
         head.lastChild.setAttribute('dir', 'auto');
      }
      return true;
   }

   global.DawnCodingEvents = {
      buildTrail: buildTrail,
      placeStep: placeStep,
      track: track,
      attached: attached,
      reconnected: reconnected,
      refresh: refresh,
      noteEvent: noteEvent,
      handlePending: handlePending,
      handleReplyResponse: handleReplyResponse,
      waiting: waiting,
      waitingCount: waitingCount,
      buildCard: buildCard,
      agentMini: agentMini,
      onChange: function (fn) {
         listeners.push(fn);
      },
   };
})(window);
