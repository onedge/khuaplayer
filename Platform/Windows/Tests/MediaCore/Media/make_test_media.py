"""Generates the decoder test clips in this directory.

The FFmpeg built for Khua has no encoders, so the clips are made once with a
full FFmpeg build (libx264, libx265, libvpx-vp9, libaom-av1) and committed.
Rerun after changing the list:

    .venv/Scripts/python Platform/Windows/Tests/MediaCore/Media/make_test_media.py
"""

from __future__ import annotations

import logging
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Moving content, so a wrong reference picture shows up as a mismatch.
SOURCE = "testsrc2=size={size}:rate=24"
FRAMES = 24

HDR10 = [
    "-color_primaries", "bt2020",
    "-color_trc", "smpte2084",
    "-colorspace", "bt2020nc",
    "-color_range", "tv",
]


@dataclass(frozen=True)
class Clip:
    name: str
    codec_args: tuple[str, ...]
    size: str = "640x360"
    frames: int = FRAMES


CLIPS: tuple[Clip, ...] = (
    Clip("h264_high_8bit.mp4",
         ("-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p", "-crf", "30")),
    # Padded decoder surfaces (1088 lines) on the hardware path.
    Clip("h264_high_1080p.mp4",
         ("-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p", "-crf", "38"),
         size="1920x1080", frames=6),
    # No D3D11VA profile: software only.
    Clip("h264_high10.mkv",
         ("-c:v", "libx264", "-profile:v", "high10", "-pix_fmt", "yuv420p10le", "-crf", "30")),
    Clip("h264_444.mkv",
         ("-c:v", "libx264", "-profile:v", "high444", "-pix_fmt", "yuv444p", "-crf", "30")),
    Clip("hevc_main.mp4",
         ("-c:v", "libx265", "-pix_fmt", "yuv420p", "-crf", "30", "-tag:v", "hvc1",
          "-x265-params", "log-level=error")),
    Clip("hevc_main10_pq.mkv",
         ("-c:v", "libx265", "-pix_fmt", "yuv420p10le", "-crf", "30", *HDR10,
          "-x265-params", "log-level=error:hdr10=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc")),
    Clip("vp9_profile0.webm",
         ("-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", "-crf", "40", "-b:v", "0", "-deadline", "realtime")),
    Clip("vp9_profile2.webm",
         ("-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p10le", "-crf", "40", "-b:v", "0", "-deadline", "realtime")),
    Clip("av1_main_8bit.mkv",
         ("-c:v", "libaom-av1", "-pix_fmt", "yuv420p", "-crf", "45", "-cpu-used", "8", "-row-mt", "1")),
    Clip("av1_main_10bit.mkv",
         ("-c:v", "libaom-av1", "-pix_fmt", "yuv420p10le", "-crf", "45", "-cpu-used", "8", "-row-mt", "1")),
)


def make(ffmpeg: str, clip: Clip) -> None:
    out = HERE / clip.name
    cmd = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", SOURCE.format(size=clip.size),
        "-frames:v", str(clip.frames), "-an", *clip.codec_args,
        str(out),
    ]
    subprocess.run(cmd, check=True)
    logging.info("%-24s %7d bytes", clip.name, out.stat().st_size)


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        logging.error("ffmpeg (a full build with the encoders above) is not on PATH")
        return 1
    for clip in CLIPS:
        make(ffmpeg, clip)
    return 0


if __name__ == "__main__":
    sys.exit(main())
