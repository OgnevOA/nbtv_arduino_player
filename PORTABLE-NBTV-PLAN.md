# Portable NBTV Player — Design & Implementation Plan

A portable system to play YouTube clips, GIFs, and video files on a 1920s-style
**32-line NBTVA mechanical (Nipkow-disc) television**, controlled from Telegram,
encoded on TrueNAS Scale, and rendered to the disc by an **M5 Atom Lite (ESP32)**
driving an **Atomic Audio-3.5 Base** (ES8311 DAC → 3.5 mm jack).

This document is the authoritative plan. It supersedes prior chat discussion.

---

## 0. Locked decisions

| # | Decision | Rationale |
|---|---|---|
| 1 | **8-bit grayscale** pixel transport | Simple; ~3 bits of headroom over NBTV's real luma depth. 4-bit is a future flag. |
| 2 | **No audio** anywhere | The disc is silent picture only. Encoder drops all audio paths. |
| 3 | **HTTP long-poll** control for v1 | Simplest firmware; no broker. WebSocket is a v2 upgrade. |
| 4 | **Single Docker image**, built by **GitHub Actions** | Easiest to operate at home; one artifact to deploy on TrueNAS. |

Additional standing facts (from calibration work in `mtv.py`):
- Standard: **NBTVA 32-line**, **12.5 fps**, vertical scan **bottom→top**, horizontal **right→left**, portrait **2:3**, **white-positive**, **blacker-than-black** line sync, **missing-pulse** frame sync.
- Base playback speed that locks the user's disc: **0.95×** (45600 Hz I²S when nominal is 48000 Hz).
- `--stabilize` (per-frame mean equalization) and a ~10 kHz band-limit are required for stable lock on moving content.

---

## 1. Architecture

```
   ┌──────────────┐      Telegram       ┌───────────────────────────┐
   │   You (phone)│  ───────────────►   │     TrueNAS Scale (app)   │
   │  Telegram app│   URL / GIF / cmd   │     single Docker image   │
   └──────────────┘                     │  ┌─────────┐ ┌──────────┐ │
                                         │  │   Bot   │ │ Encoder  │ │
                                         │  │(aiogram)│→│ ffmpeg/  │ │
                                         │  └────┬────┘ │ numpy    │ │
                                         │       │      └────┬─────┘ │
                                         │  ┌────▼───────────▼─────┐ │
                                         │  │  Stream + Control svc │ │
                                         │  │ HTTP chunked + poll   │ │
                                         │  └───────────┬───────────┘ │
                                         └──────────────┼─────────────┘
                                                        │ WiFi (LAN)
                                          pixel frames  │  + long-poll control
                                                        ▼
                                         ┌───────────────────────────┐
                                         │   M5 Atom Lite (ESP32)    │
                                         │  fetch → render → I²S     │
                                         │  NBTV signal generator    │
                                         │  (sync + levels + clock)  │
                                         └─────────────┬─────────────┘
                                              I²S → ES8311 DAC → 3.5mm
                                                        ▼
                                              Nipkow-disc TV (1920s)
```

**Division of labor**
- **Telegram** — remote control + content submission.
- **TrueNAS** — decode the messy real world into clean **pixel frames** (no sync, no audio).
- **ESP32** — the **NBTV signal generator** + disc clock. Owns sync, levels, speed.

Guiding invariant: **sync is generated on the device and never travels over the network.**
Network problems degrade the *picture* (freeze), never the *sync* (disc stays locked).

---

## 2. Core contract — the pixel frame

The single most important interface.

### Geometry
| Property | Value | Notes |
|---|---|---|
| Lines (columns) | 32 | NBTVA 32-line |
| Rows per line (transmitted) | 48 | natural 2:3 resolution; device interpolates → 114 active samples |
| Bit depth | 8-bit gray | `0x00` = black, `0xFF` = white (white-positive) |
| Frame rate | 12.5 fps | implicit; device clocks it |
| Frame payload | 32 × 48 = **1536 bytes** | pre-arranged in scan order |

### Scan order (server pre-arranges; device reads linearly)
- Outer loop = **line** `l = 0..31` → physical disc column, **right→left**.
- Inner loop = **sample** `s = 0..47`, **bottom→top**.
- All flip/mirror/invert decisions are baked **server-side** (matching `--flip-h/--flip-v/--invert-signal`).

The device performs **zero geometry math** — it reads 1536 bytes already in display order.

### What the device adds (never transmitted)
- Line sync (blacker-than-black), 6 samples/line.
- Frame sync (missing-pulse on the frame's first line).
- 48→114 interpolation and level mapping to int16.

---

## 3. Wire format

A continuous stream of frames with small headers so the device can re-lock byte
alignment after any corruption.

**Stream header (once, on connect):**
| Field | Bytes | Value |
|---|---|---|
| Magic | 4 | `"NBTV"` |
| Version | 1 | `0x01` |
| Lines | 1 | `32` |
| Rows | 1 | `48` |
| Flags | 1 | bit0 = 4-bit packed (reserved, v2) |

**Per-frame header (before each 1536-byte payload):**
| Field | Bytes | Meaning |
|---|---|---|
| Sync word | 2 | `0xA5 0x5A` (byte-realignment anchor) |
| Frame seq | 2 | wraps; device uses for drop detection |
| Payload len | 2 | `1536` |

On corruption the device scans forward for `0xA5 0x5A`, re-locks frame boundaries,
and freezes the picture meanwhile. Sync output continues throughout.

**On-disk cache format (`.nbtvf`):** stream header followed by N framed payloads.
Used for instant replay/loop without re-downloading.

---

## 4. Transport & endpoints

| Endpoint | Method | Purpose |
|---|---|---|
| `/stream?id=<job>` | GET (chunked) | the pixel-frame stream |
| `/control/poll?since=<seq>` | GET (long-poll) | block until a command exists, then return it |
| `/status` | POST | device pushes status (~1 Hz) |
| `/health` | GET | liveness |
| `/jobs` (bot-internal) | — | enqueue / replace current job |

### Flow control
No rate field. The device drains its buffer at exactly 12.5 fps; **TCP backpressure**
paces the server, which streams ahead into the device's ~0.5 s ring buffer.

### Control commands (long-poll response, JSON)
Server → device:
- `play` — `id`, `loop` (bool)
- `stop`
- `freeze`
- `speed` — `value` (e.g. `0.95`)
- `reboot`

Device → server (`/status`, ~1 Hz):
- `state` — `idle|playing|frozen|buffering`
- `id`, `frame_seq`, `buffer_ms`, `underruns`, `rssi`, `speed`

### Security (single user)
Every Telegram update is checked against one allow-listed `user_id`; everything else
is dropped silently. Device endpoints are LAN-only + a shared static token header.

---

## 5. Server (TrueNAS Scale) — single image

One Docker image, three logical parts, launched together.

### 5.1 Bot (Telegram, aiogram)
| Input | Action |
|---|---|
| YouTube URL | enqueue encode → autoplay |
| GIF / video file | enqueue encode → autoplay |
| `/stop` `/skip` `/loop` | control passthrough |
| `/speed 0.95` | set device speed (persists to NVS via device) |
| `/status` | echo device status |
| `/test` | command device to render its built-in test card |

Job model: one **now-playing** + a 1-deep **next** slot (newest submission wins).
No multi-user queue.

### 5.2 Encoder (server-side `mtv.py` pipeline, retargeted)
Reuses the existing pipeline but outputs **pixel frames**, not a WAV waveform, and
**drops audio entirely**:
1. `yt-dlp` download (cap 360p).
2. `ffmpeg`: crop to 2:3 → scale to **32×48** → grayscale → 12.5 fps, **no audio**.
3. numpy: **contrast/brightness/gamma**, then **stabilize** (per-frame mean equalization) in the **pixel domain**.
4. Arrange bytes in **scan order** (§2).
5. Emit to the stream service as a live pipe and/or a cached `.nbtvf`.

**Removed from the server:** `frames_to_signal` (sync insertion, level mapping,
lowpass, WAV writing) — these move to the device.

**Caching:** store rendered `.nbtvf` keyed by `url + encode-params` for instant
replay/loop.

### 5.3 Stream + control service (aiohttp/FastAPI)
- Serves `/stream` from a live encode or cached `.nbtvf`.
- Holds long-poll command queue per device; relays bot commands.
- Collects device `/status`; derives online/offline from heartbeat staleness.

---

## 6. ESP32 firmware

### 6.1 State machine
```
        ┌─────────┐
        │  BOOT   │  load NVS (wifi, server, token, speed)
        └────┬────┘
             ▼
        ┌─────────┐   fail   ┌──────────────┐
        │  WIFI   │ ───────► │  TEST CARD    │ (offline: local card, sync live)
        └────┬────┘          └──────┬───────┘
        ok   ▼                      │ wifi back
        ┌─────────┐                 │
        │ CONNECT │ ◄───────────────┘  start long-poll loop, POST /status
        └────┬────┘
             ▼
        ┌─────────┐  no job
        │  IDLE   │ ───────► render TEST CARD (sync stays live)
        └────┬────┘
       play  ▼
        ┌──────────────┐  prefill ring buffer (~0.5s)
        │  BUFFERING   │
        └────┬─────────┘
             ▼
        ┌──────────────┐  underrun → FREEZE last frame, keep sync, refill, resume.
        │   PLAYING    │  socket drop → CONNECT.  stop → IDLE.  freeze → FROZEN.
        └──────────────┘
```

**Invariant:** the I²S engine *always* emits a valid NBTV waveform — live frame,
frozen frame, or test card. The disc never loses lock.

### 6.2 Render path (the `frames_to_signal` port)
Per frame, build **3840 samples** (32 lines × 120 samples; 32 × 120 × 12.5 = 48000):
- Per line: 6 sync samples (frame's first line = missing-pulse pattern), then
  interpolate the 48 transmitted rows → **114 active samples**, map gray→level.
- Levels (device constants, from calibration): white ≈ `+28000`, black `0`,
  sync ≈ `-11200`, peak-normalized to ~`30000`.
- Optional cheap 1-pole smooth on the active region (stands in for `--lowpass`); off by default.

Cost: microseconds of ESP32 work per frame.

### 6.3 Clock & speed
- I²S sample rate = `48000 × speed` (e.g. `0.95` → `45600`).
- `speed` command or button changes the I²S clock divider **live** — no re-encode,
  no stream change. Device-side equivalent of `--tune`.

### 6.4 Button (Atom Lite top button)
| Gesture | Action |
|---|---|
| single | stop / start (toggle) |
| double | skip to "next" slot |
| long-press | cycle speed presets around 0.95 (fine-tune lock) |
| hold at boot | enter WiFi provisioning (SoftAP captive page) |

### 6.5 Persistent config (NVS)
`wifi_ssid`, `wifi_pass`, `server_url`, `token`, `speed`, `user_label`.
Provisioned via SoftAP captive page on first boot or boot-hold.

### 6.6 Hardware notes (Atomic Audio-3.5 Base)
- ES8311 mono codec over **I²S** (data) + **I²C** (control); 3.5 mm out is the clean
  **DAC (DACP)** path, not the Class-D amp — suitable for NBTV.
- TRRS wiring is **CTIA**; the disc connects to the DAC output line.
- Output is AC-coupled → hence the need for `--stabilize` (floating black level).

---

## 7. Failure-handling matrix

| Event | Behavior | Sync impact |
|---|---|---|
| WiFi drop | → CONNECT, render test card | none (local sync) |
| Server down | retry backoff, test card | none |
| Buffer underrun | freeze last frame, refill | none |
| Corrupt bytes | hunt `0xA5 0x5A`, re-lock | none (brief picture freeze) |
| Wrong Telegram user | dropped at allow-list | n/a |
| Power loss | reboot → last speed from NVS | re-locks on its own |

---

## 8. Bandwidth & buffer budget

| Item | Value |
|---|---|
| Frame payload | 1536 B (8-bit) |
| Frame rate | 12.5 fps |
| Stream bitrate | ~**154 kbps** |
| Device ring buffer | ~0.5 s ≈ 10 KB pixels + ~96 KB int16 I²S scratch |
| vs. full-PCM transport | 768 kbps → ~5× reduction |

Comfortable for ESP32 RAM and any LAN WiFi.

---

## 9. Repository layout (proposed)

```
NBTV/
├─ PORTABLE-NBTV-PLAN.md          # this file
├─ server/
│  ├─ app/
│  │  ├─ bot.py                   # Telegram (aiogram), allow-list
│  │  ├─ encoder.py               # mtv.py pipeline → pixel frames (no audio)
│  │  ├─ stream.py                # /stream chunked, .nbtvf cache
│  │  ├─ control.py               # /control/poll, /status, job model
│  │  └─ nbtvf.py                 # wire/cache format read+write
│  ├─ pyproject.toml
│  └─ Dockerfile                  # single image (bot+encoder+stream)
├─ firmware/                      # ESP32, Arduino IDE sketch
│  └─ nbtv_player/                # open this folder as the sketch
│     ├─ nbtv_player.ino          # setup/loop state machine
│     ├─ nbtv_render.cpp/.h       # frames_to_signal port (sync+levels+interp)
│     ├─ audio_out.cpp/.h         # ES8311 via M5EchoBase, clock/speed
│     ├─ net.cpp/.h               # stream fetch + long-poll + status
│     ├─ frame_buffer.cpp/.h      # ring buffer, underrun/freeze
│     ├─ button.cpp/.h            # gestures
│     ├─ settings.cpp/.h          # NVS, SoftAP provisioning
│     └─ nbtv_config.h            # NBTV constants + Atom pin map
└─ .github/workflows/
   ├─ server-image.yml            # build+push single Docker image (GHCR)
   └─ firmware.yml                # build firmware, attach .bin to release
```

(Existing `nbtv-tools-and-doc-V1-4/` and `Disk/` remain as reference/calibration.)

---

## 10. GitHub Actions (single-image assembly)

**`server-image.yml`**
- Trigger: push to `main` touching `server/**`, and tags `v*`.
- Steps: checkout → set up Buildx → log in to **GHCR** → `docker build` the single
  image → push `ghcr.io/<user>/nbtv-server:{sha, latest, tag}`.
- TrueNAS pulls this image (Custom App) and runs it.

**`firmware.yml`**
- Trigger: push touching `firmware/**`, and tags `v*`.
- Steps: checkout → `arduino-cli` (ESP32 core + M5Atomic-EchoBase / M5Unified /
  ArduinoJson) → `arduino-cli compile --fqbn esp32:esp32:m5stack-atom` → upload
  the `.bin` artifact; on tag, attach to a GitHub Release for flashing.
- Local dev is the Arduino IDE: open `firmware/nbtv_player/`, board "M5Stack-ATOM".

Secrets: `GHCR` uses the built-in `GITHUB_TOKEN`. Telegram `BOT_TOKEN` and
`ALLOWED_USER_ID` are **runtime** env vars on TrueNAS, never baked into the image.

---

## 11. Build phases (execution order)

1. **Firmware signal core, offline** — port `frames_to_signal`, render the built-in
   **test card** to I²S, lock the disc at 0.95. *(Hardest part, zero networking.)*
2. **Static frame over HTTP** — fetch one `.nbtvf`, loop it. Validates wire format +
   interpolation.
3. **Live stream + buffering** — `/stream` chunked, ring buffer, underrun freeze.
4. **Control** — long-poll commands + `/status`; button gestures.
5. **Server bot + encoder** — retarget `mtv.py` to pixel output (no audio), wire into
   stream service; Telegram front end + allow-list.
6. **Provisioning + NVS polish**, then GitHub Actions for both artifacts.

Phase 1 is the real risk; everything after is plumbing.

---

## 12. Deferred to v2 (explicitly out of scope for v1)

- WebSocket control (replacing long-poll).
- 4-bit packed transport (`flags` bit0).
- Multi-user / queue.
- Any audio path.
- Device-side OTA firmware updates.
