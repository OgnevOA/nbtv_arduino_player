"""Encoder: turn a video/URL/GIF into a .pcm NBTV signal file.

Server-side descendant of mtv.py's pipeline. The video is decoded to grayscale
frames (32 wide x 114 tall @ 12.5 fps) and handed to render.frames_to_pcm(),
which synthesizes the finished NBTV composite as mono 16-bit PCM. Audio is
dropped entirely (the mechanical disc is silent picture only).
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from . import nbtv, render


class EncodeError(RuntimeError):
    pass


@dataclass
class EncodeOptions:
    fit: str = "cover"              # "cover" (crop to 2:3) or "contain" (pad)
    flip_h: bool = False
    flip_v: bool = False
    stabilize: bool = True          # hold mean brightness per frame (AC-couple)
    headroom: float = 0.80          # picture-white ceiling (sync stays full)
    lowpass: float = 10000.0        # band-limit cutoff Hz (0 = off)
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
                  self.headroom, self.lowpass, self.contrast, self.brightness,
                  self.gamma, self.start, self.duration, self.max_height):
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
    """Decode source to a (n, ROWS, COLS) uint8 grey stack @ 12.5 fps.

    ROWS = nbtv.ACTIVE_SPL (114) so each line maps 1:1 to picture samples; the
    device no longer interpolates.
    """
    rows = nbtv.ROWS
    if opt.fit == "contain":
        geom = (f"scale=w={nbtv.COLS}:h={rows}:"
                f"force_original_aspect_ratio=decrease,"
                f"pad={nbtv.COLS}:{rows}:(ow-iw)/2:(oh-ih)/2:color=black")
    else:  # cover: crop to portrait 2:3 then scale
        geom = ("crop='min(iw,ih*2/3)':'min(ih,iw*3/2)',"
                f"scale={nbtv.COLS}:{rows}")
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
    per = rows * nbtv.COLS
    n = buf.size // per
    if n == 0:
        _fail("no video frames decoded")
    return buf[: n * per].reshape(n, rows, nbtv.COLS)


def _resolve(source: str, workdir: Path, opt: EncodeOptions) -> Path:
    if is_url(source):
        return download(source, workdir, opt.max_height)
    src = Path(source).expanduser().resolve()
    if not src.exists():
        _fail(f"file not found: {src}")
    return src


def _write_pcm(pcm: bytes, out_path: Path) -> None:
    tmp = out_path.with_suffix(".pcm.tmp")
    tmp.write_bytes(pcm)
    tmp.replace(out_path)


def encode_to_pcm(source: str, out_path: Path, opt: EncodeOptions,
                  workdir: Path) -> int:
    """Full pipeline: (download ->) decode -> synth -> .pcm. Returns frames."""
    src = _resolve(source, workdir, opt)
    frames = _grey_frames(src, opt)
    pcm = render.frames_to_pcm(frames, flip_h=opt.flip_h, flip_v=opt.flip_v,
                               stabilize=opt.stabilize, headroom=opt.headroom,
                               lowpass_hz=opt.lowpass)
    _write_pcm(pcm, out_path)
    return int(frames.shape[0])


def _tgs_frames(src: Path, opt: EncodeOptions, workdir: Path) -> np.ndarray:
    """Render a Telegram animated sticker (.tgs = gzipped Lottie) to grey frames.

    rlottie renders the vector animation at native size/fps; we mux it to a temp
    mp4 so the usual _grey_frames() crop/eq/fps-resample path applies unchanged.
    """
    try:
        from rlottie_python import LottieAnimation
    except ImportError:
        _fail("rlottie-python not installed (needed for animated .tgs stickers)")

    anim = LottieAnimation.from_tgs(str(src))
    w, h = anim.lottie_animation_get_size()
    fps = anim.lottie_animation_get_framerate() or nbtv.BASE_FPS
    total = anim.lottie_animation_get_totalframe() or 1

    mp4 = workdir / "tgs.mp4"
    cmd = [_ffmpeg(), "-v", "error", "-y",
           "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{w}x{h}",
           "-r", f"{fps}", "-i", "pipe:0",
           "-an", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(mp4)]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    assert proc.stdin is not None
    for i in range(total):
        img = anim.render_pillow_frame(frame_num=i)   # RGBA over black
        proc.stdin.write(img.tobytes())
    proc.stdin.close()
    if proc.wait() != 0:
        _fail("ffmpeg failed to assemble sticker frames")
    return _grey_frames(mp4, opt)


def encode_tgs_to_pcm(source: str, out_path: Path, opt: EncodeOptions,
                      workdir: Path) -> int:
    """Full pipeline for an animated (.tgs) sticker -> looping .pcm."""
    src = _resolve(source, workdir, opt)
    frames = _tgs_frames(src, opt, workdir)
    pcm = render.frames_to_pcm(frames, flip_h=opt.flip_h, flip_v=opt.flip_v,
                               stabilize=opt.stabilize, headroom=opt.headroom,
                               lowpass_hz=opt.lowpass)
    _write_pcm(pcm, out_path)
    return int(frames.shape[0])


def encode_still_to_pcm(source: str, out_path: Path, opt: EncodeOptions,
                        workdir: Path, seconds: float = 2.0) -> int:
    """Decode a still image and repeat it into a short loopable .pcm program."""
    src = _resolve(source, workdir, opt)
    frames = _grey_frames(src, opt)          # image -> 1 frame
    n = max(1, int(round(seconds * nbtv.BASE_FPS)))
    stack = np.repeat(frames[:1], n, axis=0)
    # A still has no motion to stabilize; keep true picture levels.
    pcm = render.frames_to_pcm(stack, flip_h=opt.flip_h, flip_v=opt.flip_v,
                               stabilize=False, headroom=opt.headroom,
                               lowpass_hz=opt.lowpass)
    _write_pcm(pcm, out_path)
    return n
