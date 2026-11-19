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

## Hardware

- A Reachy Mini **Lite** plugged into the DAWN machine's USB. One cable carries the motor controller, the
  microphone array/speaker (an XVF3800) and the camera (unused).
- The motors need their own power supply (6.8–7.6 V); USB alone powers only the electronics.
- Linux with PulseAudio (DAWN's `[audio] backend = "pulse"`). Tested on a Jetson AGX Orin (Ubuntu 22.04).

## Install

Requirements:
- `sudo apt install pulseaudio-utils libgirepository1.0-dev libcairo2-dev` (`parec`, and the headers the
  SDK's PyGObject dependency builds against: it has no aarch64 wheel)
- [`uv`](https://docs.astral.sh/uv/getting-started/installation/); the installer uses it for a CPython 3.12
  venv (the SDK needs Python 3.11+, never the system Python)
- MQTT credentials for the bridge on DAWN's broker

Run as the user DAWN runs as (it asks for `sudo` only to install the udev rule):

```bash
./install.sh
$EDITOR ~/.config/reachy-bridge/env          # REACHY_BRIDGE_MQTT_USER / _PASSWORD
systemctl --user start reachy-mini-daemon reachy-bridge
journalctl --user -u reachy-bridge -f
```

- The installer warns if you're not in `plugdev` (the mic array's USB control) and `dialout` (the motor
  controller); add yourself and log in again.
- To start both at boot without a login: `sudo loginctl enable-linger $USER`.
- If your user journal isn't kept, read the logs with `journalctl _SYSTEMD_USER_UNIT=reachy-bridge.service`.

Postures, motion amounts, moments and the expression map live in
`~/.config/reachy-bridge/reachy-bridge.toml`. Restart the bridge after an edit.

`install.sh` keeps an existing config. When the expression vocabulary changes
(DAWN's `EMOTE_FEELINGS`/`EMOTE_ACTS`), compare yours with the shipped
`reachy-bridge.toml` and copy the `[expressions]` table across; names DAWN sends
that your config lacks are logged once and do nothing.

## Using the robot's microphone and speaker

The robot's XVF3800 array makes a good local mic and speaker for DAWN: it cancels DAWN's own voice, so you
can interrupt a reply, and it gives the bridge the direction you spoke from.

- **Firmware:** update to Pollen's audio firmware 2.1.4 or later (USB id `38fb:1001`) with their
  [update script](https://github.com/pollen-robotics/reachy_mini/tree/main/src/reachy_mini/assets/firmware/update.sh). With Seeed's stock firmware (`2886:001a`, 2.1.0) echo cancellation didn't work in
  testing: every reply came back as input. 2.1.4 also fixes a mic that stops sending audio after a USB
  reset. Use the 2-channel build (`reachymini_ua_io16_lin_v2.1.4.bin`), not the 6-channel one: DAWN
  records the processed stereo output, the one echo cancellation is applied to.
- **Device names:** point DAWN at the robot's PulseAudio names:

  ```bash
  pactl list short sources | grep Reachy_Mini      # [audio] capture_device
  pactl list short sinks   | grep Reachy_Mini      # [audio] playback_device
  ```

- **Watch after a power cycle:** PulseAudio can switch the card's profile, which renames the input
  (`…iec958-stereo` vs `…analog-stereo`, the same microphone). DAWN then quietly records from the default
  mic instead. Check the name again after switching the robot off and on; `pactl set-card-profile`
  switches the profile back.
- **Echo cancellation listens to the left speaker channel.** A lopsided balance (a settings panel, a
  restored volume) lets every reply back in as input; the bridge keeps the channels equal.
- **Stalls:** the array can stop delivering sound (only zeros, no USB event). The bridge notices after
  60 s, while DAWN is idle, and restarts the array (at most twice per stall). The bridge's log says when.

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
