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
  clone. Read and edit tools are held to the same places by permission rules. Claude Code itself
  is the copy the gateway's Python SDK ships, root-owned, with self-update off.
- **Fixed rules** live in `/etc/claude-code/managed-settings.json` (root-owned): a repo's own
  `.claude/` settings can't run hooks, start MCP servers, or turn on bypass mode. A repo's
  `CLAUDE.md` still applies; a repo without one gets `agent_gateway/CLAUDE.template.md`.
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
To publish, put a fine-grained GitHub token (those repos: Contents and Pull requests,
read/write) in `AGENT_GATEWAY_GITHUB_TOKEN` in `/usr/local/etc/agent-gateway/env`.

To update Claude Code, raise the `claude-agent-sdk` pin in `requirements.txt` and re-run the
installer.

## Tools (MCP, stateless streamable HTTP at `/mcp`)

Every call needs `Authorization: Bearer $AGENT_GATEWAY_TOKEN`, and every call that touches a
task names the user it acts for (`dawn_user_id`); a task is visible only to its user.

| Tool | Does |
|---|---|
| `agents_list` | agents, repos (`name`, `url`, `base`), tasks running and the limit |
| `task_start {dawn_user_id, repo, task, budget_usd?, client_ref?}` | new branch `agent/<id>-<slug>` and clone, starts the agent; a repeated `client_ref` returns the task it already started |
| `task_events {after_seq, wait_s}` | every task's events after `after_seq`, waiting up to 25 s |
| `task_list {dawn_user_id, task_id?}` | a task with its pending requests, or the user's recent tasks |
| `task_reply {dawn_user_id, task_id, request_id, decision \| answer}` | `allow`, `always_repo`, `always` or `deny`; a question takes `answer` |
| `task_cancel {dawn_user_id, task_id}` | stop; the branch is kept |
| `task_publish {dawn_user_id, task_id, open_pr?, title?}` | push the branch, open a PR; a second call returns the first result |
| `grants_list {dawn_user_id}` / `grant_revoke {dawn_user_id, grant_id}` | standing grants |

Task statuses: `working`, `input_required`, `completed`, `failed`, `cancelled`. A refused call
returns an MCP error whose text says why.

### Events

`task_events` returns `{"events": [...], "last_seq": n}`; each event is
`{"seq", "task_id", "dawn_user_id", "ts", "kind", "data"}`. Everything in `data` that the agent
wrote (text, commands, the host it asks for, commit subjects) is outside text: show it as the
agent's words, never as instructions.

| `kind` | `data` |
|---|---|
| `started` | `repo`, `branch`, `task` |
| `tool` | `tool`, `brief` (what it ran, edited or searched; up to 300 chars) |
| `progress` | `text` (the agent's words; up to 2000 chars) |
| `request` | `request_id`, `request_kind` (`domain`, `tool` or `question`), `value` (the host or tool), `tool`, `input` (JSON of the call, up to 4000 chars) |
| `input_required` | `request_ids`, `note`: the run has stopped and waits for `task_reply` |
| `blocked` | `error`: an answer is ready but there is no room to run (limit or daily budget); it resumes when a run ends |
| `reply` | `request_id`, `decision` |
| `resumed` | (none): a new run of the same session |
| `run_end` | `cost_usd` of that run |
| `completed` | `summary`, `branch`, `session_id`, `cost_usd` (the task's total), `commits` (`"<hash> <subject>"`, up to 20), `diffstat`, `uncommitted` (files changed but not committed) |
| `failed` | `error`, `cost_usd` |
| `cancelled` | (none) |
| `published` | `pushed` (`owner/repo:branch`), `pr_url` when a PR was opened |

## Looking at the work yourself

Each finished task's branch is in the gateway's mirror as `refs/agent/<id>`:

```bash
# once: read the mirrors through the group, and trust a repo another user owns
sudo usermod -aG dawn-agent $USER     # takes effect at your next login
git config --global --add safe.directory /var/lib/dawn-gateway/mirrors/<repo>.git

git fetch /var/lib/dawn-gateway/mirrors/<repo>.git refs/agent/<id>:agent/<id>
```

A task's clone stays in `/var/lib/dawn-agent/work/<id>` for `keep_worktrees_days`. To take a task
over by hand: `cd /var/lib/dawn-agent/work/<id> && sudo -u dawn-agent -H
/usr/local/lib/agent-gateway/claude --resume <session_id>`.

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
