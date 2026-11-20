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
# GitHub reads: naming a pull request, its state, its threads, a check's log,
# and GitHub's refusals in DAWN's words.  GitHub is faked at github._request.

import io
import json
import time
import urllib.error

import pytest

from agent_gateway import github, store as st
from agent_gateway.config import Config
from agent_gateway.github import GitError

REPOS = {"dawn": {"url": "https://github.com/o/dawn.git", "base": "main"}}
HEAD = "ed4167b01d63" + "0" * 28


def http_error(code, headers=None, body=b"{}"):
   return urllib.error.HTTPError("https://api.github.com/x", code, "x", headers or {},
                                 io.BytesIO(body))


class GitHub:
   """github._request as GitHub answers: GraphQL by the query's first field, REST
   by URL.  Records every call."""

   def __init__(self, status=None, threads=None, rest=None):
      self.status, self.threads, self.rest, self.calls = status, threads or [], rest or {}, []

   def __call__(self, url, method="GET", headers=None, data=None, follow=True, limit=None,
                deadline=None, read=None):
      self.calls.append({"url": url, "method": method, "headers": headers or {},
                         "data": json.loads(data) if data else None, "follow": follow})
      if url == github.GITHUB_GRAPHQL:
         body = self.calls[-1]["data"]
         if "reviewThreads(first: %d, after" % github.THREADS_PAGE in body["query"]:
            answer = self.threads.pop(0)
         else:
            answer = self.status
         if isinstance(answer, Exception):
            raise answer
         return 200, {}, json.dumps(answer).encode()
      answer = self.rest[url]
      if isinstance(answer, Exception):
         raise answer
      if read is not None:
         read.update(bytes=len(answer), complete=True)
      return 200, {}, answer


def pr(**over):
   """A pull request as STATUS_QUERY returns it: behind, one required check failing."""
   node = {
      "url": "https://github.com/o/dawn/pull/115", "title": "Fix\x00 it\x1b", "state": "OPEN",
      "isDraft": False, "merged": False, "mergeable": "UNKNOWN", "mergeStateStatus": "BEHIND",
      "reviewDecision": None, "baseRefName": "main", "headRefName": "agent/x",
      "headRefOid": HEAD, "author": {"login": "KerseyFabrications"},
      "reviewRequests": {"nodes": [{"requestedReviewer": {"__typename": "User",
                                                           "login": "Alice"}}]},
      "latestReviews": {"nodes": [{"author": {"login": "copilot-pull-request-reviewer"},
                                   "state": "COMMENTED", "submittedAt": "2026-11-16T10:00:00Z",
                                   "commit": {"oid": HEAD}}]},
      "reviewThreads": {"totalCount": 5, "nodes": [{"isResolved": False}] * 4 +
                        [{"isResolved": True}]},
      "commits": {"nodes": [{"commit": {"statusCheckRollup": {"state": "FAILURE", "contexts": {
         "totalCount": 4, "nodes": [
            {"__typename": "CheckRun", "name": "build", "status": "COMPLETED",
             "conclusion": "SUCCESS", "databaseId": 1, "isRequired": True,
             "detailsUrl": "https://github.com/o/dawn/actions/runs/1/job/1",
             "checkSuite": {"app": {"slug": "github-actions"}}},
            {"__typename": "CheckRun", "name": "lint", "status": "COMPLETED",
             "conclusion": "FAILURE", "databaseId": 2, "isRequired": False,
             "detailsUrl": "https://evil.example/x",
             "checkSuite": {"app": {"slug": "github-actions"}}},
            {"__typename": "CheckRun", "name": "format-check", "status": "COMPLETED",
             "conclusion": "FAILURE", "databaseId": 133129755659, "isRequired": True,
             "detailsUrl": "https://github.com/o/dawn/actions/runs/9/job/133129755659",
             "checkSuite": {"app": {"slug": "github-actions"}}},
            {"__typename": "StatusContext", "context": "ci/legacy", "state": "PENDING",
             "targetUrl": None, "isRequired": False}]}}}}]}}
   node.update(over)
   return {"data": {"repository": {"pullRequest": node}}}


def test_parse_pr_ref():
   two = {**REPOS, "other": {"url": "https://github.com/x/y", "base": "main"}}
   assert github.parse_pr_ref("https://github.com/o/r/pull/7", None, {}) == ("o", "r", 7)
   assert github.parse_pr_ref(" o/r.js#12 ", None, {}) == ("o", "r.js", 12)
   assert github.parse_pr_ref("115", None, REPOS) == ("o", "dawn", 115)
   assert github.parse_pr_ref("115", "other", two) == ("x", "y", 115)
   assert github.parse_pr_ref("3", "a/b", two) == ("a", "b", 3)
   assert github.parse_pr_ref(115, "dawn", two) == ("o", "dawn", 115)
   for ref, repo, repos in (("115", None, two), ("115", None, {}), ("0", "a/b", {}),
                            ("o/r#x", None, {}), ("../r#1", None, {}), ("o/r/pull/1", None, {}),
                            ("https://evil.com/o/r/pull/1", None, {}), ("1; drop", "a/b", {}),
                            ("https://github.com/o/r", None, {}), ("4", "a/b/pull/3", {}),
                            ("", None, REPOS), (None, None, REPOS)):
      with pytest.raises(GitError, match="name the repo: owner/repo#N"):
         github.parse_pr_ref(ref, repo, repos)
   assert github.github_url("https://github.com/o/r.git") == ("o", "r", None)
   assert github.github_url("https://github.com/o/r.github.io") == ("o", "r.github.io", None)
   assert github.configured_repo("O", "Dawn", REPOS) == "dawn"
   assert github.configured_repo("o", "else", REPOS) is None


def test_pr_status_maps_state_and_ranks_checks(monkeypatch):
   gh = GitHub(status=pr())
   monkeypatch.setattr(github, "_request", gh)
   res = github.pr_status("o", "dawn", 115, "tok")
   q = gh.calls[0]
   assert q["data"]["variables"] == {"owner": "o", "name": "dawn", "number": 115}
   assert q["headers"]["Authorization"] == "Bearer tok"
   p = res["pr"]
   assert p["behind"] is True and p["merge_state"] == "behind" and p["mergeable"] == "unknown"
   assert p["title"] == "Fix it" and p["author"] == "kerseyfabrications" and p["state"] == "open"
   names = [c["name"] for c in res["checks"]]
   assert names == ["format-check", "lint", "ci/legacy", "build"]  # failing required first
   fmt = res["checks"][0]
   assert fmt == {"name": "format-check", "required": True, "status": "completed",
                  "conclusion": "failure", "failed": True, "job_id": 133129755659,
                  "app": "github-actions",
                  "url": "https://github.com/o/dawn/actions/runs/9/job/133129755659"}
   assert res["checks"][1]["url"] is None  # not github.com: dropped
   assert res["checks"][2]["status"] == "pending" and res["checks"][2]["job_id"] is None
   assert res["checks_total"] == 4 and "checks_note" not in res
   assert res["copilot"] == "reviewed_head" and res["reviews"][0]["commit"] == HEAD
   assert res["requested"] == ["alice"]
   assert res["threads"] == {"unresolved": 4, "resolved": 1, "total": 5}


def test_pr_status_copilot_requested_and_checks_capped(monkeypatch):
   many = [{"__typename": "CheckRun", "name": f"c{i}", "status": "COMPLETED",
            "conclusion": "SUCCESS", "databaseId": i, "isRequired": False,
            "checkSuite": {"app": {"slug": "github-actions"}}} for i in range(80)]
   rollup = {"commits": {"nodes": [{"commit": {"statusCheckRollup": {
      "contexts": {"totalCount": 130, "nodes": many}}}}]},
      "reviewRequests": {"nodes": [{"requestedReviewer": {
         "__typename": "Bot", "login": "copilot-pull-request-reviewer"}}]}}
   monkeypatch.setattr(github, "_request", GitHub(status=pr(**rollup)))
   res = github.pr_status("o", "dawn", 115, "tok")
   assert len(res["checks"]) == github.CHECKS_CAP and res["checks_total"] == 130
   assert res["copilot"] == "requested"


def test_refused_rollup_degrades_with_a_note(monkeypatch):
   ans = pr(commits={"nodes": [{"commit": {"statusCheckRollup": None}}]})
   ans["errors"] = [{"type": "FORBIDDEN", "message": "Resource not accessible by personal "
                     "access token", "path": ["repository", "pullRequest", "commits", "nodes",
                                              0, "commit", "statusCheckRollup"]}]
   monkeypatch.setattr(github, "_request", GitHub(status=ans))
   res = github.pr_status("o", "dawn", 115, "tok")
   assert res["checks"] == [] and res["pr"]["behind"] is True
   assert res["checks_note"] == ("checks unreadable: the token lacks Actions: Read-only and "
                                 "Commit statuses: Read-only for o/dawn")


def test_unseen_repo_and_missing_pr(monkeypatch):
   gone = {"data": {"repository": None},
           "errors": [{"type": "NOT_FOUND", "path": ["repository"]}]}
   monkeypatch.setattr(github, "_request", GitHub(status=gone))
   with pytest.raises(GitError, match="the token can't see o/private: it isn't in the token's "
                      "Repository access, or there's no such repo"):
      github.pr_status("o", "private", 1, "tok")
   none = {"data": {"repository": {"pullRequest": None}},
           "errors": [{"type": "NOT_FOUND", "path": ["repository", "pullRequest"]}]}
   monkeypatch.setattr(github, "_request", GitHub(status=none))
   with pytest.raises(GitError, match=r"o/dawn has no pull request #999"):
      github.pr_status("o", "dawn", 999, "tok")
   limited = {"data": None, "errors": [{"type": "RATE_LIMITED", "message": "API rate limit"}]}
   monkeypatch.setattr(github, "_request", GitHub(status=limited))
   with pytest.raises(GitError, match="rate limit is reached; try again after"):
      github.pr_status("o", "dawn", 1, "tok")


def test_refusals_in_dawns_words(monkeypatch):
   where = "o/dawn"
   perm = http_error(403, {"X-Accepted-GitHub-Permissions": "actions=read"},
                     b'{"message":"Resource not accessible by personal access token"}')
   assert str(github.refusal(perm, where, "x")) == "the token lacks Actions: Read-only for o/dawn"
   alt = http_error(403, {"x-accepted-github-permissions": "issues=read; pull_requests=write"})
   assert str(github.refusal(alt, where, "x")) == ("the token lacks Issues: Read-only or Pull "
                                                   "requests: Read and write for o/dawn")
   bare = http_error(403)
   assert str(github.refusal(bare, where, "Pull requests: Read-only")) == \
      "the token lacks Pull requests: Read-only for o/dawn"
   reset = int(time.time()) + 600
   primary = http_error(403, {"X-RateLimit-Remaining": "0", "X-RateLimit-Reset": str(reset)})
   at = time.strftime("%H:%M", time.localtime(reset))
   assert str(github.refusal(primary, where, "x")) == \
      f"GitHub's rate limit is reached; try again after {at}"
   secondary = http_error(403, {}, b'{"message":"You have exceeded a secondary rate limit"}')
   assert "try again after" in str(github.refusal(secondary, where, "x"))
   assert "try again after" in str(github.refusal(http_error(429, {"Retry-After": "30"}),
                                                  where, "x"))
   assert "token was refused" in str(github.refusal(http_error(401), where, "x"))
   assert "can't see o/dawn" in str(github.refusal(http_error(404), where, "x"))
   assert str(github.refusal(http_error(404), where, "x", "no log")) == "no log"
   assert str(github.refusal(http_error(502), where, "x")) == "GitHub didn't answer"
   assert str(github.refusal(urllib.error.URLError("down"), where, "x")) == "GitHub didn't answer"


def test_graphql_http_refusal(monkeypatch):
   monkeypatch.setattr(github, "_request", GitHub(status=http_error(401)))
   with pytest.raises(GitError, match="token was refused"):
      github.pr_status("o", "dawn", 1, "tok")


def comment(cid, who="copilot-pull-request-reviewer", body="Use safe_strscpy here."):
   return {"databaseId": cid, "author": {"login": who, "__typename": "Bot"},
           "authorAssociation": "NONE", "body": body, "createdAt": "2026-11-16T10:00:00Z",
           "url": f"https://github.com/o/dawn/pull/115#discussion_r{cid}"}


def thread(tid, first, n=1, resolved=False, outdated=False):
   cs = [comment(first + i) for i in range(n)]
   return {"id": f"PRRT_{tid}", "isResolved": resolved, "isOutdated": outdated,
           "path": "src/x.c", "line": None if outdated else 42, "originalLine": 40,
           "head": {"totalCount": n, "nodes": cs[:1]}, "tail": {"nodes": cs[-5:]}}


def threads_page(nodes, more=False, reviews=(), comments=()):
   return {"data": {"repository": {"pullRequest": {
      "headRefOid": HEAD,
      "reviewThreads": {"pageInfo": {"hasNextPage": more, "endCursor": "c1" if more else None},
                        "nodes": nodes},
      "reviews": {"nodes": list(reviews)}, "comments": {"nodes": list(comments)}}}}}


def test_pr_threads_pages_filters_and_keys(monkeypatch):
   monkeypatch.setattr(github, "_token_logins", {"tok": "kerseyfabrications"})
   rev = [{"author": {"login": "alice"}, "state": "CHANGES_REQUESTED", "body": "Please split.",
           "submittedAt": "t", "url": "https://github.com/o/dawn/pull/115#r1"},
          {"author": {"login": "bob"}, "state": "APPROVED", "body": "", "submittedAt": "t"}]
   pages = [threads_page([thread(1, 100, n=9), thread(2, 200, resolved=True)], more=True,
                         reviews=rev, comments=[comment(5, "carol", "nice")]),
            threads_page([thread(3, 300, outdated=True)])]
   gh = GitHub(threads=list(pages))
   monkeypatch.setattr(github, "_request", gh)
   res = github.pr_threads("o", "dawn", 115, "tok")
   assert gh.calls[1]["data"]["variables"]["after"] == "c1"
   assert [t["comment_id"] for t in res["threads"]] == [100, 300]  # resolved left out
   assert [t["n"] for t in res["threads"]] == [1, 2]
   first = res["threads"][0]
   assert first["comments_total"] == 9 and len(first["comments"]) == 6  # first + last 5
   assert [c["comment_id"] for c in first["comments"]] == [100, 104, 105, 106, 107, 108]
   assert first["id"] == "PRRT_1" and first["line"] == 42 and first["comments"][0]["bot"]
   assert res["threads"][1]["outdated"] and res["threads"][1]["line"] == 40
   assert [r["author"] for r in res["reviews"]] == ["alice"]  # bodies only
   assert res["comments"][0]["author"] == "carol"
   assert res["as_login"] == "kerseyfabrications" and res["head_sha"] == HEAD
   assert res["total"] == 2 and res["truncated"] is False
   gh = GitHub(threads=[threads_page([thread(1, 100), thread(2, 200, resolved=True)])])
   monkeypatch.setattr(github, "_request", gh)
   assert len(github.pr_threads("o", "dawn", 115, "tok", everything=True)["threads"]) == 2


def test_pr_threads_say_which_copilot_resolved(monkeypatch):
   monkeypatch.setattr(github, "_token_logins", {"tok": "me"})
   by_copilot, by_person = thread(1, 100, resolved=True), thread(2, 200, resolved=True)
   by_copilot["resolvedBy"] = {"login": "Copilot"}
   by_person["resolvedBy"] = {"login": "KerseyFabrications"}
   gh = GitHub(threads=[threads_page([by_copilot, by_person, thread(3, 300)])])
   monkeypatch.setattr(github, "_request", gh)
   res = github.pr_threads("o", "dawn", 115, "tok", everything=True)
   assert "resolvedBy { login }" in gh.calls[0]["data"]["query"]
   # Open first, then Copilot's, then a person's.
   assert [(t["comment_id"], t["resolved_by_copilot"]) for t in res["threads"]] == [
      (300, False), (100, True), (200, False)]


def test_pr_threads_put_replyable_threads_before_answered_ones(monkeypatch):
   # With everything, a long PR's answered threads must not use up the cap
   # ahead of a new round's thread a reply needs.
   monkeypatch.setattr(github, "_token_logins", {"tok": "me"})
   old = [thread(i, 1000 * i, resolved=True) for i in range(1, 50)]
   for i, t in enumerate(old):
      t["head"]["nodes"][0]["body"] = "x" * 5000
      t["resolvedBy"] = {"login": "Copilot" if i % 2 else "KerseyFabrications"}
   gh = GitHub(threads=[threads_page(old + [thread(99, 99000)])])
   monkeypatch.setattr(github, "_request", gh)
   res = github.pr_threads("o", "dawn", 115, "tok", everything=True)
   assert res["truncated"] and res["threads"][0]["comment_id"] == 99000


def test_pr_threads_caps(monkeypatch):
   monkeypatch.setattr(github, "_token_logins", {"tok": "me"})
   long = [thread(i, 1000 * i) for i in range(1, 50)]
   for t in long:
      t["head"]["nodes"][0]["body"] = "x" * 5000 + "\x00"
   gh = GitHub(threads=[threads_page(long, more=True)] * 3)
   monkeypatch.setattr(github, "_request", gh)
   res = github.pr_threads("o", "dawn", 115, "tok")
   assert len(gh.calls) == github.THREADS_PAGES
   assert res["truncated"] and len(json.dumps(res).encode()) <= github.THREADS_REPLY_CAP
   assert all(len(c["body"]) == github.BODY_CAP for t in res["threads"] for c in t["comments"])


LOG = "\n".join(
   ["2026-11-16T10:00:00.1234567Z ##[group]Run make"] +
   [f"2026-11-16T10:00:01.0000000Z step {i}" for i in range(400)] +
   ["2026-11-16T10:00:02.0000000Z \x1b[31msrc/x.c:4: error: bad indent\x1b[0m"] +
   [f"2026-11-16T10:00:03.0000000Z after {i}" for i in range(300)] +
   ["2026-11-16T10:00:04.0000000Z ##[error]Process completed with exit code 1."]).encode()


def test_check_log_follows_the_redirect_without_the_token(monkeypatch):
   signed = "https://results.blob.core.windows.net/log?sig=secret"
   api = f"{github.GITHUB_API}/repos/o/dawn/actions/jobs/133129755659/logs"
   gh = GitHub(status=pr(), rest={api: http_error(302, {"Location": signed}), signed: LOG})
   monkeypatch.setattr(github, "_request", gh)
   res = github.pr_check_log("o", "dawn", 115, "format-check", "tok")
   first, second = gh.calls[1], gh.calls[2]
   assert first["url"] == api and first["follow"] is False
   assert first["headers"]["Authorization"] == "Bearer tok"
   assert second["url"] == signed and "Authorization" not in second["headers"]
   ex = res["excerpt"]
   assert "src/x.c:4: error: bad indent" in ex and "\x1b" not in ex and "2026-11-16T" not in ex
   assert ex.endswith("##[error]Process completed with exit code 1.")
   assert "step 398" in ex and "step 10\n" not in ex  # context, not the whole log
   assert github.CUT in ex and res["excerpt_bytes"] <= github.EXCERPT_CAP
   assert res["log_bytes_read"] == len(LOG) and res["truncated"] is False
   assert res["conclusion"] == "failure"
   # By its job id, and as pr_status lists it, too.
   for check in ("133129755659", "format-check (job 133129755659)"):
      gh = GitHub(status=pr(), rest={api: http_error(302, {"Location": signed}), signed: LOG})
      monkeypatch.setattr(github, "_request", gh)
      assert github.pr_check_log("o", "dawn", 115, check, "tok")["name"] == "format-check"


def test_check_log_cap_and_refusals(monkeypatch):
   big = ("\n".join(f"error: line {i} " + "y" * 200 for i in range(2000))).encode()
   api = f"{github.GITHUB_API}/repos/o/dawn/actions/jobs/133129755659/logs"
   monkeypatch.setattr(github, "_request", GitHub(status=pr(), rest={api: big}))
   res = github.pr_check_log("o", "dawn", 115, "format-check", "tok")
   assert len(res["excerpt"].encode()) <= github.EXCERPT_CAP
   assert res["excerpt"].rstrip().endswith("line 1999 " + "y" * 200)
   monkeypatch.setattr(github, "_request", GitHub(status=pr()))
   with pytest.raises(GitError, match="has no check named 'nope'"):
      github.pr_check_log("o", "dawn", 115, "nope", "tok")
   with pytest.raises(GitError, match="ci/legacy isn't a GitHub Actions job"):
      github.pr_check_log("o", "dawn", 115, "ci/legacy", "tok")
   monkeypatch.setattr(github, "_request", GitHub(status=pr(), rest={api: http_error(404)}))
   with pytest.raises(GitError, match="no log for job 133129755659"):
      github.pr_check_log("o", "dawn", 115, "format-check", "tok")
   denied = http_error(403, {"X-Accepted-GitHub-Permissions": "actions=read"})
   monkeypatch.setattr(github, "_request", GitHub(status=pr(), rest={api: denied}))
   with pytest.raises(GitError, match="lacks Actions: Read-only for o/dawn"):
      github.pr_check_log("o", "dawn", 115, "format-check", "tok")


def call_tool(c, name, args):
   hdr = {"Accept": "application/json, text/event-stream", "Content-Type": "application/json",
          "Authorization": "Bearer " + "s" * 32}
   res = c.post("/mcp", json={"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": name, "arguments": args}},
                headers=hdr).json()["result"]
   return res, (None if res["isError"] else json.loads(res["content"][0]["text"]))


def test_tools_name_the_pull_request(monkeypatch):
   from starlette.testclient import TestClient
   from agent_gateway import server
   from test_gateway import runner as fake_runner
   s = st.Store(":memory:", check_same_thread=False)
   r = fake_runner(s, repos=REPOS, github_token="tok")
   s.add_task(id="t1", dawn_user_id=1, agent="claude", repo="dawn", base="main",
              branch="agent/t1", worktree="/w", task="do it", budget_usd=5.0,
              client_ref="job:42.1000")
   s.set_published("t1", json.dumps({"pr_url": "https://github.com/o/dawn/pull/115"}))
   gh = GitHub(status=pr())
   monkeypatch.setattr(github, "_request", gh)
   with TestClient(server.app(r, Config(token="s" * 32)), base_url="http://127.0.0.1:9760") as c:
      _, out = call_tool(c, "pr_status", {"dawn_user_id": 1, "client_ref": "job:42.1000"})
      assert gh.calls[0]["data"]["variables"]["number"] == 115
      assert out["configured_repo"] == "dawn" and out["job"] == "job:42.1000"
      _, out = call_tool(c, "pr_status", {"dawn_user_id": 1, "ref": "x/y#3"})
      assert out["configured_repo"] is None and out["job"] is None
      res, _ = call_tool(c, "pr_status", {"dawn_user_id": 2, "client_ref": "job:42.1000"})
      assert res["isError"] and "no such task" in str(res)  # another user's
      res, _ = call_tool(c, "pr_threads", {"dawn_user_id": 1, "ref": "../x#1"})
      assert res["isError"] and "name the repo: owner/repo#N" in str(res)
   r.cfg.github_token = ""
   with TestClient(server.app(r, Config(token="s" * 32)), base_url="http://127.0.0.1:9760") as c:
      res, _ = call_tool(c, "pr_check_log", {"dawn_user_id": 1, "ref": "1", "check": "x"})
      assert res["isError"] and "no GitHub token" in str(res)


def test_parse_pr_ref_takes_pasted_forms():
   repos = {"dawn": {"url": "https://github.com/o/dawn"}}
   for ref in ("https://github.com/o/dawn/pull/115/files", "https://github.com/o/dawn/pull/115/",
               "https://github.com/o/dawn/pull/115#discussion_r42",
               "https://github.com/o/dawn/pull/115?w=1", "#115", "o/dawn#115"):
      assert github.parse_pr_ref(ref, None, repos) == ("o", "dawn", 115), ref


def test_long_log_keeps_its_end(monkeypatch):
   """Past the keep cap, the log's end stays (where the failure is), and the
   error lines nearest it are the ones shown."""
   class Resp:
      status, headers = 200, {}

      def __init__(self, body):
         self.body = body

      def read(self, n):
         out, self.body = self.body[:n], self.body[n:]
         return out

      def __enter__(self):
         return self

      def __exit__(self, *a):
         return False
   early = "".join(f"-- Performing Test X{i} - Failed\n" for i in range(100))
   late = "".join(f"step {i}\n" for i in range(5000)) + "FAIL: test_oracle\nall done\n"
   body = (early + "x" * 4000 + "\n" + late).encode()
   monkeypatch.setattr(github, "LOG_READ_CAP", len(late) + 1000)
   monkeypatch.setattr(github, "LOG_CHUNK", 1024)
   monkeypatch.setattr(github.urllib.request, "urlopen", lambda req, timeout: Resp(body))
   read = {}
   _, _, kept = github._request("https://x/log", limit=github.LOG_READ_CAP,
                                deadline=github.time.monotonic() + 5, read=read)
   assert read == {"bytes": len(body), "complete": True} and len(kept) < len(body)
   assert kept.endswith(b"all done\n")
   ex = github.log_excerpt(kept, cut_start=True)
   assert "FAIL: test_oracle" in ex and "Performing Test" not in ex
   hits = github.log_excerpt(("".join(f"error: {i}\n" for i in range(100)) +
                              "".join(f"ok {i}\n" for i in range(300))).encode())
   assert "error: 99" in hits and "error: 0\n" not in hits  # the nearest the end


def test_excerpt_keeps_the_last_errors_when_they_overflow():
   """Error lines past their half of the budget: the ones nearest the end stay."""
   errs = "".join(f"error: e{i} " + "z" * 300 + "\n" for i in range(40))
   ex = github.log_excerpt((errs + "".join(f"ok {i}\n" for i in range(300))).encode())
   assert len(ex.encode()) <= github.EXCERPT_CAP
   assert "error: e39 " in ex and "error: e0 " not in ex


def test_pr_status_conflicting_is_behind(monkeypatch):
   gh = GitHub(status=pr(mergeable="CONFLICTING", mergeStateStatus="DIRTY"))
   monkeypatch.setattr(github, "_request", gh)
   p = github.pr_status("o", "dawn", 115, "tok")["pr"]
   assert p["behind"] is True and p["merge_state"] == "dirty" and p["mergeable"] == "conflicting"
