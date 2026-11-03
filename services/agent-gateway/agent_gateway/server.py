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

from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.server.transport_security import TransportSecuritySettings
from starlette.responses import JSONResponse

from .repos import GitError

MAX_WAIT_S = 25


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
   tool = lambda fn: mcp.tool()(explained(fn))  # noqa: E731

   @tool
   async def agents_list() -> dict:
      """Agents this gateway runs and the repos it may work in (name, url, base)."""
      return {"agents": ["claude"],
              "repos": [{"name": n, **r} for n, r in sorted(cfg.repos.items())],
              "running": len(runner.running), "max_concurrent": cfg.max_concurrent_tasks}

   @tool
   async def task_start(dawn_user_id: int, repo: str, task: str,
                        budget_usd: float | None = None, agent: str = "claude",
                        client_ref: str | None = None) -> dict:
      """Start a task: a new branch and worktree in `repo`, and an agent working on it.
      A repeated `client_ref` returns the task it already started (safe to retry)."""
      if agent != "claude":
         raise ValueError("only the 'claude' agent is available")
      return public(await runner.start(dawn_user_id, repo, task, budget_usd, client_ref))

   @tool
   async def task_events(after_seq: int = 0, wait_s: float = 20) -> dict:
      """Events after `after_seq`, every task; waits up to `wait_s` (max 25) for one."""
      events = await store.wait_events(after_seq, min(max(wait_s, 0), MAX_WAIT_S))
      return {"events": events, "last_seq": events[-1]["seq"] if events else after_seq}

   @tool
   async def task_list(dawn_user_id: int, task_id: str | None = None) -> dict:
      """One task with its pending requests, or the user's recent tasks."""
      if task_id:
         t = runner.owned(task_id, dawn_user_id)
         return {"task": public(t), "pending": store.pending_requests(task_id)}
      return {"tasks": [public(t) for t in store.tasks(dawn_user_id)]}

   @tool
   async def task_reply(dawn_user_id: int, task_id: str, request_id: int, decision: str = "",
                        answer: str | None = None) -> dict:
      """Answer a pending request.  decision: allow (this task), always_repo, always, or
      deny; a question takes `answer` instead.  Resumes the task when nothing else is
      pending and there is room."""
      return public(await runner.reply(task_id, dawn_user_id, request_id, decision, answer))

   @tool
   async def task_cancel(dawn_user_id: int, task_id: str) -> dict:
      """Stop a task.  Its branch and worktree are kept."""
      return public(await runner.cancel(task_id, dawn_user_id))

   @tool
   async def task_publish(dawn_user_id: int, task_id: str, open_pr: bool = True,
                          title: str | None = None) -> dict:
      """Push a completed task's agent/* branch and optionally open a pull request.  DAWN
      calls this only after the user confirmed.  A second call returns the first result."""
      return await runner.publish(task_id, dawn_user_id, open_pr, title)

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
