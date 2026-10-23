"""Print per-interval RMS and peak of a float WAV (TENCHU_AUDIO_DUMP) per channel."""
import array
import math
import struct
import sys

data = open(sys.argv[1], "rb").read()
channels, rate = struct.unpack_from("<HI", data, 22)
samples = array.array("f", data[44:44 + (len(data) - 44) // 4 * 4])
interval = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
frames = len(samples) // channels
step = int(rate * interval)
print(f"{channels} channels, {rate} Hz, {frames / rate:.1f} s")
for start in range(0, frames, step):
    end = min(frames, start + step)
    parts = []
    for c in range(channels):
        values = samples[start * channels + c:end * channels:channels]
        rms = math.sqrt(sum(v * v for v in values) / max(1, len(values)))
        parts.append(f"{rms:.3f}/{max((abs(v) for v in values), default=0):.2f}")
    print(f"{start / rate:6.1f}s  " + "  ".join(parts))
