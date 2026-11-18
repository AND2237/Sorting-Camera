#!/usr/bin/env python3
"""Convert a Sorting Camera .scamrec recording + JSON sidecar to VFR MP4.

Requires FFmpeg with the libx264 encoder available on PATH (or --ffmpeg path).
The source .scamrec and sidecar are never modified.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

MAGIC = b"SCAMREC\x01"
MAX_HEADER_BYTES = 1 << 20
MAX_FRAME_BYTES = 1 << 24


class ConversionError(Exception):
    pass


def load_sidecar(recording_path: Path, sidecar_path: Path) -> dict[str, Any]:
    try:
        data = json.loads(sidecar_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConversionError(f"Cannot read/parse sidecar JSON: {exc}") from exc

    if data.get("format") != "scamrec" or data.get("formatVersion") != 1:
        raise ConversionError("Unsupported sidecar format/version; expected scamrec v1.")

    expected_name = data.get("file")
    if expected_name and Path(expected_name).name != recording_path.name:
        raise ConversionError(
            f"Sidecar refers to '{expected_name}', but input is '{recording_path.name}'."
        )

    frames = data.get("frames")
    if not isinstance(frames, list) or not frames:
        raise ConversionError("Sidecar contains no frame entries.")

    required = ("offset", "bytes", "ts", "w", "h")
    previous_ts: int | None = None
    previous_offset = -1
    for index, frame in enumerate(frames):
        if not isinstance(frame, dict) or any(key not in frame for key in required):
            raise ConversionError(f"Frame entry {index} is missing required fields: {required}.")
        try:
            offset = int(frame["offset"])
            length = int(frame["bytes"])
            timestamp = int(frame["ts"])
            width = int(frame["w"])
            height = int(frame["h"])
        except (TypeError, ValueError) as exc:
            raise ConversionError(f"Frame entry {index} has invalid numeric metadata.") from exc

        if offset <= previous_offset:
            raise ConversionError(f"Frame offsets are not strictly increasing at entry {index}.")
        if length <= 0 or length > MAX_FRAME_BYTES:
            raise ConversionError(f"Invalid JPEG length {length} at frame {index}.")
        if width <= 0 or height <= 0:
            raise ConversionError(f"Invalid dimensions at frame {index}: {width}x{height}.")
        if previous_ts is not None and timestamp <= previous_ts:
            raise ConversionError(
                f"Frame timestamps must be strictly increasing; problem at frame {index}."
            )

        previous_offset = offset
        previous_ts = timestamp

    return data


def extract_frames(recording_path: Path, frames: list[dict[str, Any]], temp_dir: Path) -> tuple[int, int]:
    """Verify SCAMREC framing and extract JPEG payloads without re-encoding."""
    try:
        source = recording_path.open("rb")
    except OSError as exc:
        raise ConversionError(f"Cannot open recording: {exc}") from exc

    with source:
        magic = source.read(8)
        if magic != MAGIC:
            raise ConversionError("Invalid SCAMREC magic/header. This is not a supported v1 file.")

        raw_header_len = source.read(4)
        if len(raw_header_len) != 4:
            raise ConversionError("Recording has a truncated container header.")
        header_len = struct.unpack("<I", raw_header_len)[0]
        if header_len <= 0 or header_len > MAX_HEADER_BYTES:
            raise ConversionError(f"Implausible SCAMREC header length: {header_len}.")

        header_bytes = source.read(header_len)
        if len(header_bytes) != header_len:
            raise ConversionError("Recording has a truncated JSON header.")
        try:
            header = json.loads(header_bytes.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ConversionError(f"Invalid JSON header inside SCAMREC: {exc}") from exc

        if header.get("format") != "scamrec" or header.get("formatVersion") != 1:
            raise ConversionError("Unsupported SCAMREC container version; expected v1.")

        first_payload_offset = 12 + header_len
        if int(frames[0]["offset"]) != first_payload_offset:
            raise ConversionError(
                "The first sidecar offset does not match the end of the SCAMREC header "
                f"(sidecar={frames[0]['offset']}, expected={first_payload_offset}). "
                "Check that this JSON belongs to this recording."
            )

        dimensions = {(int(frame["w"]), int(frame["h"])) for frame in frames}
        if len(dimensions) != 1:
            raise ConversionError(
                "This recording contains changing frame dimensions. A single MP4 stream "
                "needs normalized dimensions; this converter intentionally refuses to "
                "silently resize frames."
            )
        width, height = next(iter(dimensions))
        if width % 2 or height % 2:
            raise ConversionError(
                f"H.264 yuv420p requires even dimensions; sidecar reports {width}x{height}."
            )

        expected_offset = first_payload_offset
        for index, frame in enumerate(frames):
            offset = int(frame["offset"])
            expected_length = int(frame["bytes"])

            if offset != expected_offset:
                raise ConversionError(
                    f"Unexpected frame offset at index {index}: got {offset}, "
                    f"expected {expected_offset}. The sidecar and container may not match."
                )

            source.seek(offset)
            length_bytes = source.read(4)
            if len(length_bytes) != 4:
                raise ConversionError(f"Frame {index}: missing/truncated 4-byte length prefix.")
            actual_length = struct.unpack("<I", length_bytes)[0]
            if actual_length != expected_length:
                raise ConversionError(
                    f"Frame {index}: container length {actual_length} != sidecar length "
                    f"{expected_length}. The sidecar and recording may not match."
                )

            jpeg = source.read(actual_length)
            if len(jpeg) != actual_length:
                raise ConversionError(f"Frame {index}: JPEG payload is truncated.")
            if len(jpeg) < 4 or not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
                raise ConversionError(f"Frame {index}: payload does not have valid JPEG markers.")

            frame_path = temp_dir / f"frame_{index:06d}.jpg"
            try:
                frame_path.write_bytes(jpeg)
            except OSError as exc:
                raise ConversionError(f"Cannot write temporary frame {index}: {exc}") from exc

            expected_offset = offset + 4 + actual_length

        return width, height


def write_concat_manifest(frames: list[dict[str, Any]], temp_dir: Path, last_frame_ms: float) -> tuple[float, float]:
    timestamps = [int(frame["ts"]) for frame in frames]
    intervals_ms = [timestamps[i + 1] - timestamps[i] for i in range(len(timestamps) - 1)]
    if not intervals_ms or any(delta <= 0 for delta in intervals_ms):
        raise ConversionError("At least two frames with increasing timestamps are required.")

    durations_ms = intervals_ms + [last_frame_ms]
    manifest_lines = ["ffconcat version 1.0"]

    for index, duration_ms in enumerate(durations_ms):
        manifest_lines.append(f"file 'frame_{index:06d}.jpg'")
        # Give each still-image demuxer a 1 ms time base so irregular capture
        # intervals are not rounded to the default 25 fps (40 ms) grid.
        manifest_lines.append("option framerate 1000")
        manifest_lines.append(f"duration {duration_ms / 1000.0:.9f}")

    # The final repeated frame gives the last captured image a presentation
    # interval too. It is visually identical; the source JPEG remains untouched.
    manifest_lines.append(f"file 'frame_{len(frames) - 1:06d}.jpg'")
    manifest_lines.append("option framerate 1000")

    manifest = temp_dir / "frames.ffconcat"
    manifest.write_text("\n".join(manifest_lines) + "\n", encoding="ascii", newline="\n")

    span_seconds = (timestamps[-1] - timestamps[0]) / 1000.0
    estimated_duration = span_seconds + last_frame_ms / 1000.0
    average_fps = (len(frames) - 1) / span_seconds if span_seconds > 0 else 0.0
    return estimated_duration, average_fps


def _clean_path_input(raw: str) -> str:
    """Normalize a pasted/dragged Windows path from console input."""
    value = raw.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("\"", "'"):
        value = value[1:-1]
    return os.path.expandvars(os.path.expanduser(value))


def _resolve_user_path(raw: str, *, filename_base: Path) -> Path:
    """Resolve an entered path; bare filenames are searched in filename_base first."""
    value = _clean_path_input(raw)
    candidate = Path(value)
    if candidate.is_absolute():
        return candidate.resolve()

    if candidate.parent == Path("."):
        candidates = [filename_base / candidate, Path(__file__).resolve().parent / candidate,
                      Path.cwd() / candidate]
    else:
        candidates = [Path.cwd() / candidate, Path(__file__).resolve().parent / candidate,
                      filename_base / candidate]

    for path in candidates:
        if path.is_file():
            return path.resolve()
    # Return the primary expected location so the error message is useful.
    return candidates[0].resolve()


def _ask_path(prompt: str, *, filename_base: Path) -> Path | None:
    try:
        raw = input(prompt).strip()
    except (EOFError, KeyboardInterrupt):
        print("\nOperation cancelled.")
        return None
    if not raw:
        return None
    return _resolve_user_path(raw, filename_base=filename_base)


def convert_recording(
    recording: Path,
    sidecar: Path,
    output: Path,
    *,
    ffmpeg_arg: str = "ffmpeg",
    crf: int = 18,
    preset: str = "medium",
    last_frame_ms_arg: float | None = None,
    overwrite: bool = False,
) -> int:
    recording = recording.expanduser().resolve()
    sidecar = sidecar.expanduser().resolve()
    output = output.expanduser().resolve()

    if not recording.is_file():
        raise ConversionError(f"Recording file not found: {recording}")
    if recording.suffix.lower() != ".scamrec":
        raise ConversionError("Input file must have the .scamrec extension.")
    if not sidecar.is_file():
        raise ConversionError(f"Sidecar JSON not found: {sidecar}")
    if output == recording or output == sidecar:
        raise ConversionError("Output path must not overwrite either source file.")
    if output.exists() and not overwrite:
        raise ConversionError(f"Output already exists: {output}. Use --overwrite to replace it.")
    if not 0 <= crf <= 51:
        raise ConversionError("CRF must be between 0 and 51.")

    ffmpeg = shutil.which(ffmpeg_arg) if not Path(ffmpeg_arg).is_file() else str(Path(ffmpeg_arg))
    if not ffmpeg:
        raise ConversionError(
            "FFmpeg was not found. Install FFmpeg and add it to PATH, "
            "or specify its full path with --ffmpeg."
        )

    sidecar_data = load_sidecar(recording, sidecar)
    frames = sidecar_data["frames"]
    intervals_ms = [int(frames[i + 1]["ts"]) - int(frames[i]["ts"])
                    for i in range(len(frames) - 1)]
    if not intervals_ms or any(delta <= 0 for delta in intervals_ms):
        raise ConversionError("The sidecar needs at least two frames with increasing timestamps.")

    last_frame_ms = (last_frame_ms_arg if last_frame_ms_arg is not None
                     else float(statistics.median(intervals_ms)))
    if not (0 < last_frame_ms <= 10000):
        raise ConversionError("--last-frame-ms must be greater than 0 and no more than 10000.")

    output.parent.mkdir(parents=True, exist_ok=True)

    print(f"\nRecording: {recording}")
    print(f"Sidecar:   {sidecar}")
    print(f"Frames:    {len(frames)}")

    with tempfile.TemporaryDirectory(prefix="scamrec-frames-") as temp_name:
        temp_dir = Path(temp_name)
        width, height = extract_frames(recording, frames, temp_dir)
        duration, average_fps = write_concat_manifest(frames, temp_dir, last_frame_ms)

        print(f"Resolution: {width}x{height}")
        print(f"Timestamp span: {duration:.3f} s (last-frame duration estimated as {last_frame_ms:.1f} ms)")
        print(f"Average captured rate: {average_fps:.2f} fps")
        print("Encoding H.264 MP4 with variable frame timestamps...")

        command = [
            ffmpeg,
            "-hide_banner",
            "-loglevel", "warning",
            "-y" if overwrite else "-n",
            "-f", "concat",
            "-safe", "0",
            "-i", "frames.ffconcat",
            "-map", "0:v:0",
            "-an",
            "-fps_mode", "vfr",
            "-c:v", "libx264",
            "-bf", "0",
            "-preset", preset,
            "-crf", str(crf),
            "-pix_fmt", "yuv420p",
            "-movflags", "+faststart",
            str(output),
        ]
        result = subprocess.run(command, cwd=temp_dir, text=True, capture_output=True, check=False)
        if result.returncode != 0:
            if result.stderr:
                print(result.stderr.strip(), file=sys.stderr)
            raise ConversionError(
                f"FFmpeg failed with exit code {result.returncode}. "
                "Check that this FFmpeg build includes the libx264 encoder."
            )

    if not output.is_file() or output.stat().st_size == 0:
        raise ConversionError("FFmpeg returned success but the MP4 file is missing or empty.")

    print(f"\nConversion completed successfully:\n{output}")
    print(f"Output size: {output.stat().st_size:,} bytes")
    print("The original .scamrec and .json files were not modified.")
    return 0


def interactive_main(args: argparse.Namespace) -> int:
    default_dir = Path.home() / "Pictures" / "SortingCamera" / "recordings"
    tools_dir = Path(__file__).resolve().parent

    print("=" * 64)
    print("Sorting Camera - SCAMREC to MP4 Converter")
    print("=" * 64)
    print("Enter either a .scamrec file or its matching .scamrec.json file.")
    print("You can paste a full path, drag a file into this window, or enter only its filename.")
    print("For filename-only input, the default recordings folder is searched first:")
    print(f"  {default_dir}")
    print("Type Q to quit.\n")

    try:
        raw_source = input("SCAMREC or JSON file: ").strip()
    except (EOFError, KeyboardInterrupt):
        print("\nOperation cancelled.")
        return 1
    if not raw_source or raw_source.casefold() in {"q", "quit", "exit"}:
        print("No conversion requested.")
        return 0

    source_path = _resolve_user_path(raw_source, filename_base=default_dir)
    if not source_path.is_file():
        raise ConversionError(f"Input file not found: {source_path}")

    if source_path.name.lower().endswith(".scamrec.json") or source_path.suffix.lower() == ".json":
        sidecar = source_path
        # A sidecar named recording.scamrec.json has recording.scamrec as its pair.
        inferred_recording = Path(str(sidecar)[:-5])
        if inferred_recording.is_file():
            recording = inferred_recording
            print(f"Detected matching recording: {recording}")
        else:
            entered = _ask_path(
                "Matching .scamrec file path/name: ", filename_base=default_dir
            )
            if entered is None:
                print("Operation cancelled.")
                return 1
            recording = entered
    elif source_path.suffix.lower() == ".scamrec":
        recording = source_path
        sidecar = Path(str(recording) + ".json")
        if not sidecar.is_file():
            print(f"Automatic JSON sidecar not found: {sidecar}")
            entered = _ask_path(
                "Matching .scamrec.json file path/name: ", filename_base=default_dir
            )
            if entered is None:
                print("Operation cancelled.")
                return 1
            sidecar = entered
    else:
        raise ConversionError("Please select a .scamrec file or its matching .scamrec.json file.")

    # An explicit --json option remains supported for command-line use.
    if args.sidecar is not None:
        sidecar = args.sidecar

    default_output = recording.with_suffix(".mp4")
    if args.output is not None:
        output = args.output
    else:
        try:
            raw_output = input(f"Output MP4 path [Enter = {default_output}]: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nOperation cancelled.")
            return 1
        output = (_resolve_user_path(raw_output, filename_base=recording.parent)
                  if raw_output else default_output)

    overwrite = args.overwrite
    if output.exists() and not overwrite:
        try:
            answer = input(f"\n'{output.name}' already exists. Replace it? [y/N]: ").strip().casefold()
        except (EOFError, KeyboardInterrupt):
            print("\nOperation cancelled.")
            return 1
        if answer not in {"y", "yes"}:
            print("Conversion cancelled; existing MP4 was left unchanged.")
            return 0
        overwrite = True

    return convert_recording(
        recording,
        sidecar,
        output,
        ffmpeg_arg=args.ffmpeg,
        crf=args.crf,
        preset=args.preset,
        last_frame_ms_arg=args.last_frame_ms,
        overwrite=overwrite,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Convert a Sorting Camera .scamrec + .scamrec.json pair to VFR H.264 MP4. "
                    "Run without arguments for interactive mode."
    )
    parser.add_argument("recording", nargs="?", type=Path,
                        help="Path to the .scamrec file (omit for interactive mode)")
    parser.add_argument("--json", dest="sidecar", type=Path,
                        help="Sidecar path (default: <recording>.json)")
    parser.add_argument("-o", "--output", type=Path,
                        help="Output MP4 path (default: same directory and base name)")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg executable or full path")
    parser.add_argument("--crf", type=int, default=18,
                        help="H.264 quality: lower is higher quality (default: 18)")
    parser.add_argument("--preset", default="medium", help="libx264 preset (default: medium)")
    parser.add_argument("--last-frame-ms", type=float,
                        help="Display duration assigned to final frame; default is median captured interval")
    parser.add_argument("--overwrite", action="store_true",
                        help="Replace output MP4 if it already exists")
    args = parser.parse_args(argv)

    if args.recording is None:
        return interactive_main(args)

    recording = args.recording.expanduser().resolve()
    sidecar = args.sidecar.expanduser().resolve() if args.sidecar else Path(str(recording) + ".json")
    output = args.output.expanduser().resolve() if args.output else recording.with_suffix(".mp4")
    return convert_recording(
        recording,
        sidecar,
        output,
        ffmpeg_arg=args.ffmpeg,
        crf=args.crf,
        preset=args.preset,
        last_frame_ms_arg=args.last_frame_ms,
        overwrite=args.overwrite,
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ConversionError as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
