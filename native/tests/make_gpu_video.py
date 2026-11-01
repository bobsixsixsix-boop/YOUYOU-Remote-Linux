#!/usr/bin/env python3
"""Generate synthetic GPU-regression clips using ffmpeg with libx264 on PATH.

    python3 native/tests/make_gpu_video.py /tmp/youyou-gpu-fixtures
    youyou_gpu_present_test --gpu /tmp/youyou-gpu-fixtures/quadrants-192x122.h264

The explicit --gpu test requires a VAAPI decoder and SDL GLES2/EGL renderer.
The 192x122 clip exercises visible-height cropping inside coded 192x128 storage.
No input image, account data, running desktop or network connection is used.
"""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
colors = [(81, 90, 240), (145, 54, 34), (41, 240, 110), (235, 128, 128)]
for width, height in [(192, 128), (192, 122)]:
    stem = args.output / f"quadrants-{width}x{height}"
    with stem.with_suffix(".yuv").open("wb") as output:
        for _ in range(4):
            for plane in range(3):
                w, h = (width, height) if plane == 0 else (width // 2, height // 2)
                output.write(bytes(colors[(x >= w // 2) + 2 * (y >= h // 2)][plane]
                                   for y in range(h) for x in range(w)))
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pixel_format", "yuv420p",
        "-video_size", f"{width}x{height}", "-framerate", "120",
        "-i", str(stem.with_suffix(".yuv")), "-frames:v", "4",
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-qp", "18", "-x264-params", "keyint=1:min-keyint=1:scenecut=0:aud=1",
        "-colorspace", "smpte170m", "-color_range", "tv",
        "-f", "h264", str(stem.with_suffix(".h264")),
    ], check=True)
    print(stem.with_suffix(".h264"))
