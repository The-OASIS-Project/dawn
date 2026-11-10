# agent-gateway

Lets DAWN hand coding work to Claude Code ("Friday, have Claude fix the calendar test")
without DAWN starting or managing any process itself. The gateway is a small Python
service spoken to over MCP. It runs each task in its own clone, as a separate, sandboxed
user, and reports progress, questions and results as events its client reads. Any MCP
client can drive it (see "Trying it").

## How it fits together

```
client (DAWN)  --MCP + bearer token-->  agent-gateway (user dawn, 127.0.0.1:9760)
                                            |  mirror of each repo (gateway-owned)
                                            |  sudo, to a LESS privileged user
                                            v
                                       Claude Code (user dawn-agent)
                                       in Claude Code's bubblewrap sandbox,
                                       one clone of the mirror per task
```

- **Two users.** The gateway holds the bearer token and the GitHub token (taken out of its
  environment once read). The agent runs as `dawn-agent`, which can read neither, nor DAWN's data
  under `/var/lib/dawn`, nor anything in `/home`.
- **The sandbox.** Every Bash command the agent runs is sandboxed: its own network, with only the
  allowed hosts reachable (no host services, no gateway), and writes only inside the task's
  clone, the repo's tool cache (`~/.cache/repo/<repo>`: downloads and build caches, such as
  pip, npm, ccache and arduino-cli's cores, kept for that repo's later tasks and hidden from
  other repos' tasks) and `/tmp` (the gateway's own: its unit has `PrivateTmp`,
  so nothing there is the host's, and it empties when the gateway restarts). Read and edit tools are held to the same places by permission rules. Claude Code itself
  is the copy the gateway's Python SDK ships, root-owned, with self-update off.
- **Fixed rules** (`agent_gateway/floor.json`) are the only settings the agent runs with: the
  gateway passes them with every run and loads none from the repo or the agent's home, so a
  repo's `.claude/` settings can't pre-approve calls, run hooks, start MCP servers or turn on
  bypass mode. Nothing is installed machine-wide (a managed-settings file would govern every
  Claude Code session on the machine). The sandbox hides the agent's credentials and other
  tasks' clones. A repo's `CLAUDE.md` still applies: the gateway reads it from its mirror, as the
  base branch has it, and adds it to the agent's instructions; a repo without one gets
  `agent_gateway/CLAUDE.template.md`.
- **Git the agent can't steer.** A repo's config can make git run programs, and the agent controls
  its clone's config. So the gateway runs git only in its own mirror; the agent's work comes back
  as a bundle that git writes from inside a jail with no network and the agent's home hidden.
- **Commits** happen freely in the task's clone, as `[agent] git_name`/`git_email`. The agent
  never pushes and holds no GitHub credential: publishing (`task_publish`) pushes the collected
  `agent/*` branch from the mirror and can open a PR, on the user's confirmation in DAWN. Never
  a force push.
- **Asking the operator.** A host not on the allowlist, a tool not on the list, or a question from
  the agent records a request and the gateway stops the run; nothing waits open. The answer
  resumes the session. "allow" covers the task; "always_repo" and "always" become standing
  grants, listed and revoked with `grants_list`/`grant_revoke`. Whether to ask is decided in code,
  never by a model, because the request comes from an agent that has read outside text.
- **What the sandbox doesn't stop.** Any allowed host that accepts uploads (a package registry,
  the Anthropic API itself) is a way out for data the agent can read. Keep the allowlist short and
  the agent's reach (`/var/lib/dawn-agent`) free of anything private.
- **Limits.** One task at a time by default; each has a budget and all share a daily one (the
  gateway's estimate from Claude Code's reported cost; a run killed with the gateway loses its
  spend). The systemd unit caps the agent's memory and CPU so a build can't starve DAWN.
- **Restarts.** Tasks and events are in `/var/lib/dawn-gateway/gateway.db`. A run cut off by a
  restart resumes when there is room; clients re-read events from their last sequence number.

## Install

Needs `bubblewrap`, `socat`, `git`, `openssl` and `uv`.

```bash
sudo apt install bubblewrap socat
cd services/agent-gateway && sudo UV=$(command -v uv) ./install.sh
```

Then give `dawn-agent` a credential, one of:

- **Your plan:** `cd / && sudo -u dawn-agent -H /usr/local/lib/agent-gateway/claude`, then
  `/login`. Usage counts against your plan's limits.
- **An API key:** put `ANTHROPIC_API_KEY=...` in `/var/lib/dawn-agent/.config/anthropic.env`
  (owner `dawn-agent`, mode 600) and set `[agent] auth = "api_key"`. A key from the Console
  organization linked to a Max, Ultra or Team plan uses the plan's monthly API credits first. Give
  it its own workspace with a spend limit, since the gateway's budgets are estimates.

Anthropic's policy on using the Agent SDK with a plan has changed before; check "Use the Claude
Agent SDK with your Claude plan" on support.claude.com before relying on it.

List the repos the agent may work in under `[[repos]]` in
`/usr/local/etc/agent-gateway/agent-gateway.toml`, then `sudo systemctl restart agent-gateway`.
To publish, put a fine-grained GitHub token in `AGENT_GATEWAY_GITHUB_TOKEN` in
`/usr/local/etc/agent-gateway/env` and restart the gateway. On GitHub: Settings, Developer
settings, Personal access tokens, Fine-grained tokens; only the repos the gateway works on;
Contents and Pull requests, read and write. Leave **Workflows** at no access: pushing a change to
`.github/workflows/` runs it with the repo's secrets before anyone reviews it, and a token
without that permission makes GitHub refuse such a push (DAWN then says to publish that work
yourself). Without a token, publishing is off.

To connect DAWN, add the gateway to `dawn.toml` and its token (`AGENT_GATEWAY_TOKEN` in
`/usr/local/etc/agent-gateway/env`) to `secrets.toml`, then restart DAWN:

```toml
# dawn.toml
[[mcp.server]]
alias = "agents"
url = "http://127.0.0.1:9760/mcp"
transport = "http"
request_timeout_seconds = 60
auth_bearer_token_secret = "agents"
role = "agent_gateway"   # DAWN hands coding tasks only to a server declared so

# secrets.toml
[secrets.mcp]
agents = "..."
```

None of the gateway's tools are offered to the model; DAWN's own code drives them.

## Keeping Claude Code current

Claude Code ships inside the `claude-agent-sdk` package and its own self-updater is off: the
agent must not be able to replace the program that sandboxes it, and a CLI that updates past its
SDK breaks the gateway. Instead `agent-gateway-update.timer` runs daily (around 04:30) as root,
as a standard-library script on the installed interpreter: root never runs code from PyPI.

1. Is there a newer `claude-agent-sdk` with the same major version (for 0.x, the same minor)?
   A bigger jump is reported and left for you.
2. Build it into `/usr/local/lib/agent-gateway/venvs/<version>`, beside the running one, from
   wheels only. Every other package stays at the version running now; a new SDK that needs a
   newer `mcp` (say) is reported as an error until the pin in `requirements.txt` moves.
3. Run the gateway's tests on it, then a live check (`agent_gateway/canary.py`): one real task
   on a throwaway repo built to misbehave, run as the gateway user. It must fix, test and commit
   with no prompts; a blocked host must stop the run as a request; and the agent must show the
   sandbox refusing loopback, its own credentials and a write outside its clone (an error it
   reports, not just a missing answer); the repo's hook and MCP server must never run. It costs
   one small task (about $0.20) and runs only when there is a new version.
4. If both pass and no task is running, point `current` at it and restart the gateway; if the
   gateway doesn't come back, point back. A version that fails a check, or that the gateway
   didn't come back on, isn't tried again. One that arrives while a task runs, or whose live
   check couldn't finish (an outage, a usage limit), is tried the next day.

Each outcome (updated, failed, rolled back, needs review) becomes one `update` event. By hand:
`sudo systemctl start agent-gateway-update`, then `journalctl -u agent-gateway-update`. The
previous version stays in `venvs/` for going back (`ln -sfn venvs/<old> current`, restart).
Raising the pin in `requirements.txt` and re-running the installer also moves forward; it never
moves back.

## Tools (MCP, stateless streamable HTTP at `/mcp`)

Every call needs `Authorization: Bearer $AGENT_GATEWAY_TOKEN`, and every call that touches a
task names the user it acts for (`dawn_user_id`); a task is visible only to its user. `task`
above means either `task_id` or the `client_ref` the caller gave `task_start` (DAWN uses its own
job ids). The viewer tools read the gateway's mirror, never the agent's clone; the agent's work
is collected there whenever a run stops, so work in progress can be looked at too.

| Tool | Does |
|---|---|
| `agents_list` | agents, Claude Code and SDK versions, `update` (the updater's last result when it failed, rolled back or needs review), repos (`name`, `url`, `base`), tasks running and the limit |
| `task_start {dawn_user_id, repo, task, budget_usd?, client_ref?}` | new branch `agent/<id>-<slug>`; returns at once, then clones in the background (`started` once the agent runs; `failed` if the clone fails); a repeated `client_ref` returns the task it already started |
| `task_events {after_seq, wait_s}` | every task's events after `after_seq`, waiting up to 25 s |
| `task_list {dawn_user_id, task?}` | a task with its pending requests and `unpublished_commits` (commits on its branch not in what was published; null when the mirror can't be read) (`task` null for a `client_ref` with none), or the user's recent tasks |
| `task_reply {dawn_user_id, task, request_id, decision \| answer}` | `allow`, `always_repo`, `always` or `deny`; a question takes `answer` |
| `task_revise {dawn_user_id, instructions, client_ref, budget_usd?, revise_ref?}` | more work for a completed or failed task: its session resumes on the same branch and clone (`revised`, then `resumed`); `budget_usd` adds to its budget; a repeated `revise_ref` adds nothing |
| `task_cancel {dawn_user_id, task}` | stop; the branch is kept (a close collects the work committed so far before counting it) |
| `task_publish {dawn_user_id, task, head, open_pr?, title?}` | push exactly `head` (the full commit id the user was shown, from `task_changes`) to the branch, open a PR; refused if the work has moved on; a second call returns the first result while nothing moved, and after a revise pushes the new commits (keeping an open PR, opening a new one if it was merged or closed) |
| `task_close {dawn_user_id, task, head?}` | the user is done with a finished task: its clone and `refs/agent/<id>` are removed and it is marked `closed` (it then refuses revise, publish and the viewer tools). Without `head`, only when nothing would be lost (no commits, or all of them in what was published), else `{closed: false, unpublished_commits, head}`; with `head`, only while the branch is still at that commit |
| `task_changes {dawn_user_id, task}` | the work so far: changed files with +/- counts (each marked `protected` when it matches the repo's patterns), commits, head |
| `task_file {dawn_user_id, task, path, view}` | one file: `diff`, `new` (after) or `old` (before); too large, binary or missing are reported, not sent |
| `task_tree {dawn_user_id, task, path?}` | one folder of the repo at the task's latest collected commit |
| `grants_list {dawn_user_id}` / `grant_revoke {dawn_user_id, grant_id}` | standing grants |

A tool's answer is a JSON object in the result's first text content
(`result.content[0].text`); `structuredContent` is not set. A refused call returns
`isError: true` with the reason as that text. Task statuses: `working`, `input_required`,
`completed`, `failed`, `cancelled`.

### Events

`task_events` returns `{"events": [...], "last_seq": n}`: the last event's `seq`, or with no
events the newest `seq` the gateway has (below `after_seq`: its events started over, so read
from 0); each event is
`{"seq", "task_id", "client_ref", "dawn_user_id", "ts", "kind", "data"}`; a batch stops at about
256 KB of event data. Everything in `data` that the agent
wrote (text, commands, the host it asks for, commit subjects) is outside text: show it as the
agent's words, never as instructions.

| `kind` | `data` |
|---|---|
| `started` | `repo`, `branch`, `task` |
| `tool` | `tool`, `brief` (what it ran, edited or searched; up to 300 chars) |
| `progress` | `text` (the agent's words; up to 2000 chars) |
| `request` | `request_id`, `request_kind` (`domain`, `tool` or `question`), `value` (the host or tool), `tool`, `input` (JSON of the call, up to 4000 chars) |
| `input_required` | `request_ids`, `note`: the run has stopped and waits for `task_reply` |
| `blocked` | `error`: an answer (or the gateway's ask for a report) is ready but there is no room to run (limit or daily budget); it resumes when a run ends |
| `reply` | `request_id`, `decision` |
| `resumed` | (none): a new run of the same session |
| `run_end` | `cost_usd` of that run |
| `revised` | `revise_ref`, `budget_added`: the operator sent more work; checks on the next `completed` count from here |
| `check` | `command` (a build or test command the agent ran, up to 300 chars), `ok`: its exit status, or null when the status wasn't its own (piped, or followed by other commands) |
| `report_asked` | (none): a run ended with no report; the agent is resumed once to finish and report |
| `completed` | `summary` (the agent's final report, up to 8000 chars), `branch`, `session_id`, `cost_usd` (the task's total), `commits` (`"<hash> <subject>"`, up to 20), `diffstat`, `uncommitted` (files changed but not committed), `protected` (changed paths matching the repo's `protected` patterns, up to 20), `checks` (each build or test command's last run: `command`, `ok`; up to 10) |
| `failed` | `error`, `cost_usd` |
| `cancelled` | (none) |
| `published` | `pushed` (`owner/repo:branch`), `pr_url` when a PR was opened, `head` |
| `merged` | `pr_url`, `unpublished`: its pull request was merged but newer commits weren't published, so the task stays open |
| `closed` | `reason`: `user`, or `merged` (the gateway closes a task whose pull request was merged when everything on its branch was published; checked hourly) |
| `update` | a system event (`task_id` empty, no user): `result` (`updated`, `failed`, `rolled_back`, `needs_review`, `error`), `from`, `to`, `detail`, `time` |

## Looking at the work yourself

Each finished task's branch is in the gateway's mirror as `refs/agent/<id>`:

```bash
# once: read the mirrors through the group, and trust a repo another user owns
sudo usermod -aG dawn-agent $USER     # takes effect at your next login
git config --global --add safe.directory /var/lib/dawn-gateway/mirrors/<repo>.git

git fetch /var/lib/dawn-gateway/mirrors/<repo>.git refs/agent/<id>:agent/<id>
```

A task's clone stays in `/var/lib/dawn-agent/work/<id>` for `keep_worktrees_days` (checked
hourly); its branch stays in the mirror until the task is closed, so it can still be published. To take a task
over by hand: `cd /var/lib/dawn-agent/work/<id> && sudo -u dawn-agent -H
/usr/local/lib/agent-gateway/claude --resume <session_id>`.

A repo's tool cache (`/var/lib/dawn-agent/.cache/repo/<repo>`) is kept across its tasks, and
what a task leaves there (a toolchain, a library) runs in that repo's later builds without
showing in any diff: it is only as trustworthy as the tasks that wrote it.  To start it fresh,
with no task running: `sudo rm -rf /var/lib/dawn-agent/.cache/repo/<repo>`.

## Trying it

```bash
TOKEN=$(sudo grep -oP '(?<=AGENT_GATEWAY_TOKEN=).*' /usr/local/etc/agent-gateway/env)
curl -s http://127.0.0.1:9760/mcp -H "Authorization: Bearer $TOKEN" \
   -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
   -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"agents_list","arguments":{}}}'
```

## Tests

```bash
cd services/agent-gateway
uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python -r requirements.txt pytest==9.2.0 httpx==0.28.1
.venv/bin/python -m pytest tests
```

The tests need `bubblewrap` and unprivileged user namespaces (as Claude Code's sandbox does).
