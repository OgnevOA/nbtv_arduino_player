"""NBTVA 32-line waveform synthesis (server-side).

Ported from mtv.py's frames_to_signal + lowpass_fir. Turns a stack of grey
frames into the finished NBTV composite signal as mono 16-bit PCM:

  * white-positive level mapping (with a picture "headroom" ceiling so bright
    frames can't clip the amp while the sync pulse keeps full depth),
  * blacker-than-black line sync, with the 32nd line's sync omitted as the
    missing-pulse frame marker,
  * per-frame brightness stabilization for AC-coupled receivers,
  * a ~10 kHz linear-phase low-pass that band-limits the sharp sync/pixel edges
    a real sync separator would otherwise ring on.

Playback speed, output level (gain) and polarity (invert) are intentionally NOT
applied here; those stay live on the device so the disc can be re-locked.
"""

from __future__ import annotations

import numpy as np

from . import nbtv


def lowpass_fir(x: np.ndarray, cutoff_hz: float,
                rate: int = nbtv.BASE_RATE, numtaps: int = 63) -> np.ndarray:
    """Linear-phase windowed-sinc low-pass (band-limit to NBTVA ~10 kHz)."""
    if not cutoff_hz or cutoff_hz >= rate / 2:
        return x
    fc = cutoff_hz / rate
    n = np.arange(numtaps) - (numtaps - 1) / 2
    h = 2 * fc * np.sinc(2 * fc * n) * np.blackman(numtaps)
    h /= h.sum()
    return np.convolve(x, h, mode="same")


def _normalize_peak(x: np.ndarray, peak: int = 30000) -> np.ndarray:
    """Scale down so |x| <= peak, preventing filter overshoot from clipping."""
    m = float(np.abs(x).max()) if x.size else 0.0
    return x * (peak / m) if m > peak else x


def frames_to_signal(frames: np.ndarray, *, flip_h: bool = False,
                     flip_v: bool = False, stabilize: bool = True,
                     headroom: float = 0.80) -> np.ndarray:
    """(n, ROWS, COLS) uint8 grey stack -> float32 NBTVA composite samples.

    Geometry (NBTVA): a "line" is a vertical column scanned bottom-to-top;
    successive lines step right-to-left; sync sits at the end of each line.
    """
    img = frames.astype(np.float32)
    if flip_v:
        img = img[:, ::-1, :]
    if flip_h:
        img = img[:, :, ::-1]
    img = img[:, ::-1, ::-1]          # rows bottom->top, cols into line order
    lum = img / 255.0                 # 0=black .. 1=white

    if stabilize:
        target = 0.5
        m = lum.mean(axis=(1, 2), keepdims=True)
        lum = np.clip(lum - m + target, 0.0, 1.0)

    white = nbtv.WHITE * float(headroom)          # picture ceiling
    active = nbtv.BLACK + lum * (white - nbtv.BLACK)

    n = active.shape[0]
    frame = np.empty((n, nbtv.N_LINES, nbtv.SPL), dtype=np.float32)
    frame[:, :, :nbtv.ACTIVE_SPL] = np.transpose(active, (0, 2, 1))
    frame[:, :, nbtv.ACTIVE_SPL:] = nbtv.SYNC              # line sync (full depth)
    frame[:, nbtv.N_LINES - 1, nbtv.ACTIVE_SPL:] = nbtv.BLACK  # frame sync (gap)
    return frame.reshape(-1)


def signal_to_pcm(sig: np.ndarray, lowpass_hz: float = 10000.0) -> bytes:
    """Band-limit + clamp a float32 composite to mono s16le PCM bytes."""
    if lowpass_hz:
        sig = _normalize_peak(lowpass_fir(sig, lowpass_hz))
    return np.clip(sig, -32768, 32767).astype("<i2").tobytes()


def frames_to_pcm(frames: np.ndarray, *, flip_h: bool = False,
                  flip_v: bool = False, stabilize: bool = True,
                  headroom: float = 0.80, lowpass_hz: float = 10000.0) -> bytes:
    """Full frames -> mono s16le PCM bytes."""
    sig = frames_to_signal(frames, flip_h=flip_h, flip_v=flip_v,
                           stabilize=stabilize, headroom=headroom)
    return signal_to_pcm(sig, lowpass_hz)


def test_card_frame() -> np.ndarray:
    """A single NBTVA test-card grey frame (ROWS x COLS), smooth-ish."""
    img = np.zeros((nbtv.ROWS, nbtv.COLS), dtype=np.uint8)
    img[[0, -1], :] = 255
    img[:, [0, -1]] = 255                                  # white border
    img[nbtv.ROWS // 2, :] = 255
    img[:, nbtv.COLS // 2] = 255                           # centre cross
    wedge = np.linspace(0, 255, nbtv.COLS).astype(np.uint8)
    img[nbtv.ROWS // 2 - 10: nbtv.ROWS // 2 - 2, :] = wedge  # grey wedge
    return img


def test_card_pcm(seconds: float = 1.0, headroom: float = 0.80,
                  lowpass_hz: float = 10000.0) -> bytes:
    """A short loopable test-card PCM buffer for the idle radio stream."""
    n = max(1, int(round(seconds * nbtv.BASE_FPS)))
    frames = np.repeat(test_card_frame()[None, :, :], n, axis=0)
    return frames_to_pcm(frames, stabilize=False, headroom=headroom,
                         lowpass_hz=lowpass_hz)
