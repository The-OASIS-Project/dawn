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
# Keeps Claude Code current without letting it update itself.  Claude Code
# ships inside the claude-agent-sdk package, so a new SDK release is a new
# Claude Code.  Run as root by agent-gateway-update.timer, by the managed
# interpreter in isolated mode (`python -I -S`): standard library only, and
# root never runs code from PyPI (the checks run it, as the gateway user).
#
#   1. a newer SDK release (same major version) on PyPI?  If not, done.
#   2. build it into its own venv next to the running one;
#   3. run the gateway's tests on it, then the live canary (canary.py);
#   4. if both pass and no task is running, point `current` at it and
#      restart the gateway; if the gateway doesn't come back, point back.
#
# The result goes to the gateway's update-status.json, which the gateway
# reports as an "update" event.  A version that fails is not tried again.

import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
import urllib.request

LIB = "/usr/local/lib/agent-gateway"
VENVS = f"{LIB}/venvs"
CURRENT = f"{LIB}/current"
ETC = "/usr/local/etc/agent-gateway"
STATE = "/var/lib/dawn-gateway"
SDK = "claude-agent-sdk"
TEST_DEPS = ["pytest==9.2.0", "httpx==0.28.1"]
KEEP_VENVS = 2  # the current one and the one before it, for going back by hand
# Results worth telling the operator about; the rest only go to the journal.
REPORTED = ("updated", "failed", "rolled_back", "needs_review", "error")


def version_key(v: str) -> tuple:
   return tuple(int(x) for x in re.findall(r"\d+", v))


def newer(candidate: str, installed: str) -> bool:
   return version_key(candidate) > version_key(installed)


def same_major(a: str, b: str) -> bool:
   """0.x releases may break the API at a minor bump, so 0.2 -> 0.3 counts."""
   ka, kb = version_key(a), version_key(b)
   return ka[:2] == kb[:2] if ka[0] == 0 else ka[0] == kb[0]


def sh(*argv: str, timeout: int = 1800, **kw) -> subprocess.CompletedProcess:
   return subprocess.run(argv, capture_output=True, text=True, timeout=timeout, **kw)


def latest_release() -> str:
   with urllib.request.urlopen(f"https://pypi.org/pypi/{SDK}/json", timeout=30) as r:
      return json.load(r)["info"]["version"]


def site_packages(venv: str) -> str:
   """Found on disk, not by asking the venv's python (that would run its
   packages' .pth files as root)."""
   found = glob.glob(f"{venv}/lib/python3*/site-packages")
   if len(found) != 1:
      raise RuntimeError(f"no single site-packages in {venv}")
   return found[0]


def installed(venv: str) -> list[str]:
   """name==version of every package in a venv, from its dist-info names."""
   pins = []
   for d in glob.glob(f"{site_packages(venv)}/*.dist-info"):
      m = re.match(r"(.+)-([^-]+)\.dist-info$", os.path.basename(d))
      if m:
         pins.append(f"{m.group(1)}=={m.group(2)}")
   return sorted(pins)


def bundled_claude(venv: str) -> str:
   return f"{site_packages(venv)}/claude_agent_sdk/_bundled/claude"


def gateway_call(tool: str) -> dict | None:
   """One MCP call to the running gateway, or None if it doesn't answer."""
   try:
      env = dict(line.split("=", 1) for line in open(f"{ETC}/env").read().splitlines()
                 if "=" in line and not line.startswith("#"))
      body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                         "params": {"name": tool, "arguments": {}}}).encode()
      with open(f"{ETC}/agent-gateway.toml", "rb") as f:
         port = tomllib.load(f).get("server", {}).get("port", 9760)
      req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp", data=body, headers={
         "Authorization": f"Bearer {env['AGENT_GATEWAY_TOKEN'].strip()}",
         "Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
      with urllib.request.urlopen(req, timeout=10) as r:
         return json.load(r)["result"]["structuredContent"]
   except Exception:
      return None


def gateway_idle() -> bool:
   if sh("systemctl", "is-active", "--quiet", "agent-gateway").returncode != 0:
      return True
   info = gateway_call("agents_list")
   return info is not None and info["running"] == 0


def as_gateway_user(*argv: str) -> subprocess.CompletedProcess:
   """Run as `dawn` (with its dawn-agent group), as the gateway itself runs;
   from / so nothing in dawn's own directory shadows the code under test."""
   return sh("runuser", "-u", "dawn", "--", "env", "-i", "PATH=/usr/bin:/bin",
             f"HOME={STATE}", *argv, cwd="/")


def build(version: str) -> str:
   venv = f"{VENVS}/{version}"
   shutil.rmtree(venv, ignore_errors=True)
   uv = [f"{LIB}/uv"]
   env = {**os.environ, "UV_PYTHON_INSTALL_DIR": f"{LIB}/python"}
   reqs = [line.strip() for line in open(f"{LIB}/requirements.txt")
           if line.strip() and not line.startswith("#") and not line.startswith(SDK)]
   # Everything but the SDK stays at what runs now; only what a new SDK
   # requires can move.  Wheels only: no package build runs as root.
   pinned = [p for p in installed(CURRENT) if not p.lower().startswith(("claude", "pytest",
                                                                        "httpx"))]
   constraints = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
   constraints.write("\n".join(pinned) + "\n")
   constraints.close()
   for step in ([*uv, "venv", "-q", "--python", "3.12", venv],
                [*uv, "pip", "install", "-q", "--no-build", "--python", f"{venv}/bin/python",
                 "-c", constraints.name, *reqs, f"{SDK}=={version}", *TEST_DEPS]):
      r = sh(*step, env=env)
      if r.returncode != 0:
         os.unlink(constraints.name)
         raise RuntimeError(f"build failed: {r.stderr.strip()[-400:]}")
   os.unlink(constraints.name)
   shutil.copytree(f"{site_packages(CURRENT)}/agent_gateway",
                   f"{site_packages(venv)}/agent_gateway",
                   ignore=shutil.ignore_patterns("__pycache__"))
   sh("chmod", "-R", "a+rX", venv)
   return venv


def check(venv: str) -> tuple[str, str]:
   """("passed" | "failed" | "inconclusive", detail).  Only a check that ran to
   the end and failed marks the version as failed."""
   tests = as_gateway_user(f"{venv}/bin/python", "-I", "-m", "pytest", "-q", "-p",
                           "no:cacheprovider", f"{LIB}/tests")
   if tests.returncode != 0:
      return "failed", "tests failed: " + tests.stdout.strip()[-400:]
   canary = as_gateway_user(f"{venv}/bin/python", "-I", "-m", "agent_gateway.canary",
                            "--config", f"{ETC}/agent-gateway.toml",
                            "--claude-bin", bundled_claude(venv))
   try:
      verdict = json.loads(canary.stdout.strip().splitlines()[-1])
   except (ValueError, IndexError):
      return "inconclusive", "sandbox check didn't finish: " + canary.stderr.strip()[-400:]
   detail = json.dumps(verdict)[-600:]
   if verdict.get("passed"):
      return "passed", detail
   return ("failed" if verdict.get("conclusive") else "inconclusive"), detail


def point_current(venv: str) -> None:
   tmp = f"{CURRENT}.new"
   if os.path.lexists(tmp):
      os.unlink(tmp)
   os.symlink(os.path.relpath(venv, LIB), tmp)
   os.replace(tmp, CURRENT)


def switch(old: str, new: str, version: str) -> bool:
   point_current(new)
   sh("systemctl", "restart", "agent-gateway")
   for _ in range(30):
      time.sleep(2)
      info = gateway_call("agents_list")
      if info and info.get("sdk") == version:  # the installed package's version
         return True
   point_current(old)
   sh("systemctl", "restart", "agent-gateway")
   return False


def prune() -> None:
   current = os.path.realpath(CURRENT)
   kept = sorted((d for d in os.listdir(VENVS) if os.path.isdir(f"{VENVS}/{d}")),
                 key=version_key, reverse=True)
   for d in kept[KEEP_VENVS:]:
      if os.path.realpath(f"{VENVS}/{d}") != current:
         shutil.rmtree(f"{VENVS}/{d}", ignore_errors=True)


def mark_failed(version: str, detail: str) -> None:
   with open(f"{VENVS}/{version}.failed", "w") as f:
      f.write(detail + "\n")


def read_status(path: str) -> dict:
   """The last reported status; never follows a link or blocks on a FIFO
   (the directory belongs to the gateway user)."""
   try:
      fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
   except OSError:
      return {}
   with os.fdopen(fd) as f:
      if not os.path.isfile(path) or os.fstat(f.fileno()).st_size > 65536:
         return {}
      try:
         return json.load(f)
      except ValueError:
         return {}


def write_status(path: str, status: dict) -> None:
   """A new file made with O_EXCL (mkstemp), then renamed: nothing the gateway
   user plants in its directory can redirect a root write."""
   fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".update-status-")
   with os.fdopen(fd, "w") as f:
      os.fchmod(f.fileno(), 0o644)
      json.dump(status, f)
   os.replace(tmp, path)


def update() -> dict:
   installed = os.path.basename(os.path.realpath(CURRENT))
   latest = latest_release()
   base = {"from": installed, "to": latest}
   if not newer(latest, installed):
      return {**base, "result": "current"}
   if not same_major(latest, installed):
      return {**base, "result": "needs_review",
              "detail": "a new major version may change the SDK's API; install it by hand"}
   if os.path.exists(f"{VENVS}/{latest}.failed"):
      return {**base, "result": "skipped", "detail": "this version failed its checks before"}
   if not gateway_idle():
      return {**base, "result": "deferred", "detail": "a task is running"}
   venv = build(latest)
   outcome, detail = check(venv)
   if outcome != "passed":
      shutil.rmtree(venv, ignore_errors=True)
      if outcome == "inconclusive":  # an outage or a usage limit: try again tomorrow
         return {**base, "result": "error", "detail": detail}
      mark_failed(latest, detail)
      return {**base, "result": "failed", "detail": detail}
   if not gateway_idle():
      shutil.rmtree(venv, ignore_errors=True)
      return {**base, "result": "deferred", "detail": "a task started during the checks"}
   if not switch(os.path.realpath(CURRENT), venv, latest):
      mark_failed(latest, "the gateway didn't come back on it")
      return {**base, "result": "rolled_back", "detail": "the gateway didn't come back on it"}
   prune()
   return {**base, "result": "updated", "detail": detail}


def main() -> None:
   if os.geteuid() != 0:
      sys.exit("run as root (agent-gateway-update.service)")
   try:
      status = update()
   except Exception as e:
      status = {"result": "error", "detail": str(e)[:600]}
   print(json.dumps(status))
   path = f"{STATE}/update-status.json"
   last = read_status(path)
   last.pop("time", None)
   # Each outcome is reported once (a version waiting for review isn't re-told daily).
   if status["result"] in REPORTED and status != last:
      status["time"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
      write_status(path, status)
   sys.exit(0 if status["result"] in ("current", "updated", "deferred", "skipped") else 1)


if __name__ == "__main__":
   main()
