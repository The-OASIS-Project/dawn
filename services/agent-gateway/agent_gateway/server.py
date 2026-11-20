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

import asyncio
import functools
import hmac
import json

import os
import time

from importlib.metadata import version as package_version

from claude_agent_sdk._cli_version import __cli_version__
from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.server.transport_security import TransportSecuritySettings
from starlette.responses import JSONResponse

from . import github, review
from .github import GitError
from .runner import PR_FOOTER, body_sha256, pr_body

MAX_WAIT_S = 25
UPDATE_STATUS = "update-status.json"


def public(t: dict) -> dict:
   keys = ("id", "dawn_user_id", "agent", "repo", "base", "branch", "task", "status",
           "session_id", "budget_usd", "cost_usd", "summary", "error", "client_ref",
           "model", "closed", "created", "updated")
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
   def update_problem() -> dict | None:
      """The updater's last result when it left the operator something to know."""
      path = os.path.join(cfg.state_dir, UPDATE_STATUS)
      try:
         with open(path) as f:
            status = json.load(f)
         age = time.time() - os.path.getmtime(path)
      except (OSError, ValueError):
         return None
      if status.get("result") not in ("failed", "rolled_back", "needs_review", "error"):
         return None
      # Versions are the SDK's (the updater installs SDK releases by number).
      if status.get("to") and status["to"] == package_version("claude-agent-sdk"):
         return None  # installed since (by hand)
      if status.get("result") == "error" and age > 86400:
         return None  # a check that failed once; the next ones passed quietly
      return {k: str(status.get(k, ""))[:200] for k in ("result", "to", "detail")}
   tool = lambda fn: mcp.tool()(explained(fn))  # noqa: E731

   @tool
   async def agents_list() -> dict:
      """Agents this gateway runs and the repos it may work in (name, url, base)."""
      return {"agents": ["claude"], "claude_code": __cli_version__, "update": update_problem(),
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

   def closed_check(t: dict) -> None:
      if t["closed"]:
         raise ValueError("the task was closed; its work is gone from the gateway")

   @tool
   async def task_list(dawn_user_id: int, task_id: str | None = None,
                       client_ref: str | None = None) -> dict:
      """One task (by task_id or client_ref) with its pending requests, or the user's
      recent tasks.  A client_ref with no task: task null."""
      if client_ref and not task_id and not store.task_by_ref(dawn_user_id, client_ref):
         return {"task": None, "pending": []}
      if task_id or client_ref:
         t = task_of(dawn_user_id, task_id, client_ref)
         try:
            left = 0 if t["closed"] else await runner.unpublished_commits(t)
         except GitError:
            left = None  # unknown (the mirror can't be read): the task still answers
         return {"task": public(t), "pending": store.pending_requests(t["id"]),
                 "unpublished_commits": left, "run_seq": store.run_seq(t["id"])}
      return {"tasks": [public(t) for t in store.tasks(dawn_user_id)]}

   @tool
   async def task_revise(dawn_user_id: int, instructions: str, task_id: str | None = None,
                         client_ref: str | None = None, budget_usd: float | None = None,
                         revise_ref: str | None = None, merge_base: bool = False) -> dict:
      """More work for a completed or failed task: its agent session resumes on the same
      branch and clone with `instructions`; `budget_usd` is added to its budget (default:
      the configured task budget).  A repeated `revise_ref` returns the task and adds
      nothing (safe to retry).  A task whose pull request is open gets its open feedback
      too (its agent reads it with its pr_feedback tool): feedback has the counts
      (threads, checks: failing required ones; null with no open pull request) and
      feedback_error says why it couldn't be read.  `merge_base`: the gateway merges the
      base branch into the task's branch first (the agent's sandbox can't write paths
      like .claude/, which a base may change): merge says what it did (merged: a commit
      or "up to date"; or conflicts, left in progress for the agent).  A conflict in a
      path the sandbox protects, or a clone with uncommitted changes, refuses the revise
      with nothing changed."""
      t = task_of(dawn_user_id, task_id, client_ref)
      t = await runner.revise(t["id"], dawn_user_id, instructions, budget_usd, revise_ref,
                              merge_base)
      return {**public(t), **runner.revise_feedback(t["id"])}

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
   async def task_close(dawn_user_id: int, task_id: str | None = None,
                        client_ref: str | None = None, head: str | None = None) -> dict:
      """Drop a finished task's work (its clone and branch) once the user is done
      with it.  Without `head`: closes only when nothing would be lost, else answers
      closed false with unpublished_commits and the head to confirm.  With `head`:
      closes only while the branch is still at that commit."""
      t = task_of(dawn_user_id, task_id, client_ref)
      res = await runner.close(t["id"], dawn_user_id, head)
      if not res.get("closed"):
         res["branch"] = t["branch"]
      if "task" in res:
         res["task"] = public(res["task"])
      return res

   @tool
   async def task_publish(dawn_user_id: int, head: str, task_id: str | None = None,
                          client_ref: str | None = None, open_pr: bool = True,
                          title: str | None = None,
                          expect_body_sha256: str | None = None,
                          pr_title: str | None = None,
                          pr_description: str | None = None,
                          replaces_body_sha256: str | None = None,
                          edit_open_pr: bool = False) -> dict:
      """Push a completed task's agent/* branch and optionally open a pull request.  DAWN
      calls this only after the user confirmed, passing the head commit they were shown;
      a branch that moved since is refused, and with `expect_body_sha256` (task_changes'
      pr_body_sha256) so is a description that changed since.  `pr_title` and
      `pr_description` are the user's own text (the description gets pr_footer appended):
      for a new pull request, instead of the agent's; when the task's pull request is
      open, they replace its title and/or description after any push (with nothing new
      to push, only they change), the description only while it is still the one shown
      (`replaces_body_sha256`: task_changes' open_pr.body_sha256); with `edit_open_pr`
      (the user was shown such an edit), one merged or closed since is refused, never
      replaced by a new one.  A description ending in pr_footer gets it once.  The answer's
      pr_updated names what was replaced.  A second call returns the first result while
      nothing moved; after a revise it pushes the new commits, keeping an open PR."""
      t = task_of(dawn_user_id, task_id, client_ref)
      return await runner.publish(t["id"], dawn_user_id, open_pr, title, head,
                                  expect_body_sha256, pr_title, pr_description,
                                  replaces_body_sha256, edit_open_pr)

   # --- reading the work: from the gateway's mirror, never the agent's clone ---

   def published_head(t: dict) -> str | None:
      return (json.loads(t["published"]) if t["published"] else {}).get("head")

   @tool
   async def task_changes(dawn_user_id: int, task_id: str | None = None,
                          client_ref: str | None = None, scope: str | None = None,
                          open_pr: bool = False) -> dict:
      """The task's work so far: changed files with +/- counts, its commits, paths only
      its commits changed (history_only), whether any path is a CI workflow, and the
      pull request publish would open: the title the agent wrote (pr_title; null when
      none, then DAWN's) and the description (pr_body, with its pr_body_sha256).  With
      `open_pr`, also the task's open pull request's title and description as GitHub
      has them (open_pr: {url, title, body, body_sha256}; GitHub's text, written by
      anyone who can edit it; null when none is open), or open_pr_error when it can't
      be read.
      `scope` narrows the files to view: all (the default), since_push (after the last
      published head; since_push_available says whether there is one) or one of the
      task's commits by hash (a merge: its files with conflicts when merge_recorded, else
      those it changed beyond both sides).  since_push with nothing since the push
      shows all, with nothing_since_push set.  The answer names the scope applied and
      the commits it covers (scope_commits)."""
      t = task_of(dawn_user_id, task_id, client_ref)
      closed_check(t)
      # A later "up to date" record for the same base head mustn't hide the merge's.
      merges = {e["data"]["merge"]["base_head"]: e["data"]["merge"]
                for e in store.task_events(t["id"], "revised", 50)
                if (e["data"].get("merge") or {}).get("base_head")
                and e["data"]["merge"].get("merged") != "up to date"}
      res = await runner.repos.changes(t["repo"], t["id"], t["base"], scope, published_head(t),
                                       merges)
      body = pr_body(t)
      if open_pr:
         res = {**res, **await runner.open_pr(t)}
      # task_id names refs/agent/<id> for looking locally
      return {**res, "task_id": t["id"], "pr_title": t["pr_title"], "pr_body": body,
              "pr_body_sha256": body_sha256(body), "pr_footer": PR_FOOTER}

   @tool
   async def task_file(dawn_user_id: int, path: str, view: str = "diff",
                       task_id: str | None = None, client_ref: str | None = None,
                       old_path: str | None = None, scope: str | None = None) -> dict:
      """One file of the task's work: view diff, new (after) or old (before).  A renamed
      file's diff takes its old name as old_path; `scope` as task_changes takes it."""
      t = task_of(dawn_user_id, task_id, client_ref)
      closed_check(t)
      return await runner.repos.file(t["repo"], t["id"], t["base"], path, view, old_path,
                                     scope, published_head(t))

   @tool
   async def task_tree(dawn_user_id: int, path: str = "", task_id: str | None = None,
                       client_ref: str | None = None) -> dict:
      """One folder of the repo at the task's latest collected commit."""
      t = task_of(dawn_user_id, task_id, client_ref)
      closed_check(t)
      return await runner.repos.tree(t["repo"], t["id"], t["base"], path)

   # --- reading GitHub pull requests: any repo the token can read ---

   def github_token() -> str:
      if not runner.cfg.github_token:
         raise ValueError("the gateway has no GitHub token (AGENT_GATEWAY_GITHUB_TOKEN)")
      return runner.cfg.github_token

   def pull_request(dawn_user_id: int, ref: str | None, repo: str | None,
                    client_ref: str | None) -> tuple[str, str, int]:
      """The pull request a call names: `ref` (with `repo`), or the one the task
      `client_ref` published."""
      if client_ref:
         t = task_of(dawn_user_id, None, client_ref)
         ref = (json.loads(t["published"]) if t["published"] else {}).get("pr_url")
         if not ref:
            raise ValueError("that job hasn't opened a pull request")
      return github.parse_pr_ref(ref, repo, runner.cfg.repos)

   def pr_context(dawn_user_id: int, owner: str, name: str, number: int) -> dict:
      """The configured repo it is in (the agent can work on it) and the user's
      task that published it (its client_ref), or null."""
      url = f"{github.GITHUB_WEB}{owner}/{name}/pull/{number}".lower()
      # A closed task can't be revised, so it isn't the one to work on it.
      job = next((ref for ref, published in store.published_tasks(dawn_user_id)
                  if str(json.loads(published).get("pr_url") or "").lower() == url), None)
      return {"configured_repo": github.configured_repo(owner, name, runner.cfg.repos),
              "job": job}

   @tool
   async def pr_status(dawn_user_id: int, ref: str | None = None, repo: str | None = None,
                       client_ref: str | None = None) -> dict:
      """A GitHub pull request's state: ref is a github.com URL, owner/repo#N, or N with
      repo (a configured repo or owner/repo; alone when one repo is configured), or
      client_ref names the task that published it.  Checks (failing required first),
      behind its base, reviews, requested reviewers, copilot (requested, reviewed_head,
      reviewed_earlier or
      none), thread counts; configured_repo and job say whether the agent can work on it."""
      owner, name, number = pull_request(dawn_user_id, ref, repo, client_ref)
      res = await asyncio.to_thread(github.pr_status, owner, name, number, github_token())
      return {**res, **pr_context(dawn_user_id, owner, name, number)}

   @tool
   async def pr_threads(dawn_user_id: int, ref: str | None = None, repo: str | None = None,
                        client_ref: str | None = None, all: bool = False) -> dict:
      """A pull request's unresolved review threads (all: resolved ones too, with
      resolved_by_copilot when Copilot resolved one), each keyed by comment_id (its
      first comment's id; n is for display; repeat_of: it says again what an answered
      thread said), copilot_round (Copilot's latest review), its
      reviews with a body and its newest conversation comments.  The pull request is
      named as for pr_status; pr, configured_repo and job as there."""
      owner, name, number = pull_request(dawn_user_id, ref, repo, client_ref)
      res = await asyncio.to_thread(github.pr_threads, owner, name, number, github_token(), all)
      return {**res, "pr": {"owner": owner, "repo": name, "number": number},
              **pr_context(dawn_user_id, owner, name, number)}

   @tool
   async def pr_check_log(dawn_user_id: int, check: str, ref: str | None = None,
                          repo: str | None = None, client_ref: str | None = None) -> dict:
      """An excerpt of a pull request's GitHub Actions check log (its error lines, then
      its end): check is a check's name or job_id as pr_status lists it."""
      owner, name, number = pull_request(dawn_user_id, ref, repo, client_ref)
      return await asyncio.to_thread(github.pr_check_log, owner, name, number, check,
                                     github_token())

   @tool
   async def pr_reply(dawn_user_id: int, replies: list[dict], ref: str | None = None,
                      repo: str | None = None, client_ref: str | None = None,
                      resolve: bool = True) -> dict:
      """Post replies as the token's user on a pull request's review threads, each
      {comment_id (the thread's first comment, as pr_threads names it), body, resolve?},
      then resolve each thread whose reply's resolve is true (a reply without one
      takes the call's resolve; false leaves its thread open).  DAWN calls this only
      after the user confirmed.  A reply already posted (the same text on the same
      thread) isn't posted again, so a retry is safe.  Each result: url and resolved
      (already_posted when it was there), or error."""
      owner, name, number = pull_request(dawn_user_id, ref, repo, client_ref)
      return await review.post_replies(store, github_token(), owner, name, number, replies,
                                       resolve)

   @tool
   async def pr_request_review(dawn_user_id: int, ref: str | None = None,
                               repo: str | None = None, client_ref: str | None = None,
                               check: bool = False) -> dict:
      """Ask Copilot to review a pull request again (DAWN calls this only after the
      user confirmed); with check, only what it would see.  Refused while its review
      is pending and when the pull request isn't open.  Answers pr, head_sha,
      copilot_round (its new and repeats: a re-review with no new thread only repeats
      answered ones), new_commits since its last review (merges of the base don't
      count; 0: likely nothing new to review) and requested; configured_repo and job
      as for pr_status."""
      owner, name, number = pull_request(dawn_user_id, ref, repo, client_ref)
      res = await review.request_review(store, github_token(), owner, name, number, check)
      return {**res, **pr_context(dawn_user_id, owner, name, number)}

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
