# Messaging Channels Setup (Telegram / Slack / Discord / SMS)

DAWN can talk to you through the chat apps you already use. Link a Telegram,
Slack, Discord, or SMS conversation to your DAWN account and you get the full
assistant — tools, memory, scheduler, the works — from your phone or desktop
chat client, without opening the WebUI.

Each linked channel is a **forever-conversation**: messages persist into one
conversation that shows up in the WebUI history alongside your voice and web
chats, and DAWN extracts memory from it like any other session. You can reset
the thread at any time with `/new`.

> **SMS here is the messaging-channels path** (link your number, then text
> Friday directly). It is **not** the same as the ECHO phone integration, which
> turns inbound calls/texts into HUD notifications. If you have ECHO set up,
> both can coexist — see [Notes](#notes-and-troubleshooting).

---

## How linking works (all providers)

1. Give DAWN the provider's bot/app token. Two ways:
   - **WebUI (recommended):** Settings → **Secrets** has fields for the Telegram,
     Discord, and Slack tokens. Paste the token and Save — the matching driver
     **starts immediately, no restart needed**.
   - **`secrets.toml`:** add the token by hand (see each provider below) and
     restart DAWN.

   The matching driver only loads when its token is present. *Adding* a token in
   the WebUI starts the driver live; **rotating or removing** a token is
   restart-to-apply (the driver's listener can't be torn down cleanly while
   running). SMS needs no token — see [SMS](#sms).
2. Generate a one-time **link code** for your user, either in the WebUI
   **Messaging Channels** settings panel or with the admin CLI:
   ```bash
   ./build/dawn-admin messaging generate-link-code --user <username> [--provider telegram|slack|discord|sms]
   ```
   Codes are 8-character Crockford base32 (no I/L/O/U) and expire in 10 minutes.
3. From the chat app, send `/link CODE` to the bot (Slack users: send
   `link CODE` without the slash — Slack reserves `/` for its own commands).
4. DAWN confirms the link. From then on, every message in that conversation
   reaches the assistant.

Once linked, just talk normally — no wake word needed (except SMS outside its
active window; see [SMS](#sms)). Send `/new` (Slack: ask the assistant to
"start a new conversation") to reset the forever-conversation and start fresh.

---

## Telegram

1. Open [@BotFather](https://t.me/BotFather), send `/newbot`, and follow the
   prompts. BotFather returns a token like `123456:ABC-DEF...`.
2. Give DAWN the token — paste it into Settings → Secrets → **Telegram Bot
   Token** and Save (driver starts immediately), or add it to `secrets.toml` and
   restart:
   ```toml
   [secrets]
   telegram_bot_token = "123456:ABC-DEF..."
   ```
3. Generate a link code and DM your bot `/link CODE`.

## Discord

Conversations are **DM-only, text-only** (you talk to the bot in a DM). The bot
can additionally **read and summarize server channels** on request — see
[Reading & summarizing Discord channels](#reading--summarizing-discord-channels).

1. Create an application + bot at
   [discord.com/developers/applications](https://discord.com/developers/applications).
2. Under **Bot** settings, enable the **MESSAGE CONTENT INTENT** toggle (a
   privileged intent — required to read DM text).
3. Give DAWN the bot token — Settings → Secrets → **Discord Bot Token** and Save
   (driver starts immediately), or add it to `secrets.toml` and restart:
   ```toml
   [secrets]
   discord_bot_token = "..."
   ```
4. Invite the bot so you can DM it, generate a code, and DM `/link CODE`.

## Slack

v1 is **DM-only, single-workspace**, over Socket Mode (no public URL needed).

1. Create an app at [api.slack.com/apps](https://api.slack.com/apps).
2. **Socket Mode** → enable. This generates an **App-Level Token** (`xapp-...`).
3. **OAuth & Permissions** → add bot scopes: `chat:write`, `im:history`,
   `im:read`, `app_mentions:read`. Install the app to your workspace and copy
   the **Bot User OAuth Token** (`xoxb-...`).
4. **Event Subscriptions** → enable, and subscribe to bot events: `message.im`,
   `app_mention`.
5. Give DAWN both tokens — Settings → Secrets → **Slack App Token** + **Slack Bot
   Token** and Save (driver starts immediately once *both* are set), or add them
   to `secrets.toml` and restart:
   ```toml
   [secrets]
   slack_app_token = "xapp-..."
   slack_bot_token = "xoxb-..."
   ```
6. Generate a code and DM the bot `link CODE` (no leading slash).

## SMS

SMS routes through the **ECHO** cellular daemon (SIM7600G-H modem). Set up ECHO
first (see the [echo repo](https://github.com/The-OASIS-Project/echo)); no token
goes in `secrets.toml` — DAWN registers the `sms` provider automatically when
the messaging engine is running.

1. Generate a link code for your user (with `--provider sms` if you like).
2. From the phone you want to link, text `/link CODE` to your DAWN/ECHO number.
3. After linking, you have an **active-conversation window**: for
   `active_window_sec` seconds after each exchange (default 600 = 10 min),
   replies route straight to the assistant with no wake word. Outside the
   window, an SMS needs the wake word ("Hey Friday, ...") or it falls through to
   the normal HUD-notification path. Tune in `dawn.toml`:
   ```toml
   [messaging.sms]
   active_window_sec = 600   # 0 disables (every SMS then needs a wake word); range 0-86400
   ```

---

## Delivering scheduled events to a channel

Scheduled events — timers, alarms, reminders, tasks, and briefings — can be
delivered to a messaging channel instead of (or in addition to being) spoken
aloud. Ask the assistant from within a channel ("schedule a morning briefing")
and it sets the event's delivery target to the current channel automatically.
When an event has a delivery target, that channel is the destination and the
local TTS/banner are suppressed for it.

---

## Reading & summarizing Discord channels

Friday can read recent messages from a Discord **server channel** and summarize
them — "catch me up on #general from today". This is **Discord-only** (Telegram
bots can't read channel history and SMS has no channels), and it is **read-only
and pull-based**: the bot reads a channel only when you (or a scheduled digest)
ask. It does not start watching server traffic.

### One-time setup — invite the bot to your server

Reading needs the bot to be a member of the server with permission to see the
channels:

1. In the [Developer Portal](https://discord.com/developers/applications) →
   your app → **OAuth2 → URL Generator**, select the **bot** scope and the
   **View Channels** + **Read Message History** permissions.
2. Open the generated URL and add the bot to your server.

The **Message Content Intent** you already enabled for DMs also covers reading
channel content over REST — no extra toggle.

### Using it

Just ask, from any chat with Friday (or the WebUI):

- "Catch me up on #general."
- "Summarize the dev-chat channel from this morning."
- "What did I miss in #announcements in the last 2 hours?"

Or summarize a **whole server** at once — every readable channel, each summarized:

- "Sum up everything on my server."
- "What's been happening across the server today?"

If the bot is in more than one server, name it ("…on My Server") or Friday will
ask which. A whole-server sweep is bounded — the most-recent messages per
channel, up to ~30 channels — and quiet channels are noted as having no recent
activity, so a busy server stays fast and a turn never runs away.

Friday matches the channel name against the channels the bot can see (fuzzy, so
"dev chat" finds `#dev-chat`). If the same name exists in more than one server,
she'll ask which server — you can also say it up front ("#general in My
Server").

**Time range.** You can bound how far back to read with a `since` (start) and an
optional `until` (end) — natural phrases ("today", "this morning", "last week",
"last month", "yesterday", "2 hours ago") or exact dates ("2026-06-01"):

- *"…since last week"* → from then up to now.
- *"…last month"* → roughly the last month up to now.
- *"…between June 1 and June 7"* → a closed range (use dates for precision;
  vague phrases like "until yesterday" land at about now).
- No range given → the most-recent messages (up to 300 per channel; the newest
  are kept if a channel is very busy).

> **Visibility note.** Friday can read **any** text/announcement channel the
> *bot* has been added to — which may include channels you personally aren't in.
> The only access control is which servers and channels you invite the bot to.
> Invite it only where you're comfortable having its contents summarized.

Reads are rate-limited per user and audited in the daemon log (who read which
channel, and how many messages) — never the message bodies, never the token.

### Scheduled digests

Because `read_channel` is a normal schedulable tool, you can ask for a recurring
digest delivered to a channel:

- "Every weekday at 8am, summarize #announcements and send it to my Discord DM."

The scheduler runs the read, the assistant summarizes, and the summary is
delivered to the channel you named (see [Delivering scheduled
events](#delivering-scheduled-events-to-a-channel)). Deliver digests to a **DM**
rather than back into a channel the digest itself reads, so tomorrow's digest
doesn't summarize today's. (Only the read-only actions — `read_channel`,
`read_server`, and `list_discord_channels` — may run from a schedule; `send` and
other actions require a live conversation.)

---

## Per-channel model & reasoning

Each channel's conversation carries its own LLM settings — the same
per-conversation mechanism the WebUI uses, stored on the channel's
forever-conversation. You set them in Settings → **Messaging Channels**, per
channel:

- **Reasoning** — *Default* (inherit the global setting, shown as e.g.
  "Default (Off)"), *Off*, or *On*. **New messaging conversations default to
  On.** Extended thinking makes the model reliably *use its tools* over chat —
  without it, smaller/faster models tend to *say* they did something ("Briefing
  scheduled") without actually emitting the tool call. The cost is a few extra
  seconds before the reply starts.
- **Effort** — reasoning token budget when Reasoning is on: *Default*, *Low*
  (the shipped default), *Medium*, *High*.
- **Model** — shown read-only (e.g. `Model: Default (claude-sonnet-4.6)`).
  Change it **from within the chat**: ask the assistant, e.g. "switch to Claude"
  or "use the local model". That change persists to the channel's conversation
  and survives restarts. A channel whose conversation resolves to the OpenRouter
  provider routes through OpenRouter with its stored/default model.

Changes apply to that channel's conversation; an in-progress session picks them
up on its next session (re)creation. "Default" everywhere means "inherit the
global LLM," so leaving them alone just follows your main configuration.

---

## Managing channels

**WebUI** — Settings → **Messaging Channels**: list your linked channels,
generate link codes, rename a channel's display name, unlink (soft-delete,
preserving history), re-enable a previously unlinked channel, and set each
channel's [Reasoning/Effort](#per-channel-model--reasoning). Each row shows a
status dot: **Active** (driver running), **Not connected** (linked, but the
provider's driver isn't loaded — add its token in Settings → Secrets), or
**Unlinked**.

**Operator CLI** — `dawn-admin messaging`:

| Command | Purpose |
|---|---|
| `generate-link-code --user <u> [--provider <p>]` | Issue a one-time link code |
| `list-channels --user <u>` | List a user's linked channels |
| `unlink --user <u> --name <display_name>` | Soft-delete a channel (history preserved) |
| `reenable --user <u> --name <display_name>` | Restore a previously unlinked channel |
| `link-attempts [--provider <p>]` | Review recent `/link` attempts (abuse audit) |

Unlinking sets `is_enabled = 0` but keeps the row and its conversation, so a
later re-link of the same address resumes the prior thread and custom name.

---

## Notes and troubleshooting

- **Driver not loading?** A driver registers only when its token is present.
  Adding the token via Settings → Secrets starts it immediately; if you edited
  `secrets.toml` by hand, restart DAWN. Check the daemon log for
  `registered driver '<provider>'`. A linked channel whose driver isn't running
  shows **Not connected** in the Messaging Channels panel — that means the token
  is missing (or you rotated/removed it and haven't restarted).
- **`/link` says invalid/expired?** Codes last 10 minutes and are single-use.
  Generate a fresh one. Review attempts with `dawn-admin messaging link-attempts`.
- **Messages from an unlinked sender are ignored** by design — the bot never
  replies to strangers.
- **SMS vs ECHO phone:** the messaging-channels SMS path (this doc) routes your
  texts to the LLM as a conversation. The ECHO phone integration
  (`PHONE_SMS_DESIGN`) surfaces inbound calls/texts as MIRAGE HUD notifications
  with contact photos. An inbound SMS from a linked number inside its active
  window goes to the assistant; otherwise it falls through to the HUD path.
- **Rate limits & length caps** are enforced per channel (inbound flood
  protection, and per-provider outbound length caps with natural-break
  splitting — e.g., SMS is capped and long replies are summarized with an offer
  to continue in the WebUI).

For the design rationale and internals, see the architecture notes in the
project's design docs.
