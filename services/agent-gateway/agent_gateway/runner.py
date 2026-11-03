#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
#
# Task lifecycle over the Claude Agent SDK.  A task is a series of runs on
# one Claude Code session.  A run ends when the agent finishes, fails, or
# reaches something only the operator can decide: the gateway records the
# request and interrupts the run itself, so nothing is held open while the
# operator decides.  The answer resumes the session in a new run.
#
# Everything here runs on one asyncio loop; the await points are where other
# calls can interleave, so state is re-read after each one that matters.

import asyncio
import json
import logging
import os
import secrets
import time

from claude_agent_sdk import (AssistantMessage, ClaudeAgentOptions, ClaudeSDKClient,
                              PermissionResultAllow, PermissionResultDeny, ResultMessage,
                              SystemMessage)

from . import policy, store as st
from .repos import GitError, Repos, slug

log = logging.getLogger("agent-gateway")


TEXT_CAP = 2000
PAYLOAD_CAP = 4000

GUIDANCE = """You are running unattended for the operator, who reaches you through their \
assistant.  Commits in this task's worktree are pre-approved: commit your work when it is \
done and tested, with the git identity already configured (never set user.name or \
user.email yourself).  Never push or open pull requests; the operator publishes.  If \
something is denied or you need the operator, say so in one line and end your turn: you \
will be resumed with the answer."""

TEMPLATE = os.path.join(os.path.dirname(__file__), "CLAUDE.template.md")
FLOOR = os.path.join(os.path.dirname(__file__), "floor.json")

CBM_WRITE_TOOLS = ("index_repository", "delete_project", "manage_adr")


class Blocked(ValueError):
   """No room to run now (concurrency or the daily cap); retried when a run ends."""


def clip(text: str, cap: int = TEXT_CAP) -> str:
   return text if len(text) <= cap else text[:cap] + " [...]"


def floor_settings(agent_home: str, work_dir: str, worktree: str, domains: list[str]) -> str:
   """The agent's fixed rules plus this run's pieces, passed with every run.
   Command-line settings outrank a repo's own and can't be changed by the
   agent, and a run's sandbox settings would otherwise replace these (seen: a
   per-run `filesystem` block made the credentials readable again).  `~` is
   written out so nothing depends on how it is expanded."""
   home = agent_home.rstrip("/") + "/"
   with open(FLOOR) as f:
      s = json.load(f)
   # A permission rule's absolute path starts with //; a sandbox path is plain.
   s["permissions"]["deny"] = [r.replace("(~/", "(/" + home) for r in s["permissions"]["deny"]]
   s["sandbox"]["network"]["allowedDomains"] = domains
   fs = s["sandbox"]["filesystem"]
   fs["denyRead"] = [p.replace("~/", home, 1) if p.startswith("~/") else p
                     for p in fs["denyRead"]] + [work_dir]  # and other tasks' clones
   fs["allowRead"] = [worktree]
   return json.dumps(s)


def brief(tool: str, ti: dict) -> str:
   for key in ("command", "file_path", "pattern", "host", "description"):
      if key in ti:
         return clip(str(ti[key]), 300)
   return clip(json.dumps(ti), 300)


class Runner:
   def __init__(self, cfg, store: st.Store, repos: Repos):
      self.cfg = cfg
      self.store = store
      self.repos = repos
      self.running: dict[str, asyncio.Task] = {}
      self.clients: dict[str, ClaudeSDKClient] = {}
      self.starting = 0
      self.background: set[asyncio.Task] = set()
      self.inflight: dict[tuple[int, str], asyncio.Future] = {}

   def soon(self, coro) -> None:
      """Run `coro` after the current step; the task is kept until it ends."""
      t = asyncio.get_running_loop().create_task(coro)
      self.background.add(t)
      t.add_done_callback(self.background.discard)

   # --- room to run ---------------------------------------------------------

   def check_capacity(self) -> None:
      busy = len(self.running) + self.starting
      if busy >= self.cfg.max_concurrent_tasks:
         raise Blocked(f"{busy} tasks already running (limit {self.cfg.max_concurrent_tasks})")
      if self.store.spent_since(time.time() - 86400) >= self.cfg.daily_budget_usd:
         raise Blocked(f"daily budget of ${self.cfg.daily_budget_usd:.2f} used")

   # --- starting, resuming, recovering ----------------------------------

   async def start(self, dawn_user_id: int, repo: str, task: str, budget_usd: float | None,
                   client_ref: str | None) -> dict:
      if not client_ref:
         return await self.start_new(dawn_user_id, repo, task, budget_usd, None)
      # A retried call returns the task the first one started, even mid-start.
      existing = self.store.task_by_ref(dawn_user_id, client_ref)
      if existing:
         return existing
      key = (dawn_user_id, client_ref)
      if key in self.inflight:
         return await asyncio.shield(self.inflight[key])
      self.inflight[key] = asyncio.get_running_loop().create_future()
      try:
         t = await self.start_new(dawn_user_id, repo, task, budget_usd, client_ref)
         self.inflight[key].set_result(t)
         return t
      except BaseException as e:
         self.inflight[key].set_exception(e)
         self.inflight[key].exception()  # retrieved here, so never "unhandled"
         raise
      finally:
         del self.inflight[key]

   async def start_new(self, dawn_user_id: int, repo: str, task: str,
                       budget_usd: float | None, client_ref: str | None) -> dict:
      if repo not in self.cfg.repos:
         raise ValueError(f"unknown repo '{repo}' (known: {', '.join(self.cfg.repos)})")
      if not task.strip():
         raise ValueError("empty task")
      if budget_usd is not None and budget_usd <= 0:
         raise ValueError("budget_usd must be positive")
      self.check_capacity()
      task_id = secrets.token_hex(4)
      base = self.cfg.repos[repo]["base"]
      branch = f"agent/{task_id}-{slug(task)}"
      self.starting += 1  # holds the slot across the clone
      budget = min(budget_usd or self.cfg.task_budget_usd, self.cfg.task_budget_usd * 4)
      try:
         await self.prune()
         worktree = await self.repos.prepare(repo, base, branch, task_id)
         self.store.add_task(id=task_id, dawn_user_id=dawn_user_id, agent="claude",
                             repo=repo, base=base, branch=branch, worktree=worktree,
                             task=task, budget_usd=budget, client_ref=client_ref)
      except Exception:
         try:
            await self.repos.remove(task_id)
         except Exception as e:
            log.warning("could not remove the half-made worktree %s: %s", task_id, e)
         raise
      finally:
         self.starting -= 1
      self.store.add_event(task_id, "started", repo=repo, branch=branch, task=clip(task))
      self.launch(task_id, task)
      return self.store.task(task_id)

   def launch(self, task_id: str, prompt: str) -> None:
      self.store.update_task(task_id, status=st.WORKING, error=None)
      self.running[task_id] = asyncio.get_running_loop().create_task(self.run(task_id, prompt))

   async def deliver(self, task_id: str) -> bool:
      """Resume the session with everything the agent hasn't been told yet.
      With no room, the task waits as input_required until a run ends."""
      task = self.store.task(task_id)
      if task_id in self.running or task["status"] not in st.OPEN:
         return False
      if not self.store.has_notes(task_id) or self.store.pending_requests(task_id):
         return False
      if task["cost_usd"] >= task["budget_usd"]:
         self.fail(task_id, f"task budget of ${task['budget_usd']:.2f} used")
         return False
      try:
         self.check_capacity()
      except Blocked as e:
         if task["error"] != str(e):
            self.store.update_task(task_id, status=st.INPUT_REQUIRED, error=str(e))
            self.store.add_event(task_id, "blocked", error=str(e))
         return False
      notes = self.store.take_notes(task_id)
      self.store.add_event(task_id, "resumed")
      self.launch(task_id, "\n".join(notes))
      return True

   async def retry_blocked(self) -> None:
      for t in reversed(self.store.tasks(status=st.INPUT_REQUIRED)):
         await self.deliver(t["id"])

   async def recover(self) -> None:
      """After a gateway restart: runs that were cut off resume as room allows.
      A killed run's spend is lost: the CLI reports cost only in its result."""
      for t in self.store.tasks(status=st.WORKING):
         if t["session_id"]:
            self.store.add_note(t["id"], "The gateway restarted mid-run.  Check the worktree "
                                "and continue the task.")
            self.store.update_task(t["id"], status=st.INPUT_REQUIRED)
         else:
            self.fail(t["id"], "gateway restarted before the run had a session")
      await self.retry_blocked()

   async def prune(self) -> None:
      before = time.time() - self.cfg.keep_worktrees_days * 86400
      for task_id in self.store.stale_finished(before):
         try:
            await self.repos.remove(task_id)
            self.store.update_task(task_id, worktree="")
         except Exception as e:
            log.warning("could not remove worktree of %s: %s", task_id, e)

   # --- one run ----------------------------------------------------------

   def permission_handler(self, task: dict, granted_tools: list[str]):
      """Claude Code's permission callback for one run: allow, deny, or record a
      request for the operator and stop the run."""

      async def can_use_tool(tool, tool_input, ctx):
         d = policy.decide(tool, tool_input, task["worktree"], granted_tools,
                           self.cfg.work_dir)
         if d.allow:
            return PermissionResultAllow()
         if d.ask_kind:
            shown = clip(json.dumps(tool_input), PAYLOAD_CAP)
            payload = {"tool": tool, "input": shown}
            if d.ask_value:
               payload["value"] = d.ask_value
            rid = self.store.add_request(task["id"], d.ask_kind, payload)
            self.store.add_event(task["id"], "request", request_id=rid,
                                 request_kind=d.ask_kind, value=d.ask_value, tool=tool,
                                 input=shown)
            # Stopping is not left to the model: the run is interrupted once
            # this callback has answered.
            client = self.clients.get(task["id"])
            if client:
               self.soon(self.interrupt(client))
         return PermissionResultDeny(message=d.message)

      return can_use_tool

   async def interrupt(self, client: ClaudeSDKClient) -> None:
      try:
         await asyncio.wait_for(client.interrupt(), 5)
      except Exception as e:  # the run may already be ending
         log.debug("interrupt: %s", e)

   async def options(self, task: dict) -> ClaudeAgentOptions:
      domains = sorted({*self.cfg.default_domains, *self.store.grants_for(task, "domain")})
      granted_tools = self.store.grants_for(task, "tool")
      system = GUIDANCE
      if not await self.repos.has_claude_md(task["repo"], task["base"]):
         with open(TEMPLATE) as f:
            system += "\n\nThis repository has no CLAUDE.md.  Follow these rules:\n" + f.read()
      mcp = {"cbm": {"type": "sse", "url": self.cfg.cbm_url}} if self.cfg.cbm_url else {}
      return ClaudeAgentOptions(
         cwd=task["worktree"], cli_path=self.cfg.claude_cli, resume=task["session_id"],
         model=self.cfg.model, permission_mode="acceptEdits",
         setting_sources=["user", "project"], strict_mcp_config=True, mcp_servers=mcp,
         disallowed_tools=[f"mcp__cbm__{t}" for t in CBM_WRITE_TOOLS],
         settings=floor_settings(self.cfg.agent_home, self.cfg.work_dir, task["worktree"],
                                 domains),
         # The CLI restores a session's running cost on resume, so its budget
         # check is against the whole task: give it the whole budget.
         max_budget_usd=task["budget_usd"],
         can_use_tool=self.permission_handler(task, granted_tools),
         system_prompt={"type": "preset", "preset": "claude_code", "append": system},
         env={"DAWN_AGENT_AUTH": self.cfg.auth,
              **({"DAWN_AGENT_CLAUDE": self.cfg.claude_bin} if self.cfg.claude_bin else {})},
         stderr=lambda line: log.debug("claude[%s]: %s", task["id"], line))

   async def run(self, task_id: str, prompt: str) -> None:
      task = self.store.task(task_id)
      result, costed = None, False
      try:
         async with ClaudeSDKClient(options=await self.options(task)) as client:
            self.clients[task_id] = client
            await client.query(prompt)
            async for m in client.receive_response():
               if isinstance(m, SystemMessage) and m.subtype == "init":
                  # Saved at once, so a run cut off by a gateway restart resumes.
                  self.store.update_task(task_id, session_id=m.data.get("session_id"))
               elif isinstance(m, AssistantMessage):
                  for b in m.content:
                     if getattr(b, "text", None):
                        self.store.add_event(task_id, "progress", text=clip(b.text))
                     elif getattr(b, "name", None):
                        self.store.add_event(task_id, "tool", tool=b.name,
                                             brief=brief(b.name, b.input))
               elif isinstance(m, ResultMessage):
                  result = m
                  self.store.update_task(task_id, session_id=m.session_id)
         self.release(task_id)  # so finish() can resume the session in a new run
         self.record_cost(task_id, result)
         costed = True
         await self.finish(task_id, result)
      except asyncio.CancelledError:
         if not costed:
            self.record_cost(task_id, result)
         self.finish_cancelled(task_id)
         raise
      except Exception as e:  # the SDK raises on CLI failures; collect() on git ones
         log.exception("task %s run failed", task_id)
         if not costed:
            self.record_cost(task_id, result)
         if self.store.task(task_id)["status"] != st.CANCELLED:
            self.fail(task_id, clip(str(e), 500))
      finally:
         self.release(task_id)
         self.soon(self.retry_blocked())

   def release(self, task_id: str) -> None:
      if self.running.get(task_id) is asyncio.current_task():
         del self.running[task_id]
         self.clients.pop(task_id, None)

   def record_cost(self, task_id: str, r: ResultMessage | None) -> None:
      """A resumed session reports its running total, so this run's spend is
      the growth since the last report (all of it if the total restarted)."""
      cost = 0.0
      if r and r.total_cost_usd is not None:
         before = self.store.task(task_id)["session_cost"]
         total = r.total_cost_usd
         cost = total - before if total >= before else total
         self.store.update_task(task_id, session_cost=total)
      self.store.add_cost(task_id, cost)
      self.store.add_event(task_id, "run_end", cost_usd=cost)

   async def finish(self, task_id: str, r: ResultMessage | None) -> None:
      task = self.store.task(task_id)
      if task["status"] == st.CANCELLED:
         return
      # Whenever a run stops, its work so far goes to the mirror, so it can be
      # looked at before an approval or a cancel, not only once it's done.
      try:
         outcome = await self.repos.collect(task["repo"], task_id, task["branch"],
                                            task["base"])
      except GitError as e:
         log.warning("task %s: could not collect its branch: %s", task_id, e)
         outcome = None
      if self.store.task(task_id)["status"] == st.CANCELLED:
         return  # cancelled while the branch was being collected
      if task_id in self.running:
         return  # answered while the branch was being collected: a new run has it
      pending = self.store.pending_requests(task_id)
      if pending:
         self.store.update_task(task_id, status=st.INPUT_REQUIRED)
         self.store.add_event(task_id, "input_required",
                              request_ids=[p["id"] for p in pending],
                              note=clip(r.result or "", 500) if r else "")
         return
      if self.store.has_notes(task_id):
         # Decided while this run was ending: resume now, or wait for room.
         self.store.update_task(task_id, status=st.INPUT_REQUIRED)
         await self.deliver(task_id)
         return
      if r is None or r.is_error:
         err = (r.result or r.subtype) if r else "run ended without a result"
         if r and r.subtype == "error_max_budget_usd":
            err = f"task budget of ${task['budget_usd']:.2f} used"
         self.fail(task_id, clip(err, 500))
         return
      if outcome is None:
         self.fail(task_id, "the task's work could not be collected")
         return
      self.store.update_task(task_id, status=st.COMPLETED, summary=clip(r.result or ""))
      self.store.add_event(task_id, "completed", summary=clip(r.result or ""),
                           branch=task["branch"], session_id=task["session_id"],
                           cost_usd=self.store.task(task_id)["cost_usd"], **outcome)

   def fail(self, task_id: str, error: str) -> None:
      self.store.update_task(task_id, status=st.FAILED, error=error)
      self.store.add_event(task_id, "failed", error=error,
                           cost_usd=self.store.task(task_id)["cost_usd"])
      for p in self.store.pending_requests(task_id):
         self.store.decide_request(p["id"], "cancelled")

   def finish_cancelled(self, task_id: str) -> None:
      if self.store.task(task_id)["status"] != st.CANCELLED:
         self.store.update_task(task_id, status=st.CANCELLED)
         self.store.add_event(task_id, "cancelled")

   # --- operator actions ---------------------------------------------------

   def owned(self, task_id: str, dawn_user_id: int) -> dict:
      t = self.store.task(task_id)
      if not t or t["dawn_user_id"] != dawn_user_id:
         raise ValueError("no such task")
      return t

   async def reply(self, task_id: str, dawn_user_id: int, request_id: int, decision: str,
                   answer: str | None) -> dict:
      t = self.owned(task_id, dawn_user_id)
      req = self.store.request(request_id)
      if not req or req["task_id"] != task_id or req["status"] != "pending":
         raise ValueError("no such pending request")
      if req["kind"] == "question":
         if not answer:
            raise ValueError("a question needs an answer")
         note = f"The operator answered your question: {answer}"
         self.store.decide_request(request_id, "answered", answer, note)
      elif decision == "deny":
         note = (f"The operator declined {req['kind']} '{req['payload'].get('value')}'.  "
                 "Continue without it, or stop and say what you need.")
         self.store.decide_request(request_id, "denied", None, note)
      elif decision in ("allow", "always_repo", "always"):
         value = req["payload"]["value"]
         scope = {"allow": (t["repo"], task_id), "always_repo": (t["repo"], None),
                  "always": (None, None)}[decision]
         self.store.add_grant(dawn_user_id, req["kind"], value, *scope)
         note = f"The operator allowed {req['kind']} '{value}'.  Retry it and continue."
         self.store.decide_request(request_id, "approved", decision, note)
      else:
         raise ValueError("decision must be allow, always_repo, always or deny")
      self.store.add_event(task_id, "reply", request_id=request_id, decision=decision)
      await self.deliver(task_id)
      return self.store.task(task_id)

   async def cancel(self, task_id: str, dawn_user_id: int) -> dict:
      t = self.owned(task_id, dawn_user_id)
      if t["status"] not in st.OPEN:
         return t
      # Marked first: a run ending while it is stopped must not read as completed.
      self.finish_cancelled(task_id)
      for p in self.store.pending_requests(task_id):
         self.store.decide_request(p["id"], "cancelled")
      run = self.running.get(task_id)
      if run:
         client = self.clients.get(task_id)
         if client:
            await self.interrupt(client)
         # The interrupted run usually ends by itself and records its cost.
         await asyncio.wait([run], timeout=10)
         if not run.done():
            run.cancel()
            await asyncio.wait([run], timeout=5)
      return self.store.task(task_id)

   async def publish(self, task_id: str, dawn_user_id: int, open_pr: bool, title: str | None,
                     head: str) -> dict:
      """`head` is the commit the user was shown; a branch that moved since is refused."""
      t = self.owned(task_id, dawn_user_id)
      if t["published"]:
         return json.loads(t["published"])  # already done: the same answer again
      if t["status"] != st.COMPLETED:
         raise ValueError("only a completed task is published")
      now, _ = await self.repos.agent_ref(t["repo"], task_id, t["base"])
      if head.lower() != now:
         raise ValueError(f"the branch moved since it was shown (it is now at {now[:12]})")
      if not self.cfg.github_token:
         raise ValueError("no GitHub token configured (AGENT_GATEWAY_GITHUB_TOKEN)")
      body = (t["summary"] or "") + f"\n\nTask: {t['task']}"
      res = await self.repos.publish(t["repo"], now, t["branch"], t["base"],
                                     self.cfg.github_token, open_pr,
                                     title or t["task"].splitlines()[0][:72], clip(body, 6000))
      self.store.update_task(task_id, published=json.dumps(res))
      self.store.add_event(task_id, "published", **res)
      return res
