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
# What happens when Claude Code asks permission for a call its settings
# didn't already allow.  Decided here in code, never by a model: the request
# comes from an agent that has read outside text, so letting a model judge
# what is "obvious" would let that text argue its way through.

import os
import re
from dataclasses import dataclass

HOSTNAME = re.compile(r"(?=.{1,253}$)([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}",
                      re.IGNORECASE)

# Every Bash call runs in the sandbox (allowUnsandboxedCommands = false), so
# Claude Code's own "can't analyse this command" prompts are allowed: the
# sandbox, not command text, is the boundary.
SANDBOXED = {"Bash", "BashOutput", "KillShell"}

# Tools that read or only organise the agent's own work.  Secrets are kept out
# by the floor's Read deny rules and by Unix permissions.
CBM_READ_TOOLS = ("search_graph", "query_graph", "get_architecture", "get_code_snippet",
                  "search_code", "trace_path", "get_graph_schema", "list_projects")

# The agent runs with no settings loaded but the gateway's floor (no repo or
# user settings), so nothing can pre-approve a call around this function;
# every call that would prompt comes here.
READ_ONLY = {*(f"mcp__cbm__{t}" for t in CBM_READ_TOOLS),
             "Read", "Glob", "Grep", "LS", "NotebookRead", "TodoWrite", "ToolSearch",
             "Task", "Agent", "TaskOutput", "Skill", "EnterPlanMode", "ExitPlanMode"}

EDITS = {"Edit", "Write", "MultiEdit", "NotebookEdit"}


@dataclass
class Decision:
   allow: bool
   message: str = ""
   ask_kind: str | None = None  # "domain", "tool" or "question": record a request
   ask_value: str | None = None


def inside(path: str, root: str) -> bool:
   real, top = os.path.realpath(path), os.path.realpath(root)
   return real == top or real.startswith(top.rstrip(os.sep) + os.sep)


GLOB_CHARS = re.compile(r"[*?\[{]")


def read_paths(tool: str, tool_input: dict) -> list[str]:
   """Every path a read tool call names.  An absolute Glob pattern is a path
   too: its part before the first wildcard is where the search starts."""
   paths = [str(tool_input.get(k) or "") for k in ("file_path", "notebook_path", "path")]
   pattern = str(tool_input.get("pattern") or "")
   if tool == "Glob" and pattern.startswith("/"):
      paths.append(os.path.dirname(GLOB_CHARS.split(pattern, 1)[0]) or "/")
   return [p for p in paths if p]


def reaches_hidden(path: str, worktree: str, hidden: list[str]) -> bool:
   """Inside a hidden directory (other than the task's own worktree), or a
   search rooted above one, which would walk into it.  A relative or `~` path
   is refused: one that reaches here already leaves the worktree (reads inside
   it are allowed before asking), and this process can't resolve it as the
   agent does."""
   if not path.startswith("/"):
      return True
   for h in hidden:
      if inside(path, h) and not inside(path, worktree):
         return True
      if inside(h, path):
         return True
   return False


WAIT = ("The operator has been asked. Stop here: end your turn with a one-line status of "
        "where the work stands. You will be resumed with the answer.")


def decide(tool: str, tool_input: dict, worktree: str, granted_tools: list[str],
           hidden: list[str] = ()) -> Decision:
   """`hidden`: directories the read tools may not reach (other tasks'
   clones, the gateway's own files, DAWN's data); the sandbox hides the same
   ones from Bash."""
   if tool in SANDBOXED:
      return Decision(True)
   if tool in READ_ONLY:
      if any(reaches_hidden(p, worktree, hidden) for p in read_paths(tool, tool_input)):
         return Decision(False, "That path is not readable from this task.")
      return Decision(True)
   if tool in EDITS:
      path = tool_input.get("file_path") or tool_input.get("notebook_path") or ""
      if path and inside(path, worktree):
         return Decision(True)
      return Decision(False, "Edits are allowed only inside the task worktree.")
   if tool == "SandboxNetworkAccess":
      host = str(tool_input.get("host", ""))
      if not HOSTNAME.fullmatch(host):
         return Decision(False, "Only named hosts can be requested.")
      return Decision(False, WAIT, "domain", host.lower())
   if tool == "AskUserQuestion":
      return Decision(False, WAIT, "question", None)
   if tool in granted_tools:
      return Decision(True)
   return Decision(False, WAIT, "tool", tool)
