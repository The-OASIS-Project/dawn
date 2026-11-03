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

import os
import tomllib
from dataclasses import dataclass, field

LIB = "/usr/local/lib/agent-gateway"


@dataclass
class Config:
   host: str = "127.0.0.1"
   port: int = 9760
   state_dir: str = "/var/lib/dawn-gateway"
   mirrors_dir: str = "/var/lib/dawn-gateway/mirrors"
   agent_home: str = "/var/lib/dawn-agent"
   work_dir: str = "/var/lib/dawn-agent/work"
   claude_cli: str = f"{LIB}/claude-as-agent"
   agent_gitconfig: str = f"{LIB}/agent-gitconfig"
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
   "paths": ("state_dir", "mirrors_dir", "agent_home", "work_dir"),
   "agent": ("auth", "model", "git_name", "git_email"),
   "limits": ("task_budget_usd", "daily_budget_usd", "max_concurrent_tasks",
              "keep_worktrees_days"),
   "network": ("default_domains",),
}


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
      if set(r) - {"name", "url", "base"} or "name" not in r or "url" not in r:
         raise ValueError("each [[repos]] needs name and url (and optional base)")
      c.repos[r["name"]] = {"url": r["url"], "base": r.get("base", "main")}
   if c.auth not in ("subscription", "api_key"):
      raise ValueError("[agent] auth must be 'subscription' or 'api_key'")
   if c.task_budget_usd <= 0 or c.max_concurrent_tasks < 1:
      raise ValueError("[limits] budgets must be positive and max_concurrent_tasks >= 1")
   c.token = os.environ.pop("AGENT_GATEWAY_TOKEN", "")
   c.github_token = os.environ.pop("AGENT_GATEWAY_GITHUB_TOKEN", "")
   if len(c.token) < 32:
      raise ValueError("AGENT_GATEWAY_TOKEN must be set (32+ characters)")
   return c
