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

   async def has_claude_md(self, name: str, base: str) -> bool:
      try:
         await self.gw("-C", self.mirror(name), "cat-file", "-e",
                       f"refs/heads/{base}:CLAUDE.md")
         return True
      except GitError:
         return False

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

   async def remove(self, task_id: str) -> None:
      if re.fullmatch(r"[0-9a-f]{8}", task_id):
         await run([*self.run_as, "rm", "-rf", "--one-file-system", self.workdir(task_id)])

   async def publish(self, name: str, task_id: str, branch: str, base: str, token: str,
                     open_pr: bool, title: str, body: str) -> dict:
      """Push the collected branch from the mirror; optionally open a PR.  The
      token goes in a header, never in argv; no force push."""
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
                 f"refs/agent/{task_id}:refs/heads/{branch}"], env=env)
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
