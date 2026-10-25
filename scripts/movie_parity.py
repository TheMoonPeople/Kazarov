"""Compare a gameplay-parity movie fixture with retail video and decoded PS2 PCM."""

import argparse
import hashlib
import json
import subprocess
import sys
import wave
from fractions import Fraction
from pathlib import Path


def command(tool: Path, arguments: list[str]) -> bytes:
    result = subprocess.run([str(tool), *arguments], capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    return result.stdout


def probe(ffprobe: Path, path: Path, stream: str) -> dict:
    document = json.loads(command(ffprobe, [
        "-v", "error", "-select_streams", stream, "-show_entries",
        "stream=width,height,avg_frame_rate,duration,channels,sample_rate",
        "-of", "json", str(path),
    ]))
    streams = document.get("streams", [])
    if len(streams) != 1:
        raise RuntimeError(f"expected one {stream} stream in {path}")
    return streams[0]


def digest_stream(ffmpeg: Path, arguments: list[str]) -> tuple[str, int]:
    process = subprocess.Popen([str(ffmpeg), "-v", "error", "-threads", "1", *arguments],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    digest = hashlib.sha256()
    size = 0
    assert process.stdout is not None
    for chunk in iter(lambda: process.stdout.read(1 << 20), b""):
        digest.update(chunk)
        size += len(chunk)
    stderr = process.stderr.read().decode("utf-8", "replace") if process.stderr else ""
    if process.wait():
        raise RuntimeError(stderr)
    return digest.hexdigest(), size


def verify_case(directory: Path, ffmpeg: Path, ffprobe: Path, case: dict) -> str:
    video = directory / case["video"]
    mp4 = directory / case["mp4"]
    original = probe(ffprobe, video, "v:0")
    converted = probe(ffprobe, mp4, "v:0")
    dimensions = (int(original["width"]), int(original["height"]))
    if dimensions != (int(converted["width"]), int(converted["height"])):
        raise AssertionError(f"video dimensions differ: {case['source_alias']}")
    original_rate = Fraction(original["avg_frame_rate"])
    converted_rate = Fraction(converted["avg_frame_rate"])
    if original_rate != converted_rate:
        raise AssertionError(f"video frame rate differs: {case['source_alias']}: {original_rate} vs {converted_rate}")
    hashes = []
    for path in (video, mp4):
        hashes.append(digest_stream(ffmpeg, ["-i", str(path), "-map", "0:v:0",
                                              "-fps_mode", "passthrough", "-pix_fmt", "yuv420p",
                                              "-f", "rawvideo", "pipe:1"]))
    if hashes[0] != hashes[1]:
        raise AssertionError(f"decoded video or frame count differs: {case['source_alias']}: {hashes}")
    frame_size = dimensions[0] * dimensions[1] * 3 // 2
    if hashes[0][1] % frame_size:
        raise AssertionError(f"truncated decoded video frame: {case['source_alias']}")
    frames = hashes[0][1] // frame_size
    for label, path in (("retail", video), ("archive", mp4)):
        stream = original if label == "retail" else converted
        duration = stream.get("duration")
        if duration not in (None, "N/A") and abs(float(duration) - frames / float(original_rate)) > 1.0 / float(original_rate):
            raise AssertionError(f"{label} video duration differs from frame count: {case['source_alias']}")
    wav_name = case.get("wav")
    if wav_name:
        with wave.open(str(directory / wav_name), "rb") as expected_wav:
            if expected_wav.getsampwidth() != 2:
                raise AssertionError(f"native movie PCM is not 16-bit: {case['source_alias']}")
            expected = expected_wav.readframes(expected_wav.getnframes())
            channels, rate = expected_wav.getnchannels(), expected_wav.getframerate()
        stream = probe(ffprobe, mp4, "a:0")
        if (int(stream["channels"]), int(stream["sample_rate"])) != (channels, rate):
            raise AssertionError(f"ALAC channel/rate mismatch: {case['source_alias']}")
        decoded_hash, decoded_size = digest_stream(ffmpeg, ["-i", str(mp4), "-map", "0:a:0",
            "-acodec", "pcm_s16le", "-f", "s16le", "pipe:1"])
        if (decoded_hash, decoded_size) != (hashlib.sha256(expected).hexdigest(), len(expected)):
            raise AssertionError(f"lossless ALAC PCM mismatch: {case['source_alias']}")
    elif json.loads(command(ffprobe, ["-v", "error", "-select_streams", "a",
        "-show_entries", "stream=index", "-of", "json", str(mp4)]))["streams"]:
        raise AssertionError(f"unexpected audio stream: {case['source_alias']}")
    return f"{case['source_alias']}: {frames} decoded frames at {original_rate} fps, " \
           f"video SHA256 {hashes[0][0]}, audio {'exact PCM' if wav_name else 'absent'}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path, help="gameplay-parity fixture directory")
    parser.add_argument("--ffmpeg", type=Path, default=Path("ffmpeg"))
    parser.add_argument("--ffprobe", type=Path, default=Path("ffprobe"))
    args = parser.parse_args()
    directory = args.fixture / "movies"
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("kind") != "movie_parity" or not manifest.get("cases"):
        raise RuntimeError("movie parity fixture is empty or invalid")
    for case in manifest["cases"]:
        print(verify_case(directory, args.ffmpeg, args.ffprobe, case), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, AssertionError, OSError) as error:
        print(f"movie parity failed: {error}", file=sys.stderr)
        sys.exit(1)
