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
# The gateway's MCP interface: short calls only (stateless streamable HTTP,
# JSON replies), each needing DAWN's bearer token.  DAWN pulls events with a
# long-poll; nothing here ever calls DAWN.

import functools
import hmac
import json

import os

from importlib.metadata import version as package_version

from claude_agent_sdk._cli_version import __cli_version__
from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.server.transport_security import TransportSecuritySettings
from starlette.responses import JSONResponse

from .repos import GitError

MAX_WAIT_S = 25
UPDATE_STATUS = "update-status.json"


def public(t: dict) -> dict:
   keys = ("id", "dawn_user_id", "agent", "repo", "base", "branch", "task", "status",
           "session_id", "budget_usd", "cost_usd", "summary", "error", "client_ref",
           "created", "updated")
   out = {k: t[k] for k in keys}
   out["published"] = json.loads(t["published"]) if t["published"] else None
   return out


def explained(fn):
   """Refusals reach DAWN with their reason; other errors stay generic."""

   @functools.wraps(fn)
   async def wrapper(*args, **kwargs):
      try:
         return await fn(*args, **kwargs)
      except (ValueError, GitError) as e:
         raise ToolError(str(e)) from e
   return wrapper


def build(runner, cfg) -> MCPServer:
   mcp = MCPServer("agent-gateway", instructions=(
      "Runs coding agents for DAWN.  Every call names the DAWN user it acts for."))
   store = runner.store

   def report_update() -> None:
      """The updater (root, from its timer) leaves its result in a file; each
      new result becomes one system event."""
      path = os.path.join(cfg.state_dir, UPDATE_STATUS)
      try:
         with open(path) as f:
            raw = f.read()
         status = json.loads(raw)
      except (OSError, ValueError):
         return
      if store.meta("update_reported") != raw:
         store.set_meta("update_reported", raw)
         store.add_event("", "update", **{k: str(v)[:500] for k, v in status.items()})
   tool = lambda fn: mcp.tool()(explained(fn))  # noqa: E731

   @tool
   async def agents_list() -> dict:
      """Agents this gateway runs and the repos it may work in (name, url, base)."""
      return {"agents": ["claude"], "claude_code": __cli_version__,
              "sdk": package_version("claude-agent-sdk"),
              "repos": [{"name": n, **r} for n, r in sorted(cfg.repos.items())],
              "running": len(runner.running), "max_concurrent": cfg.max_concurrent_tasks}

   @tool
   async def task_start(dawn_user_id: int, repo: str, task: str,
                        budget_usd: float | None = None, agent: str = "claude",
                        client_ref: str | None = None) -> dict:
      """Start a task: a new branch and worktree in `repo`, and an agent working on it.
      Returns once the task is recorded; its clone is made in the background (a clone
      that fails ends the task as failed).  A repeated `client_ref` returns the task it
      already started (safe to retry)."""
      if agent != "claude":
         raise ValueError("only the 'claude' agent is available")
      return public(await runner.start(dawn_user_id, repo, task, budget_usd, client_ref))

   @tool
   async def task_events(after_seq: int = 0, wait_s: float = 20) -> dict:
      """Events after `after_seq`, every task, plus system events (no task_id); waits up
      to `wait_s` (max 25) for one."""
      report_update()
      events = await store.wait_events(after_seq, min(max(wait_s, 0), MAX_WAIT_S))
      # With none, the newest there is: below after_seq, the events started over.
      return {"events": events, "last_seq": events[-1]["seq"] if events else store.latest_seq()}

   def task_of(dawn_user_id: int, task_id: str | None, client_ref: str | None) -> dict:
      """A task named by its id or by the caller's client_ref; the user's own only."""
      if client_ref:
         t = store.task_by_ref(dawn_user_id, client_ref)
         if not t:
            raise ValueError("no such task")
         return t
      if not task_id:
         raise ValueError("name the task by task_id or client_ref")
      return runner.owned(task_id, dawn_user_id)

   @tool
   async def task_list(dawn_user_id: int, task_id: str | None = None,
                       client_ref: str | None = None) -> dict:
      """One task (by task_id or client_ref) with its pending requests, or the user's
      recent tasks.  A client_ref with no task: task null."""
      if client_ref and not task_id and not store.task_by_ref(dawn_user_id, client_ref):
         return {"task": None, "pending": []}
      if task_id or client_ref:
         t = task_of(dawn_user_id, task_id, client_ref)
         return {"task": public(t), "pending": store.pending_requests(t["id"])}
      return {"tasks": [public(t) for t in store.tasks(dawn_user_id)]}

   @tool
   async def task_revise(dawn_user_id: int, instructions: str, task_id: str | None = None,
                         client_ref: str | None = None, budget_usd: float | None = None,
                         revise_ref: str | None = None) -> dict:
      """More work for a completed or failed task: its agent session resumes on the same
      branch and clone with `instructions`; `budget_usd` is added to its budget (default:
      the configured task budget).  A repeated `revise_ref` returns the task and adds
      nothing (safe to retry)."""
      t = task_of(dawn_user_id, task_id, client_ref)
      return public(await runner.revise(t["id"], dawn_user_id, instructions, budget_usd,
                                        revise_ref))

   @tool
   async def task_reply(dawn_user_id: int, request_id: int, decision: str = "",
                        answer: str | None = None, task_id: str | None = None,
                        client_ref: str | None = None) -> dict:
      """Answer a pending request.  decision: allow (this task), always_repo, always, or
      deny; a question takes `answer` instead.  Resumes the task when nothing else is
      pending and there is room."""
      t = task_of(dawn_user_id, task_id, client_ref)
      return public(await runner.reply(t["id"], dawn_user_id, request_id, decision, answer))

   @tool
   async def task_cancel(dawn_user_id: int, task_id: str | None = None,
                         client_ref: str | None = None) -> dict:
      """Stop a task (one still being cloned too).  Its branch and worktree are kept."""
      if client_ref and not task_id:
         return public(await runner.cancel_ref(dawn_user_id, client_ref))
      t = task_of(dawn_user_id, task_id, client_ref)
      return public(await runner.cancel(t["id"], dawn_user_id))

   @tool
   async def task_publish(dawn_user_id: int, head: str, task_id: str | None = None,
                          client_ref: str | None = None, open_pr: bool = True,
                          title: str | None = None) -> dict:
      """Push a completed task's agent/* branch and optionally open a pull request.  DAWN
      calls this only after the user confirmed, passing the head commit they were shown;
      a branch that moved since is refused.  A second call returns the first result while
      nothing moved; after a revise it pushes the new commits, keeping an open PR."""
      t = task_of(dawn_user_id, task_id, client_ref)
      return await runner.publish(t["id"], dawn_user_id, open_pr, title, head)

   # --- reading the work: from the gateway's mirror, never the agent's clone ---

   @tool
   async def task_changes(dawn_user_id: int, task_id: str | None = None,
                          client_ref: str | None = None) -> dict:
      """The task's work so far: changed files with +/- counts, and its commits."""
      t = task_of(dawn_user_id, task_id, client_ref)
      return await runner.repos.changes(t["repo"], t["id"], t["base"])

   @tool
   async def task_file(dawn_user_id: int, path: str, view: str = "diff",
                       task_id: str | None = None, client_ref: str | None = None,
                       old_path: str | None = None) -> dict:
      """One file of the task's work: view diff, new (after) or old (before).  A renamed
      file's diff takes its old name as old_path."""
      t = task_of(dawn_user_id, task_id, client_ref)
      return await runner.repos.file(t["repo"], t["id"], t["base"], path, view, old_path)

   @tool
   async def task_tree(dawn_user_id: int, path: str = "", task_id: str | None = None,
                       client_ref: str | None = None) -> dict:
      """One folder of the repo at the task's latest collected commit."""
      t = task_of(dawn_user_id, task_id, client_ref)
      return await runner.repos.tree(t["repo"], t["id"], t["base"], path)

   @tool
   async def grants_list(dawn_user_id: int) -> dict:
      """The user's standing grants (domains and tools allowed without asking)."""
      return {"grants": store.grants(dawn_user_id)}

   @tool
   async def grant_revoke(dawn_user_id: int, grant_id: int) -> dict:
      """Remove a standing grant."""
      if not store.revoke_grant(grant_id, dawn_user_id):
         raise ValueError("no such grant")
      return {"revoked": grant_id}

   return mcp


class BearerAuth:
   """Every request carries DAWN's token; compared in constant time."""

   def __init__(self, app, token: str):
      self.app = app
      self.expected = f"Bearer {token}".encode()

   async def __call__(self, scope, receive, send):
      if scope["type"] == "http":
         got = dict(scope["headers"]).get(b"authorization", b"")
         if not hmac.compare_digest(got, self.expected):
            await JSONResponse({"error": "unauthorized"}, status_code=401)(scope, receive, send)
            return
      await self.app(scope, receive, send)


def app(runner, cfg):
   mcp = build(runner, cfg)
   hosts = [f"{cfg.host}:{cfg.port}", f"localhost:{cfg.port}"]
   security = TransportSecuritySettings(enable_dns_rebinding_protection=True,
                                        allowed_hosts=hosts, allowed_origins=[])
   inner = mcp.streamable_http_app(stateless_http=True, json_response=True,
                                   transport_security=security, host=cfg.host)
   return BearerAuth(inner, cfg.token)
