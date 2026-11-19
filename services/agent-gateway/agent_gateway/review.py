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
# Writing on a pull request for DAWN, after the user's yes: replies on its
# review threads (each posted once, however often it is asked) and asking
# Copilot to review again.  GitHub's calls run off the event loop; what was
# posted is recorded in the store on it.

import asyncio
import hashlib
import json
import time

from . import github
from .github import GitError

REPLY_PACE_S = 1.0  # between two replies: well under GitHub's content-creation limits
COPILOT_PENDING_S = 3600  # a request with no review since counts as pending this long

_locks: dict[str, asyncio.Lock] = {}  # one write at a time per pull request


def _lock(where: str) -> asyncio.Lock:
   return _locks.setdefault(where.lower(), asyncio.Lock())


def reply_key(where: str, comment_id: int, body: str) -> str:
   """The store's key for a reply: its pull request, thread and text."""
   text = f"{where.lower()}:{comment_id}:{hashlib.sha256(body.encode()).hexdigest()}"
   return "posted_reply:" + hashlib.sha256(text.encode()).hexdigest()


def _checked(replies, resolve: bool) -> list[tuple[int, str, bool]]:
   if not isinstance(replies, list) or not 0 < len(replies) <= github.REPLIES_CAP:
      raise ValueError(f"replies: 1 to {github.REPLIES_CAP} of {{comment_id, body}}")
   out, seen = [], set()
   for r in replies:
      cid = r.get("comment_id") if isinstance(r, dict) else None
      body = r.get("body") if isinstance(r, dict) else None
      if not isinstance(cid, int) or isinstance(cid, bool) or cid <= 0 or cid in seen:
         raise ValueError("each reply names a different thread by its comment_id (a number)")
      if not isinstance(body, str) or not body.strip() or len(body) > github.REPLY_BODY_LIMIT:
         raise ValueError(f"each reply's body is 1 to {github.REPLY_BODY_LIMIT} characters")
      own = r.get("resolve", resolve)
      if not isinstance(own, bool):
         raise ValueError("a reply's resolve is true or false")
      seen.add(cid)
      out.append((cid, body, own))
   return out


async def post_replies(store, token: str, owner: str, repo: str, number: int, replies,
                       resolve: bool) -> dict:
   """Post each reply on the thread its comment_id starts, then resolve that thread
   when the reply's own resolve says so (`resolve` for a reply that doesn't).  A reply already posted (recorded here, or found on the
   thread by the token's user with the same text: an answer lost after it was
   posted) isn't posted again.  One refused reply leaves the rest to go; a
   refusal for the whole call (permission, rate limit) stops the ones after it."""
   items = _checked(replies, resolve)
   where = f"{owner}/{repo}#{number}"
   async with _lock(where):
      found, _, _ = await asyncio.to_thread(github.threads_raw, owner, repo, number, token)
      me = await asyncio.to_thread(github.as_login, token)
      threads = {t["comment_id"]: t for t in found}
      results, posted, stop = [], False, None
      for cid, body, close in items:
         t = threads.get(cid)
         if stop or t is None:
            results.append({"comment_id": cid, "error": stop or (
               f"comment {cid} doesn't start a review thread on {where}")})
            continue
         key = reply_key(where, cid, body)
         url, again = store.meta(key), False
         if url is None and me:
            same = github.clean(body, github.BODY_CAP)
            mine = next((c for c in t["comments"][1:]
                         if c["author"] == me and c["body"] == same), None)
            if mine is not None:
               url = mine["url"] or ""
               store.set_meta(key, url)
         if url is not None:
            again = True
         else:
            if posted:
               await asyncio.sleep(REPLY_PACE_S)
            try:
               url = await asyncio.to_thread(github.post_reply, owner, repo, number, cid, body,
                                             token) or ""
            except github.ItemError as e:
               results.append({"comment_id": cid, "error": str(e)})
               continue
            except GitError as e:
               stop = str(e)
               results.append({"comment_id": cid, "error": stop})
               continue
            posted = True
            store.set_meta(key, url)
         item = {"comment_id": cid, "url": url or None}
         if again:
            item["already_posted"] = True
         if close:
            if t["resolved"]:
               item["resolved"] = True
            else:
               try:
                  item["resolved"] = await asyncio.to_thread(github.resolve_thread, t["id"],
                                                             token, f"{owner}/{repo}")
               except GitError as e:
                  item["resolved"] = False
                  item["resolve_error"] = str(e)
         results.append(item)
   return {"results": results, "as_login": me}


def _asked_key(where: str) -> str:
   return "copilot_asked:" + where.lower()


async def request_review(store, token: str, owner: str, repo: str, number: int,
                         check: bool) -> dict:
   """Ask Copilot to review the pull request again, when github.review_gate
   allows it; with `check`, only what the gate says (for the preview)."""
   where = f"{owner}/{repo}#{number}"
   async with _lock(where):
      raw = store.meta(_asked_key(where))
      try:
         asked_at = float(json.loads(raw)["at"]) if raw else 0.0
      except (ValueError, KeyError, TypeError):
         asked_at = 0.0
      gate = await asyncio.to_thread(github.review_gate, owner, repo, number, token, asked_at,
                                     COPILOT_PENDING_S)
      if check:
         return {**gate, "requested": False}
      res = await asyncio.to_thread(github.request_copilot, owner, repo, number, token)
      store.set_meta(_asked_key(where), json.dumps({"at": time.time()}))
      return {**gate, **res}
