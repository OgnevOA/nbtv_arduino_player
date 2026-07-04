# Portable NBTV Player — Design & Implementation Plan

A portable system to play YouTube clips, GIFs, and video files on a 1920s-style
**32-line NBTVA mechanical (Nipkow-disc) television**, controlled from Telegram,
encoded on TrueNAS Scale, and rendered to the disc by an **M5 Atom Lite (ESP32)**
driving an **Atomic Audio-3.5 Base** (ES8311 codec → 3.5 mm TRRS jack).

This document is the authoritative plan. It supersedes prior chat discussion.

---

## 0. Locked decisions

| # | Decision | Rationale |
|---|---|---|
| 1 | **Radio architecture**: the server synthesizes the finished NBTV signal and streams it as audio | The `mtv.py` synthesis already produces a perfect signal on a PC; move it to the server and let the device be a thin player. Deletes all fragile on-device DSP. |
| 2 | **Lossless mono 16-bit PCM @ 48 kHz** transport | Compression (MP3/AAC/Opus) smears the sharp sync pulses (ringing/pre-echo) and breaks sync. Lossless preserves the waveform exactly. |
| 3 | **No audio** anywhere | The disc is silent picture only. The "audio" on the wire *is* the video signal. |
| 4 | **Device keeps live: speed, gain, invert**; server bakes: levels, sync, stabilize, headroom, low-pass | The disc's physical speed drifts every session, so speed/level must be trimmable on the device without a re-encode. |
| 5 | **HTTP long-poll** control + **single Docker image** built by **GitHub Actions** | Simplest firmware and ops; one artifact to deploy on TrueNAS. |

Standing facts (from calibration work in `mtv.py`):
- Standard: **NBTVA 32-line**, **12.5 fps**, vertical scan **bottom→top**, horizontal **right→left**, portrait **2:3**, **white-positive**, **blacker-than-black** line sync, **missing-pulse** frame sync.
- Signal levels: white `+28000`, black `0`, sync `-11200`; picture scaled by a **headroom** factor (~0.80) while sync keeps full depth.
- `stabilize` (per-frame mean equalization) and a ~10 kHz **low-pass over the whole composite** are required for stable lock on moving content.

---

## 1. Architecture

```
   ┌──────────────┐      Telegram       ┌───────────────────────────┐
   │   You (phone)│  ───────────────►   │     TrueNAS Scale (app)   │
   │  Telegram app│   URL / GIF / cmd   │     single Docker image   │
   └──────────────┘                     │  ┌─────────┐ ┌──────────┐ │
                                         │  │   Bot   │ │ Encoder  │ │
                                         │  │(aiogram)│→│ ffmpeg + │ │
                                         │  └────┬────┘ │ numpy    │ │
                                         │       │      │ synth →  │ │
                                         │       │      │ .pcm     │ │
                                         │  ┌────▼──────┴──────────┐ │
                                         │  │  Radio + Control svc  │ │
                                         │  │ /radio.wav + poll     │ │
                                         │  └───────────┬───────────┘ │
                                         └──────────────┼─────────────┘
                                                        │ WiFi (LAN)
                                        lossless PCM    │  + long-poll control
                                        (radio stream)  ▼
                                         ┌───────────────────────────┐
                                         │   M5 Atom Lite (ESP32)    │
                                         │  buffer → resample(speed) │
                                         │  → gain/invert → I²S      │
                                         └─────────────┬─────────────┘
                                            I²S → ES8311 → 3.5 mm TRRS
                                                        ▼
                                              Nipkow-disc TV (1920s)
```

**Division of labor**
- **Telegram** — remote control + content submission.
- **TrueNAS** — decode the messy real world into a **finished NBTV composite signal** (sync + levels + band-limit) and serve it as a continuous PCM radio stream.
- **ESP32** — a **thin radio client**: buffer PCM, resample for disc-lock (`speed`), apply `gain`/`invert`, push to the codec.

The server always streams something: the current program's PCM, or a **test-card**
loop when idle. Bonus: `/radio.wav` is directly playable in `ffplay`/VLC for debugging.

---

## 2. Core contract — the PCM radio stream

The single most important interface. The server emits the **finished** signal; the
device does no synthesis.

### Signal (baked server-side)
| Property | Value |
|---|---|
| Sample format | mono, signed 16-bit little-endian |
| Sample rate | 48000 Hz (nominal, = speed 1.0) |
| Frame timing | 32 lines × 120 samples = 3840 samples/frame → 12.5 fps |
| Levels | white `+28000 × headroom`, black `0`, sync `-11200` (full depth) |
| Line sync | 6 samples blacker-than-black at the end of each line |
| Frame sync | 32nd line's sync omitted (missing-pulse marker) |
| Conditioning | per-frame stabilize + ~10 kHz low-pass over the whole composite |

### What the device applies live (never baked)
- **speed** — resample ratio for disc-lock (consume `speed` input samples per output).
- **gain** — digital output level (sync-separator headroom on this amp).
- **invert** — polarity for the kit's sync sense.

### On-disk cache (`.pcm`)
Raw mono s16le, keyed by `source + encode-params`, for instant replay/loop without
re-downloading or re-encoding.

---

## 3. Wire format

`/radio.wav` = a standard 44-byte **WAV header** (PCM, 1ch, 48000, 16-bit; RIFF/data
sizes `0xFFFFFFFF` for an endless stream) followed by PCM samples forever. The device
discards the 44-byte header and streams the body into its ring buffer. Players like
`ffplay`/VLC accept the same URL directly.

The body is delivered with HTTP chunked transfer-encoding; the device de-chunks it.
There are no per-frame headers — frame/line/sync structure is entirely *in* the PCM.

---

## 4. Transport & endpoints

| Endpoint | Method | Purpose |
|---|---|---|
| `/radio.wav` | GET (chunked) | endless PCM: current program looped, or test card when idle |
| `/control/poll?since=<seq>` | GET (long-poll) | block until a device command exists, then return it |
| `/status` | POST | device pushes status (~1 Hz) |
| `/health` | GET | liveness |

### Flow control
No rate field. The device drains its buffer at its real playback rate; **TCP
backpressure** paces the server, which streams ahead into the device's ~0.5 s ring
buffer. Program changes bump a token so the streamer switches source seamlessly
without a reconnect.

### Control commands (long-poll response, JSON)
Server → device (device-local knobs only):
- `speed` — `value` (e.g. `1.005`)
- `invert` — toggle polarity
- `reboot`

Program changes (`play`/`stop`/`skip`/`loop`) are **server-side** — they change the
bytes on `/radio.wav`, so the device needs no command for them.

Device → server (`/status`, ~1 Hz): `state` (`playing|buffering|nolink`),
`buffer_ms`, `underruns`, `rssi`, `speed`.

### Security (single user)
Every Telegram update is checked against one allow-listed `user_id`. Device endpoints
are LAN-only + a shared static token header (`X-Device-Token`).

---

## 5. Server (TrueNAS Scale) — single image

One Docker image, launched together.

### 5.1 Bot (Telegram, aiogram) — `bot.py`
| Input | Action |
|---|---|
| YouTube URL / GIF / video | encode → `.pcm` → set as current program (loop) |
| `/stop` | go idle (test card) |
| `/skip` | play the queued item |
| `/loop on\|off` | loop the current item |
| `/speed 0.95` | device speed command (persists to NVS on device) |
| `/invert` | device polarity toggle |
| `/status` | echo program + device status |
| `/test` | go idle (streams the test card) |

Program model: one **now-playing** + a 1-deep **next** slot (newest wins).

### 5.2 Encoder + synth — `encoder.py` + `render.py`
1. `yt-dlp` download (cap 360p).
2. `ffmpeg`: crop to 2:3 → scale to **32 × 114** grayscale → 12.5 fps, no audio.
3. `render.frames_to_pcm` (port of `mtv.py` `frames_to_signal` + `lowpass_fir`):
   stabilize → level map with **headroom** → line/frame sync → **10 kHz low-pass** →
   clamp to mono s16le.
4. Cache as `.pcm` keyed by `source + params`.

`render.py` also builds the idle **test-card** PCM once at startup.

### 5.3 Radio + control service — `stream.py`
- `/radio.wav`: writes the WAV header, then loops the current program's `.pcm`
  (or the test card when idle), switching source on the program token. TCP
  backpressure paces it.
- Holds the long-poll command queue; relays bot device-commands.
- Collects device `/status`; derives online/offline from heartbeat staleness.

---

## 6. ESP32 firmware

### 6.1 Loop (no state machine)
The device always does the same thing: pull PCM from the ring, resample, output.

```
   BOOT ─ load NVS (wifi, server, token, speed, gain, invert)
     │
     ├─ (no creds / button held) → SoftAP provisioning portal
     ▼
   audio_begin() @ 48 kHz ─ start net tasks (radio + poll + status)
     ▼
   loop(): drain commands / serial / button → emit_block()
                                              (resample ring → gain/invert → I²S)
```

Two background tasks on core 0: the **radio client** (fills the ring) and the
**poll/status** task. The output loop runs on core 1.

### 6.2 Radio client — `net.cpp`
Connect `/radio.wav`, de-chunk, discard the 44-byte WAV header, then read PCM into
the **sample ring buffer** with backpressure (`sb_push` blocks when full).
Auto-reconnects on drop. `poll_task` handles `speed`/`invert`/`reboot` + posts status.

### 6.3 Output — `nbtv_player.ino` + `sample_buffer.*`
- `sample_buffer`: thread-safe int16 ring (~0.5 s = 24000 samples).
- `emit_block()`: a continuous linear resampler. Playback runs at a fixed 48 kHz;
  it consumes `speed` input samples per output sample (fractional index carried
  across blocks), then applies `invert` and `gain`, mono→stereo, `audio_write`.
  On underrun it holds the last sample and counts it; the I²S never stops.

### 6.4 Clock & speed
The ES8311 runs fixed at 48 kHz (its clock table has no `48000×speed` entry, and
M5's I²S is stereo). Speed is done purely by the resampler ratio — no clock change,
no re-encode. Device-side equivalent of `--tune`.

### 6.5 Button (Atom Lite top button)
| Gesture | Action |
|---|---|
| single | print current tune line |
| double | toggle invert |
| long-press | cycle speed presets around 0.95 (fine-tune lock) |
| hold at boot | enter WiFi provisioning (SoftAP captive page) |

### 6.6 Persistent config (NVS) — `settings.*`
`wifi_ssid`, `wifi_pass`, `server_url`, `token`, `speed`, `invert`, `gain`.
Optional compile-time `secrets.h` overrides connection fields at boot.

### 6.7 Hardware notes (Atomic Audio-3.5 Base)
- ES8311 mono codec over **I²S** (data) + **I²C** (control), NS4150B Class-D amp.
- Output is hot for a speaker, so **gain ≈ 0.05** keeps the signal in the amp's linear
  region (higher clips the sync tips → lock loss).
- TRRS wiring is **CTIA**; mono is mirrored to L+R so any tip/ring tap works.
- AC-coupled output → hence server-side `stabilize` (floating black level).

---

## 7. Failure-handling matrix

| Event | Behavior | Sync impact |
|---|---|---|
| WiFi drop | radio client reconnects; ring drains | brief (until buffer empties), then silence/roll |
| Server down | retry backoff | as above |
| Buffer underrun | hold last sample, count it, resume | brief |
| Program change | seamless source switch (token) | none |
| Wrong Telegram user | dropped at allow-list | n/a |
| Power loss | reboot → speed/gain/invert from NVS | re-locks on its own |

The main robustness lever is the ring buffer size vs. WiFi quality: lossless PCM is
~96 KB/s, so a weak link is the primary underrun risk.

---

## 8. Bandwidth & buffer budget

| Item | Value |
|---|---|
| Stream format | mono s16le @ 48 kHz |
| Stream bitrate | ~**768 kbps** (~96 KB/s) |
| Device ring buffer | ~0.5 s ≈ 48 KB (24000 × int16) |
| Trade-off | lossless (sync-safe) over compressed (sync-breaking) |

Comfortable for ESP32 RAM; needs a decent LAN WiFi link.

---

## 9. Repository layout

```
NBTV/
├─ PORTABLE-NBTV-PLAN.md          # this file
├─ server/
│  ├─ app/
│  │  ├─ bot.py                   # Telegram (aiogram), allow-list
│  │  ├─ encoder.py               # download + ffmpeg grey frames -> .pcm
│  │  ├─ render.py                # frames_to_pcm (synth + lowpass + test card)
│  │  ├─ stream.py                # /radio.wav + /control/poll + /status
│  │  ├─ control.py               # program model, token, device commands
│  │  ├─ nbtv.py                  # constants + WAV/PCM helpers
│  │  ├─ config.py                # env config
│  │  └─ server.py / __main__.py  # aiohttp + bot in one loop
│  ├─ docker-compose.yml          # TrueNAS Custom App (inline env)
│  ├─ pyproject.toml / requirements.txt
│  └─ Dockerfile                  # single image (bot+encoder+radio)
├─ firmware/                      # ESP32, Arduino IDE sketch
│  └─ nbtv_player/                # open this folder as the sketch
│     ├─ nbtv_player.ino          # setup/loop + resampler
│     ├─ audio_out.cpp/.h         # ES8311 via M5EchoBase, mono->stereo I²S
│     ├─ net.cpp/.h               # /radio.wav client + long-poll + status
│     ├─ sample_buffer.cpp/.h     # int16 ring buffer, underrun handling
│     ├─ button.cpp/.h            # gestures
│     ├─ settings.cpp/.h          # NVS, SoftAP provisioning, secrets.h
│     └─ nbtv_config.h            # NBTV timing + Atom pin map
└─ .github/workflows/
   └─ server-image.yml            # build+push single Docker image (GHCR)
```

Firmware is built/flashed locally from the Arduino IDE (no CI); only the server
image is built by GitHub Actions.

(Local-only `nbtv-tools-and-doc-V1-4/` and `Disk/` remain as reference/calibration,
gitignored.)

---

## 10. GitHub Actions (single-image assembly)

**`server-image.yml`** — push to `main` touching `server/**` (and tags `v*`):
checkout → Buildx → GHCR login → build → push `ghcr.io/<user>/nbtv-server:{sha,latest,tag}`.
TrueNAS pulls it (Custom App).

**Firmware** — no CI. Built and flashed locally from the Arduino IDE: open
`firmware/nbtv_player/`, board "M5Stack-ATOM", libraries M5Atomic-EchoBase /
M5Unified / ArduinoJson.

Secrets: GHCR uses the built-in `GITHUB_TOKEN`. `BOT_TOKEN`, `ALLOWED_USER_ID`,
`DEVICE_TOKEN` are **runtime** env on TrueNAS, never baked into the image.

---

## 11. Validation

1. **Server**: `ffplay http://<host>:32125/radio.wav` plays the test-card signal
   (proves synth + stream end-to-end).
2. **Device**: lock the test card, then a video; re-trim `speed` live. Busy scenes
   hold because headroom + low-pass are baked server-side.
3. **Resilience**: drop WiFi briefly to confirm buffer ride-through / graceful underrun.

---

## 12. Deferred to v2 (out of scope for v1)

- Lossless-compressed transport (FLAC) for weak WiFi.
- WebSocket control (replacing long-poll).
- Multi-user / queue.
- Device-side OTA firmware updates.
```
