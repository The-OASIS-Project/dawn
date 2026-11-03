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
import os
import re
import tempfile
import urllib.request

# Mount points the Claude Code sandbox puts over protected paths (seen with
# 2.1.317).  They exist only inside the sandbox, but `git add -A` there would
# stage them.  A path that is in the repo is unaffected.
SANDBOX_PLACEHOLDERS = [".bashrc", ".bash_profile", ".zshrc", ".zprofile", ".profile",
                        ".gitconfig", ".gitmodules", ".ripgreprc", ".mcp.json", ".idea",
                        ".vscode", ".claude/agents", ".claude/commands", ".claude/hooks",
                        ".claude/launch.json", ".claude/loop.md", ".claude/output-styles",
                        ".claude/routines", ".claude/scheduled", ".claude/scheduled_tasks.json",
                        ".claude/settings.local.json", ".claude/skills", ".claude/workflows",
                        ".claude/.cc-writes"]

SUBJECT_CAP = 200
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
   out, err = await proc.communicate(stdin.encode() if stdin is not None else None)
   if proc.returncode != 0:
      raise GitError(f"{' '.join(argv[-3:])}: {err.decode(errors='replace').strip()[:400]}")
   return out


def text(out: bytes) -> str:
   return out.decode(errors="replace").strip()


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

   async def collect(self, name: str, task_id: str, branch: str, base: str) -> dict:
      """Bring the task's branch into the mirror (as refs/agent/<id>) and
      describe it.  Only data crosses: a bundle, checked before it is read."""
      dirty = text(await self.jailed_git(task_id, "status", "--porcelain"))
      try:
         bundle = await self.jailed_git(task_id, "bundle", "create", "-",
                                        f"refs/heads/{branch}", f"^refs/remotes/origin/{base}")
      except GitError as e:
         if "empty bundle" not in str(e):
            raise
         # No commits now: an earlier collect's commits must not stay behind.
         await run(["git", "-C", self.mirror(name), "update-ref", "-d", f"refs/agent/{task_id}"])
         return {"commits": [], "diffstat": "", "uncommitted": len(dirty.splitlines())}
      m, ref = self.mirror(name), f"refs/agent/{task_id}"
      with tempfile.NamedTemporaryFile(suffix=".bundle") as f:
         f.write(bundle)
         f.flush()
         await self.gw("-C", m, "bundle", "verify", "-q", f.name)
         await self.gw("-C", m, "fetch", "-q", f.name, f"+refs/heads/{branch}:{ref}")
      commits = await self.gw("-C", m, "log", "--format=%h %s", f"refs/heads/{base}..{ref}")
      stat = await self.gw("-C", m, "diff", "--shortstat", f"refs/heads/{base}...{ref}")
      return {"commits": [c[:SUBJECT_CAP] for c in commits.splitlines()[:20]],
              "diffstat": stat, "uncommitted": len(dirty.splitlines())}

   # --- reading the agent's work (in the mirror only) -----------------------

   async def agent_ref(self, name: str, task_id: str, base: str) -> tuple[str, str]:
      """(head, merge base) of the task's collected branch."""
      m, ref = self.mirror(name), f"refs/agent/{task_id}"
      try:
         head = await self.gw("-C", m, "rev-parse", "--verify", "-q", ref)
      except GitError:
         raise GitError("nothing collected for this task yet")
      return head, await self.gw("-C", m, "merge-base", f"refs/heads/{base}", head)

   async def changes(self, name: str, task_id: str, base: str) -> dict:
      head, mb = await self.agent_ref(name, task_id, base)
      m = self.mirror(name)
      numstat = await run(["git", "-C", m, "diff", "--no-ext-diff", "--no-textconv", "--numstat",
                           "-z", mb, head])
      files, fields = [], numstat.split(b"\0")
      i = 0
      while i < len(fields) and fields[i]:
         added, removed, path = fields[i].decode(errors="replace").split("\t", 2)
         if not path:  # a rename: old and new paths follow
            path = f"{fields[i + 1].decode(errors='replace')} -> " \
                   f"{fields[i + 2].decode(errors='replace')}"
            i += 2
         files.append({"path": path, "added": None if added == "-" else int(added),
                       "removed": None if removed == "-" else int(removed)})
         i += 1
      log = await self.gw("-C", m, "log", "--format=%H%x00%s", f"-{COMMITS_CAP}", f"{mb}..{head}")
      commits = [dict(zip(("hash", "subject"), c.split("\0", 1))) for c in log.splitlines()]
      for c in commits:
         c["subject"] = c.get("subject", "")[:SUBJECT_CAP]
      return {"head": head, "base": mb, "files": files, "commits": commits}

   async def file(self, name: str, task_id: str, base: str, path: str, view: str) -> dict:
      """One file: its diff, or its content after (new) or before (old) the task."""
      head, mb = await self.agent_ref(name, task_id, base)
      p, m = safe_path(path), self.mirror(name)
      if view == "diff":
         for rev in (mb, head):  # git builds the whole diff in memory: check sizes first
            size = await self.size_of(m, f"{rev}:{p}")
            if size > FILE_CAP:
               return {"path": p, "view": view, "too_large": size}
         out = await run(["git", "--literal-pathspecs", "-C", m, "diff", "--no-ext-diff",
                          "--no-textconv", mb, head, "--", p])
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
                     open_pr: bool, title: str, body: str) -> dict:
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
      await run(["git", "-C", self.mirror(name), "push", "-q", url,
                 f"{head}:refs/heads/{branch}"], env=env)
      result = {"pushed": f"{owner}/{repo}:{branch}"}
      if open_pr:
         result["pr_url"] = await asyncio.to_thread(
            open_pull_request, owner, repo, token, branch, base, title, body)
      return result


def open_pull_request(owner, repo, token, head, base, title, body) -> str:
   req = urllib.request.Request(
      f"https://api.github.com/repos/{owner}/{repo}/pulls", method="POST",
      data=json.dumps({"title": title, "head": head, "base": base, "body": body}).encode(),
      headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"})
   with urllib.request.urlopen(req, timeout=30) as resp:
      return json.load(resp)["html_url"]
