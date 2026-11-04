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
import json
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

   async def collect(self, name, task_id, branch, base):
      return {"commits": [], "diffstat": "", "uncommitted": 0}

   async def agent_ref(self, name, task_id, base):
      return "abcdef1234567890", "base000"

   async def claude_md(self, name, base):
      return "Commits need the developer's OK."

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


def test_cancel_during_start_stops_the_task_once_it_exists(store):
   """A caller that gave up waiting on a start cancels it by client_ref; the task
   the start then makes is cancelled, and a retried start returns it as such."""
   class SlowRepos(FakeRepos):
      async def prepare(self, *a):
         await asyncio.sleep(0.05)
         return "/w/x"

   class Quiet(FakeRunner):
      def launch(self, task_id, prompt):  # no run to interrupt
         self.store.update_task(task_id, status=st.WORKING, error=None)
   r = Quiet(Config(token="x" * 32, repos={"r": {"url": "u", "base": "main"}}), store,
             SlowRepos())

   async def go():
      start = asyncio.create_task(r.start(1, "r", "fix", None, "job:7"))
      await asyncio.sleep(0.01)
      assert await r.cancel_ref(1, "job:7") is None  # still starting
      return await start
   t = asyncio.run(go())
   assert t["status"] == st.CANCELLED and not r.cancel_on_start
   again = asyncio.run(r.start(1, "r", "fix", None, "job:7"))
   assert again["id"] == t["id"] and again["status"] == st.CANCELLED
   with pytest.raises(ValueError):
      asyncio.run(r.cancel_ref(1, "job:8"))


def test_cancelled_start_never_reaches_the_agent(store, monkeypatch):
   """The real launch path: a start cancelled mid-clone never opens an agent
   session, and a cancel just after launch stops the run before it does."""
   from agent_gateway import runner as rmod
   opened = []

   class Client:
      def __init__(self, options):
         pass

      async def __aenter__(self):
         opened.append(1)
         return self

      async def __aexit__(self, *a):
         return False

   monkeypatch.setattr(rmod, "ClaudeSDKClient", Client)

   class SlowRepos(FakeRepos):
      async def prepare(self, *a):
         await asyncio.sleep(0.05)
         return "/w/x"

   class R(Runner):
      async def options(self, task):
         await asyncio.sleep(0.05)

   r = R(Config(token="x" * 32, repos={"r": {"url": "u", "base": "main"}}), store, SlowRepos())

   async def go():
      start = asyncio.create_task(r.start(1, "r", "fix", None, "job:7"))
      await asyncio.sleep(0.01)
      await r.cancel_ref(1, "job:7")
      t = await start
      later = await r.start(1, "r", "fix", None, "job:8")  # launched, agent not yet reached
      stopped = await r.cancel(later["id"], 1)
      return t, stopped
   t, stopped = asyncio.run(go())
   assert t["status"] == st.CANCELLED and stopped["status"] == st.CANCELLED
   assert not opened and not r.running and not r.cancel_on_start


def test_failed_start_clears_its_cancel(store):
   class Broken(FakeRepos):
      async def prepare(self, *a):
         await asyncio.sleep(0.02)
         raise ValueError("clone failed")
   r = runner(store)
   r.repos = Broken()

   async def go():
      start = asyncio.create_task(r.start(1, "r", "fix", None, "job:7"))
      await asyncio.sleep(0.005)
      await r.cancel_ref(1, "job:7")
      with pytest.raises(ValueError):
         await start
   asyncio.run(go())
   assert not r.cancel_on_start


def test_cancelled_start_wakes_a_blocked_task(store):
   """A start that runs nothing frees its slot: a task blocked behind it resumes."""
   class SlowRepos(FakeRepos):
      async def prepare(self, *a):
         await asyncio.sleep(0.05)
         return "/w/x"
   r = FakeRunner(Config(token="x" * 32, repos={"r": {"url": "u", "base": "main"}}), store,
                  SlowRepos())
   add_task(store, "b1")
   store.update_task("b1", status=st.INPUT_REQUIRED)
   rid = store.add_request("b1", "question", {"value": "q"})

   async def go():
      start = asyncio.create_task(r.start(1, "r", "fix", None, "job:7"))
      await asyncio.sleep(0.01)
      await r.reply("b1", 1, rid, "answer", "yes")  # the start holds the only slot
      assert store.task("b1")["status"] == st.INPUT_REQUIRED
      await r.cancel_ref(1, "job:7")
      await start
      await asyncio.sleep(0.1)
   asyncio.run(go())
   assert [t for t, _ in r.resumed] == ["b1"]


def test_cancel_before_the_agent_is_reached(store, monkeypatch):
   """A run stopped while it gets ready (options, connect) never reaches the agent,
   says cancelled once, and frees its slot for a blocked task."""
   from agent_gateway import runner as rmod

   class Client:
      def __init__(self, options):
         pass

      async def __aenter__(self):
         raise AssertionError("agent reached")

      async def __aexit__(self, *a):
         return False

   monkeypatch.setattr(rmod, "ClaudeSDKClient", Client)

   class R(Runner):
      async def options(self, task):
         await asyncio.sleep(0.2)

   r = R(Config(token="x" * 32, repos={"r": {"url": "u", "base": "main"}}), store, FakeRepos())
   retried = []

   async def go():
      t = await r.start(1, "r", "fix", None, "job:9")
      await asyncio.sleep(0.01)  # the run is inside options()
      orig = r.retry_blocked

      async def rb():
         retried.append(1)
         await orig()
      r.retry_blocked = rb
      out = await r.cancel(t["id"], 1)
      await asyncio.sleep(0.05)
      return t["id"], out
   tid, out = asyncio.run(go())
   kinds = [e["kind"] for e in store.events_after(0) if e["task_id"] == tid]
   assert out["status"] == st.CANCELLED and not r.running and retried
   assert kinds.count("cancelled") == 1


def test_reply_during_collect_does_not_complete_the_task(store):
   class SlowCollect(FakeRepos):
      async def collect(self, *a):
         await asyncio.sleep(0.05)
         return await super().collect(*a)
   r = runner(store)
   r.repos = SlowCollect()
   add_task(store)
   rid = store.add_request("t1", "question", {})

   class Result:
      is_error, result, subtype = False, "done", "success"

   async def go():
      ending = asyncio.ensure_future(r.finish("t1", Result()))
      await asyncio.sleep(0.01)                            # mid-collect
      await r.reply("t1", 1, rid, "", "42")                 # starts a new run
      await ending
   asyncio.run(go())
   assert store.task("t1")["status"] == st.WORKING and r.resumed


def test_publish_pushes_the_commit_that_was_shown(store):
   class Pushing(FakeRepos):
      async def publish(self, name, head, *a):
         self.pushed = head
         return {"pushed": f"o/r:{head[:7]}"}
   r = runner(store, github_token="tok")
   r.repos = Pushing()
   add_task(store)
   store.update_task("t1", status=st.COMPLETED)
   asyncio.run(r.publish("t1", 1, False, None, "ABCDEF1234567890"))
   assert r.repos.pushed == "abcdef1234567890"


def test_publish_refuses_a_moved_branch(store):
   r = runner(store, github_token="tok")
   add_task(store)
   store.update_task("t1", status=st.COMPLETED)
   with pytest.raises(ValueError, match="moved"):
      asyncio.run(r.publish("t1", 1, False, None, "1111111"))
   with pytest.raises(ValueError, match="moved"):
      asyncio.run(r.publish("t1", 1, False, None, "abcdef1"))  # a prefix isn't enough


def test_events_carry_client_ref_and_batches_are_capped(store):
   store.add_task(id="t1", dawn_user_id=1, agent="claude", repo="r", base="main",
                  branch="agent/t1", worktree="/w", task="x", budget_usd=5.0,
                  client_ref="job:42")
   for _ in range(5):
      store.add_event("t1", "progress", text="x" * 1000)
   ev = store.events_after(0, max_bytes=2500)
   assert len(ev) == 2 and all(e["client_ref"] == "job:42" for e in ev)
   assert len(store.events_after(0, max_bytes=10)) == 1          # always at least one


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
   assert asyncio.run(repos.claude_md("r", "main")) is None
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
   # The viewer reads the collected branch in the mirror.
   ch = asyncio.run(repos.changes("r", "aaaa0001", "main"))
   assert [f["path"] for f in ch["files"]] == ["g"] and ch["files"][0]["added"] == 1
   assert [c["subject"] for c in ch["commits"]] == ["agent work"]
   diff = asyncio.run(repos.file("r", "aaaa0001", "main", "g", "diff"))
   assert "+y" in diff["text"]
   assert asyncio.run(repos.file("r", "aaaa0001", "main", "g", "new"))["text"] == "y\n"
   assert asyncio.run(repos.file("r", "aaaa0001", "main", "g", "old"))["missing"]
   names = {e["name"] for e in asyncio.run(repos.tree("r", "aaaa0001", "main", ""))["entries"]}
   assert names == {"f", "g"}
   # Pathspec magic and globs are literal names here, so they select nothing.
   assert asyncio.run(repos.file("r", "aaaa0001", "main", "*", "diff"))["text"] == ""
   sub = asyncio.run(repos.tree("r", "aaaa0001", "main", "/"))
   assert {e["name"] for e in sub["entries"]} == {"f", "g"}
   # A leading / is read as the repo's root, never the system's.
   assert asyncio.run(repos.file("r", "aaaa0001", "main", "/etc/passwd", "new"))["missing"]
   # A name starting with - is a file name here, never an option.
   assert asyncio.run(repos.file("r", "aaaa0001", "main", "-c", "new"))["missing"]
   for bad in ("../x", "a/../../b", "a\0b"):
      with pytest.raises(GitError):
         asyncio.run(repos.file("r", "aaaa0001", "main", bad, "new"))
   # Too large for the viewer: refused before git builds the diff.
   big = tmp_path / "home/work/aaaa0001/big.txt"
   big.write_text("x\n" * 300_000)
   git("add", "big.txt", cwd=wt)
   git("commit", "-qm", "big", cwd=wt)
   asyncio.run(repos.collect("r", "aaaa0001", "agent/aaaa0001-x", "main"))
   assert asyncio.run(repos.file("r", "aaaa0001", "main", "big.txt", "diff"))["too_large"]
   # The agent drops its commits: the earlier collected work must not linger.
   git("reset", "-q", "--hard", "origin/main", cwd=wt)
   gone = asyncio.run(repos.collect("r", "aaaa0001", "agent/aaaa0001-x", "main"))
   assert gone["commits"] == []
   with pytest.raises(GitError, match="nothing collected"):
      asyncio.run(repos.changes("r", "aaaa0001", "main"))
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
      # DAWN knows a gateway by task_start + task_events (mcp_schema_is_agent_gateway) and
      # then offers none of its tools to a model: renaming either one breaks that.
      assert {"task_start", "task_events", "task_reply", "task_publish", "grant_revoke"} <= names
      auth = {**hdr, "Authorization": "Bearer " + "s" * 32}
      for args in ({"dawn_user_id": 1, "client_ref": "job:9"}, {"dawn_user_id": 1}):
         res = c.post("/mcp", json={"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
            "name": "task_list" if "client_ref" in args else "task_cancel",
            "arguments": args}}, headers=auth).json()["result"]
         assert res["isError"] and ("no such task" in str(res) or "name the task" in str(res))
      call = {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
              "params": {"name": "task_cancel", "arguments": {"dawn_user_id": 1, "task_id": "x"}}}
      res = c.post("/mcp", json=call, headers={**hdr, "Authorization": "Bearer " + "s" * 32})
      assert res.json()["result"]["isError"] and "no such task" in res.text
      from agent_gateway import update as up
      listed = up.tool_result(c.post("/mcp", json=body | {"method": "tools/call", "params": {
         "name": "agents_list", "arguments": {}}}, headers={**hdr, "Authorization": "Bearer "
                                                            + "s" * 32}).json())
      assert listed["running"] == 0 and "sdk" in listed    # what the updater reads
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


def test_update_version_rules():
   from agent_gateway import update as up
   assert up.newer("0.2.178", "0.2.177") and not up.newer("0.2.177", "0.2.177")
   assert up.newer("0.2.1000", "0.2.999")
   assert up.same_major("0.2.178", "0.2.177")
   assert not up.same_major("0.3.0", "0.2.177")      # 0.x: a minor bump may break the API
   assert up.same_major("1.4.0", "1.0.2") and not up.same_major("2.0.0", "1.9.9")


def test_update_reports_each_outcome_once(tmp_path, monkeypatch, capsys):
   from agent_gateway import update as up
   monkeypatch.setattr(up, "STATE", str(tmp_path))
   monkeypatch.setattr(up.os, "geteuid", lambda: 0)
   outcome = {"from": "0.2.177", "to": "0.3.0", "result": "needs_review", "detail": "x"}
   monkeypatch.setattr(up, "update", lambda: dict(outcome))
   status = tmp_path / "update-status.json"
   for _ in range(2):
      with pytest.raises(SystemExit):
         up.main()
   first = status.stat().st_mtime_ns
   with pytest.raises(SystemExit):
      up.main()
   assert status.stat().st_mtime_ns == first          # same outcome: not re-reported
   monkeypatch.setattr(up, "update", lambda: {"result": "current"})
   with pytest.raises(SystemExit) as e:
      up.main()
   assert e.value.code == 0 and json.loads(status.read_text())["result"] == "needs_review"


def test_system_events_have_no_user(store):
   add_task(store)
   store.add_event("", "update", result="updated", to="0.2.178")
   store.add_event("t1", "progress", text="x")
   ev = store.events_after(0)
   assert ev[0]["kind"] == "update" and ev[0]["dawn_user_id"] is None
   assert ev[1]["dawn_user_id"] == 1


def test_update_status_write_ignores_planted_links(tmp_path):
   from agent_gateway import update as up
   victim = tmp_path / "victim"
   victim.write_text("keep")
   status = tmp_path / "update-status.json"
   for planted in (tmp_path / "update-status.json.new", tmp_path / ".update-status-x"):
      planted.symlink_to(victim)
   up.write_status(str(status), {"result": "updated"})
   assert victim.read_text() == "keep" and not status.is_symlink()
   assert (status.stat().st_mode & 0o777) == 0o644
   status.unlink()
   status.symlink_to(victim)                              # a link is never read
   assert up.read_status(str(status)) == {}
   status.unlink()
   os.mkfifo(status)                                      # nor does a FIFO block it
   assert up.read_status(str(status)) == {}


def test_update_reads_installed_versions_without_running_python(tmp_path):
   from agent_gateway import update as up
   site = tmp_path / "v/lib/python3.12/site-packages"
   (site / "mcp-2.3.0.dist-info").mkdir(parents=True)
   (site / "claude_agent_sdk-0.2.177.dist-info").mkdir()
   assert up.site_packages(str(tmp_path / "v")) == str(site)
   assert up.installed(str(tmp_path / "v")) == ["claude_agent_sdk==0.2.177", "mcp==2.3.0"]


def test_canary_needs_positive_refusals():
   from agent_gateway import canary
   probe = ("1) curl example\n000\n2) curl loopback\ncurl: (7) Failed to connect\n"
            "3. creds\nhead: Permission denied\n4) touch\ntouch: Read-only file system\n")
   for n, (name, words) in zip((2, 3, 4), canary.REFUSED.items()):
      assert any(w in canary.section(probe, n) for w in words), name
   assert canary.section("2) skipped, not run\n", 2) == " skipped, not run\n"
   assert not any(w in canary.section("2) skipped\n", 2)
                  for w in canary.REFUSED["no loopback"])


def test_every_run_carries_the_whole_floor():
   from agent_gateway.runner import floor_settings
   s = json.loads(floor_settings("/var/lib/dawn-agent", "/var/lib/dawn-agent/work",
                                 "/var/lib/dawn-agent/work/aaaa0001", ["api.anthropic.com"]))
   fs = s["sandbox"]["filesystem"]
   assert "/var/lib/dawn-agent/.claude/.credentials.json" in fs["denyRead"]
   assert "/var/lib/dawn-agent/work" in fs["denyRead"]
   assert fs["allowRead"] == ["/var/lib/dawn-agent/work/aaaa0001"]
   assert s["disableAllHooks"] and not s["enableAllProjectMcpServers"]
   assert s["sandbox"]["allowUnsandboxedCommands"] is False
   assert "Read(//var/lib/dawn-agent/.claude/.credentials.json)" in s["permissions"]["deny"]
   assert "Read(//home/**)" in s["permissions"]["deny"]
   assert "~" not in json.dumps(s)


def test_canary_credentials_probe_counts_bytes():
   from agent_gateway.canary import refused
   assert refused("no credentials", " wc -c\n0\n[exit=0]\n")            # /dev/null over it
   assert refused("no credentials", "bash: permission denied\n")
   assert not refused("no credentials", " wc -c\n412\n[exit=0]\n")     # the real file
   assert not refused("no credentials", " not run\n")
   assert not refused("no loopback", " curl: (52) empty reply\n")


def test_canary_sections_survive_bare_number_lines():
   from agent_gateway.canary import refused, section
   probe = ("1) curl\n000\n2) curl loopback\ncurl: (7) Failed to connect\n"
            "3) wc -c < ~/.claude/.credentials.json\n0\n[exit=0]\n"
            "4) touch\ntouch: Read-only file system\n"
            "5) Read\nOther tasks' files are not readable.\n"
            "6) Write\nEdits are allowed only inside the task worktree.\n")
   assert refused("no credentials", section(probe, 3))
   assert refused("no writes outside the clone", section(probe, 4))
   assert refused("no reading other tasks", section(probe, 5))
   assert refused("no edits outside the clone", section(probe, 6))
   fenced = "3) wc -c\n```\n0\n```\n4) touch\n"
   assert refused("no credentials", section(fenced, 3))


def test_floor_hides_transcripts_and_policy_decides_code_graph_reads():
   from agent_gateway import policy
   floor = json.load(open(os.path.join(os.path.dirname(policy.__file__), "floor.json")))
   assert "~/.claude/projects" in floor["sandbox"]["filesystem"]["denyRead"]
   assert policy.decide("mcp__cbm__search_graph", {}, "/w/a", []).allow
   assert not policy.decide("mcp__cbm__delete_project", {}, "/w/a", []).allow


def test_repo_claude_md_comes_from_the_mirror(tmp_path):
   src = tmp_path / "src"
   src.mkdir()
   git("init", "-q", "-b", "main", cwd=src)
   (src / "CLAUDE.md").write_text("Run make test.\n")
   git("add", "CLAUDE.md", cwd=src)
   git("-c", "user.email=a@b", "-c", "user.name=a", "commit", "-qm", "init", cwd=src)
   repos = Repos({"r": {"url": str(src), "base": "main"}}, str(tmp_path / "m"), "/w", [],
                 "/h", "/g")
   asyncio.run(repos.refresh("r"))
   assert asyncio.run(repos.claude_md("r", "main")) == "Run make test.\n"


def test_agent_runs_on_the_floor_alone(store):
   """No settings from the repo or the agent's home: the repo's rules come in
   through the instructions instead."""
   r = runner(store)
   t = add_task(store)
   opts = asyncio.run(r.options(t))
   assert opts.setting_sources == []
   assert "Commits need the developer's OK." in opts.system_prompt["append"]
   assert json.loads(opts.settings)["disableAllHooks"] is True
