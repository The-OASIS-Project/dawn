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
# GitHub for the gateway: every call it makes to GitHub goes through
# _request().  Publishing opens and adopts pull requests; the reads hand DAWN
# a pull request's state, its review threads and a failing check's log.
# Anyone who can comment on a pull request writes some of that text, so each
# string is cut to its cap, control characters are dropped and only github.com
# links are kept; nothing in it is interpreted.

import calendar
import collections
import difflib
import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request

GITHUB_API = "https://api.github.com"
GITHUB_GRAPHQL = f"{GITHUB_API}/graphql"
GITHUB_WEB = "https://github.com/"
GITHUB_API_VERSION = "2022-11-28"
GITHUB_TIMEOUT_S = 30

# What a read hands DAWN: each string at most this long.
BODY_CAP = 2000
TITLE_CAP = 300
LOGIN_CAP = 40
PATH_CAP = 300
URL_CAP = 200
NAME_CAP = 100            # a check's name
ID_CAP = 100              # a GraphQL node id
CHECKS_CAP = 50           # checks in pr_status (failing required first)
CONTEXTS_ASKED = 100      # checks asked of GitHub; checks_total counts them all
REQUESTS_ASKED = 20       # requested reviewers
REVIEWS_ASKED = 30
THREAD_COUNT_ASKED = 100  # threads pr_status counts
THREADS_PAGE = 50
THREADS_PAGES = 3
THREAD_TAIL = 5           # a thread's comments: its first and these last ones
CONVERSATION_ASKED = 10   # the pull request's own comments, newest
THREADS_REPLY_CAP = 64 * 1024

LOG_READ_CAP = 16 * 1024 * 1024  # of a job's log kept: its end (where the failure is)
LOG_STREAM_CAP = 256 * 1024 * 1024  # read at most, the rest dropped as it streams
LOG_READ_S = 30
LOG_CHUNK = 64 * 1024
LOG_TAIL_LINES = 200
LOG_CONTEXT = 2                  # lines around an error line
LOG_HITS_CAP = 40
EXCERPT_CAP = 12 * 1024
CUT = "[...]"

# What a revise hands the agent (pr_feedback).
FEEDBACK_THREADS_CAP = 30
FEEDBACK_CHECKS_CAP = 3  # failing required checks whose log excerpt is read
FEEDBACK_CAP = 48 * 1024

# A thread saying again what an answered one from an earlier review, same file, said.
REPEAT_SIMILARITY = 0.85
REPEAT_COMPARED = 400  # characters of each (normalized) first comment compared
REPEAT_LINES = 20  # a repeat sits within this many lines of the one it repeats

LIST_SEP = 2              # ", " between two items of a JSON list
REPLIES_CAP = 20          # replies one pr_reply posts
REPLY_BODY_LIMIT = 65536  # GitHub's own limit on a comment's text
ERROR_SHOWN = 200         # GitHub's own words in a refusal
COMMITS_ASKED = 100       # commits read to tell what came after Copilot's last review

RATE_WAIT_S = 60  # a secondary limit that doesn't say for how long
HTTP_REDIRECTS = (301, 302, 303, 307, 308)
COPILOT = "copilot-pull-request-reviewer"
COPILOT_RESOLVER = "copilot"  # the login a thread Copilot resolved names (GraphQL resolvedBy)
COPILOT_REVIEWER = COPILOT + "[bot]"  # how a REST review request names it
WRITE_NEED = "Pull requests: Read and write"
FAILED = ("failure", "error", "timed_out", "cancelled", "action_required", "startup_failure")

TOKEN_REFUSED = ("the gateway's GitHub token was refused (expired or revoked); see the README "
                 "to replace it")
NO_ANSWER = "GitHub didn't answer"
NAME_THE_REPO = "name the repo: owner/repo#N"

# Fine-grained token permissions as GitHub's settings page names them.
PERMISSIONS = {"actions": "Actions", "checks": "Checks", "contents": "Contents",
               "issues": "Issues", "metadata": "Metadata", "pull_requests": "Pull requests",
               "statuses": "Commit statuses"}
LEVELS = {"read": "Read-only", "write": "Read and write"}
CHECKS_NEED = "Actions: Read-only and Commit statuses: Read-only"
PR_NEED = "Pull requests: Read-only"


class GitError(Exception):
   pass


_SEG = r"[A-Za-z0-9._-]+"
_GITHUB_URL = re.compile(rf"https://github\.com/({_SEG})/({_SEG}?)(?:\.git)?(?:/pull/(\d+))?")


def github_url(url: str) -> tuple[str, str, int | None] | None:
   """(owner, repo, pull request number or None) of a github.com repo or pull
   request address, or None."""
   m = _GITHUB_URL.fullmatch(str(url or ""))
   if not m or {m.group(1), m.group(2)} & {".", ".."}:
      return None
   return m.group(1), m.group(2), int(m.group(3)) if m.group(3) else None


def parse_pr_ref(ref, repo: str | None, repos: dict) -> tuple[str, str, int]:
   """(owner, repo, number) of a pull request named as a github.com address,
   owner/repo#N, or N with `repo` (a configured repo's name, or owner/repo) or
   with exactly one repo configured."""
   ref = str(ref if ref is not None else "").strip()
   # As pasted: a pull request page's own paths and anchors ("/files",
   # "#discussion_r…", a trailing slash), and "#115".
   ref = re.sub(r"(/pull/\d+)(?:/[^?#]*)?(?:[?#].*)?$", r"\1", ref)
   if re.fullmatch(r"#\d+", ref):
      ref = ref[1:]
   found = github_url(ref)
   if found and found[2]:
      return found
   m = re.fullmatch(rf"({_SEG})/({_SEG})#(\d+)", ref)
   if m:
      found = github_url(f"{GITHUB_WEB}{m.group(1)}/{m.group(2)}")
      if found and int(m.group(3)) > 0:
         return found[0], found[1], int(m.group(3))
   elif re.fullmatch(r"\d+", ref) and int(ref) > 0:
      where = None
      if repo:
         where = repos[repo]["url"] if repo in repos else f"{GITHUB_WEB}{repo}"
      elif len(repos) == 1:
         where = next(iter(repos.values()))["url"]
      found = github_url(where) if where else None
      if found and found[2] is None:
         return found[0], found[1], int(ref)
   raise GitError(NAME_THE_REPO)


def configured_repo(owner: str, repo: str, repos: dict) -> str | None:
   """The configured repo at github.com/owner/repo, by its name, or None."""
   for name, r in sorted(repos.items()):
      found = github_url(r.get("url"))
      if found and (found[0].lower(), found[1].lower()) == (owner.lower(), repo.lower()):
         return name
   return None


# --- HTTP ---------------------------------------------------------------------


class _NoRedirect(urllib.request.HTTPRedirectHandler):
   def redirect_request(self, req, fp, code, msg, headers, newurl):
      return None  # the 3xx comes back as an HTTPError


_no_redirect = urllib.request.build_opener(_NoRedirect)


def _request(url: str, method: str = "GET", headers: dict | None = None,
             data: bytes | None = None, follow: bool = True, limit: int | None = None,
             deadline: float | None = None, read: dict | None = None) -> tuple[int, dict, bytes]:
   """All of the gateway's GitHub HTTP: (status, headers, body).  An error
   status raises urllib's HTTPError (with `follow` false, a redirect too).
   With `limit`, the body's last `limit` bytes, streamed until `deadline`
   (monotonic) or LOG_STREAM_CAP; `read` then gets {"bytes", "complete"}."""
   req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
   opener = urllib.request.urlopen if follow else _no_redirect.open
   with opener(req, timeout=GITHUB_TIMEOUT_S) as resp:
      if limit is None:
         return resp.status, resp.headers, resp.read()
      chunks, kept, got, complete = collections.deque(), 0, 0, False
      while got < LOG_STREAM_CAP and time.monotonic() < deadline:
         chunk = resp.read(LOG_CHUNK)
         if not chunk:
            complete = True
            break
         chunks.append(chunk)
         kept += len(chunk)
         got += len(chunk)
         while kept - len(chunks[0]) >= limit:  # only the end is kept
            kept -= len(chunks.popleft())
      if read is not None:
         read.update(bytes=got, complete=complete)
      return resp.status, resp.headers, b"".join(chunks)


def github_headers(token: str) -> dict:
   return {"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
           "X-GitHub-Api-Version": GITHUB_API_VERSION}


def open_pull_request(owner, repo, token, head, base, title, body) -> str:
   _, _, raw = _request(
      f"{GITHUB_API}/repos/{owner}/{repo}/pulls", method="POST",
      data=json.dumps({"title": title, "head": head, "base": base, "body": body}).encode(),
      headers=github_headers(token))
   return json.loads(raw)["html_url"]


def github_get(path: str, token: str) -> dict | list:
   """GET `path` from GitHub's API, parsed from JSON."""
   return json.loads(_request(f"{GITHUB_API}{path}", headers=github_headers(token))[2])


def graphql(query: str, variables: dict, token: str) -> tuple[dict, list]:
   """A GraphQL query's (data, errors); a part GitHub refused is null in data,
   with its error naming the part's path."""
   _, _, raw = _request(GITHUB_GRAPHQL, method="POST",
                        data=json.dumps({"query": query, "variables": variables}).encode(),
                        headers={**github_headers(token), "Content-Type": "application/json"})
   ans = json.loads(raw)
   return ans.get("data") or {}, ans.get("errors") or []


def error_text(e: urllib.error.HTTPError) -> str:
   """GitHub's error answer, lowercased; "" when it can't be read."""
   try:
      return e.read().decode(errors="replace").lower()
   except Exception:  # a cut-off body raises IncompleteRead, not an OSError
      return ""


_token_logins: dict[str, str] = {}  # token -> its user's login, for the life of the process


def token_login(token: str) -> str:
   """The token's user's login, lowercased; asked of GitHub once per process."""
   if token not in _token_logins:
      login = github_get("/user", token).get("login")
      if not login:
         raise GitError("GitHub didn't say whose the token is")
      _token_logins[token] = login.lower()  # logins ignore case
   return _token_logins[token]


def adopt_pull_request(owner, repo, token, branch, base, earlier_pr) -> str:
   """The open pull request GitHub says the branch already has, when this token's
   user opened it; anyone else's is refused by name, never taken over."""
   q = urllib.parse.urlencode({"head": f"{owner}:{branch}", "base": base, "state": "open"})
   try:
      prs = github_get(f"/repos/{owner}/{repo}/pulls?{q}", token)
   except (OSError, ValueError):
      if earlier_pr:
         return earlier_pr
      raise
   if not prs:
      if earlier_pr:
         return earlier_pr
      raise GitError("GitHub says a pull request for the branch already exists, but none "
                     "is open for it")
   pr = prs[0]
   url = pr.get("html_url")
   if not url:
      raise GitError("GitHub listed the branch's pull request without its address")
   try:
      mine = str((pr.get("user") or {}).get("login") or "").lower() == token_login(token)
   except (OSError, ValueError):
      if earlier_pr:  # who the token is couldn't be read: the PR this task knew
         return earlier_pr
      raise
   if not mine:
      raise GitError(f"the branch already has a pull request opened by someone else: {url}")
   return url


def read_pull_request(pr_url: str, token: str) -> dict | None:
   """The pull request at `pr_url` (https://github.com/o/r/pull/N) as GitHub's
   API gives it, or None when it can't be read."""
   found = github_url(pr_url)
   if not found or not found[2]:
      return None
   try:
      pr = github_get(f"/repos/{found[0]}/{found[1]}/pulls/{found[2]}", token)
   except (OSError, ValueError):
      return None
   return pr if isinstance(pr, dict) else None


def state_of(pr: dict | None) -> str | None:
   """A pull request read by read_pull_request: "open", "merged" or "closed", or
   None when it couldn't be read."""
   if pr is None:
      return None
   return "merged" if pr.get("merged_at") else pr.get("state")


def pr_state(pr_url: str, token: str) -> str | None:
   """The pull request at `pr_url`: "open", "merged" or "closed", or None when it
   can't be read."""
   return state_of(read_pull_request(pr_url, token))


def update_pull_request(pr_url: str, token: str, title: str | None, body: str | None) -> None:
   """Replace the title and/or description of the pull request at `pr_url`."""
   found = github_url(pr_url)
   if not found or not found[2]:
      raise GitError(f"not a pull request's address: {pr_url}")
   fields = {k: v for k, v in (("title", title), ("body", body)) if v is not None}
   _request(f"{GITHUB_API}/repos/{found[0]}/{found[1]}/pulls/{found[2]}", method="PATCH",
            data=json.dumps(fields).encode(), headers=github_headers(token))


# --- refusals -------------------------------------------------------------------


def _at(when: float) -> str:
   return (f"GitHub's rate limit is reached; try again after "
           f"{time.strftime('%H:%M', time.localtime(when))}")


def permission_error(accepted: str | None, where: str, need: str) -> str:
   """The permission a refused call needed, as GitHub's settings page names it:
   from X-Accepted-GitHub-Permissions ("a=read,b=read; c=read": a and b, or c),
   else `need`."""
   sets = []
   for alt in (accepted or "").split(";"):
      parts = []
      for p in alt.split(","):
         key, _, level = p.strip().partition("=")
         if key in PERMISSIONS:
            parts.append(f"{PERMISSIONS[key]}: {LEVELS.get(level, 'Read-only')}")
      if parts:
         sets.append(" and ".join(parts))
   return f"the token lacks {' or '.join(sets) or need} for {where}"


def refusal(e: Exception, where: str, need: str, missing: str | None = None) -> GitError:
   """A failed GitHub call in DAWN's words.  `need`: the permission the call takes;
   `missing`: what a 404 means (default: the repo can't be seen)."""
   if not isinstance(e, urllib.error.HTTPError):
      return GitError(NO_ANSWER)  # not reached, timed out, or not JSON
   hdr = {k.lower(): v for k, v in (e.headers or {}).items()}
   text = error_text(e)
   if e.code == 401:
      return GitError(TOKEN_REFUSED)
   if e.code in (403, 429):
      retry, reset = hdr.get("retry-after", ""), hdr.get("x-ratelimit-reset", "")
      if retry.isdigit():
         return GitError(_at(time.time() + int(retry)))
      if hdr.get("x-ratelimit-remaining") == "0" and reset.isdigit():
         return GitError(_at(int(reset)))
      if e.code == 429 or "rate limit" in text:
         return GitError(_at(time.time() + RATE_WAIT_S))
      return GitError(permission_error(hdr.get("x-accepted-github-permissions"), where, need))
   if e.code == 404:
      return GitError(missing or f"the token can't see {where}: it isn't in the token's "
                      "Repository access, or there's no such repo")
   return GitError(NO_ANSWER if e.code >= 500 else f"GitHub refused it ({e.code})")


def _pull_request(query: str, variables: dict, token: str, where: str, number: int):
   """The query's pullRequest and its errors (a refused part degrades only that part)."""
   try:
      data, errors = graphql(query, variables, token)
   except (OSError, ValueError) as e:
      raise refusal(e, where, PR_NEED) from e
   kinds = {err.get("type") for err in errors}
   if "RATE_LIMITED" in kinds:
      raise GitError(_at(time.time() + RATE_WAIT_S))
   repo = data.get("repository")
   if repo is None:
      if "FORBIDDEN" in kinds:
         raise GitError(permission_error(None, where, PR_NEED))
      raise GitError(f"the token can't see {where}: it isn't in the token's Repository access, "
                     "or there's no such repo")
   if repo.get("pullRequest") is None:
      if "FORBIDDEN" in kinds:
         raise GitError(permission_error(None, where, PR_NEED))
      raise GitError(f"{where} has no pull request #{number}")
   return repo["pullRequest"], errors


def _refused_parts(errors: list) -> set:
   """Names in the paths of the parts GitHub refused (FORBIDDEN)."""
   return {p for err in errors if err.get("type") == "FORBIDDEN"
           for p in (err.get("path") or []) if isinstance(p, str)}


# --- what reads hand DAWN -------------------------------------------------------

_CONTROLS = re.compile(r"[\x00-\x08\x0b-\x1f]")


def clean(s, cap: int) -> str:
   """`s` cut to `cap` characters, with NULs and control characters (but tab and
   newline) dropped; "" for anything not a string."""
   return _CONTROLS.sub("", s)[:cap] if isinstance(s, str) else ""


def link(u) -> str | None:
   """A github.com link, or None (anything else is dropped)."""
   u = clean(u, URL_CAP + 1)
   return u if u.startswith(GITHUB_WEB) and len(u) <= URL_CAP and " " not in u else None


def login(actor) -> str:
   """An author's login, lowercased, without the "[bot]" GitHub's REST adds."""
   name = clean((actor or {}).get("login"), LOGIN_CAP).lower()
   return name[:-len("[bot]")] if name.endswith("[bot]") else name


def lower(s) -> str | None:
   return clean(s, NAME_CAP).lower() or None


STATUS_QUERY = """
query($owner: String!, $name: String!, $number: Int!) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      url title state isDraft merged mergeable mergeStateStatus reviewDecision
      baseRefName headRefName headRefOid
      author { login }
      reviewRequests(first: %(requests)d) {
        nodes { requestedReviewer { __typename ... on User { login } ... on Bot { login }
                                    ... on Team { name } } }
      }
      latestReviews(first: %(reviews)d) {
        nodes { author { login } state submittedAt commit { oid } }
      }
      reviewThreads(first: %(threads)d) { totalCount nodes { isResolved } }
      commits(last: 1) { nodes { commit { statusCheckRollup { state
        contexts(first: %(contexts)d) { totalCount nodes {
          __typename
          ... on CheckRun { name status conclusion databaseId detailsUrl
                            isRequired(pullRequestNumber: $number) checkSuite { app { slug } } }
          ... on StatusContext { context state targetUrl
                                 isRequired(pullRequestNumber: $number) }
        } }
      } } } }
    }
  }
}
""" % {"requests": REQUESTS_ASKED, "reviews": REVIEWS_ASKED, "threads": THREAD_COUNT_ASKED,
       "contexts": CONTEXTS_ASKED}


def _check(n: dict) -> dict:
   if n.get("__typename") == "StatusContext":
      state = lower(n.get("state"))
      done = state not in ("pending", "expected")
      return {"name": clean(n.get("context"), NAME_CAP), "required": bool(n.get("isRequired")),
              "status": "completed" if done else "pending", "conclusion": state if done else None,
              "failed": done and state in FAILED,
              "job_id": None, "app": None, "url": link(n.get("targetUrl"))}
   app = clean(((n.get("checkSuite") or {}).get("app") or {}).get("slug"), LOGIN_CAP) or None
   job = n.get("databaseId")
   conclusion = lower(n.get("conclusion"))
   return {"name": clean(n.get("name"), NAME_CAP), "required": bool(n.get("isRequired")),
           "status": lower(n.get("status")), "conclusion": conclusion,
           "failed": conclusion in FAILED,
           "job_id": job if app == "github-actions" and isinstance(job, int) else None,
           "app": app, "url": link(n.get("detailsUrl"))}


def _rank(c: dict) -> tuple:
   failed = c["failed"]
   return (not (failed and c["required"]), not failed, c["status"] == "completed", c["name"])


def _status(owner: str, repo: str, number: int, token: str) -> tuple[dict, list, int, str]:
   """The pull request, all its checks ranked, how many GitHub has, and a note
   when they couldn't be read."""
   where = f"{owner}/{repo}"
   pr, errors = _pull_request(STATUS_QUERY, {"owner": owner, "name": repo, "number": number},
                              token, where, number)
   commits = ((pr.get("commits") or {}).get("nodes") or [None])[0] or {}
   rollup = (commits.get("commit") or {}).get("statusCheckRollup") or {}
   contexts = rollup.get("contexts") or {}
   checks = sorted((_check(n) for n in contexts.get("nodes") or [] if n), key=_rank)
   note = ""
   if _refused_parts(errors) & {"commits", "commit", "statusCheckRollup", "contexts"}:
      note = f"checks unreadable: {permission_error(None, where, CHECKS_NEED)}"
   return pr, checks, contexts.get("totalCount") or len(checks), note


def pr_status(owner: str, repo: str, number: int, token: str) -> dict:
   """A pull request's state, its checks (failing required first), reviews,
   requested reviewers, whether Copilot reviews it, and its thread counts."""
   pr, checks, total, note = _status(owner, repo, number, token)
   reviews = [{"login": login(r.get("author")), "state": lower(r.get("state")),
               "commit": clean((r.get("commit") or {}).get("oid"), NAME_CAP) or None,
               "submitted": clean(r.get("submittedAt"), NAME_CAP)}
              for r in (pr.get("latestReviews") or {}).get("nodes") or [] if r]
   requested = []
   for r in (pr.get("reviewRequests") or {}).get("nodes") or []:
      who = (r or {}).get("requestedReviewer") or {}
      name = login(who) if "login" in who else clean(who.get("name"), LOGIN_CAP)
      if name:
         requested.append(name)
   head = clean(pr.get("headRefOid"), NAME_CAP)
   copilot_at = next((r["commit"] for r in reviews if r["login"] == COPILOT), None)
   copilot = ("requested" if COPILOT in requested
              else "reviewed_head" if copilot_at and copilot_at == head
              else "reviewed_earlier" if copilot_at else "none")
   threads = (pr.get("reviewThreads") or {}).get("nodes") or []
   resolved = sum(1 for t in threads if t and t.get("isResolved"))
   merge_state = lower(pr.get("mergeStateStatus"))
   out = {"pr": {"owner": owner, "repo": repo, "number": number, "url": link(pr.get("url")),
                 "title": clean(pr.get("title"), TITLE_CAP), "author": login(pr.get("author")),
                 "state": "merged" if pr.get("merged") else lower(pr.get("state")),
                 "draft": bool(pr.get("isDraft")), "merged": bool(pr.get("merged")),
                 "mergeable": lower(pr.get("mergeable")), "merge_state": merge_state,
                 # A conflict with the base means the base has commits the head lacks.
                 "behind": merge_state == "behind" or lower(pr.get("mergeable")) == "conflicting",
                 "base": clean(pr.get("baseRefName"), NAME_CAP),
                 "head_ref": clean(pr.get("headRefName"), NAME_CAP),
                 "head_sha": clean(pr.get("headRefOid"), NAME_CAP),
                 "review_decision": lower(pr.get("reviewDecision"))},
          "checks": checks[:CHECKS_CAP], "checks_total": total,
          "reviews": reviews, "requested": requested, "copilot": copilot,
          "threads": {"unresolved": len(threads) - resolved, "resolved": resolved,
                      "total": (pr.get("reviewThreads") or {}).get("totalCount") or len(threads)}}
   if note:
      out["checks_note"] = note
   return out


THREADS_QUERY = """
query($owner: String!, $name: String!, $number: Int!, $after: String, $first: Boolean!) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      headRefOid
      reviewThreads(first: %(page)d, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes { id isResolved resolvedBy { login } isOutdated path line originalLine
                head: comments(first: 1) { totalCount nodes { ...C } }
                tail: comments(last: %(tail)d) { nodes { ...C } } }
      }
      reviews(last: %(reviews)d) @include(if: $first) {
        nodes { databaseId author { login __typename } state body submittedAt url
                commit { oid } } }
      comments(last: %(conversation)d) @include(if: $first) {
        nodes { author { login __typename } authorAssociation body createdAt url } }
    }
  }
}
fragment C on PullRequestReviewComment {
  databaseId author { login __typename } authorAssociation body createdAt url
  pullRequestReview { databaseId }
}
""" % {"page": THREADS_PAGE, "tail": THREAD_TAIL, "reviews": REVIEWS_ASKED,
       "conversation": CONVERSATION_ASKED}


def _comment(c: dict) -> dict:
   author = c.get("author") or {}
   out = {"author": login(author), "bot": author.get("__typename") == "Bot",
          "association": lower(c.get("authorAssociation")), "body": clean(c.get("body"), BODY_CAP),
          "created": clean(c.get("createdAt"), NAME_CAP), "url": link(c.get("url"))}
   if isinstance(c.get("databaseId"), int):
      out = {"comment_id": c["databaseId"], **out}
   return out


def _thread(t: dict) -> dict | None:
   head = t.get("head") or {}
   comments, seen = [], set()
   for c in [*(head.get("nodes") or []), *((t.get("tail") or {}).get("nodes") or [])]:
      if c and c.get("databaseId") not in seen:
         seen.add(c.get("databaseId"))
         comments.append(_comment(c))
   if not comments or "comment_id" not in comments[0]:
      return None  # its first comment is gone: nothing to name it by
   outdated = bool(t.get("isOutdated"))
   line = t.get("originalLine") if outdated or t.get("line") is None else t.get("line")
   first = (head.get("nodes") or [{}])[0] or {}
   resolved = bool(t.get("isResolved"))
   return {"id": clean(t.get("id"), ID_CAP), "comment_id": comments[0]["comment_id"],
           "path": clean(t.get("path"), PATH_CAP), "line": line if isinstance(line, int) else None,
           "outdated": outdated, "resolved": resolved,
           "resolved_by_copilot": resolved and login(t.get("resolvedBy")) == COPILOT_RESOLVER,
           "comments": comments,
           "comments_total": head.get("totalCount") or len(comments),
           # The review it came in, kept internal: which of Copilot's rounds it is from.
           "review_id": (first.get("pullRequestReview") or {}).get("databaseId")}


def _size(o) -> int:
   return len(json.dumps(o, ensure_ascii=False).encode())


def threads_raw(owner: str, repo: str, number: int, token: str,
                deadline: float | None = None) -> tuple[list, dict, bool]:
   """Every review thread GitHub lists (resolved too; THREADS_PAGES pages at most,
   fewer once `deadline`, monotonic, has passed), the first page's pull request
   (its reviews and conversation), and whether threads were left unread."""
   where = f"{owner}/{repo}"
   found, after, more, first = [], None, False, None
   for _page in range(THREADS_PAGES):
      pr, _errors = _pull_request(THREADS_QUERY, {"owner": owner, "name": repo,
                                                  "number": number, "after": after,
                                                  "first": after is None},
                                  token, where, number)
      first = first or pr  # reviews and comments: as the first page read them
      page = pr.get("reviewThreads") or {}
      found += [t for t in map(_thread, filter(None, page.get("nodes") or [])) if t]
      more = bool((page.get("pageInfo") or {}).get("hasNextPage"))
      after = (page.get("pageInfo") or {}).get("endCursor")
      if not more or not after or (deadline is not None and time.monotonic() >= deadline):
         break
   return found, first, more


def as_login(token: str) -> str | None:
   """The token's user's login, or None when GitHub can't say."""
   try:
      return token_login(token)
   except (OSError, ValueError, GitError):
      return None


def _norm(text: str) -> str:
   return " ".join(text.lower().split())


def _answered(t: dict, me: str | None) -> bool:
   """Answered or declined: resolved, or replied to by the token's user."""
   return t["resolved"] or bool(me) and any(c["author"] == me for c in t["comments"][1:])


def _earlier(a: dict, t: dict) -> bool:
   """Whether thread `a` is from a review before `t`'s (review ids grow), or
   either's review isn't known."""
   ra, rt = a.get("review_id"), t.get("review_id")
   return ra is None or rt is None or ra < rt


def _mark_repeats(threads: list, me: str | None) -> None:
   """Each thread that says again what an answered one from an earlier review,
   on the same file and near the same line, said gets repeat_of: that thread's
   comment_id.  Answered or
   not: replying to a repeat doesn't make it new."""
   answered = [(a, _norm(a["comments"][0]["body"])[:REPEAT_COMPARED])
               for a in threads if _answered(a, me)]
   for t in threads:
      said = _norm(t["comments"][0]["body"])[:REPEAT_COMPARED]
      for a, before in answered:
         if a is t or a["path"] != t["path"] or not _earlier(a, t):
            continue
         # The same words about another spot in the file are another finding.
         if a.get("line") and t.get("line") and abs(a["line"] - t["line"]) > REPEAT_LINES:
            continue
         # autojunk off: on text over 200 characters it drops the common characters.
         m = difflib.SequenceMatcher(None, said, before, autojunk=False)
         if (m.real_quick_ratio() >= REPEAT_SIMILARITY and m.quick_ratio() >= REPEAT_SIMILARITY
               and m.ratio() >= REPEAT_SIMILARITY):
            t["repeat_of"] = a["comment_id"]
            break


def copilot_round(threads: list, reviews: list) -> dict | None:
   """Copilot's latest review: the commit it reviewed, whether it had reviewed
   before (a re-review), and its threads: how many, how many new, how many
   repeats of threads already answered.  None when Copilot hasn't reviewed."""
   mine = [r for r in reviews if r and login(r.get("author")) == COPILOT]
   if not mine:
      return None
   last = mine[-1]
   rid = last.get("databaseId")
   own = [t for t in threads if rid is not None and t.get("review_id") == rid]
   repeats = sum(1 for t in own if "repeat_of" in t)
   return {"commit": clean((last.get("commit") or {}).get("oid"), NAME_CAP) or None,
           "submitted": clean(last.get("submittedAt"), NAME_CAP), "rereview": len(mine) > 1,
           "threads": len(own), "new": len(own) - repeats, "repeats": repeats}


def pr_threads(owner: str, repo: str, number: int, token: str, everything: bool = False) -> dict:
   """A pull request's unresolved review threads (with `everything`, resolved ones
   too), each by its first comment's id and marked repeat_of when it repeats an
   answered one; Copilot's latest round; its reviews with a body; its newest
   conversation comments.  Cut to THREADS_REPLY_CAP, threads first."""
   found, first, more = threads_raw(owner, repo, number, token)
   me = as_login(token)
   _mark_repeats(found, me)
   reviews_raw = (first.get("reviews") or {}).get("nodes") or []
   shown = [{k: v for k, v in t.items() if k != "review_id"} for t in found
            if everything or not t["resolved"]]
   # Open threads first, then ones Copilot resolved, then a person's: the cap
   # cuts old answered history, never a new round's thread (stable: each group
   # keeps GitHub's order).
   shown.sort(key=lambda t: 0 if not t["resolved"] else 1 if t["resolved_by_copilot"] else 2)
   reviews = [{"author": login(r.get("author")), "state": lower(r.get("state")),
               "body": clean(r.get("body"), BODY_CAP), "submitted": clean(r.get("submittedAt"),
                                                                          NAME_CAP),
               "url": link(r.get("url"))}
              for r in reviews_raw if r and str(r.get("body") or "").strip()]
   conversation = [_comment(c) for c in (first.get("comments") or {}).get("nodes") or [] if c]
   out = {"threads": [], "reviews": [], "comments": [], "as_login": me,
          "head_sha": clean(first.get("headRefOid"), NAME_CAP), "total": len(shown),
          "truncated": more, "copilot_round": copilot_round(found, reviews_raw)}
   room = THREADS_REPLY_CAP - _size(out)
   for key, items in (("threads", shown), ("reviews", reviews[::-1]),
                      ("comments", conversation[::-1])):
      for item in items:
         if key == "threads":
            item = {**item, "n": len(out["threads"]) + 1}
         cost = _size(item) + LIST_SEP
         if cost > room:
            out["truncated"] = True
            break
         room -= cost
         out[key].append(item)
   out["reviews"].reverse()   # oldest first again
   out["comments"].reverse()
   return out


_ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
_STAMP = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z ?")
_ERROR_LINE = re.compile(r"##\[error\]|error:|Error|FAILED|FAIL:|failed")


def _cut_bytes(text: str, cap: int, keep_end: bool = False) -> str:
   """`text` in at most `cap` UTF-8 bytes, on a whole character: its start, or
   with `keep_end` its end."""
   raw = text.encode()
   if len(raw) <= cap:
      return text
   return (raw[-cap:] if keep_end else raw[:cap]).decode(errors="ignore")


def log_excerpt(raw: bytes, cut_start: bool = False) -> str:
   """What of a job's log says why it failed: its error lines with
   LOG_CONTEXT lines around each (the LOG_HITS_CAP nearest the end, where the
   failure is), then its last LOG_TAIL_LINES lines, without colours or
   timestamps, in EXCERPT_CAP bytes.  `cut_start`: `raw` is the log's end, so
   its first line may be partial (dropped)."""
   lines = [_STAMP.sub("", clean(_ANSI.sub("", line), len(line)))
            for line in raw.decode(errors="replace").splitlines()]
   if cut_start and lines:
      lines = lines[1:]
   tail = max(0, len(lines) - LOG_TAIL_LINES)
   hits = [i for i in range(tail) if _ERROR_LINE.search(lines[i])][-LOG_HITS_CAP:]
   shown = sorted({j for i in hits
                   for j in range(max(0, i - LOG_CONTEXT), min(tail, i + LOG_CONTEXT + 1))})
   head, last = [], -1
   for i in shown:
      if i != last + 1:
         head.append(CUT)
      head.append(lines[i])
      last = i
   if tail > 0 and last != tail - 1:
      head.append(CUT)
   end = "\n".join(lines[tail:])
   if not head:
      return _cut_bytes(end, EXCERPT_CAP, keep_end=True)
   start = "\n".join(head)
   if len(start.encode()) + len(end.encode()) + 1 <= EXCERPT_CAP:
      return f"{start}\n{end}"
   # Too long: the error lines nearest the end take up to half, the log's end the rest.
   start = _cut_bytes(start, EXCERPT_CAP // 2, keep_end=True)
   room = EXCERPT_CAP - len(start.encode()) - len(CUT) - 2
   return f"{start}\n{CUT}\n{_cut_bytes(end, room, keep_end=True)}"


def pr_check_log(owner: str, repo: str, number: int, check, token: str) -> dict:
   """The log excerpt of one of a pull request's GitHub Actions checks, named by
   its name or its job id (the failing one first when names repeat)."""
   where = f"{owner}/{repo}"
   _, checks, _, note = _status(owner, repo, number, token)
   want = str(check if check is not None else "").strip()
   listed = re.fullmatch(r".* \(job (\d+)\)", want)  # as pr_status lists it
   if listed:
      want = listed.group(1)
   match = next((c for c in checks if (want.isdigit() and c["job_id"] == int(want))
                 or c["name"].lower() == want.lower()), None)
   if match is None:
      raise GitError(note or f"{where}#{number} has no check named {clean(want, NAME_CAP)!r}")
   if match["job_id"] is None:
      raise GitError(f"{match['name']} isn't a GitHub Actions job, so its log can't be read "
                     f"here; see {match['url'] or 'the pull request'}")
   log = job_log(owner, repo, match["job_id"], token, time.monotonic() + LOG_READ_S)
   return {"name": match["name"], "conclusion": match["conclusion"], "url": match["url"], **log}


def job_log(owner: str, repo: str, job_id: int, token: str, deadline: float) -> dict:
   """The excerpt of GitHub Actions job `job_id`'s log, read until `deadline`
   (monotonic): {excerpt, excerpt_bytes, log_bytes_read, truncated}."""
   where = f"{owner}/{repo}"
   api = f"{GITHUB_API}/repos/{owner}/{repo}/actions/jobs/{job_id}/logs"
   gone = f"GitHub has no log for job {job_id} on {where} (logs expire)"
   read = {"bytes": 0, "complete": False}
   try:
      try:
         _, _, body = _request(api, headers=github_headers(token), follow=False,
                               limit=LOG_READ_CAP, deadline=deadline, read=read)
      except urllib.error.HTTPError as e:
         location = (e.headers or {}).get("Location") if e.code in HTTP_REDIRECTS else None
         if not location:
            raise
         # A signed link, valid a minute: fetched without the token, never logged.
         _, _, body = _request(location, limit=LOG_READ_CAP, deadline=deadline, read=read)
   except (OSError, ValueError) as e:
      raise refusal(e, where, "Actions: Read-only", gone) from None
   excerpt = log_excerpt(body, cut_start=read["bytes"] > len(body))
   return {"excerpt": excerpt, "excerpt_bytes": len(excerpt.encode()),
           "log_bytes_read": read["bytes"],
           "truncated": read["bytes"] > len(body) or not read["complete"]}


# --- what a revise hands the agent -----------------------------------------------


def _feedback_thread(t: dict) -> dict:
   out = {"comment_id": t["comment_id"], "path": t["path"], "line": t["line"],
          "outdated": t["outdated"], "comments_total": t["comments_total"],
          "comments": [{k: c[k] for k in ("author", "bot", "association", "body")}
                       for c in t["comments"]]}
   if "repeat_of" in t:
      out["repeat_of"] = t["repeat_of"]
   return out


def pr_feedback(owner: str, repo: str, number: int, token: str, deadline: float) -> dict:
   """What a revise hands the agent: the pull request's unresolved review threads
   (current ones first, then outdated), its failing required checks with their
   log excerpts, and each person's latest review when it requests changes.  Read
   until `deadline` (monotonic); in FEEDBACK_CAP bytes, what didn't fit counted.
   A pull request that isn't open: only its state."""
   pr, checks, _, note = _status(owner, repo, number, token)
   state = "merged" if pr.get("merged") else lower(pr.get("state"))
   out = {"pull_request": link(pr.get("url")), "state": state,
          "head": clean(pr.get("headRefOid"), NAME_CAP)}
   if state != "open":
      return out
   found, first, more = threads_raw(owner, repo, number, token, deadline)
   _mark_repeats(found, as_login(token))
   threads = sorted((t for t in found if not t["resolved"]), key=lambda t: t["outdated"])
   failing = [c for c in checks if c["failed"] and c["required"]]
   latest = {}  # a person's reviews, oldest first: the last one stands
   for r in (first.get("reviews") or {}).get("nodes") or []:
      if r and (r.get("author") or {}).get("__typename") != "Bot":
         latest[login(r.get("author"))] = r
   asked = [{"author": who, "body": clean(r.get("body"), BODY_CAP)} for who, r in latest.items()
            if lower(r.get("state")) == "changes_requested" and str(r.get("body") or "").strip()]
   out.update(threads=[], threads_total=len(threads), threads_unread=more, checks=[],
              checks_total=len(failing), changes_requested=[])
   if note:
      out["checks_note"] = note
   room = FEEDBACK_CAP - _size(out)  # the threads first: a long CI log can't crowd them out
   for key, items in (("changes_requested", asked),
                      ("threads", [_feedback_thread(t) for t in threads[:FEEDBACK_THREADS_CAP]])):
      for item in items:
         cost = _size(item) + LIST_SEP
         if cost > room:
            break
         room -= cost
         out[key].append(item)
   shown = failing[:FEEDBACK_CHECKS_CAP]
   for i, c in enumerate(shown):
      item = {"name": c["name"], "url": c["url"]}
      if c["job_id"] is None:
         item["log_error"] = "not a GitHub Actions job: its log isn't read here"
      elif time.monotonic() >= deadline:
         item["log_error"] = "not read: out of time"
      else:
         try:
            item["log"] = job_log(owner, repo, c["job_id"], token, deadline)["excerpt"]
         except GitError as e:
            item["log_error"] = str(e)
      over = _size(item) + LIST_SEP - room // (len(shown) - i)  # each log its share
      if "log" in item and over > 0:
         log = item.pop("log")  # its end kept, after CUT and a newline (two bytes in JSON)
         kept = _cut_bytes(log, max(0, len(log.encode()) - over - len(CUT) - 2), keep_end=True)
         if kept:
            item["log"] = f"{CUT}\n{kept}"
         else:
            item["log_error"] = "not shown: no room left"
      cost = _size(item) + LIST_SEP
      if cost > room:
         break
      room -= cost
      out["checks"].append(item)
   out["threads_not_shown"] = len(threads) - len(out["threads"])
   return out


# --- writing: replies, resolving, asking Copilot ---------------------------------


class ItemError(GitError):
   """One reply refused (its comment isn't there); the others still go."""


def post_reply(owner: str, repo: str, number: int, comment_id: int, body: str,
               token: str) -> str | None:
   """Reply on the review thread started by comment `comment_id`; its address."""
   where = f"{owner}/{repo}"
   try:
      _, _, raw = _request(
         f"{GITHUB_API}/repos/{owner}/{repo}/pulls/{number}/comments/{comment_id}/replies",
         method="POST", data=json.dumps({"body": body}).encode(),
         headers={**github_headers(token), "Content-Type": "application/json"})
      return link(json.loads(raw).get("html_url"))
   except urllib.error.HTTPError as e:
      if e.code in (404, 422):
         raise ItemError(f"GitHub refused the reply on comment {comment_id} ({e.code}): it "
                         "isn't a top-level review comment on this pull request") from None
      raise refusal(e, where, WRITE_NEED) from None
   except (OSError, ValueError) as e:
      raise refusal(e, where, WRITE_NEED) from None


RESOLVE_MUTATION = """
mutation($id: ID!) { resolveReviewThread(input: {threadId: $id}) { thread { isResolved } } }
"""


def resolve_thread(thread_id: str, token: str, where: str) -> bool:
   """Resolve a review thread (its GraphQL id); whether GitHub says it is."""
   try:
      data, errors = graphql(RESOLVE_MUTATION, {"id": thread_id}, token)
   except (OSError, ValueError) as e:
      raise refusal(e, where, WRITE_NEED) from None
   if errors:
      if any(err.get("type") == "FORBIDDEN" for err in errors):
         raise GitError(permission_error(None, where, WRITE_NEED))
      said = clean(errors[0].get("message"), ERROR_SHOWN)
      raise GitError(f"GitHub didn't resolve the thread: {said}")
   return bool(((data.get("resolveReviewThread") or {}).get("thread") or {}).get("isResolved"))


GATE_QUERY = """
query($owner: String!, $name: String!, $number: Int!) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      url state merged headRefOid
      reviewRequests(first: %(requests)d) {
        nodes { requestedReviewer { __typename ... on User { login } ... on Bot { login } } }
      }
      commits(last: %(commits)d) { nodes { commit { oid parents { totalCount } } } }
    }
  }
}
""" % {"requests": REQUESTS_ASKED, "commits": COMMITS_ASKED}


def _when(stamp: str) -> float:
   """An ISO 8601 time GitHub gives, in seconds; 0 when there is none."""
   try:
      return float(calendar.timegm(time.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ")))
   except (TypeError, ValueError):
      return 0.0


def review_gate(owner: str, repo: str, number: int, token: str, asked_at: float = 0.0,
                pending_s: float = 0.0) -> dict:
   """Whether Copilot may be asked to review again: refused (GitError) while the
   pull request isn't open and while its review is pending (requested, or asked at
   `asked_at` within `pending_s` with no review since).  Else the pull request,
   Copilot's round (its repeats marked), and the commits since its last review
   (new_commits; merges of the base don't count; None when its commit is past the
   ones read): what the user weighs before asking."""
   where = f"{owner}/{repo}#{number}"
   pr, _ = _pull_request(GATE_QUERY, {"owner": owner, "name": repo, "number": number}, token,
                         f"{owner}/{repo}", number)
   state = "merged" if pr.get("merged") else lower(pr.get("state"))
   if state != "open":
      raise GitError(f"{where} is {state}: Copilot reviews only an open pull request")
   found, first, _ = threads_raw(owner, repo, number, token)
   _mark_repeats(found, as_login(token))
   reviews = (first.get("reviews") or {}).get("nodes") or []
   last = copilot_round(found, reviews)
   requested = any(login((r or {}).get("requestedReviewer")) == COPILOT
                   for r in (pr.get("reviewRequests") or {}).get("nodes") or [])
   reviewed_at = _when(last["submitted"]) if last else 0.0
   if requested or (asked_at > reviewed_at and time.time() - asked_at < pending_s):
      raise GitError(f"Copilot's review of {where} is already pending; wait for it")
   new = None
   if last and last["commit"]:
      nodes = (pr.get("commits") or {}).get("nodes") or []
      commits = [(n or {}).get("commit") or {} for n in nodes]
      oids = [c.get("oid") for c in commits]
      if last["commit"] in oids:
         after = commits[oids.index(last["commit"]) + 1:]
         new = sum(1 for c in after if ((c.get("parents") or {}).get("totalCount") or 0) < 2)
   return {"pr": {"owner": owner, "repo": repo, "number": number, "url": link(pr.get("url"))},
           "head_sha": clean(pr.get("headRefOid"), NAME_CAP), "copilot_round": last,
           "new_commits": new}


def request_copilot(owner: str, repo: str, number: int, token: str) -> dict:
   """Ask Copilot to review the pull request."""
   where = f"{owner}/{repo}#{number}"
   try:
      _request(
         f"{GITHUB_API}/repos/{owner}/{repo}/pulls/{number}/requested_reviewers",
         method="POST", data=json.dumps({"reviewers": [COPILOT_REVIEWER]}).encode(),
         headers={**github_headers(token), "Content-Type": "application/json"})
   except urllib.error.HTTPError as e:
      if e.code == 422:
         try:
            said = clean(json.loads(error_text(e) or "{}").get("message"), ERROR_SHOWN)
         except ValueError:
            said = ""
         raise GitError(f"GitHub refused to ask Copilot to review {where} (422"
                        f"{': ' + said if said else ''}): Copilot code review may be off for "
                        "this repo, or the account's Copilot review quota used up") from None
      raise refusal(e, f"{owner}/{repo}", WRITE_NEED) from None
   except (OSError, ValueError) as e:
      raise refusal(e, f"{owner}/{repo}", WRITE_NEED) from None
   return {"requested": True}
