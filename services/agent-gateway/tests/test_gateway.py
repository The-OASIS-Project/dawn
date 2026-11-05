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
from agent_gateway.runner import NO_BACKGROUND_TOOLS, Runner


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


def test_latest_seq(store):
   assert store.latest_seq() == 0
   add_task(store)
   store.add_event("t1", "progress", text="a")
   assert store.latest_seq() == store.events_after(0)[-1]["seq"]


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
   def workdir(self, task_id):
      return f"/w/{task_id}"

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


async def settle(r):
   """Let a start's background clone (and what it schedules) finish."""
   while r.background or r.cloning:
      await asyncio.gather(*list(r.background), *list(r.cloning.values()),
                           return_exceptions=True)


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


def test_final_report_kept_whole(store):
   """The agent's final report (what it did, how it checked it) isn't cut at the
   progress-line cap."""
   r = runner(store)
   add_task(store)
   report = "did it. " * 600 + "Tests: 12 passed."

   class Result:
      is_error, result, subtype, total_cost_usd = False, report, "success", 0.5
   asyncio.run(r.finish("t1", Result()))
   assert store.task("t1")["summary"].endswith("Tests: 12 passed.")


def test_run_without_a_report_is_asked_once(store):
   """A run that ends with no report isn't done: the agent is asked once to
   finish and report; a second silent end fails the task."""
   r = runner(store)
   add_task(store)

   class Silent:
      is_error, result, subtype, total_cost_usd = False, "", "success", 0.1
   asyncio.run(r.finish("t1", Silent()))
   assert store.task("t1")["status"] != st.COMPLETED
   assert r.resumed and "without a report" in r.resumed[0][1]
   del r.running["t1"]                                    # that run ends silently too
   asyncio.run(r.finish("t1", Silent()))
   assert store.task("t1")["status"] == st.FAILED
   assert len(r.resumed) == 1


def test_report_ask_is_per_run(store):
   """Asked once, then resumed for another reason (an answer): a later silent
   end is asked again, not failed."""
   r = runner(store)
   add_task(store)

   class Silent:
      is_error, result, subtype, total_cost_usd = False, "", "success", 0.1
   asyncio.run(r.finish("t1", Silent()))                  # asked
   del r.running["t1"]
   store.add_note("t1", "the operator's answer")          # its run asked a question...
   store.update_task("t1", status=st.INPUT_REQUIRED)
   asyncio.run(r.deliver("t1"))                           # ...and was resumed with the answer
   del r.running["t1"]
   asyncio.run(r.finish("t1", Silent()))
   assert store.task("t1")["status"] != st.FAILED
   assert len(r.resumed) == 3 and "without a report" in r.resumed[2][1]


def test_background_work_is_off():
   """A run is one turn: nothing may be left running after it."""
   with open(os.path.join(os.path.dirname(__file__), "..", "claude-agent-exec")) as f:
      assert "CLAUDE_CODE_DISABLE_BACKGROUND_TASKS=1" in f.read()
   assert "Monitor" in NO_BACKGROUND_TOOLS


def test_claude_codes_own_cache_stays_out_of_the_tool_cache():
   """XDG_CACHE_HOME would move Claude Code's own cache (written outside the
   sandbox) into a folder a task can write."""
   with open(os.path.join(os.path.dirname(__file__), "..", "claude-agent-exec")) as f:
      assert "XDG_CACHE_HOME" not in "".join(l for l in f if not l.lstrip().startswith("#"))


def test_build_and_test_commands_are_recorded():
   """A shell command that builds or runs tests counts as a check; its outcome
   is known only when the line's exit status is that command's own."""
   from agent_gateway.runner import check_command
   own = lambda c, **kw: check_command("Bash", {"command": c, **kw})
   assert own("make -C build-debug -j8 tests-ci") == ("make -C build-debug -j8 tests-ci", True)
   assert own("cd build && ctest -L ci")[1] is True
   assert own("cmake --preset debug")[1] is True
   assert own("python3 -m pytest -q tests")[1] is True
   assert own("npm run test")[1] is True
   assert own("CCACHE_DISABLE=1 make -j8")[1] is True
   assert own("./build-debug/tests/test_contacts_db")[1] is True
   assert own("tests/test_pentest.sh --section x")[1] is True
   # Piped, or-ed, followed by something, or in the background: no outcome.
   assert own("make -j8 2>&1 | tail -40")[1] is False
   assert own("make -j8 2>&1")[1] is True
   assert own("cmake --build b >log 2>&1")[1] is True
   assert own("make || true")[1] is False
   assert own("false || make")[1] is False
   assert own("make; echo done")[1] is False
   assert own("ctest -L ci", run_in_background=True)[1] is False
   # Not a build or test, quoted text included; a newline separates commands.
   assert own('git commit -qm "Fix calc (make test passes)"') is None
   assert own("git commit -m 'fix; make test green'") is None
   assert own("cd build\nmake")[1] is True
   assert own("grep -rn make src/") is None
   assert own("git log --oneline") is None
   assert own("cat Makefile") is None
   assert check_command("Read", {"command": "make"}) is None


def test_completed_lists_the_checks_it_saw(store):
   """The completed event carries the build/test commands the gateway saw and
   whether each succeeded: evidence that isn't the agent's own words."""
   r = runner(store)
   add_task(store)
   store.add_event("t1", "check", command="make tests-ci", ok=False)  # fixed below
   store.add_event("t1", "check", command="ctest -L ci | tail", ok=None)
   store.add_event("t1", "check", command="ctest -L ci", ok=False)
   store.add_event("t1", "check", command="make tests-ci", ok=True)

   class Result:
      is_error, result, subtype, total_cost_usd = False, "done", "success", 0.5
   asyncio.run(r.finish("t1", Result()))
   done = [e for e in store.events_after(0) if e["kind"] == "completed"][-1]
   # Each command's last run, in the order of those last runs.
   assert done["data"]["checks"] == [{"command": "ctest -L ci | tail", "ok": None},
                                     {"command": "ctest -L ci", "ok": False},
                                     {"command": "make tests-ci", "ok": True}]


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

   async def go():
      a = await r.start(1, "r", "fix it", None, "ref-1")
      b = await r.start(1, "r", "fix it", None, "ref-1")
      c = await r.start(2, "r", "fix it", None, "ref-1")  # another user
      await settle(r)
      return a, b, c
   a, b, c = asyncio.run(go())
   assert a["id"] == b["id"] and c["id"] != a["id"]
   assert [t for t, _ in r.resumed] == [a["id"], c["id"]]
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


class SlowRepos(FakeRepos):
   async def prepare(self, *a):
      await asyncio.sleep(0.05)
      return "/w/x"


def test_start_returns_before_the_clone(store):
   """The caller hears at once; the agent starts once the clone is made."""
   r = runner(store)
   r.repos = SlowRepos()

   async def go():
      t = await r.start(1, "r", "fix", None, "ref")
      assert t["status"] == st.WORKING and t["branch"].startswith("agent/")
      assert r.resumed == [] and r.starting == 1     # still cloning, holding its slot
      again = await r.start(1, "r", "fix", None, "ref")
      await settle(r)
      return t, again
   t, again = asyncio.run(go())
   assert again["id"] == t["id"] and [x for x, _ in r.resumed] == [t["id"]]
   assert r.starting == 0
   kinds = [e["kind"] for e in store.events_after(0)]
   assert kinds.count("started") == 1


def test_cancel_during_the_clone_never_launches(store):
   """A task cancelled while its clone is made is never launched, and a retried
   start returns it as cancelled."""
   r = runner(store)
   r.repos = SlowRepos()

   async def go():
      t = await r.start(1, "r", "fix", None, "job:7")
      stopped = await r.cancel_ref(1, "job:7")
      assert r.starting == 0 and not r.cloning       # the clone is stopped: its slot is free
      await settle(r)
      return t, stopped
   t, stopped = asyncio.run(go())
   assert stopped["status"] == st.CANCELLED and r.resumed == [] and r.starting == 0
   again = asyncio.run(r.start(1, "r", "fix", None, "job:7"))
   assert again["id"] == t["id"] and again["status"] == st.CANCELLED
   with pytest.raises(ValueError):
      asyncio.run(r.cancel_ref(1, "job:8"))


def test_cancelled_start_never_reaches_the_agent(store, monkeypatch):
   """The real launch path: a cancel just after launch stops the run before it
   opens an agent session."""
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

   class R(Runner):
      async def options(self, task):
         await asyncio.sleep(0.05)

   r = R(Config(token="x" * 32, repos={"r": {"url": "u", "base": "main"}}), store, FakeRepos())

   async def go():
      t = await r.start(1, "r", "fix", None, "job:8")
      for _ in range(50):                            # launched, agent not yet reached
         if t["id"] in r.running:
            break
         await asyncio.sleep(0.001)
      stopped = await r.cancel(t["id"], 1)
      await settle(r)
      return stopped
   stopped = asyncio.run(go())
   assert stopped["status"] == st.CANCELLED and not opened and not r.running


def test_failed_clone_fails_the_task(store):
   class Broken(FakeRepos):
      async def prepare(self, *a):
         await asyncio.sleep(0.02)
         raise ValueError("clone failed")
   r = runner(store)
   r.repos = Broken()

   async def go():
      t = await r.start(1, "r", "fix", None, "job:7")
      await settle(r)
      return t
   t = asyncio.run(go())
   done = store.task(t["id"])
   assert done["status"] == st.FAILED and "could not prepare its clone" in done["error"]
   assert "clone failed" not in done["error"]                # git's words stay in the log
   assert r.resumed == [] and r.starting == 0


def test_cancelled_start_wakes_a_blocked_task(store):
   """A start that runs nothing frees its slot: a task blocked behind it resumes."""
   r = FakeRunner(Config(token="x" * 32, repos={"r": {"url": "u", "base": "main"}}), store,
                  SlowRepos())
   add_task(store, "b1")
   store.update_task("b1", status=st.INPUT_REQUIRED)
   rid = store.add_request("b1", "question", {"value": "q"})

   async def go():
      await r.start(1, "r", "fix", None, "job:7")
      await r.reply("b1", 1, rid, "answer", "yes")  # the start holds the only slot
      assert store.task("b1")["status"] == st.INPUT_REQUIRED
      await r.cancel_ref(1, "job:7")
      await settle(r)
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
   # A rename listed by changes: its diff, given both names, is the rename, not a new file.
   git("mv", "f", "f2", cwd=wt)
   git("commit", "-qm", "rename f", cwd=wt)
   asyncio.run(repos.collect("r", "aaaa0001", "agent/aaaa0001-x", "main"))
   ch = asyncio.run(repos.changes("r", "aaaa0001", "main"))
   assert "f -> f2" in [f["path"] for f in ch["files"]]
   moved = asyncio.run(repos.file("r", "aaaa0001", "main", "f2", "diff", "f"))["text"]
   assert "rename from f" in moved and "new file" not in moved
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
      res = c.post("/mcp", json={"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
         "name": "task_list", "arguments": {"dawn_user_id": 1, "client_ref": "job:9"}}},
         headers=auth).json()["result"]
      assert not res["isError"] and json.loads(res["content"][0]["text"])["task"] is None
      res = c.post("/mcp", json={"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {
         "name": "task_cancel", "arguments": {"dawn_user_id": 1}}}, headers=auth).json()["result"]
      assert res["isError"] and "name the task" in str(res)
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
                                 "/var/lib/dawn-agent/work/aaaa0001", ["api.anthropic.com"], "dawn"))
   fs = s["sandbox"]["filesystem"]
   # Writable beyond the clone: /tmp and that repo's tool cache; other repos'
   # caches hidden, its own readable.
   assert fs["allowWrite"] == ["/tmp", "/var/lib/dawn-agent/.cache/repo/dawn"]
   assert "/var/lib/dawn-agent/.cache/repo" in fs["denyRead"]
   assert "/var/lib/dawn-agent/.cache/repo/dawn" in fs["allowRead"]
   assert "Read(//var/lib/dawn-agent/.cache/repo/**)" in s["permissions"]["deny"]
   odd = json.loads(floor_settings("/var/lib/dawn-agent", "/w", "/w/a", [], ".."))
   assert odd["sandbox"]["filesystem"]["allowWrite"] == ["/tmp"]
   assert odd["sandbox"]["filesystem"]["allowRead"] == ["/w/a"]
   assert "/var/lib/dawn-agent/.claude/.credentials.json" in fs["denyRead"]
   assert "/var/lib/dawn-agent/work" in fs["denyRead"]
   assert fs["allowRead"] == ["/var/lib/dawn-agent/work/aaaa0001",
                             "/var/lib/dawn-agent/.cache/repo/dawn"]
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
