"""Encoder: turn a video/URL/GIF into a .nbtvf pixel-frame file.

This is the server-side descendant of mtv.py's pipeline, retargeted from
"synthesise the WAV waveform" to "emit pixel frames". All sync insertion,
level mapping and the 48->114 interpolation now live on the device, so this
stage stops at clean grayscale pixels arranged in NBTVA scan order.

Audio is dropped entirely (the mechanical disc is silent picture only).
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import nbtv


class EncodeError(RuntimeError):
    pass


@dataclass
class EncodeOptions:
    fit: str = "cover"              # "cover" (crop to 2:3) or "contain" (pad)
    flip_h: bool = False
    flip_v: bool = False
    stabilize: bool = True          # hold mean brightness per frame (AC-couple)
    contrast: float = 1.0
    brightness: float = 0.0
    gamma: float = 1.0
    start: str | None = None
    duration: str | None = None
    max_height: int = 360

    def cache_key(self, source: str) -> str:
        h = hashlib.sha1()
        h.update(source.encode())
        for v in (self.fit, self.flip_h, self.flip_v, self.stabilize,
                  self.contrast, self.brightness, self.gamma,
                  self.start, self.duration, self.max_height):
            h.update(repr(v).encode())
        return h.hexdigest()[:16]


def _ffmpeg() -> str:
    return shutil.which("ffmpeg") or _fail("ffmpeg not found on PATH")


def _fail(msg: str):
    raise EncodeError(msg)


def is_url(s: str) -> bool:
    return s.startswith("http://") or s.startswith("https://")


def download(url: str, workdir: Path, max_height: int) -> Path:
    """Download a URL with yt-dlp, capped at max_height. Returns the file."""
    ytdlp = shutil.which("yt-dlp") or _fail("yt-dlp not found on PATH")
    out_tmpl = str(workdir / "source.%(ext)s")
    fmt = f"bv*[height<={max_height}]+ba/b[height<={max_height}]/b"
    subprocess.run(
        [ytdlp, "-f", fmt, "--merge-output-format", "mp4",
         "--no-playlist", "-o", out_tmpl, url],
        check=True,
    )
    candidates = sorted(workdir.glob("source.*"))
    if not candidates:
        _fail("download produced no file")
    return candidates[0]


def _grey_frames(src: Path, opt: EncodeOptions) -> np.ndarray:
    """Decode source to a (n, ROWS_TX, COLS) uint8 grey stack @ 12.5 fps."""
    if opt.fit == "contain":
        geom = (f"scale=w={nbtv.COLS}:h={nbtv.ROWS_TX}:"
                f"force_original_aspect_ratio=decrease,"
                f"pad={nbtv.COLS}:{nbtv.ROWS_TX}:(ow-iw)/2:(oh-ih)/2:color=black")
    else:  # cover: crop to portrait 2:3 then scale
        geom = ("crop='min(iw,ih*2/3)':'min(ih,iw*3/2)',"
                f"scale={nbtv.COLS}:{nbtv.ROWS_TX}")
    vf = f"fps={nbtv.BASE_FPS},{geom}"
    if opt.contrast != 1.0 or opt.brightness != 0.0 or opt.gamma != 1.0:
        vf += (f",eq=contrast={opt.contrast}:brightness={opt.brightness}"
               f":gamma={opt.gamma}")
    vf += ",format=gray"

    cmd = [_ffmpeg(), "-v", "error", "-y"]
    if opt.start:
        cmd += ["-ss", str(opt.start)]
    cmd += ["-i", str(src)]
    if opt.duration:
        cmd += ["-t", str(opt.duration)]
    cmd += ["-an", "-vf", vf, "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"]

    proc = subprocess.run(cmd, stdout=subprocess.PIPE)
    if proc.returncode != 0 or not proc.stdout:
        _fail("ffmpeg failed to decode video frames")
    buf = np.frombuffer(proc.stdout, dtype=np.uint8)
    per = nbtv.ROWS_TX * nbtv.COLS
    n = buf.size // per
    if n == 0:
        _fail("no video frames decoded")
    return buf[: n * per].reshape(n, nbtv.ROWS_TX, nbtv.COLS)


def frames_to_pixels(frames: np.ndarray, opt: EncodeOptions) -> np.ndarray:
    """Arrange grey frames into NBTVA scan order: (n, COLS, ROWS_TX) uint8.

    Matches mtv.py frames_to_signal geometry: rows scanned bottom->top, lines
    step right->left. The device receives bytes line-major (line 0 sample 0..47,
    line 1 sample 0..47, ...) already in display order.
    """
    img = frames.astype(np.float32)
    if opt.flip_v:
        img = img[:, ::-1, :]
    if opt.flip_h:
        img = img[:, :, ::-1]
    # rows bottom->top, cols into line order (right->left)
    img = img[:, ::-1, ::-1]

    if opt.stabilize:
        target = 127.5
        m = img.mean(axis=(1, 2), keepdims=True)
        img = np.clip(img - m + target, 0.0, 255.0)

    # (n, ROWS_TX, COLS) -> (n, COLS, ROWS_TX): out[frame, line, sample]
    out = np.transpose(img, (0, 2, 1))
    return np.clip(np.round(out), 0, 255).astype(np.uint8)


def encode_to_nbtvf(source: str, out_path: Path, opt: EncodeOptions,
                    workdir: Path) -> int:
    """Full pipeline: (download ->) decode -> pixels -> .nbtvf. Returns frames."""
    if is_url(source):
        src = download(source, workdir, opt.max_height)
    else:
        src = Path(source).expanduser().resolve()
        if not src.exists():
            _fail(f"file not found: {src}")

    frames = _grey_frames(src, opt)
    pixels = frames_to_pixels(frames, opt)  # (n, COLS, ROWS_TX)

    tmp = out_path.with_suffix(".nbtvf.tmp")
    with open(tmp, "wb") as fp:
        n = nbtv.write_nbtvf(fp, (f.tobytes() for f in pixels))
    tmp.replace(out_path)
    return n


def make_test_card() -> bytes:
    """A single NBTVA scan-order test frame (server-side; the device has its own)."""
    img = np.zeros((nbtv.ROWS_TX, nbtv.COLS), dtype=np.uint8)
    img[[0, -1], :] = 255
    img[:, [0, -1]] = 255
    img[nbtv.ROWS_TX // 2, :] = 255
    img[:, nbtv.COLS // 2] = 255
    wedge = np.linspace(0, 255, nbtv.COLS).astype(np.uint8)
    img[nbtv.ROWS_TX // 2 - 8: nbtv.ROWS_TX // 2 - 2, :] = wedge
    frames = img[None, :, :]
    return frames_to_pixels(frames, EncodeOptions(stabilize=False))[0].tobytes()
