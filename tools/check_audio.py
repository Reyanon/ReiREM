#!/usr/bin/env python3
"""Per-chip output check of harness WAVs (accuracy hashes cannot catch cases where everything is wrong together).

Usage: python tools/check_audio.py fm.wav pcm.wav
  fm.wav  = WAV from a -DREI_SOUND_SOLO=1 harness (YM2151 only)
  pcm.wav = WAV from a -DREI_SOUND_SOLO=2 harness (GA20 only)
Both chips must have sound in the play section (30~50s) of a 3000-frame autoplay.
"""
import array
import math
import sys


def rms(path, a, b):
    data = array.array("h", open(path, "rb").read()[44:])
    seg = data[a * 96000:b * 96000]
    return math.sqrt(sum(v * v for v in seg) / max(1, len(seg)))


def main():
    ok = True
    for path, name in zip(sys.argv[1:3], ["YM2151(FM)", "GA20(PCM)"]):
        r = rms(path, 30, 50)
        print(f"{name}: play RMS {r:.0f}")
        if r < 100:
            print(f"  FAIL: {name} has almost no output")
            ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
