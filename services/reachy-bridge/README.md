# reachy-bridge

Gives DAWN's **local voice interface** a body: a Reachy Mini **Lite** on the
Jetson's USB. The bridge reads DAWN's OCP events over MQTT and drives the robot
through Pollen's SDK. DAWN never talks to the robot and starts neither process.

```
dawn ──MQTT dawn/events──► reachy-bridge ──ws 127.0.0.1:8000──► reachy-mini-daemon ──USB──► robot
     ◄──MQTT reachy/status──┘    ▲ PulseAudio monitor (speech), XVF3800 DoA (USB control)
```

## What it does

| DAWN | Robot |
|---|---|
| `dawn/status` offline / online | asleep / awake |
| 15 minutes with no turn and nothing said | asleep; the wake word, DAWN speaking or a typed command wakes it (speech near the mic alone doesn't) |
| `wake_word` | perks up, turns toward the speaker (direction of arrival) |
| `PROCESS_COMMAND` | thinking pose |
| `speaking_start` … `speaking_end` | speaking pose, head moves with the speech level |
| `expression` (`[emote:name]` in the reply) | a scaled-down clip from Pollen's emotions library, picked at random from the name's list (never the same twice running) |

While the bridge reports `reachy/status` online, DAWN tells the model on the
local surface that it can use `[emote:name]` tags. No `dawn.toml` setting is
involved.

## Install

Requirements:
- the Lite plugged into the Jetson
- `uv`
- `parec` (`pulseaudio-utils`)
- DAWN's `[audio] backend = "pulse"`

Run as the user DAWN runs as:

```bash
./install.sh
$EDITOR ~/.config/reachy-bridge/env          # MQTT user/password for the bridge
systemctl --user start reachy-mini-daemon reachy-bridge
journalctl --user -u reachy-bridge -f
```

Postures, motion amounts and the expression map live in
`~/.config/reachy-bridge/reachy-bridge.toml`. Restart the bridge after an edit.

`install.sh` keeps an existing config. When the expression vocabulary changes
(DAWN's `EMOTE_FEELINGS`/`EMOTE_ACTS`), compare yours with the shipped
`reachy-bridge.toml` and copy the `[expressions]` table across; names DAWN sends
that your config lacks are logged once and do nothing.

## Notes

- **Daemon:** runs `--no-media`, because the Jetson's GStreamer is too old for its media path. It binds
  127.0.0.1 and has no authentication, so don't expose it. It also trusts every local user: on a machine
  shared with other accounts, any of them can move the robot, or install and run a robot app as the user
  it runs as.
- **Expression clips:** played motion-only (the library's sounds never play) and scaled down. The first
  start downloads the dataset from Hugging Face.
- **Accepted input:** DAWN's state, speaking events and vocabulary expression names. Nothing on MQTT can
  move the robot directly.
- **Mic and speaker health:** the robot's XVF3800 mic array can stall, sending only exact zeros with no
  USB event, so DAWN hears nothing. After `stalled_after_s` of that (a mute or 0% in PulseAudio doesn't
  count) the bridge reboots the array while DAWN is idle, at most twice per stall; PulseAudio puts
  DAWN's streams back on its own. It also keeps the speaker's channels equal: the array takes its echo
  cancellation reference from the left channel, and a low left channel lets every reply back in as
  input. Both find the device by `device_match` in its PulseAudio name.
- **SDK internals:** the speech offsets use the SDK's own protocol model (`SetSpeechOffsetsCmd`), not
  its public API: check it on every `reachy-mini` upgrade.
