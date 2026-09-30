"""Generate MediaAccess's built-in download sounds (v2.74).

Writes sounds/download_success.wav and sounds/download_failure.wav at the
repository root: WAV PCM 16-bit, 44.1 kHz, mono, peak -12 dBFS, 8 ms fades,
under one second. Made from plain sine tones here, so there is no licence
question. Re-run only to change the sounds; the WAV files are committed.
"""
import math
import os
import struct
import wave

RATE = 44100
PEAK = 10 ** (-12 / 20)   # -12 dBFS
FADE = int(0.008 * RATE)  # 8 ms


def note(freq, dur, decay):
    """One soft bell-like note: sine + a little 2nd harmonic, exponential decay."""
    n = int(dur * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        v = math.sin(2 * math.pi * freq * t) + 0.18 * math.sin(4 * math.pi * freq * t)
        v *= math.exp(-t * decay)
        if i < FADE:
            v *= i / FADE
        if i > n - FADE:
            v *= (n - i) / FADE
        out.append(v)
    return out


def silence(dur):
    return [0.0] * int(dur * RATE)


def write(path, samples):
    top = max(abs(s) for s in samples) or 1.0
    scale = PEAK / top
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', int(round(s * scale * 32767))) for s in samples))
    print('%s: %.2f s' % (path, len(samples) / RATE))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, 'sounds')
    os.makedirs(out_dir, exist_ok=True)
    # Success: two rising notes (G5 then C6), bright and short.
    success = note(784.0, 0.16, 9.0) + silence(0.03) + note(1046.5, 0.32, 7.0)
    # Failure: two falling, lower notes (A4 then E4), a little longer.
    failure = note(440.0, 0.20, 6.0) + silence(0.04) + note(329.6, 0.42, 4.5)
    write(os.path.join(out_dir, 'download_success.wav'), success)
    write(os.path.join(out_dir, 'download_failure.wav'), failure)


if __name__ == '__main__':
    main()
