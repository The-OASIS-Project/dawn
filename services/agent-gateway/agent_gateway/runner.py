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
import re
import secrets
import time

from claude_agent_sdk import (AssistantMessage, ClaudeAgentOptions, ClaudeSDKClient,
                              PermissionResultAllow, PermissionResultDeny, ResultMessage,
                              SystemMessage, UserMessage)

from . import policy, store as st
from .repos import GitError, Repos, pr_state, slug

log = logging.getLogger("agent-gateway")


TEXT_CAP = 2000
SUMMARY_CAP = 8000  # the agent's final report: what it did and how it checked it
PAYLOAD_CAP = 4000
# How often old clones are removed and published pull requests are checked.
HOUSEKEEPING_S = 3600

GUIDANCE = """You are running unattended for the operator, who reaches you through their \
assistant.  Commits in this task's worktree are pre-approved: commit your work when it is \
done and tested, with the git identity already configured (never set user.name or \
user.email yourself).  Never push or open pull requests; the operator publishes.  If \
something is denied or you need the operator, say so in one line and end your turn: you \
will be resumed with the answer.  Run each build or test command as its own Bash call, \
not piped and with nothing after it (no `; echo $?`, no `&& tail`), so the operator's \
gateway records its exit status; send its output to a log file and read the log in the \
next call, e.g. `make -C build tests-ci > build.log 2>&1`, then `tail build.log`."""

TEMPLATE = os.path.join(os.path.dirname(__file__), "CLAUDE.template.md")
FLOOR = os.path.join(os.path.dirname(__file__), "floor.json")

CBM_WRITE_TOOLS = ("index_repository", "delete_project", "manage_adr")

# A run is one turn: work left running in the background when it ends is lost,
# so waiting on it (Monitor) is off, as are background commands (claude-agent-exec).
NO_BACKGROUND_TOOLS = ("Monitor",)

# Said to the agent once when a run ends with no report.
REPORT_ASK = ("Your last turn ended without a report.  Finish any step still open (run it in "
              "the foreground), then report what you did and how you checked it.")


class Blocked(ValueError):
   """No room to run now (concurrency or the daily cap); retried when a run ends."""


def clip(text: str, cap: int = TEXT_CAP) -> str:
   return text if len(text) <= cap else text[:cap] + " [...]"


# The repo's tool cache under the agent's home (claude-agent-exec points pip,
# uv, npm, arduino-cli and ccache there): one directory per repo.
REPO_CACHE_SUBDIR = ".cache/repo"


def _model_id(value) -> str | None:
   """The model an init message names; None when absent or too long to be one."""
   text = str(value or "")
   return text if 0 < len(text) <= 80 else None


def floor_settings(agent_home: str, work_dir: str, worktree: str, domains: list[str],
                   repo: str = "", hidden: list[str] = ()) -> str:
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
   # The hidden directories as file-tool rules too, resolved by Claude Code
   # itself (relative paths, symlinks), not only by the permission callback.
   s["permissions"]["deny"] += [f"Read(/{p.rstrip('/')}/**)" for p in hidden
                                if f"Read(/{p.rstrip('/')}/**)" not in s["permissions"]["deny"]]
   s["sandbox"]["network"]["allowedDomains"] = domains
   fs = s["sandbox"]["filesystem"]
   # Other tasks' clones and other repos' caches are hidden; its own are not.
   # The permission rules bind only the file tools: what they hide that a
   # shell command could still read (the mirrors, DAWN's data) is hidden here.
   fs["denyRead"] = [p.replace("~/", home, 1) if p.startswith("~/") else p
                     for p in fs["denyRead"]] + [work_dir, home + REPO_CACHE_SUBDIR,
                                                 *hidden]
   fs["allowRead"] = [worktree]
   # /tmp: builds and tests expect to write there.  Run as agent-gateway.service
   # (PrivateTmp), it is the gateway's own /tmp, not the host's; a gateway
   # started by hand shares the host's.
   fs["allowWrite"] = ["/tmp"]
   if re.fullmatch(r"[A-Za-z0-9._-]+", repo) and repo not in (".", ".."):  # its tool cache
      fs["allowRead"].append(home + REPO_CACHE_SUBDIR + "/" + repo)
      fs["allowWrite"].append(home + REPO_CACHE_SUBDIR + "/" + repo)
   return json.dumps(s)


# A shell command that builds or runs tests: the gateway records whether it
# succeeded, so a report can say what was checked from what the gateway saw,
# not only from the agent's words.
# Checked against each segment of the command line (split at ; & | and
# parentheses), after any VAR=value prefixes.
CHECK_SEGMENT = re.compile(
   r"(make|cmake|ctest|ninja|meson|pytest|tox|python3?\s+-m\s+pytest|cargo\s+(build|test)|"
   r"go\s+(build|test)|(npm|pnpm|yarn)\s+(run\s+)?test)(\s|$)")
# A test program run by path (./build-debug/tests/test_x, tests/test_y.sh).
TEST_PROGRAM = re.compile(r"(^|/)tests?/[^/\s]*test[^/\s]*$|(^|/)test_[^/\s]+$")
CHECKS_REPORTED = 10  # the last build/test commands a completed event lists
CHECKS_SCANNED = 100  # recorded runs read to find each command's last


def check_command(tool: str, ti: dict) -> tuple[str, bool] | None:
   """For a Bash call that builds or runs tests: its command, and whether the
   line's exit status is that command's own (nothing after it, no || before it
   that could skip it, not run in the background).  Else None."""
   command = str(ti.get("command", "")) if tool == "Bash" else ""
   # Quoted text is never a command (a commit message saying "make test").
   bare = re.sub(r"'[^']*'|\"(?:\\.|[^\"\\])*\"", "''", command)
   bare = re.sub(r"\d*[<>]&\d*|&>>?", " ", bare)  # 2>&1, &>log: redirections, not separators
   parts = re.split(r"(&&|\|\||[;&|()\n])", bare)  # segments and the separators between
   segments = parts[0::2]
   for i, segment in enumerate(segments):
      words = segment.split()
      while words and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=\S*", words[0]):
         words.pop(0)
      if words and (CHECK_SEGMENT.match(" ".join(words)) or TEST_PROGRAM.search(words[0])):
         after = [p for p in parts[2 * i + 1:] if p.strip()]
         before = parts[1:2 * i:2]
         own = not after and "||" not in before and not ti.get("run_in_background")
         return command, own
   return None


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
      self.cloning: dict[str, asyncio.Task] = {}  # tasks whose clone is being made
      # Tasks a publish, close, revise or cleanup is changing: each refuses
      # (or skips) a task another holds, as each awaits partway.
      self.busy: set[str] = set()
      self.background: set[asyncio.Task] = set()

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
      """Record the task and return at once: its clone is made in the background
      (prepare_and_launch), so the caller hears within a second and never times
      out on a slow clone.  A repeated `client_ref` returns the task it started.
      Nothing is awaited between the lookup and the insert, so two calls with one
      `client_ref` can't both make a task."""
      if client_ref:
         existing = self.store.task_by_ref(dawn_user_id, client_ref)
         if existing:
            return existing
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
      budget = min(budget_usd or self.cfg.task_budget_usd, self.cfg.task_budget_usd * 4)
      self.store.add_task(id=task_id, dawn_user_id=dawn_user_id, agent="claude", repo=repo,
                          base=base, branch=branch, worktree=self.repos.workdir(task_id),
                          task=task, budget_usd=budget, client_ref=client_ref)
      self.starting += 1  # holds the slot across the clone
      self.cloning[task_id] = asyncio.get_running_loop().create_task(
         self.prepare_and_launch(task_id))
      return self.store.task(task_id)

   async def cancel_ref(self, dawn_user_id: int, client_ref: str) -> dict:
      """Cancel the task started as `client_ref` (one still being cloned too)."""
      t = self.store.task_by_ref(dawn_user_id, client_ref)
      if not t:
         raise ValueError("no such task")
      return await self.cancel(t["id"], dawn_user_id)

   async def prepare_and_launch(self, task_id: str) -> None:
      """Clone the task's repo, then start its first run, unless it was
      cancelled meanwhile.  A clone that fails fails the task."""
      t = self.store.task(task_id)
      try:
         await self.prune()
         worktree = await self.repos.prepare(t["repo"], t["base"], t["branch"], task_id)
         self.store.update_task(task_id, worktree=worktree)
      except Exception as e:
         # git's own words stay in the log: they may name the remote's URL.
         log.warning("task %s: could not prepare its clone: %s", task_id, e)
         try:
            await self.repos.remove(task_id)
         except Exception as e2:
            log.warning("could not remove the half-made worktree %s: %s", task_id, e2)
         if self.store.task(task_id)["status"] in st.OPEN:
            self.fail(task_id, "could not prepare its clone (the gateway's log says why)")
         return
      finally:
         # A cancel stops the clone (its git is killed): the slot frees at once.
         self.cloning.pop(task_id, None)
         self.starting -= 1
         self.soon(self.retry_blocked())  # a start that runs nothing frees its slot here
      if self.store.task(task_id)["status"] not in st.OPEN:
         return  # cancelled while it was cloned: never launched
      self.store.add_event(task_id, "started", repo=t["repo"], branch=t["branch"],
                           task=clip(t["task"]))
      self.launch(task_id, t["task"])

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
         t = self.store.task(task_id)
         if t["status"] in st.OPEN or t["updated"] >= before or task_id in self.busy:
            continue  # revised since the list was read, or being changed
         self.busy.add(task_id)
         try:
            if await self.collect_for_close(t):
               continue  # uncommitted edits: kept until the task is closed
            await self.repos.remove(task_id)
            self.store.update_task(task_id, worktree="")
         except Exception as e:  # the clone stays: its work couldn't be kept, or removed
            log.warning("could not remove worktree of %s: %s", task_id, e)
         finally:
            self.busy.discard(task_id)

   async def housekeeping(self) -> None:
      """Hourly: old clones go, and a task whose pull request was merged is closed."""
      while True:
         try:
            await self.prune()
            await self.watch_merges()
         except Exception as e:
            log.warning("housekeeping: %s", e)
         await asyncio.sleep(HOUSEKEEPING_S)

   async def watch_merges(self) -> None:
      if not self.cfg.github_token:
         return
      for t in self.store.tasks(status=st.COMPLETED, limit=None):
         try:
            await self.watch_merge(t)
         except Exception as e:  # one task's trouble doesn't stop the others' checks
            log.warning("merge watch, task %s: %s", t["id"], e)

   async def watch_merge(self, t: dict) -> None:
      record = t["published"]
      prev = json.loads(record) if record else None
      if t["closed"] or not prev or not prev.get("pr_url") or prev.get("merged_seen"):
         return
      state = await asyncio.to_thread(pr_state, prev["pr_url"], self.cfg.github_token)
      t = self.store.task(t["id"])  # what changed while GitHub answered
      if (state != "merged" or t["closed"] or t["status"] != st.COMPLETED
            or t["published"] != record or t["id"] in self.busy):
         return  # a newer publish (another PR) or a revise: this answer is about the past
      self.busy.add(t["id"])  # no revise between the count and the close
      try:
         left = await self.unpublished_commits(t)
         dirty = (await self.repos.uncommitted(t["id"])
                  if t["worktree"] and os.path.isdir(t["worktree"]) else 0)
         if left == 0 and dirty == 0:
            await self.close_held(t, "merged")
         else:
            # Newer work isn't in what merged: said once, nothing dropped.
            self.store.set_published(t["id"], json.dumps({**prev, "merged_seen": True}))
            self.store.add_event(t["id"], "merged", pr_url=prev["pr_url"], unpublished=left,
                                 uncommitted=dirty)
      finally:
         self.busy.discard(t["id"])

   async def unpublished_commits(self, t: dict) -> int:
      prev = json.loads(t["published"]) if t["published"] else None
      return await self.repos.unpublished(t["repo"], t["id"], t["base"],
                                          prev.get("head") if prev else None)

   async def close(self, task_id: str, dawn_user_id: int, head: str | None) -> dict:
      """Drop a finished task's work: its clone and its collected branch.  Without
      `head`, only when nothing would be lost (no unpublished commits and no
      uncommitted edits); with it, only while the branch is still at that commit
      (the one the user agreed to lose; "" when only edits would be)."""
      t = self.owned(task_id, dawn_user_id)
      if t["closed"]:
         return {"closed": True, "task": t}
      if t["status"] in st.OPEN:
         raise ValueError("the task is still running; cancel it first")
      if task_id in self.cloning or task_id in self.running:
         raise ValueError("the task is still stopping; try the close again in a moment")
      if task_id in self.busy:
         raise ValueError("the task is busy (being published, revised or cleaned up); try again")
      self.busy.add(task_id)  # held across every await below: no revise or publish slips in
      try:
         dirty = await self.collect_for_close(t)
         left = await self.unpublished_commits(t)
         if left or dirty:
            now = (await self.repos.agent_ref(t["repo"], task_id, t["base"]))[0] if left else ""
            if head is None:
               return {"closed": False, "unpublished_commits": left, "uncommitted": dirty,
                       "head": now}
            if head.lower() != now:
               raise ValueError(f"the branch moved since it was shown (it is now at "
                                f"{now[:12] or 'no commits'})")
         await self.close_held(t, "user")
      finally:
         self.busy.discard(task_id)
      return {"closed": True, "task": self.store.task(task_id)}

   async def collect_for_close(self, t: dict) -> int:
      """The clone's commits into the mirror before they're counted (a run that
      was cancelled, or stopped late, never collected its own), and its
      uncommitted edits, counted: both are lost with the clone.  An empty clone
      keeps what was collected before (a cancel is when its state is least
      trusted).  Work that can't be read refuses the close."""
      if (not t["worktree"] or not os.path.isdir(t["worktree"])
            or not self.store.task_events(t["id"], "started", 1)):
         return 0  # no clone, or one cancelled before it was made
      try:
         if t["status"] == st.COMPLETED:  # its commits were collected as it completed
            return await self.repos.uncommitted(t["id"])
         out = await self.repos.collect(t["repo"], t["id"], t["branch"], t["base"],
                                        keep_on_empty=True)
         return out["uncommitted"]
      except (GitError, KeyError) as e:  # KeyError: its repo left the config
         log.warning("task %s: could not collect its branch before closing: %s", t["id"], e)
         raise ValueError("the task's work couldn't be read, so nothing was closed") from e

   async def close_held(self, t: dict, reason: str) -> None:
      """Drop the work; the caller holds the task in `busy`."""
      await self.repos.remove(t["id"])
      await self.repos.drop(t["repo"], t["id"])
      self.store.update_task(t["id"], worktree="")
      self.store.set_closed(t["id"])
      self.store.add_event(t["id"], "closed", reason=reason)

   # --- one run ----------------------------------------------------------

   def hidden_paths(self) -> list[str]:
      """What no task may read beyond the other tasks' clones: the gateway's
      own files and the operator's [paths] deny_read."""
      return [self.cfg.state_dir, self.cfg.mirrors_dir, *self.cfg.deny_read]

   def permission_handler(self, task: dict, granted_tools: list[str]):
      """Claude Code's permission callback for one run: allow, deny, or record a
      request for the operator and stop the run."""

      hidden = [self.cfg.work_dir, *self.hidden_paths()]

      async def can_use_tool(tool, tool_input, ctx):
         d = policy.decide(tool, tool_input, task["worktree"], granted_tools, hidden)
         if d.allow:
            return PermissionResultAllow()
         if d.ask_kind and self.store.task(task["id"])["status"] not in st.OPEN:
            return PermissionResultDeny(message="the task was stopped")  # no request left behind
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
      # No settings are loaded from the repo or the agent's home (a repo's
      # .claude/settings.json could pre-approve calls around the policy), so
      # the repo's CLAUDE.md comes from here, as the base branch has it.
      rules = await self.repos.claude_md(task["repo"], task["base"])
      if rules is not None:
         system = GUIDANCE + "\n\nThe repository's CLAUDE.md (its rules):\n" + rules
      else:
         with open(TEMPLATE) as f:
            system = GUIDANCE + "\n\nThis repository has no CLAUDE.md.  Follow these rules:\n" + \
               f.read()
      mcp = {"cbm": {"type": "sse", "url": self.cfg.cbm_url}} if self.cfg.cbm_url else {}
      return ClaudeAgentOptions(
         cwd=task["worktree"], cli_path=self.cfg.claude_cli, resume=task["session_id"],
         model=self.cfg.model, permission_mode="acceptEdits",
         setting_sources=[], strict_mcp_config=True, mcp_servers=mcp,
         disallowed_tools=[f"mcp__cbm__{t}" for t in CBM_WRITE_TOOLS] + list(NO_BACKGROUND_TOOLS),
         settings=floor_settings(self.cfg.agent_home, self.cfg.work_dir, task["worktree"],
                                 domains, task["repo"],
                                 self.hidden_paths()),
         # The CLI restores a session's running cost on resume, so its budget
         # check is against the whole task: give it the whole budget.
         max_budget_usd=task["budget_usd"],
         can_use_tool=self.permission_handler(task, granted_tools),
         system_prompt={"type": "preset", "preset": "claude_code", "append": system},
         env={"DAWN_AGENT_AUTH": self.cfg.auth, "DAWN_AGENT_REPO": task["repo"],
              **({"DAWN_AGENT_CLAUDE": self.cfg.claude_bin} if self.cfg.claude_bin else {})},
         stderr=lambda line: log.debug("claude[%s]: %s", task["id"], line))

   async def run(self, task_id: str, prompt: str) -> None:
      task = self.store.task(task_id)
      result, costed = None, False
      checks: dict[str, tuple[str, bool]] = {}  # a build/test call's id -> (command, own status)
      try:
         async with ClaudeSDKClient(options=await self.options(task)) as client:
            self.clients[task_id] = client
            await client.query(prompt)
            async for m in client.receive_response():
               if isinstance(m, SystemMessage) and m.subtype == "init":
                  # Saved at once, so a run cut off by a gateway restart resumes; the
                  # model is what Claude Code reports it runs on (config may leave it
                  # to Claude Code's default).
                  self.store.update_task(task_id, session_id=m.data.get("session_id"),
                                         model=_model_id(m.data.get("model")))
               elif isinstance(m, AssistantMessage):
                  for b in m.content:
                     if getattr(b, "text", None):
                        self.store.add_event(task_id, "progress", text=clip(b.text))
                     elif getattr(b, "name", None):
                        self.store.add_event(task_id, "tool", tool=b.name,
                                             brief=brief(b.name, b.input))
                        check = check_command(b.name, b.input)
                        if check:
                           checks[b.id] = check
               elif isinstance(m, UserMessage) and isinstance(m.content, list):
                  for b in m.content:
                     check = checks.pop(getattr(b, "tool_use_id", None), None)
                     if check:
                        command, own = check
                        # Its outcome only when the status is the command's own.
                        ok = (not getattr(b, "is_error", False)) if own else None
                        self.store.add_event(task_id, "check", command=clip(command, 300), ok=ok)
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

   def ended_the_report_ask(self, task_id: str) -> bool:
      """Whether the run that just ended is the one started by asking for a
      report: the only resume since the last ask (a later reply or restart
      resumes it again, and that run is asked anew)."""
      asks = self.store.task_events(task_id, "report_asked", 1)
      resumes = self.store.task_events(task_id, "resumed", 2)
      if not asks or not resumes or resumes[-1]["seq"] < asks[0]["seq"]:
         return False
      return len(resumes) < 2 or resumes[0]["seq"] < asks[0]["seq"]

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
      if not (r.result or "").strip():
         # No report: ask once; the run that was asked ending silent too fails.
         if self.ended_the_report_ask(task_id):
            self.fail(task_id, "the agent ended without a report")
            return
         self.store.add_event(task_id, "report_asked")
         self.store.add_note(task_id, REPORT_ASK)
         self.store.update_task(task_id, status=st.INPUT_REQUIRED)  # retried if no room
         await self.deliver(task_id)
         return
      self.store.update_task(task_id, status=st.COMPLETED,
                             summary=clip(r.result or "", SUMMARY_CAP))
      # Each command's last run: a build that failed and was then fixed counts
      # as fixed.
      last: dict[str, object] = {}
      revised = self.store.task_events(task_id, "revised", 1)
      since = revised[0]["seq"] if revised else 0  # this work's checks, not an earlier run's
      for e in (e for e in self.store.task_events(task_id, "check", CHECKS_SCANNED)
                if e["seq"] > since):
         last.pop(e["data"]["command"], None)
         last[e["data"]["command"]] = e["data"]["ok"]
      checks = [{"command": c, "ok": ok} for c, ok in list(last.items())[-CHECKS_REPORTED:]]
      self.store.add_event(task_id, "completed", summary=clip(r.result or "", SUMMARY_CAP),
                           branch=task["branch"], session_id=task["session_id"],
                           cost_usd=self.store.task(task_id)["cost_usd"], checks=checks,
                           **outcome)

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
      clone = self.cloning.get(task_id)
      if clone:
         clone.cancel()
         await asyncio.wait([clone], timeout=5)
         if clone.done() and self.cloning.get(task_id) is clone:
            # Cancelled before its first step, it never ran its own finally.
            del self.cloning[task_id]
            self.starting -= 1
            self.soon(self.retry_blocked())
      run = self.running.get(task_id)
      if run:
         client = self.clients.get(task_id)
         if client:
            await self.interrupt(client)
            # The interrupted run usually ends by itself and records its cost.
            await asyncio.wait([run], timeout=10)
         if not run.done():  # or the agent isn't reached yet: stop before it is
            run.cancel()
            await asyncio.wait([run], timeout=5)
         if run.done() and self.running.get(task_id) is run:
            # Cancelled before its first step, it never ran its own finally.
            del self.running[task_id]
            self.clients.pop(task_id, None)
            self.soon(self.retry_blocked())
      return self.store.task(task_id)

   async def revise(self, task_id: str, dawn_user_id: int, instructions: str,
                    budget_usd: float | None, revise_ref: str | None) -> dict:
      """More work for a finished task: its session resumes on the same branch and
      clone with `instructions`.  A repeated `revise_ref` returns the task and adds
      nothing (safe to retry)."""
      t = self.owned(task_id, dawn_user_id)
      last = self.store.task_events(task_id, "revised", 1)
      if revise_ref and last and last[0]["data"].get("revise_ref") == revise_ref:
         return t
      if not instructions.strip():
         raise ValueError("empty instructions")
      if t["status"] not in (st.COMPLETED, st.FAILED):
         raise ValueError(f"the task is {t['status']}; only a finished one takes more work")
      if not t["session_id"]:
         raise ValueError("the task has no session to resume; start a new task")
      if t["closed"]:
         raise ValueError("the task was closed; start a new task")
      if task_id in self.busy:
         raise ValueError("the task is busy (being published or cleaned up); try again")
      if not t["worktree"] or not os.path.isdir(t["worktree"]):
         raise ValueError("the task's clone is gone; start a new task")
      if budget_usd is not None and budget_usd <= 0:
         raise ValueError("budget_usd must be positive")
      added = min(budget_usd or self.cfg.task_budget_usd, self.cfg.task_budget_usd * 4)
      # Before deliver(): it refuses a task whose spend has reached its budget.
      self.store.update_task(task_id, status=st.INPUT_REQUIRED, summary=None, error=None,
                             budget_usd=t["budget_usd"] + added)
      # Its seq scopes the next completion's build and test checks to this work.
      self.store.add_event(task_id, "revised", revise_ref=revise_ref, budget_added=added)
      self.store.add_note(task_id, "The operator asks for more work on this task: " +
                          instructions)
      await self.deliver(task_id)
      return self.store.task(task_id)

   async def publish(self, task_id: str, dawn_user_id: int, open_pr: bool, title: str | None,
                     head: str) -> dict:
      """`head` is the commit the user was shown; a branch that moved since is refused."""
      t = self.owned(task_id, dawn_user_id)
      if t["closed"]:
         raise ValueError("the task was closed; its work is gone from the gateway")
      if task_id in self.busy:
         raise ValueError("the task is busy (being closed or cleaned up); try again")
      self.busy.add(task_id)
      try:
         return await self.publish_held(t, open_pr, title, head)
      finally:
         self.busy.discard(task_id)

   async def publish_held(self, t: dict, open_pr: bool, title: str | None, head: str) -> dict:
      task_id = t["id"]
      prev = json.loads(t["published"]) if t["published"] else None
      if prev and t["status"] == st.COMPLETED and (prev.get("pr_url") or not open_pr):
         now, _ = await self.repos.agent_ref(t["repo"], task_id, t["base"])
         if prev.get("head") == now:
            return prev  # already done: the same answer again (an older record has no
                         # head: pushed again, harmless when nothing moved)
      if t["status"] != st.COMPLETED:
         raise ValueError("only a completed task is published")
      now, _ = await self.repos.agent_ref(t["repo"], task_id, t["base"])
      if head.lower() != now:
         raise ValueError(f"the branch moved since it was shown (it is now at {now[:12]})")
      if not self.cfg.github_token:
         raise ValueError("no GitHub token configured (AGENT_GATEWAY_GITHUB_TOKEN)")
      # The agent's summary only: the task text was written to the agent, not for
      # the public.
      body = (t["summary"] or "") + "\n\nMade by a coding agent for DAWN."
      # Revised since it was published: push the new commits; its PR stays while
      # it is open (a merged or closed one gets a new PR).
      pr = prev.get("pr_url") if prev else None
      earlier = None  # what a refused new PR falls back to: one whose state couldn't be read
      if pr:
         state = await asyncio.to_thread(pr_state, pr, self.cfg.github_token)
         if state != "open":
            earlier = pr if state is None else None
            pr = None
      res = await self.repos.publish(t["repo"], now, t["branch"], t["base"],
                                     self.cfg.github_token, open_pr and not pr,
                                     title or t["task"].splitlines()[0][:72], clip(body, 6000),
                                     earlier)
      if pr:
         res["pr_url"] = pr
      res["head"] = now
      self.store.update_task(task_id, published=json.dumps(res))
      self.store.add_event(task_id, "published", **res)
      return res
