# Code Projects (the coding harness) — user & operator guide

DAWN can index your source repositories into a **code graph** and let the assistant
(Friday) answer questions about them — "what calls `foo`?", "trace the path from
`main`", "where is X defined?", "what changed on this branch?". This guide covers
what it is, how to set it up, and how to use it.

- **What does the work:** an external, operator-launched code-graph server called
  **cbm** (`codebase-memory-mcp`). DAWN never launches it (security invariant); you
  run it as the `cbm-mcp` service and DAWN connects to it over HTTP+SSE on
  localhost (`127.0.0.1:9750`).
- **What DAWN adds:** a project registry (import/link/refresh/rebuild/delete +
  branch tracking), a name-translation boundary that keeps your filesystem layout
  out of the LLM's view, and the WebUI + `dawn-admin` surfaces to manage it.

---

## 1. Prerequisites

1. **Build with the feature on.** The daemon must be built with both
   `DAWN_ENABLE_MCP_BRIDGE_TOOL=ON` and `DAWN_ENABLE_CODE_PROJECTS=ON` (and
   libgit2 ≥ 1.6 — see `DEPENDENCIES.md`). The `default`, `full`, and `debug`
   presets enable both flags together.
2. **Run the cbm server.** Install + start the `cbm-mcp` service (mcp-proxy fronting
   the `codebase-memory-mcp` binary over SSE on `127.0.0.1:9750`). Full install +
   `dawn.toml` `[mcp]` block: **[services/cbm-mcp/README.md](../services/cbm-mcp/README.md)**.
   Check it: `systemctl status cbm-mcp`.
3. **Enable it in `dawn.toml`:**
   ```toml
   [code_projects]
   enabled = true
   source_root = "/var/lib/dawn/source"   # where imported repos are cloned
   import_user_required = "admin"          # "" = any user, "admin" = operator-only
   allowed_host_pattern = "([a-z0-9-]+\\.)?(github\\.com|gitlab\\.com|codeberg\\.org)"
   # For linked local checkouts (section 4) only:
   # allowed_local_roots = ["/home/you/code"]
   ```
   Restart the daemon. If cbm isn't connected, a clone will succeed but indexing
   reports "no code server connected — start cbm-mcp, then re-index".

---

## 2. Two ways to add a project

| | **Import** (clone a remote) | **Linked local checkout** (existing checkout) |
|---|---|---|
| Source | an HTTPS git URL | a path already on disk |
| DAWN clones it? | yes, into `source_root/<name>` | no — reads your working tree in place |
| `kind` | `clone` | `local` |
| Branch | DAWN tracks + can switch it | tracks whatever you have checked out (read-only) |
| Who can | per `import_user_required` | **admin only** |
| From | WebUI, `dawn-admin`, or by asking Friday (§3) | WebUI or `dawn-admin` |
| Needs | host allowlist | `allowed_local_roots` + cbm read access (section 4) |

Both index the repo into the code graph; once `ready`, ask Friday about it.

---

## 3. Using it

### Asking Friday to add a repo
Say or type something like *"add The-OASIS-Project/dawn to my code projects"*. The
`code_project` tool takes a GitHub `owner/repo`, any github.com URL (a page under
the repo such as `/tree/main` works too), the SSH clone address
(`git@github.com:owner/repo.git`), or an HTTPS clone URL on another allowed host.

Nothing is cloned when you ask. Friday first checks the request the same way an
import checks it (feature enabled, `import_user_required`, host allowlist, name,
branch, duplicate), confirms the repository exists and is public, then reads it
back: *"Ready to add https://github.com/The-OASIS-Project/dawn (its default branch)
as the code project 'dawn'…"*. Your **yes in your next reply** starts the clone and
the code-graph index in the background. If you change the subject first, ask again.
Once the project is `ready`, ask Friday to make it active and ask about its code.

- The project gets the repo's name (`socket.io` becomes `socket-io`) unless you
  give one.
- The project tracks the repo's default branch, unless you name a branch or paste
  a URL under `/tree/<branch>`. DAWN looks the branch up among the repo's own
  branches, so `/tree/feature/x` tracks `feature/x` and `/tree/main/src` tracks
  `main`. A branch you name wins over the URL's, and the read-back always says
  which branch it will track.
- A project added this way belongs to you and is never shared with other users.
  Shared (global) projects and linked local checkouts stay in the WebUI and `dawn-admin`.
- With the default `import_user_required = "admin"`, only admins can add projects
  this way, the same rule as the WebUI.
- Only a live conversation can add a project. A background job or scheduled task
  can't, and a confirm that arrives by text from an unverified sender waits for
  your reply code.

### WebUI
Click the **Coding** icon in the header (visible to authenticated users when the
feature is enabled). The popover has:
- a segmented **Import URL / Link local** toggle (Link is admin-only),
- the project list with each repo's status, tracked branch, and a `local`/`global`
  tag, and per-project actions: **↻ refresh**, **♻ rebuild**, **⛖ set branch**
  (clone repos only), **× delete**.

Admins see every project (including operator/CLI imports); regular users see their
own + any shared (global) projects.

### dawn-admin (CLI / operator)
```
dawn-admin code-project list
dawn-admin code-project import <url> [--name n] [--branch b] [--global]
dawn-admin code-project link <path> [--name n]
dawn-admin code-project set-branch <name> <branch>
dawn-admin code-project refresh <name>
dawn-admin code-project rebuild <name>
dawn-admin code-project delete <name>
```
Note: `dawn-admin` imports/links run as the **operator** (no owner) — those show up
for **admin** WebUI users, not regular users.

### refresh vs rebuild (important)
- **refresh** = cheap. Clone repos: `git fetch` + check out the tracked branch +
  *incremental* re-index. Local repos: re-detect the current branch + re-index.
- **rebuild** = clean. Drops the cbm graph, then full re-index. **Use rebuild after
  upgrading the cbm binary** or any time you suspect a stale graph — incremental
  indexing keeps old nodes, rebuild guarantees a fresh graph.

### branches
- **Import** with `--branch`/the branch field to clone a specific branch.
- **set-branch** (clone repos) changes the tracked branch: it fetches, checks it
  out, and **rebuilds** (a branch switch changes the code). It's rejected for local
  repos — those track whatever you've checked out; `refresh` re-detects it.

### delete
Removes the cbm graph and the DB row. For a **clone** it also removes DAWN's clone
under `source_root`. For a **local** (linked) repo it **never touches your working
tree** — only the registration is removed.

---

## 4. Linked local checkouts: permissions & the cbm sandbox

Linking a local repo means **cbm reads its file contents**, and those contents reach
the LLM. So linking local checkouts is admin-only, gated to `allowed_local_roots`, and needs two
things set up:

1. **Config** — list the parent directories you'll link under. Set it in the WebUI
   (**Settings → Code Projects → Allowed local roots**, one path per line) or in
   `dawn.toml`:
   ```toml
   [code_projects]
   allowed_local_roots = ["/home/you/code"]
   ```
   Only repos resolving inside one of these can be linked. Keep secret-bearing trees
   out of it.

2. **cbm sandbox + read access.** `cbm-mcp` runs sandboxed (`ProtectHome=true`,
   which hides all of `/home`), so it can't read your code until the unit is granted
   a read-only bind per root plus a traverse ACL. The **cbm-mcp installer does this
   for you** — pass the roots (or answer its prompt):
   ```bash
   sudo services/cbm-mcp/install.sh --local-roots "/home/you/code"
   ```
   That writes a drop-in at `/etc/systemd/system/cbm-mcp.service.d/10-local-roots.conf`
   (`ProtectHome=tmpfs` + `BindReadOnlyPaths=…` per root), runs the traverse ACLs
   (`setfacl -u dawn:--x` on the ancestors), and reloads the service. Re-run with new
   roots to update it. Keep these in sync with `allowed_local_roots` above.

   *Manual fallback* (if you're not using the installer): `sudo systemctl edit cbm-mcp`,
   add `ProtectHome=tmpfs` + `BindReadOnlyPaths=/home/you/code:/home/you/code` under
   `[Service]`, then `sudo setfacl -m u:dawn:--x /home/you` (traverse only — can't
   list/read your home) and restart. (Single-developer alternative: run `cbm-mcp` as
   your own user instead of `dawn` — no ACL needed, weaker isolation; the bind step is
   still required.)

If the sandbox/permissions aren't right, a link is accepted but indexing errors —
check `/var/log/dawn/cbm-mcp.log`.

**Symlinks:** DAWN validates only the linked repo's *root* against `allowed_local_roots`,
but cbm's file discovery **skips symlinks** (`lstat` + `S_ISLNK`), so a symlink inside the
tree is never followed or indexed — its target's contents don't reach the LLM even if it
points outside the allowed root. The `BindReadOnlyPaths` sandbox is the second backstop
(an out-of-bind target isn't reachable at all). Still, keep secret material out of linked
trees.

> `allowed_local_roots` is editable from the WebUI Settings panel, and the cbm-mcp
> installer automates the sandbox grant (`--local-roots`). Wiring the grant fully into
> the top-level DAWN installer (so it's one step end-to-end) is a possible future polish.

---

## 5. Sharing the code graph with a coding assistant (Claude Code, etc.)

The same cbm server can back both Friday and a developer-side coding assistant, so
they share one graph store. Point the assistant's MCP client at the same SSE
endpoint (`http://localhost:9750/sse`). It's safe (per-call project scoping, no
cross-contamination); the only caveat is that a long index on one side briefly
blocks the other (single shared process). If that latency matters, run a second
`mcp-proxy` + cbm instance on another port pointed at the same `CBM_CACHE_DIR`
(separate process, WAL-safe). The read-access/sandbox requirements are the same as
§4 above. Full deep-dive (concurrency analysis, second-instance setup) lives in the
[atlas dawn archive](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/archive)
(`CODING_HARNESS_CBM_SHARING.md`).

---

## 6. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Project stuck `error: clone ready, but no code server connected` | cbm not running — `systemctl start cbm-mcp`, then `refresh`. |
| Link accepted but indexing fails | cbm can't read the path — section 4 (bind mount + traverse perms); check `cbm-mcp.log`. |
| Stale answers after a cbm upgrade | `rebuild` (not refresh). |
| Project not visible in the WebUI | operator/CLI imports show for admins only; regular users see their own + global. |
| Friday says adding is restricted to administrators | `import_user_required = "admin"` (the default): add it as an admin, or set it to `""`. |
| Friday can't find a repo you know exists | it's private. Only public repos can be added. |
| `set-branch` rejected | the project is `local` — branch tracks your checkout; just `git checkout` then `refresh`. |
| Row stuck `indexing` after a crash | it self-heals to `error: interrupted — rebuild to retry` on the next daemon start. |
