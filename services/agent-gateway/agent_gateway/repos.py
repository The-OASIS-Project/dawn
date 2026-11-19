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
# Git for the gateway.  The rule: the gateway never runs git in anything the
# agent can write, because the agent controls that repo's config and git
# runs commands named there.
#
#   mirror (gateway-owned, bare)  --clone-->  task clone (agent-owned)
#   mirror  <--bundle, made inside a jail with no network--  task clone
#
# Each task gets its own self-contained clone of the base branch, made before
# the agent starts; the agent's sandbox can't read the mirror at all.  Its work comes
# back as a bundle that git writes to stdout from inside bubblewrap (read-only
# filesystem, no network, the agent's home hidden), so whatever the repo's
# config runs there can do nothing.  Reading results and publishing happen
# in the mirror.

import asyncio
import base64
import json
import logging
import os
import re
import tempfile
import urllib.error

from .github import GitError, adopt_pull_request, error_text, github_url, open_pull_request

log = logging.getLogger("agent-gateway")

SUBMODULE_TIMEOUT_S = 300  # fetching a clone's submodules before its task starts
# Refreshing the mirror for a revise's merge, which DAWN waits for (it then
# merges what the mirror has, and names the commit).
MERGE_REFRESH_S = 45

# Mount points the Claude Code sandbox puts over protected paths (seen with
# 2.1.317).  They exist only inside the sandbox, but `git add -A` there would
# stage them.  A path that is in the repo is unaffected.
SANDBOX_PLACEHOLDERS = [".bashrc", ".bash_profile", ".zshrc", ".zprofile", ".profile",
                        ".gitconfig", ".gitmodules", ".ripgreprc", ".mcp.json", ".idea",
                        ".vscode", ".claude/agents", ".claude/commands", ".claude/hooks",
                        ".claude/launch.json", ".claude/loop.md", ".claude/output-styles",
                        ".claude/settings.json",
                        ".claude/routines", ".claude/scheduled", ".claude/scheduled_tasks.json",
                        ".claude/settings.local.json", ".claude/skills", ".claude/workflows",
                        ".claude/.cc-writes"]

SUBJECT_CAP = 200
CHECK_CAP = 20            # files to check named in a completed event
HELD_CAP = 20             # conflicts the sandbox can't write, named in a refusal
FILE_CAP = 512 * 1024     # one file's content or diff in a viewer reply
# What one reply may take once encoded: the text is JSON in the tool result,
# which is JSON again in the MCP reply, and DAWN reads at most 1 MiB of it.
REPLY_CAP = 900 * 1024
FILES_CAP = 2000          # changed files in one changes listing (files to check first)
TREE_CAP = 2000           # entries in one folder listing
COMMITS_CAP = 100
HASH_RE = re.compile(r"[0-9a-f]{4,40}")  # a commit as the viewer names it
CLAUDE_MD_CAP = 64 * 1024

async def run(argv: list[str], cwd: str | None = None, env: dict | None = None,
              stdin: str | None = None) -> bytes:
   proc = await asyncio.create_subprocess_exec(
      *argv, cwd=cwd, env=env, stdin=asyncio.subprocess.PIPE if stdin is not None else None,
      stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
   try:
      out, err = await proc.communicate(stdin.encode() if stdin is not None else None)
   except asyncio.CancelledError:
      # A timeout or a cancel: stop the child too (sudo passes SIGTERM on; a
      # SIGKILL to it would orphan what it started).
      proc.terminate()
      try:
         await asyncio.wait_for(proc.wait(), 5)
      except asyncio.TimeoutError:
         proc.kill()
      raise
   if proc.returncode != 0:
      raise GitError(f"{' '.join(argv[-3:])}: {err.decode(errors='replace').strip()[:400]}")
   return out


def text(out: bytes) -> str:
   return out.decode(errors="replace").strip()


def glob_re(pattern: str) -> re.Pattern:
   """A repo path pattern: `**` any number of folders, `*` and `?` within one."""
   out, i = "", 0
   while i < len(pattern):
      if pattern.startswith("**/", i):
         out, i = out + "(?:.*/)?", i + 3
      elif pattern.startswith("**", i):
         out, i = out + ".*", i + 2
      elif pattern[i] == "*":
         out, i = out + "[^/]*", i + 1
      elif pattern[i] == "?":
         out, i = out + "[^/]", i + 1
      else:
         out, i = out + re.escape(pattern[i]), i + 1
   return re.compile(out, re.DOTALL)


def check_why(path: str, checks: list[dict]) -> str | None:
   """Why `path` is a file to check before publishing: the reason of the first
   matching pattern that has one ("" when only patterns without one match), or
   None when it isn't one.  A control character in a path counts: no real file
   needs one, and one could hide a path from the patterns."""
   if any(ord(c) < 0x20 or ord(c) == 0x7f for c in path):
      return "a control character in its name"
   whys = [c["why"] for c in checks if glob_re(c["path"]).fullmatch(path)]
   return next((w for w in whys if w), "") if whys else None


def check_mark(paths: list[str], checks: list[dict]) -> dict:
   """A changed file's `to_check` flag (any of its names matching), with a
   reason (`why`) when one of its matches has one."""
   whys = [w for w in (check_why(p, checks) for p in paths) if w is not None]
   why = next((w for w in whys if w), "")
   return {"to_check": bool(whys), **({"why": why} if why else {})}


def sandbox_protected(path: str) -> bool:
   """A path the agent's sandbox keeps read-only: anything under .claude/, and the
   sandbox's placeholder paths and what is under them."""
   return path.startswith(".claude/") or any(path == p or path.startswith(p + "/")
                                             for p in SANDBOX_PLACEHOLDERS)


def safe_path(path: str) -> str:
   """A path inside the repo, as git names it; refused otherwise."""
   p = path.strip().strip("/")
   if len(p) > 1024 or "\0" in p or any(part in ("..", ".") for part in p.split("/")):
      raise GitError("not a path inside the repo")
   return p


def slug(s: str) -> str:
   s = re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")
   return s[:32].rstrip("-") or "task"


class Repos:
   def __init__(self, repos: dict, mirrors_dir: str, work_dir: str, run_as: list[str],
                agent_home: str, agent_gitconfig: str,
                identity: tuple[str, str] = ("DAWN agent", "dawn-agent@localhost"),
                hidden: list[str] = ()):
      self.repos = repos  # name -> {"url": ..., "base": ...}
      # Hidden in the jail as from the agent (the gateway's state, deny_read),
      # with the mirrors and every task's clone: only its own clone is bound.
      self.hidden = [*hidden]
      self.mirrors_dir = mirrors_dir
      self.work_dir = work_dir
      self.run_as = run_as
      self.agent_home = agent_home
      # Root-owned; lets the agent user clone from the gateway-owned mirror.
      self.agent_env = ["env", f"GIT_CONFIG_GLOBAL={agent_gitconfig}"]
      self.identity = identity

   def mirror(self, name: str) -> str:
      return os.path.join(self.mirrors_dir, f"{name}.git")

   def workdir(self, task_id: str) -> str:
      return os.path.join(self.work_dir, task_id)

   async def gw(self, *args: str) -> str:
      """git as the gateway user, in the mirror (trusted)."""
      return text(await run(["git", *args]))

   async def as_agent(self, *argv: str, stdin: str | None = None) -> str:
      return text(await run([*self.run_as, *self.agent_env, *argv], stdin=stdin))

   async def jailed_git(self, task_id: str, *args: str, writable: bool = False,
                        read: str | None = None) -> bytes:
      """git in a task clone, as the agent user, inside bubblewrap: read-only
      (the clone itself writable only when asked), no network, no system sockets
      (/run), the agent's home hidden; @read is bound read-only wherever it is."""
      w = self.workdir(task_id)
      hide = []
      for p in (*self.hidden, self.mirrors_dir, self.work_dir):
         if os.path.isdir(p):
            hide += ["--tmpfs", p]
         elif os.path.exists(p):
            hide += ["--ro-bind", "/dev/null", p]
      extra = ["--ro-bind", read, read] if read else []
      return await run([*self.run_as, *self.agent_env, "bwrap", "--unshare-all",
                        "--die-with-parent", "--new-session", "--ro-bind", "/", "/",
                        "--tmpfs", "/tmp", "--tmpfs", "/run", "--tmpfs", self.agent_home,
                        *hide, *extra, "--bind" if writable else "--ro-bind", w, w,
                        "--dev", "/dev", "--proc", "/proc", "--setenv", "HOME", "/nonexistent",
                        "git", "-C", w, *args])

   async def sync_base(self, name: str, base: str, task_id: str,
                       refresh_s: float = SUBMODULE_TIMEOUT_S) -> str:
      """Before a follow-up: the mirror fetched from the remote (bounded in
      time; one that can't be reached leaves it as it was), then the task
      clone's origin/<base> set to it, so work merged since the task started is
      in reach.  Only data crosses, as in collect: a bundle of the base the
      gateway makes, fetched in the jail with hooks and transports off.
      Returns the base head the gateway gave it."""
      try:
         await asyncio.wait_for(self.refresh(name), refresh_s)
      except (GitError, asyncio.TimeoutError) as e:
         log.warning("%s: mirror not refreshed before a follow-up: %s", name, e or "timed out")
      m = self.mirror(name)
      new = await self.gw("-C", m, "rev-parse", "--verify", "-q", f"refs/heads/{base}")
      try:
         old = text(await self.jailed_git(task_id, "rev-parse", "-q", "--verify",
                                          f"refs/remotes/origin/{base}"))
      except GitError:
         old = ""
      if old == new:
         return new  # nothing moved: no bundle, nothing fetched
      # Only the commits the clone lacks (a whole history is ~100 MB on a big
      # repo, and stored again in the clone); all of it when the clone's base
      # isn't in the mirror (rewritten upstream).
      try:
         await self.gw("-C", m, "cat-file", "-e", f"{old}^{{commit}}")
         since = [f"^{old}"] if old else []
      except GitError:
         since = []
      # In the mirrors directory: readable by the agent user (its group), as
      # the mirrors are, and gone from this process's view only when removed.
      path = os.path.join(self.mirrors_dir, f".sync-{task_id}.bundle")
      try:
         await self.gw("-C", m, "bundle", "create", "-q", path, f"refs/heads/{base}", *since)
         os.chmod(path, 0o640)
         # The head the bundle carries (a refresh by another task may have moved
         # the mirror's since).
         head = (await self.gw("bundle", "list-heads", path, f"refs/heads/{base}")).split()[0]
         await self.jailed_git(task_id, "-c", "core.hooksPath=/dev/null",
                               "-c", "protocol.allow=never", "-c", "protocol.file.allow=always",
                               "-c", "gc.auto=0", "fetch", "-q", "--no-tags", path,
                               f"+refs/heads/{base}:refs/remotes/origin/{base}", writable=True,
                               read=path)
      finally:
         if os.path.exists(path):
            os.remove(path)
      return head

   async def merge_base(self, name: str, base: str, task_id: str) -> dict:
      """Merges <base>, brought up to date first (sync_base), into the task's
      branch in its clone, in the jail with hooks off: the agent's sandbox keeps
      paths like .claude/ read-only, so a base that changes them can't be merged
      by the agent.  Returns {"merged": head} ("up to date" when there was
      nothing to merge), or {"conflicts": paths} with the merge left in progress
      for the agent to resolve and commit.  Refuses (ValueError, nothing merged) a
      clone with uncommitted changes or a merge already in progress, and a merge
      with a conflict in a path the sandbox protects (aborted)."""
      # Untracked files don't count: a merge leaves them alone, and refuses
      # (nothing started) one it would overwrite.
      if await self.has_ref(task_id, "MERGE_HEAD"):
         raise ValueError(f"a merge of {base} is still in progress in the task's clone; nothing "
                          "was sent.  Revise without merge_base to let the agent finish it")
      if text(await self.jailed_git(task_id, "status", "--porcelain", "--untracked-files=no")):
         raise ValueError(f"the task's clone has uncommitted changes; {base} wasn't merged and "
                          "nothing was sent")
      head = await self.sync_base(name, base, task_id, MERGE_REFRESH_S)
      if await self.has_ancestor(task_id, head, "HEAD"):
         return {"merged": "up to date", "base_head": head}
      quiet = ["-c", "core.hooksPath=/dev/null", "-c", "gc.auto=0", "-c", "commit.gpgSign=false"]
      try:
         # By its name (the commit sync_base set it to): the message reads
         # "Merge remote-tracking branch 'origin/<base>'".
         await self.jailed_git(task_id, *quiet, "merge", "-q", "--no-edit",
                               f"refs/remotes/origin/{base}", writable=True)
      except GitError as e:
         out = await self.jailed_git(task_id, "diff", "--name-only", "--diff-filter=U", "-z")
         conflicts = [p for p in out.decode(errors="replace").split("\0") if p]
         held = [p for p in conflicts if sandbox_protected(p)]
         if not conflicts or held:
            if await self.has_ref(task_id, "MERGE_HEAD"):
               await self.jailed_git(task_id, *quiet, "merge", "--abort", writable=True)
            if not conflicts:
               raise
            raise ValueError(f"merging {base} conflicts in {', '.join(held[:HELD_CAP])}, "
                             "which the agent's sandbox can't write: those need your hand.  "
                             "The merge was aborted; the branch is as it was and nothing was sent") from e
         return {"conflicts": conflicts, "base_head": head}
      merged = text(await self.jailed_git(task_id, "rev-parse", "HEAD"))
      return {"merged": merged, "base_head": head}

   async def has_ref(self, task_id: str, ref: str) -> bool:
      try:
         await self.jailed_git(task_id, "rev-parse", "-q", "--verify", ref)
         return True
      except GitError:
         return False

   async def has_ancestor(self, task_id: str, ancestor: str, rev: str) -> bool:
      try:
         await self.jailed_git(task_id, "merge-base", "--is-ancestor", ancestor, rev)
         return True
      except GitError:
         return False

   # --- before the agent runs ----------------------------------------------

   async def refresh(self, name: str) -> str:
      m = self.mirror(name)
      if not os.path.isdir(m):
         # Group-readable whatever the umask, never group-writable: the agent
         # user reads it to clone and must not be able to change a ref.
         await self.gw("init", "-q", "--bare", "--shared=0640", m)
         await self.gw("-C", m, "remote", "add", "origin", self.repos[name]["url"])
      await self.gw("-C", m, "fetch", "-q", "origin", "+refs/heads/*:refs/heads/*")
      return m

   async def prepare(self, name: str, base: str, branch: str, task_id: str) -> str:
      m = await self.refresh(name)
      w = self.workdir(task_id)
      await self.as_agent("git", "clone", "-q", "--no-local", "--single-branch", "--branch",
                          base, m, w)
      await self.as_agent("git", "-C", w, "checkout", "-q", "-b", branch)
      # Without an identity the agent's first commit fails and it improvises
      # one (seen: the logged-in account's name and email).
      await self.as_agent("git", "-C", w, "config", "user.name", self.identity[0])
      await self.as_agent("git", "-C", w, "config", "user.email", self.identity[1])
      await self.as_agent("tee", "-a", os.path.join(w, ".git", "info", "exclude"),
                          stdin="".join(f"/{p}\n" for p in SANDBOX_PLACEHOLDERS))
      # Submodules as the base branch names them, fetched before the agent
      # starts (its sandbox keeps .gitmodules and parts of .git read-only).
      # Only the base branch's own (not theirs: a third party's .gitmodules
      # isn't fetched outside the sandbox), and bounded in time.
      if os.path.exists(os.path.join(w, ".gitmodules")):
         try:
            await asyncio.wait_for(
               self.as_agent("git", "-C", w, "-c", "protocol.file.allow=never",
                             "-c", "http.lowSpeedLimit=1000", "-c", "http.lowSpeedTime=60",
                             "submodule", "update", "--init", "--depth", "1", "-q"),
               SUBMODULE_TIMEOUT_S)
         except (GitError, asyncio.TimeoutError) as e:
            log.warning("task %s: submodules not fetched: %s", task_id, e or "timed out")
      return w

   async def claude_md(self, name: str, base: str) -> str | None:
      """The repo's CLAUDE.md as the base branch has it (from the mirror, so
      nothing the agent writes during the task changes its rules), or None."""
      rev = f"refs/heads/{base}:CLAUDE.md"
      m = self.mirror(name)
      size = await self.size_of(m, rev)
      if size == 0 or size > CLAUDE_MD_CAP:
         return None
      return (await run(["git", "-C", m, "cat-file", "blob", rev])).decode(errors="replace")

   # --- after the agent runs --------------------------------------------

   async def collect(self, name: str, task_id: str, branch: str, base: str,
                     keep_on_empty: bool = False) -> dict:
      """Bring the task's branch into the mirror (as refs/agent/<id>) and
      describe it.  Only data crosses: a bundle, checked before it is read."""
      dirty = text(await self.jailed_git(task_id, "status", "--porcelain"))
      try:
         bundle = await self.jailed_git(task_id, "bundle", "create", "-",
                                        f"refs/heads/{branch}", f"^refs/remotes/origin/{base}")
      except GitError as e:
         if "empty bundle" not in str(e):
            raise
         # No commits now: an earlier collect's commits must not stay behind
         # (unless the caller keeps them, erring toward keeping work).
         if not keep_on_empty:
            await run(["git", "-C", self.mirror(name), "update-ref", "-d",
                       f"refs/agent/{task_id}"])
         return {"commits": [], "diffstat": "", "uncommitted": len(dirty.splitlines())}
      m, ref = self.mirror(name), f"refs/agent/{task_id}"
      with tempfile.NamedTemporaryFile(suffix=".bundle") as f:
         f.write(bundle)
         f.flush()
         await self.gw("-C", m, "bundle", "verify", "-q", f.name)
         await self.gw("-C", m, "fetch", "-q", f.name, f"+refs/heads/{branch}:{ref}")
      commits = await self.gw("-C", m, "log", "--format=%h %s", f"refs/heads/{base}..{ref}")
      stat = await self.gw("-C", m, "diff", "--shortstat", f"refs/heads/{base}...{ref}")
      names = await run(["git", "-C", m, "diff", "--no-renames", "--name-only", "-z",
                         f"refs/heads/{base}...{ref}"])
      checks = self.checks(name)
      touched = [{"path": p, **({"why": w} if w else {})}
                 for p in names.decode(errors="replace").split("\0") if p
                 for w in [check_why(p, checks)] if w is not None]
      return {"commits": [c[:SUBJECT_CAP] for c in commits.splitlines()[:20]],
              "diffstat": stat, "uncommitted": len(dirty.splitlines()),
              "to_check": touched[:CHECK_CAP], "to_check_count": len(touched)}

   # --- reading the agent's work (in the mirror only) -----------------------

   def checks(self, name: str) -> list[dict]:
      """The repo's files to check before publishing, and always CI workflows."""
      return [{"path": ".github/workflows/**", "why": "CI, which runs with the repo's secrets"},
              *self.repos[name].get("check", [])]

   async def uncommitted(self, task_id: str) -> int:
      """Files changed in the task's clone but not committed (lost with the clone)."""
      return len(text(await self.jailed_git(task_id, "status", "--porcelain")).splitlines())

   async def unpublished(self, name: str, task_id: str, base: str,
                         published_head: str | None) -> int:
      """Commits on the task's collected branch that are neither on its base nor in
      what was published: the work only the mirror has.  0 with none collected."""
      m, ref = self.mirror(name), f"refs/agent/{task_id}"
      try:
         await self.gw("-C", m, "rev-parse", "--verify", "-q", ref)
      except GitError:
         return 0
      shut = [f"^{published_head}"] if published_head else []
      # --ignore-missing: a published commit gone from the mirror excludes nothing,
      # so its work counts as unpublished (errs toward keeping it).
      return int(await self.gw("-C", m, "rev-list", "--count", "--ignore-missing", ref,
                               f"^refs/heads/{base}", *shut))

   async def drop(self, name: str, task_id: str) -> None:
      """The task's collected branch, out of the mirror."""
      await run(["git", "-C", self.mirror(name), "update-ref", "-d", f"refs/agent/{task_id}"])

   async def agent_ref(self, name: str, task_id: str, base: str) -> tuple[str, str]:
      """(head, merge base) of the task's collected branch."""
      m, ref = self.mirror(name), f"refs/agent/{task_id}"
      try:
         head = await self.gw("-C", m, "rev-parse", "--verify", "-q", ref)
      except GitError:
         raise GitError("nothing collected for this task: work is collected when a run ends, "
                        "and only what it committed")
      return head, await self.gw("-C", m, "merge-base", f"refs/heads/{base}", head)

   async def since_push(self, m: str, head: str, published: str | None) -> str | None:
      """The published head when the task has commits after it, else None."""
      if not published or not HASH_RE.fullmatch(published) or published == head:
         return None
      try:
         await self.gw("-C", m, "merge-base", "--is-ancestor", published, head)
      except GitError:
         return None  # gone from the mirror, or not under the head any more
      return published

   async def scope_range(self, m: str, mb: str, head: str, scope: str | None,
                         published: str | None, merges: dict | None = None) -> dict:
      """What a viewer scope shows: the diff from..to (limited to `only` paths when
      set) and the commits it covers.  all: the whole task.  since_push: after the
      last published head; with none, all (nothing_since_push set).  A hash: one of the
      task's own commits against its first parent.  A merge: the files with conflicts
      when the gateway recorded the merge (merge_recorded), else an approximation, the
      files the merge changed beyond both sides (differing from every parent).
      Nothing the user sent reaches git."""
      since = await self.since_push(m, head, published)
      out = {"scope": "all", "from": mb, "to": head, "only": None, "range": f"{mb}..{head}",
             "since_push_available": since is not None}
      if scope in (None, "", "all"):
         return out
      if scope == "since_push":
         if since is None:
            return {**out, "nothing_since_push": True}
         return {**out, "scope": scope, "from": since, "range": f"{since}..{head}"}
      own = (await self.gw("-C", m, "rev-list", f"{mb}..{head}")).split()
      found = [c for c in own if HASH_RE.fullmatch(scope) and c.startswith(scope)]
      if len(found) != 1:
         raise GitError("not one of this task's commits")
      commit = found[0]
      parents = (await self.gw("-C", m, "rev-list", "--parents", "-n1", commit)).split()[1:]
      out = {**out, "scope": commit, "from": parents[0], "to": commit, "range": None,
             "merge": len(parents) > 1}
      if len(parents) > 1:
         rec = (merges or {}).get(parents[1]) or {}
         out["merge_recorded"] = True
         if rec.get("merged") and commit.startswith(rec["merged"]):
            out["only"] = []  # the gateway's own clean merge: nothing was conflicted
         elif rec.get("conflicts") and len(rec["conflicts"]) == rec.get("conflicts_count"):
            out["only"] = rec["conflicts"]
         else:
            out["merge_recorded"] = False
            sides = [set((await run(["git", "-C", m, "diff", "--no-renames", "--name-only",
                                     "-z", p, commit])).decode(errors="replace").split("\0"))
                     for p in parents]
            out["only"] = sorted(set.intersection(*sides) - {""})
      return out

   async def blobs(self, m: str, rev: str, paths: list[str]) -> dict:
      """path -> blob id at `rev`, for those of `paths` it has."""
      out = await run(["git", "--literal-pathspecs", "-C", m, "ls-tree", "-r", "-z", rev,
                       "--", *paths])
      return {r.split("\t", 1)[1]: r.split()[2]
              for r in out.decode(errors="replace").split("\0") if r}

   async def drop_main(self, m: str, head: str, published: str, mb: str,
                       files: list[dict]) -> list[dict]:
      """since_push across a merge of the base: files only main changed are left
      out, files both changed are marked includes_main.  Main is the base head the
      newest merge brought in (not the base now, which a refresh moves), and a file
      is main's alone only when the published version had no change of the agent's
      to it (a conflict resolved to main's side undoes the agent's published one)."""
      merge = await self.gw("-C", m, "rev-list", "--first-parent", "--min-parents=2", "-n1",
                            f"{published}..{head}")
      tip = await self.gw("-C", m, "rev-parse", f"{merge}^2")
      try:  # a merge of the base: its side is in the base the branch now forks from
         await self.gw("-C", m, "merge-base", "--is-ancestor", tip, mb)
      except GitError:
         return files  # the agent merged a branch of its own: nothing there is main's
      fork = await self.gw("-C", m, "merge-base", published, tip)
      names = [f["path"].split(" -> ")[-1] for f in files]
      at_head, at_tip, at_fork, at_pub = [await self.blobs(m, r, names)
                                          for r in (head, tip, fork, published)]
      kept = []
      for f, n in zip(files, names):
         main = at_tip.get(n) != at_fork.get(n)
         if main and at_head.get(n) == at_tip.get(n) and at_pub.get(n) == at_fork.get(n):
            continue
         kept.append({**f, "includes_main": True} if main else f)
      return kept

   async def changes(self, name: str, task_id: str, base: str, scope: str | None = None,
                     published: str | None = None, merges: dict | None = None) -> dict:
      head, mb = await self.agent_ref(name, task_id, base)
      m = self.mirror(name)
      sc = await self.scope_range(m, mb, head, scope, published, merges)
      only = sc["only"]
      numstat = b"" if only == [] else await run(
         ["git", "--literal-pathspecs", "-C", m, "diff", "--no-ext-diff", "--no-textconv", "-M",
          "--numstat", "-z", sc["from"], sc["to"], *(["--", *only] if only else [])])
      files, fields = [], numstat.split(b"\0")
      checks = self.checks(name)
      i = 0
      while i < len(fields) and fields[i]:
         added, removed, path = fields[i].decode(errors="replace").split("\t", 2)
         if not path:  # a rename: old and new paths follow
            path = f"{fields[i + 1].decode(errors='replace')} -> " \
                   f"{fields[i + 2].decode(errors='replace')}"
            i += 2
         files.append({"path": path, "added": None if added == "-" else int(added),
                       "removed": None if removed == "-" else int(removed),
                       **check_mark(path.split(" -> "), checks)})
         i += 1
      # A conflict resolved by keeping the branch's side has no diff against the
      # first parent: still listed, as how it was resolved.
      listed = {n for f in files for n in f["path"].split(" -> ")}
      for p in only or []:
         if p not in listed:
            files.append({"path": p, "added": 0, "removed": 0, **check_mark([p], checks)})
      if sc["scope"] == "since_push" and await self.gw(
            "-C", m, "rev-list", "--first-parent", "--min-parents=2", "-n1", sc["range"]):
         files = await self.drop_main(m, head, sc["from"], mb, files)
      files.sort(key=lambda f: not f["to_check"])  # files to check first: never cut off a listing
      # Paths a commit changed that the net diff doesn't show (added then removed, or
      # changed back): a push publishes the commits, so these are public too.
      # --no-renames lists both names of a rename; either name of a net rename counts
      # as listed.
      net = {n for f in files for n in f["path"].split(" -> ")}
      names = await run(["git", "-C", m, "log", "--no-renames", "--format=", "--name-only",
                         "-z", f"{mb}..{head}"])
      seen = dict.fromkeys(n for n in names.decode(errors="replace").split("\0") if n)
      history = [{"path": p, **check_mark([p], checks)} for p in seen if p not in net]
      history.sort(key=lambda f: not f["to_check"])
      workflows = any(p.startswith(".github/workflows/") for p in (*net, *seen))
      total = len(files)
      files = files[:FILES_CAP]
      log = await self.gw("-C", m, "log", "--format=%H%x00%P%x00%s", f"-{COMMITS_CAP}",
                          f"{mb}..{head}")
      commits = []
      for c in log.splitlines():
         h, parents, subject = (c.split("\0", 2) + ["", ""])[:3]
         commits.append({"hash": h, "subject": subject[:SUBJECT_CAP],
                         "merge": len(parents.split()) > 1})
      # The task's own commits (since_push across a merge: not main's it brought in).
      covers = [sc["to"]] if sc["range"] is None else \
         (await self.gw("-C", m, "rev-list", f"-{COMMITS_CAP}", head, f"^{sc['from']}",
                        f"^{mb}")).split()
      return {"head": head, "base": mb, "base_name": base, "files": files, "files_total": total,
              "commits": commits, "history_only": history[:FILES_CAP],
              "workflows": workflows, "scope": sc["scope"], "scope_commits": covers,
              "scope_merge": sc.get("merge", False),
              "merge_recorded": sc.get("merge_recorded", False),
              "nothing_since_push": sc.get("nothing_since_push", False),
              "since_push_available": sc["since_push_available"]}

   async def file(self, name: str, task_id: str, base: str, path: str, view: str,
                  old_path: str | None = None, scope: str | None = None,
                  published: str | None = None) -> dict:
      """One file: its diff, or its content after (new) or before (old) the task, or
      the viewer scope's range (scope_range).  A renamed file's diff names its old
      path too, so git pairs the two."""
      head, mb = await self.agent_ref(name, task_id, base)
      m = self.mirror(name)
      sc = await self.scope_range(m, mb, head, scope, published)
      mb, head, p = sc["from"], sc["to"], safe_path(path)
      if view == "diff":
         old = safe_path(old_path) if old_path else p
         # git builds the whole diff in memory: check sizes first
         for rev, side in ((mb, old), (head, p)):
            size = await self.size_of(m, f"{rev}:{side}")
            if size > FILE_CAP:
               return {"path": p, "view": view, "too_large": size}
         out = await run(["git", "--literal-pathspecs", "-C", m, "diff", "--no-ext-diff",
                          "--no-textconv", "-M", mb, head, "--", *dict.fromkeys((old, p))])
      elif view in ("new", "old"):
         rev = f"{head if view == 'new' else mb}:{p}"
         try:
            size = int(await self.gw("-C", m, "cat-file", "-s", rev))
         except GitError:
            return {"path": p, "view": view, "missing": True}
         if size > FILE_CAP:
            return {"path": p, "view": view, "too_large": size}
         out = await run(["git", "-C", m, "cat-file", "blob", rev])
      else:
         raise GitError("view must be diff, new or old")
      if len(out) > FILE_CAP:
         return {"path": p, "view": view, "too_large": len(out)}
      if b"\0" in out:
         return {"path": p, "view": view, "binary": True}
      text = out.decode(errors="replace")
      # As sent: the text JSON-encoded twice (non-ASCII as itself), in bytes.
      encoded = json.dumps(json.dumps(text, ensure_ascii=False), ensure_ascii=False)
      if len(encoded.encode()) > REPLY_CAP:  # a lockfile's escapes double it
         return {"path": p, "view": view, "too_large": len(out)}
      return {"path": p, "view": view, "text": text}

   async def size_of(self, m: str, rev: str) -> int:
      """Size of a blob, or 0 if the path isn't there."""
      try:
         return int(await self.gw("-C", m, "cat-file", "-s", rev))
      except GitError:
         return 0

   async def tree(self, name: str, task_id: str, base: str, path: str) -> dict:
      """One folder of the repo at the task's commit."""
      head, _ = await self.agent_ref(name, task_id, base)
      p = safe_path(path) if path.strip("/") else ""
      out = await run(["git", "-C", self.mirror(name), "ls-tree", "-z", "--long",
                       f"{head}:{p}" if p else head])
      entries = []
      for rec in out.split(b"\0")[:TREE_CAP]:
         if rec:
            meta, entry = rec.decode(errors="replace").split("\t", 1)
            _, kind, _, size = meta.split(None, 3)
            entries.append({"name": entry, "type": kind,
                            "size": None if size.strip() == "-" else int(size)})
      return {"path": p, "head": head, "entries": entries}

   async def remove(self, task_id: str) -> None:
      if re.fullmatch(r"[0-9a-f]{8}", task_id):
         await run([*self.run_as, "rm", "-rf", "--one-file-system", self.workdir(task_id)])

   async def publish(self, name: str, head: str, branch: str, base: str, token: str,
                     open_pr: bool, title: str, body: str, earlier_pr: str | None = None) -> dict:
      """Push `head` (the checked commit itself, not a ref that could move) from the
      mirror to `branch`; optionally open a PR.  The token goes in a header, never
      in argv; no force push."""
      url = self.repos[name]["url"]
      found = github_url(url)
      if not found or found[2] is not None:
         raise GitError("publishing needs an https://github.com/owner/repo URL")
      owner, repo = found[0], found[1]
      basic = base64.b64encode(f"x-access-token:{token}".encode()).decode()
      env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": self.mirrors_dir,
             "GIT_TERMINAL_PROMPT": "0", "GIT_CONFIG_COUNT": "1",
             "GIT_CONFIG_KEY_0": "http.https://github.com/.extraheader",
             "GIT_CONFIG_VALUE_0": f"AUTHORIZATION: basic {basic}"}
      try:
         await run(["git", "-C", self.mirror(name), "push", "-q", url,
                    f"{head}:refs/heads/{branch}"], env=env)
      except GitError as e:
         if "refusing to allow" in str(e) and "workflow" in str(e):  # GitHub's own words
            # The token can't touch .github/workflows (as it should not): pushing
            # those would run them with the repo's secrets before any review.
            raise GitError("this work changes CI workflows (.github/workflows), which the "
                           "gateway's token can't push; publish it yourself") from e
         raise
      result = {"pushed": f"{owner}/{repo}:{branch}"}
      if open_pr:
         try:
            result["pr_url"] = await asyncio.to_thread(
               open_pull_request, owner, repo, token, branch, base, title, body)
         except urllib.error.HTTPError as e:
            if e.code == 422 and "pull request already exists" in error_text(e):
               # One already open for the branch (a publish whose answer was lost): take
               # it over only when this token's own user opened it.
               result["pr_url"] = await asyncio.to_thread(
                  adopt_pull_request, owner, repo, token, branch, base, earlier_pr)
            elif earlier_pr:
               result["pr_url"] = earlier_pr
            else:
               raise
         except (OSError, ValueError):
            # The push went up; one already open for the branch (its state unread)
            # makes GitHub refuse a second: keep pointing at it.
            if not earlier_pr:
               raise
            result["pr_url"] = earlier_pr
      return result
