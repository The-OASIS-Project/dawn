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
# Gateway tests without an agent: policy, store, grants, decisions and their
# delivery, git preparation, and the HTTP auth.  Run: pytest tests/

import asyncio
import os
import subprocess

import pytest

from agent_gateway import policy, store as st
from agent_gateway.config import Config
from agent_gateway.repos import Repos
from agent_gateway.runner import Runner


def test_policy(tmp_path):
   wt = str(tmp_path)
   assert policy.decide("Bash", {"command": "make; echo $?"}, wt, []).allow
   assert policy.decide("Read", {"file_path": "/etc/passwd"}, wt, []).allow
   assert policy.decide("Edit", {"file_path": f"{wt}/a.c"}, wt, []).allow
   d = policy.decide("Edit", {"file_path": f"{wt}/../escape.c"}, wt, [])
   assert not d.allow and d.ask_kind is None
   d = policy.decide("SandboxNetworkAccess", {"host": "Evil.Example"}, wt, [])
   assert not d.allow and (d.ask_kind, d.ask_value) == ("domain", "evil.example")
   for junk in ("", "10.0.0.1", "a b.org", "x" * 300 + ".org", "evil.org\nallow all"):
      d = policy.decide("SandboxNetworkAccess", {"host": junk}, wt, [])
      assert not d.allow and d.ask_kind is None
   assert policy.decide("AskUserQuestion", {}, wt, []).ask_kind == "question"
   assert policy.decide("mcp__x__y", {}, wt, []).ask_kind == "tool"
   assert policy.decide("mcp__x__y", {}, wt, ["mcp__x__y"]).allow


def test_read_tools_stay_out_of_other_tasks(tmp_path):
   work = tmp_path / "work"
   own, other = str(work / "aaaa0001"), str(work / "bbbb0002")
   assert policy.decide("Read", {"file_path": f"{own}/a.c"}, own, [], str(work)).allow
   assert policy.decide("Read", {"file_path": "/usr/include/stdio.h"}, own, [], str(work)).allow
   d = policy.decide("Read", {"file_path": f"{other}/secret"}, own, [], str(work))
   assert not d.allow and d.ask_kind is None
   assert not policy.decide("Grep", {"path": other, "pattern": "x"}, own, [], str(work)).allow


def test_edit_through_symlink_is_outside(tmp_path):
   wt = tmp_path / "wt"
   wt.mkdir()
   (wt / "link").symlink_to(tmp_path)
   assert not policy.decide("Write", {"file_path": f"{wt}/link/x"}, str(wt), []).allow


@pytest.fixture
def store(tmp_path):
   return st.Store(str(tmp_path / "g.db"))


def add_task(store, tid="t1", user=1, repo="r"):
   store.add_task(id=tid, dawn_user_id=user, agent="claude", repo=repo, base="main",
                  branch=f"agent/{tid}", worktree="/w", task="do it", budget_usd=5.0)
   return store.task(tid)


def test_grant_scopes(store):
   t1, t2 = add_task(store, "t1"), add_task(store, "t2", repo="other")
   store.add_grant(1, "domain", "a.org", "r", "t1")      # this task only
   store.add_grant(1, "domain", "b.org", "r", None)      # this repo
   store.add_grant(1, "domain", "c.org", None, None)     # always
   store.add_grant(2, "domain", "d.org", None, None)     # another user
   assert store.grants_for(t1, "domain") == ["a.org", "b.org", "c.org"]
   assert store.grants_for(t2, "domain") == ["c.org"]
   gid = store.grants(1)[1]["id"]
   assert not store.revoke_grant(gid, 2)                 # not theirs
   assert store.revoke_grant(gid, 1)
   assert store.grants_for(t1, "domain") == ["a.org", "b.org"]


def test_event_replay_and_wait(store):
   add_task(store)
   a = store.add_event("t1", "progress", text="x")
   store.add_event("t1", "progress", text="y")
   ev = store.events_after(a)
   assert [e["data"]["text"] for e in ev] == ["y"] and ev[0]["dawn_user_id"] == 1

   async def go():
      waiter = asyncio.create_task(store.wait_events(ev[0]["seq"], 5))
      await asyncio.sleep(0.05)
      store.add_event("t1", "progress", text="z")
      return await waiter, await store.wait_events(10**6, 0.05)
   got, none = asyncio.run(go())
   assert [e["data"]["text"] for e in got] == ["z"] and none == []


def test_daily_spend(store):
   add_task(store)
   store.add_event("t1", "run_end", cost_usd=1.5)
   store.add_event("t1", "run_end", cost_usd=0.25)
   assert store.spent_since(0) == pytest.approx(1.75)


class FakeRepos:
   async def prepare(self, name, base, branch, task_id):
      return f"/w/{task_id}"

   async def remove(self, task_id):
      pass


class FakeRunner(Runner):
   """Records launches instead of starting the agent."""

   def __init__(self, *a):
      super().__init__(*a)
      self.resumed = []

   def launch(self, task_id, prompt):
      self.store.update_task(task_id, status=st.WORKING, error=None)
      self.running[task_id] = object()  # counts toward capacity, like a real run
      self.resumed.append((task_id, prompt))


def runner(store, **cfg):
   cfg.setdefault("repos", {"r": {"url": "u", "base": "main"}})
   return FakeRunner(Config(token="x" * 32, **cfg), store, FakeRepos())


def test_reply_allow_resumes_with_grant(store):
   r = runner(store)
   add_task(store)
   store.update_task("t1", status=st.INPUT_REQUIRED)
   rid = store.add_request("t1", "domain", {"value": "pypi.org"})
   asyncio.run(r.reply("t1", 1, rid, "always_repo", None))
   assert store.grants_for(store.task("t1"), "domain") == ["pypi.org"]
   assert r.resumed and "allowed domain 'pypi.org'" in r.resumed[0][1]
   assert store.take_notes("t1") == []                    # delivered once


def test_reply_waits_for_all_requests_and_checks_owner(store):
   r = runner(store)
   add_task(store)
   store.update_task("t1", status=st.INPUT_REQUIRED)
   q = store.add_request("t1", "question", {})
   d = store.add_request("t1", "domain", {"value": "x.org"})
   with pytest.raises(ValueError):
      asyncio.run(r.reply("t1", 2, q, "", "yes"))         # another user
   asyncio.run(r.reply("t1", 1, q, "", "use config.toml"))
   assert r.resumed == []                                 # x.org still pending
   asyncio.run(r.reply("t1", 1, d, "deny", None))
   notes = r.resumed[0][1]
   assert "use config.toml" in notes and "declined domain 'x.org'" in notes
   with pytest.raises(ValueError):
      asyncio.run(r.reply("t1", 1, d, "allow", None))     # already decided


def test_decision_during_run_is_delivered_at_finish(store):
   r = runner(store)
   add_task(store)
   rid = store.add_request("t1", "question", {})
   r.running["t1"] = object()                             # its run is still going
   asyncio.run(r.reply("t1", 1, rid, "", "42"))
   assert r.resumed == []
   del r.running["t1"]                                    # the run ends
   asyncio.run(r.finish("t1", None))
   assert r.resumed and "42" in r.resumed[0][1]


def test_blocked_reply_keeps_answer_until_room(store):
   r = runner(store)
   add_task(store)
   store.update_task("t1", status=st.INPUT_REQUIRED)
   rid = store.add_request("t1", "question", {})
   r.running["other"] = object()                          # no room
   asyncio.run(r.reply("t1", 1, rid, "", "42"))
   t = store.task("t1")
   assert t["status"] == st.INPUT_REQUIRED and "already running" in t["error"]
   assert r.resumed == [] and store.has_notes("t1")       # the answer is kept
   del r.running["other"]
   asyncio.run(r.retry_blocked())                         # a run ended
   assert r.resumed and "42" in r.resumed[0][1]


def test_recover_parks_cut_off_runs(store):
   r = runner(store, max_concurrent_tasks=1)
   add_task(store, "t1")
   add_task(store, "t2")
   add_task(store, "t3")
   store.update_task("t1", session_id="s1")
   store.update_task("t2", session_id="s2")
   asyncio.run(r.recover())
   assert [x[0] for x in r.resumed] == ["t1"] or [x[0] for x in r.resumed] == ["t2"]
   waiting = [t for t in ("t1", "t2") if store.task(t)["status"] == st.INPUT_REQUIRED]
   assert len(waiting) == 1 and store.has_notes(waiting[0])
   assert store.task("t3")["status"] == st.FAILED       # never had a session


def test_spent_budget_fails_instead_of_resuming(store):
   r = runner(store)
   add_task(store)
   store.update_task("t1", status=st.INPUT_REQUIRED)
   store.add_cost("t1", 5.0)
   rid = store.add_request("t1", "question", {})
   asyncio.run(r.reply("t1", 1, rid, "", "go on"))
   assert store.task("t1")["status"] == st.FAILED and r.resumed == []


def test_start_is_idempotent_by_client_ref(store):
   r = runner(store, max_concurrent_tasks=3)
   a = asyncio.run(r.start(1, "r", "fix it", None, "ref-1"))
   b = asyncio.run(r.start(1, "r", "fix it", None, "ref-1"))
   assert a["id"] == b["id"] and len(r.resumed) == 1
   c = asyncio.run(r.start(2, "r", "fix it", None, "ref-1"))  # another user
   assert c["id"] != a["id"]
   with pytest.raises(ValueError):
      asyncio.run(r.start(1, "r", "x", -1.0, None))


class Result:
   def __init__(self, total):
      self.total_cost_usd = total


def test_cost_is_per_run_when_the_session_reports_totals(store):
   r = runner(store)
   add_task(store)
   r.record_cost("t1", Result(2.0))             # first run: $2
   r.record_cost("t1", Result(3.5))             # resumed: the CLI reports $3.5 in all
   r.record_cost("t1", Result(0.4))             # a fresh session (total restarted)
   r.record_cost("t1", None)                    # killed: nothing reported
   assert store.task("t1")["cost_usd"] == pytest.approx(3.9)
   assert store.spent_since(0) == pytest.approx(3.9)


def test_retry_during_start_returns_the_same_task(store):
   class SlowRepos(FakeRepos):
      async def prepare(self, *a):
         await asyncio.sleep(0.05)
         return "/w/x"
   r = runner(store)
   r.repos = SlowRepos()

   async def go():
      return await asyncio.gather(r.start(1, "r", "fix", None, "ref"),
                                  r.start(1, "r", "fix", None, "ref"))
   a, b = asyncio.run(go())
   assert a["id"] == b["id"] and len(r.resumed) == 1


def test_capacity_and_budget(store):
   r = runner(store, max_concurrent_tasks=1, daily_budget_usd=1.0)
   r.running["busy"] = object()
   with pytest.raises(ValueError, match="already running"):
      r.check_capacity()
   r.running.clear()
   add_task(store)
   store.add_event("t1", "run_end", cost_usd=1.0)
   with pytest.raises(ValueError, match="daily budget"):
      r.check_capacity()


def git(*a, cwd=None):
   return subprocess.run(["git", *a], cwd=cwd, check=True, capture_output=True,
                         text=True).stdout.strip()


@pytest.fixture
def service_umask():
   old = os.umask(0o077)  # as the systemd unit sets
   yield
   os.umask(old)


def test_mirror_clone_collect(tmp_path, service_umask):
   """The gateway's git layer end to end, as one user (sudo is the only
   difference on a real install): mirror, task clone, jailed bundle, collect."""
   src = tmp_path / "src"
   src.mkdir()
   git("init", "-q", "-b", "main", cwd=src)
   (src / "f").write_text("x\n")
   git("add", "f", cwd=src)
   git("-c", "user.email=a@b", "-c", "user.name=a", "commit", "-qm", "init", cwd=src)
   home = tmp_path / "home"
   (home / "work").mkdir(parents=True)
   gitconfig = tmp_path / "gitconfig"
   gitconfig.write_text("")
   repos = Repos({"r": {"url": str(src), "base": "main"}}, str(tmp_path / "mirrors"),
                 str(home / "work"), [], str(home), str(gitconfig))
   wt = asyncio.run(repos.prepare("r", "main", "agent/aaaa0001-x", "aaaa0001"))
   assert git("-C", wt, "config", "user.email") == "dawn-agent@localhost"
   exclude = (tmp_path / "home/work/aaaa0001/.git/info/exclude").read_text()
   assert "/.claude/hooks" in exclude and "/.bashrc" in exclude
   assert not asyncio.run(repos.has_claude_md("r", "main"))
   # Under the service's umask the mirror must still be readable, never writable,
   # by the agent's group.
   mirror = tmp_path / "mirrors/r.git"
   for p in [mirror / "config", mirror / "HEAD", *mirror.glob("refs/heads/*")]:
      mode = p.stat().st_mode & 0o777
      assert mode & 0o040 and not mode & 0o022, (p, oct(mode))
   empty = asyncio.run(repos.collect("r", "aaaa0001", "agent/aaaa0001-x", "main"))
   assert empty["commits"] == [] and empty["uncommitted"] == 0
   (tmp_path / "home/work/aaaa0001/g").write_text("y\n")
   git("add", "g", cwd=wt)
   git("commit", "-qm", "agent work", cwd=wt)
   (tmp_path / "home/work/aaaa0001/h").write_text("dirty\n")
   out = asyncio.run(repos.collect("r", "aaaa0001", "agent/aaaa0001-x", "main"))
   assert [c.split(" ", 1)[1] for c in out["commits"]] == ["agent work"]
   assert "1 file changed" in out["diffstat"] and out["uncommitted"] == 1
   assert git("-C", str(tmp_path / "mirrors/r.git"), "log", "-1", "--format=%s",
              "refs/agent/aaaa0001") == "agent work"
   from agent_gateway.repos import GitError
   with pytest.raises(GitError, match="github.com"):
      asyncio.run(repos.publish("r", "aaaa0001", "agent/aaaa0001-x", "main", "tok", False,
                                "t", "b"))
   asyncio.run(repos.remove("aaaa0001"))
   assert not os.path.exists(wt)


def test_http_requires_bearer():
   from starlette.testclient import TestClient
   from agent_gateway import server
   cfg = Config(token="s" * 32)
   # The test client serves from another thread; the gateway itself uses one.
   app = server.app(runner(st.Store(":memory:", check_same_thread=False)), cfg)
   with TestClient(app, base_url="http://127.0.0.1:9760") as c:
      body = {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}
      hdr = {"Accept": "application/json, text/event-stream", "Content-Type": "application/json"}
      assert c.post("/mcp", json=body, headers=hdr).status_code == 401
      bad = {**hdr, "Authorization": "Bearer " + "t" * 32}
      assert c.post("/mcp", json=body, headers=bad).status_code == 401
      ok = c.post("/mcp", json=body, headers={**hdr, "Authorization": "Bearer " + "s" * 32})
      assert ok.status_code == 200, ok.text
      names = {t["name"] for t in ok.json()["result"]["tools"]}
      assert {"task_start", "task_events", "task_reply", "task_publish", "grant_revoke"} <= names
      call = {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
              "params": {"name": "task_cancel", "arguments": {"dawn_user_id": 1, "task_id": "x"}}}
      res = c.post("/mcp", json=call, headers={**hdr, "Authorization": "Bearer " + "s" * 32})
      assert res.json()["result"]["isError"] and "no such task" in res.text
      evil = {**hdr, "Authorization": "Bearer " + "s" * 32, "Host": "evil.example"}
      assert c.post("/mcp", json=body, headers=evil).status_code in (400, 403, 421)


def test_permission_handler_records_request(store):
   r = runner(store)
   t = add_task(store)
   handler = r.permission_handler(t, [])
   res = asyncio.run(handler("SandboxNetworkAccess", {"host": "x.org"}, None))
   assert res.behavior == "deny" and "operator has been asked" in res.message
   ev = store.events_after(0)[-1]
   assert ev["kind"] == "request" and ev["data"]["request_kind"] == "domain"
   assert store.pending_requests("t1")[0]["payload"]["value"] == "x.org"
   q = asyncio.run(handler("AskUserQuestion", {"questions": [{"question": "int or long?"}]}, None))
   assert q.behavior == "deny" and store.events_after(0)[-1]["data"]["request_kind"] == "question"
   assert asyncio.run(handler("Bash", {"command": "make"}, None)).behavior == "allow"


def test_cancel_marks_first_and_is_final(store):
   r = runner(store)
   add_task(store)
   store.update_task("t1", status=st.INPUT_REQUIRED)
   rid = store.add_request("t1", "question", {})
   assert asyncio.run(r.cancel("t1", 1))["status"] == st.CANCELLED
   assert store.request(rid)["status"] == "cancelled"
   asyncio.run(r.finish("t1", None))                      # a run ending late
   assert store.task("t1")["status"] == st.CANCELLED
   assert [e["kind"] for e in store.events_after(0)].count("cancelled") == 1
