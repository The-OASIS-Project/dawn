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
# Gateway configuration: a TOML file for settings, the environment for the
# two secrets (DAWN's bearer token and the GitHub token).  The secrets are
# taken out of the environment once read, so no child process inherits them.

import logging
import os
import tomllib
from dataclasses import dataclass, field

LIB = "/usr/local/lib/agent-gateway"
log = logging.getLogger("agent-gateway")


@dataclass
class Config:
   host: str = "127.0.0.1"
   port: int = 9760
   state_dir: str = "/var/lib/dawn-gateway"
   mirrors_dir: str = "/var/lib/dawn-gateway/mirrors"
   agent_home: str = "/var/lib/dawn-agent"
   work_dir: str = "/var/lib/dawn-agent/work"
   # Hidden from the agent's shell too (its file tools are denied by the floor):
   # DAWN's own data.  The gateway's state dir (its mirrors) always is.
   deny_read: list[str] = field(default_factory=lambda: ["/var/lib/dawn"])
   claude_cli: str = f"{LIB}/claude-as-agent"
   agent_gitconfig: str = f"{LIB}/agent-gitconfig"
   claude_bin: str | None = None  # an installed version to run; None = current
   run_as: list[str] = field(default_factory=lambda: ["sudo", "-n", "-u", "dawn-agent", "-H"])
   auth: str = "subscription"  # or "api_key"
   model: str | None = None
   git_name: str = "DAWN agent"
   git_email: str = "dawn-agent@localhost"
   task_budget_usd: float = 5.0
   daily_budget_usd: float = 25.0
   max_concurrent_tasks: int = 1
   keep_worktrees_days: int = 14
   default_domains: list[str] = field(default_factory=lambda: ["api.anthropic.com"])
   cbm_url: str | None = None
   repos: dict = field(default_factory=dict)
   token: str = ""
   github_token: str = ""


# Which keys each TOML section may set.  Paths to programs and the sudo
# prefix are not settable from the file.
SECTIONS = {
   "server": ("host", "port"),
   "paths": ("state_dir", "mirrors_dir", "agent_home", "work_dir", "deny_read"),
   "agent": ("auth", "model", "git_name", "git_email"),
   "limits": ("task_budget_usd", "daily_budget_usd", "max_concurrent_tasks",
              "keep_worktrees_days"),
   "network": ("default_domains",),
}


# Files to check before publishing, for a repo that names none: a task that changes
# one has it named to the user before publishing (never blocked).
DEFAULT_CHECK = [
   {"path": ".github/**", "why": "CI, which runs with the repo's secrets"},
   {"path": "CLAUDE.md", "why": "the agent's own instructions"},
   {"path": "AGENTS.md", "why": "the agent's own instructions"},
   {"path": ".claude/**", "why": "the agent's own instructions"},
]
WHY_CAP = 80  # characters of a pattern's reason


def check_list(repo: str, entries) -> list[dict]:
   """A repo's `check` list: each entry a glob string, or {path, why}."""
   bad = ValueError(f"[[repos]] {repo}: check is a list of repo-relative path patterns "
                    "(no leading or trailing /), each a string or { path, why }")
   if not isinstance(entries, list):
      raise bad
   out = []
   for e in entries:
      if isinstance(e, str):
         e = {"path": e}
      if not isinstance(e, dict) or set(e) - {"path", "why"}:
         raise bad
      path, why = e.get("path"), e.get("why", "")
      if not isinstance(path, str) or not path or path[0] == "/" or path[-1] == "/" \
            or not isinstance(why, str):
         raise bad
      why = "".join(" " if ord(ch) < 0x20 or ord(ch) == 0x7f else ch for ch in why)
      if len(why) > WHY_CAP:
         log.warning("[[repos]] %s: the reason for %s is cut to %d characters",
                     repo, path, WHY_CAP)
         why = why[:WHY_CAP]
      out.append({"path": path, "why": why})
   return out


def load(path: str, **overrides) -> Config:
   with open(path, "rb") as f:
      t = tomllib.load(f)
   c = Config(**overrides)
   for section, items in t.items():
      if section in ("repos", "cbm"):
         continue
      allowed = SECTIONS.get(section)
      if allowed is None:
         raise ValueError(f"unknown section [{section}]")
      for k, v in items.items():
         if k not in allowed:
            raise ValueError(f"unknown key [{section}] {k}")
         setattr(c, k, v)
   c.cbm_url = t.get("cbm", {}).get("url")
   for r in t.get("repos", []):
      if set(r) - {"name", "url", "base", "check", "protected"} or "name" not in r \
            or "url" not in r:
         raise ValueError("each [[repos]] needs name and url (optional: base, check)")
      if "protected" in r:  # the key's old name, read as check
         if "check" in r:
            log.warning("[[repos]] %s: check and protected are both set; using check "
                        "(remove protected)", r["name"])
         else:
            log.warning("[[repos]] %s: protected is now called check; rename it", r["name"])
      check = check_list(r["name"], r.get("check", r.get("protected", DEFAULT_CHECK)))
      c.repos[r["name"]] = {"url": r["url"], "base": r.get("base", "main"), "check": check}
   if not isinstance(c.deny_read, list) or not all(isinstance(p, str) and p.startswith("/")
                                                    for p in c.deny_read):
      raise ValueError("[paths] deny_read is a list of absolute paths")
   if c.auth not in ("subscription", "api_key"):
      raise ValueError("[agent] auth must be 'subscription' or 'api_key'")
   if c.task_budget_usd <= 0 or c.max_concurrent_tasks < 1:
      raise ValueError("[limits] budgets must be positive and max_concurrent_tasks >= 1")
   c.token = os.environ.pop("AGENT_GATEWAY_TOKEN", "")
   c.github_token = os.environ.pop("AGENT_GATEWAY_GITHUB_TOKEN", "")
   if len(c.token) < 32:
      raise ValueError("AGENT_GATEWAY_TOKEN must be set (32+ characters)")
   return c
