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
| `wake_word` | perks up, turns toward the speaker (direction of arrival) |
| `PROCESS_COMMAND` | thinking pose |
| `speaking_start` … `speaking_end` | speaking pose, head moves with the speech level |
| `expression` (`[emote:name]` in the reply) | a scaled-down clip from Pollen's emotions library |

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

## Notes

- **Daemon:** runs `--no-media`, because the Jetson's GStreamer is too old for its media path. It binds
  127.0.0.1 and has no authentication, so don't expose it.
- **Expression clips:** played motion-only (the library's sounds never play) and scaled down. The first
  start downloads the dataset from Hugging Face.
- **Accepted input:** DAWN's state, speaking events and vocabulary expression names. Nothing on MQTT can
  move the robot directly.
- **SDK internals:** the speech offsets use the SDK's own protocol model (`SetSpeechOffsetsCmd`), not
  its public API: check it on every `reachy-mini` upgrade.
