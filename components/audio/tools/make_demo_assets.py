#!/usr/bin/env python3
"""Make the example apps' sounds: a small drum kit, a plucked string and a demo
song, all synthesised here so the repository carries no third-party audio.

    make_demo_assets.py <out dir>

Writes smp/kick.wav (PCM16), smp/hat.wav (PCM8), smp/snare.wav (PCM16),
smp/pluck.wav (IMA ADPCM if ffmpeg is installed, else PCM16, with a sustain
loop in a "smpl" chunk) and music/demo.mid (format 1, 120 BPM, 8 bars:
lead on channel 0, bass on 1 with a CC 74 sweep, drums on 9).
"""
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import wave

RATE = 22050


def write_wav(path, frames, bits=16, loop=None):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(bits // 8)
        w.setframerate(RATE)
        if bits == 16:
            w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(v * 32767)))) for v in frames))
        else:
            w.writeframes(bytes(max(0, min(255, int(v * 127) + 128)) for v in frames))
    if loop:
        # A "smpl" chunk with one forward loop (end inclusive, as the format has it).
        smpl = struct.pack("<9I", 0, 0, int(1e9 / RATE), 60, 0, 0, 0, 1, 0)
        smpl += struct.pack("<6I", 0, 0, loop[0], loop[1] - 1, 0, 0)
        with open(path, "r+b") as f:
            data = f.read()
            data += b"smpl" + struct.pack("<I", len(smpl)) + smpl
            data = data[:4] + struct.pack("<I", len(data) - 8) + data[8:]
            f.seek(0)
            f.write(data)


def kick():
    n = int(RATE * 0.35)
    out, ph = [], 0.0
    for i in range(n):
        t = i / RATE
        f = 45 + 120 * math.exp(-t * 30)
        ph += 2 * math.pi * f / RATE
        out.append(0.95 * math.sin(ph) * math.exp(-t * 9))
    return out


def snare():
    rnd = random.Random(2)
    n = int(RATE * 0.25)
    return [0.6 * (rnd.uniform(-1, 1) * math.exp(-i / RATE * 18) + 0.5 * math.sin(2 * math.pi * 190 * i / RATE)
                   * math.exp(-i / RATE * 25)) for i in range(n)]


def hat():
    rnd = random.Random(1)
    n = int(RATE * 0.08)
    prev, out = 0.0, []
    for i in range(n):
        x = rnd.uniform(-1, 1)
        out.append(0.5 * (x - prev) * math.exp(-i / RATE * 60))     # high-passed noise
        prev = x
    return out


def pluck():
    # Karplus-Strong at middle C (261.6 Hz), 1 s; the last whole periods loop.
    period = round(RATE / 261.63)
    rnd = random.Random(3)
    buf = [rnd.uniform(-1, 1) for _ in range(period)]
    out = []
    for i in range(RATE):
        v = buf[i % period]
        buf[i % period] = 0.996 * 0.5 * (v + buf[(i + 1) % period])
        out.append(0.8 * v)
    start = RATE - period * 40
    return out, (start, start + period * 20)


def vlq(n):
    b = [n & 0x7F]
    n >>= 7
    while n:
        b.insert(0, 0x80 | (n & 0x7F))
        n >>= 7
    return bytes(b)


def track(events):
    """events: (tick, bytes) -> an MTrk chunk."""
    events.sort(key=lambda e: e[0])
    data, last = b"", 0
    for t, ev in events:
        data += vlq(t - last) + ev
        last = t
    data += b"\x00\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + data


def demo_mid():
    ppq, bar = 480, 1920
    tempo = [(0, b"\xff\x51\x03" + (500000).to_bytes(3, "big")), (0, b"\xff\x58\x04\x04\x02\x18\x08")]
    lead, bass, drums = [], [], []
    melody = [72, 74, 76, 79, 76, 74, 72, 67]
    roots = [48, 45, 41, 43]
    for b in range(8):
        root = roots[b % 4]
        for s in range(8):                      # eighth-note bass
            t = b * bar + s * 240
            n = root + (12 if s % 2 else 0)
            bass += [(t, bytes([0x91, n, 96])), (t + 200, bytes([0x81, n, 0]))]
        for q in range(4):                      # drums: kick 1 and 3, snare 2 and 4, hats
            t = b * bar + q * ppq
            k = 36 if q % 2 == 0 else 38
            drums += [(t, bytes([0x99, k, 110])), (t + 100, bytes([0x89, k, 0]))]
            for h in (0, 240):
                drums += [(t + h, bytes([0x99, 42, 70])), (t + h + 60, bytes([0x89, 42, 0]))]
        if b >= 2:                              # the lead comes in on bar 3
            for q in range(4):
                n = melody[(b * 4 + q) % len(melody)]
                t = b * bar + q * ppq
                lead += [(t, bytes([0x90, n, 100])), (t + 400, bytes([0x80, n, 0]))]
    for i in range(64):                         # CC 74 sweep on the bass, up and back
        v = int(32 + 80 * abs(math.sin(i / 64 * math.pi)))
        bass.append((i * bar * 8 // 64, bytes([0xB1, 74, v])))
    chunks = [tempo, lead, bass, drums]
    head = b"MThd" + struct.pack(">IHHH", 6, 1, len(chunks), ppq)
    return head + b"".join(track(c) for c in chunks)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    out = sys.argv[1]
    os.makedirs(os.path.join(out, "smp"), exist_ok=True)
    os.makedirs(os.path.join(out, "music"), exist_ok=True)
    write_wav(os.path.join(out, "smp/kick.wav"), kick())
    write_wav(os.path.join(out, "smp/snare.wav"), snare())
    write_wav(os.path.join(out, "smp/hat.wav"), hat(), bits=8)
    frames, loop = pluck()
    path = os.path.join(out, "smp/pluck.wav")
    if shutil.which("ffmpeg"):
        tmp = path + ".tmp.wav"
        write_wav(tmp, frames)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", tmp, "-c:a", "adpcm_ima_wav", "-block_size",
                        "256", "-bitexact", "-map_metadata", "-1", path], check=True)
        os.remove(tmp)
    else:
        write_wav(path, frames, loop=loop)
    with open(os.path.join(out, "music/demo.mid"), "wb") as f:
        f.write(demo_mid())
    for root, _, files in os.walk(out):
        for name in sorted(files):
            p = os.path.join(root, name)
            print(f"{os.path.relpath(p, out)}: {os.path.getsize(p)} bytes")


if __name__ == "__main__":
    main()
