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
import urllib.request

log = logging.getLogger("agent-gateway")

SUBMODULE_TIMEOUT_S = 300  # fetching a clone's submodules before its task starts

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
PROTECTED_CAP = 20        # protected paths named in a completed event
FILE_CAP = 512 * 1024     # one file's content or diff in a viewer reply
TREE_CAP = 2000           # entries in one folder listing
COMMITS_CAP = 100
CLAUDE_MD_CAP = 64 * 1024


class GitError(Exception):
   pass


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


def is_protected(path: str, patterns: list[str]) -> bool:
   """Whether `path` matches a pattern, or carries a control character (no real
   file needs one, and one could hide a path from the patterns)."""
   if any(ord(c) < 0x20 or ord(c) == 0x7f for c in path):
      return True
   return any(glob_re(p).fullmatch(path) for p in patterns)


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
                identity: tuple[str, str] = ("DAWN agent", "dawn-agent@localhost")):
      self.repos = repos  # name -> {"url": ..., "base": ...}
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

   async def jailed_git(self, task_id: str, *args: str) -> bytes:
      """git in a task clone, as the agent user, inside bubblewrap: read-only,
      no network, no system sockets (/run), the agent's home hidden."""
      w = self.workdir(task_id)
      return await run([*self.run_as, *self.agent_env, "bwrap", "--unshare-all",
                        "--die-with-parent", "--new-session", "--ro-bind", "/", "/",
                        "--tmpfs", "/tmp", "--tmpfs", "/run", "--tmpfs", self.agent_home,
                        "--ro-bind", w, w, "--dev", "/dev", "--proc", "/proc",
                        "--setenv", "HOME", "/nonexistent", "git", "-C", w, *args])

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
      patterns = self.protected(name)
      touched = [p for p in names.decode(errors="replace").split("\0")
                 if p and is_protected(p, patterns)]
      return {"commits": [c[:SUBJECT_CAP] for c in commits.splitlines()[:20]],
              "diffstat": stat, "uncommitted": len(dirty.splitlines()),
              "protected": touched[:PROTECTED_CAP], "protected_count": len(touched)}

   # --- reading the agent's work (in the mirror only) -----------------------

   def protected(self, name: str) -> list[str]:
      """The repo's protected patterns, and always CI workflows: a pushed one runs
      with the repo's secrets."""
      return [".github/workflows/**", *self.repos[name].get("protected", [])]

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

   async def changes(self, name: str, task_id: str, base: str) -> dict:
      head, mb = await self.agent_ref(name, task_id, base)
      m = self.mirror(name)
      numstat = await run(["git", "-C", m, "diff", "--no-ext-diff", "--no-textconv", "-M",
                           "--numstat", "-z", mb, head])
      files, fields = [], numstat.split(b"\0")
      i = 0
      while i < len(fields) and fields[i]:
         added, removed, path = fields[i].decode(errors="replace").split("\t", 2)
         if not path:  # a rename: old and new paths follow
            path = f"{fields[i + 1].decode(errors='replace')} -> " \
                   f"{fields[i + 2].decode(errors='replace')}"
            i += 2
         patterns = self.protected(name)
         files.append({"path": path, "added": None if added == "-" else int(added),
                       "removed": None if removed == "-" else int(removed),
                       "protected": any(is_protected(p, patterns)
                                        for p in path.split(" -> "))})
         i += 1
      files.sort(key=lambda f: not f["protected"])  # protected first: never cut off a listing
      log = await self.gw("-C", m, "log", "--format=%H%x00%s", f"-{COMMITS_CAP}", f"{mb}..{head}")
      commits = [dict(zip(("hash", "subject"), c.split("\0", 1))) for c in log.splitlines()]
      for c in commits:
         c["subject"] = c.get("subject", "")[:SUBJECT_CAP]
      return {"head": head, "base": mb, "files": files, "commits": commits}

   async def file(self, name: str, task_id: str, base: str, path: str, view: str,
                  old_path: str | None = None) -> dict:
      """One file: its diff, or its content after (new) or before (old) the task.  A
      renamed file's diff names its old path too, so git pairs the two."""
      head, mb = await self.agent_ref(name, task_id, base)
      p, m = safe_path(path), self.mirror(name)
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
      return {"path": p, "view": view, "text": out.decode(errors="replace")}

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
      m = re.match(r"https://github\.com/([^/]+)/([^/.]+?)(\.git)?$", url)
      if not m:
         raise GitError("publishing needs an https://github.com/owner/repo URL")
      owner, repo = m.group(1), m.group(2)
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
         except (OSError, ValueError):
            # The push went up; one already open for the branch (its state unread)
            # makes GitHub refuse a second: keep pointing at it.
            if not earlier_pr:
               raise
            result["pr_url"] = earlier_pr
      return result


def open_pull_request(owner, repo, token, head, base, title, body) -> str:
   req = urllib.request.Request(
      f"https://api.github.com/repos/{owner}/{repo}/pulls", method="POST",
      data=json.dumps({"title": title, "head": head, "base": base, "body": body}).encode(),
      headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"})
   with urllib.request.urlopen(req, timeout=30) as resp:
      return json.load(resp)["html_url"]


def pr_state(pr_url: str, token: str) -> str | None:
   """The pull request at `pr_url` (https://github.com/o/r/pull/N): "open",
   "merged" or "closed", or None when it can't be read."""
   m = re.match(r"https://github\.com/([^/]+)/([^/]+)/pull/(\d+)$", pr_url)
   if not m:
      return None
   req = urllib.request.Request(
      f"https://api.github.com/repos/{m.group(1)}/{m.group(2)}/pulls/{m.group(3)}",
      headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"})
   try:
      with urllib.request.urlopen(req, timeout=30) as resp:
         pr = json.load(resp)
   except (OSError, ValueError):
      return None
   return "merged" if pr.get("merged_at") else pr.get("state")
