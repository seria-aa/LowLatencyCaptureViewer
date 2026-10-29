"""Independent split-file packet continuity, absolute sync and decoder checks."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def run(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace"))
    return result.stdout


def initialization_state(path):
    """Exclude only a recognizable incomplete init prefix, never arbitrary damage."""
    length = path.stat().st_size
    if not length:
        return "incomplete"
    with path.open("rb") as stream:
        offset = 0
        have_ftyp = False
        while offset + 8 <= length:
            stream.seek(offset)
            header = stream.read(8)
            size = int.from_bytes(header[:4], "big")
            kind = header[4:]
            allowed = kind == b"ftyp" if offset == 0 else have_ftyp and kind in (b"free", b"skip", b"moov")
            header_size = 8
            if size == 1:
                if offset + 16 > length:
                    return "incomplete" if allowed else "invalid"
                size = int.from_bytes(stream.read(8), "big")
                header_size = 16
            if size < header_size or (kind == b"ftyp" and size < header_size + 8):
                return "invalid"
            if offset + size > length:
                return "incomplete" if allowed else "invalid"
            if kind == b"moov":
                # All initialized files must still pass ffprobe and full decoding.
                return "initialized"
            if not allowed:
                return "invalid"
            if kind == b"ftyp":
                have_ftyp = True
            offset += size
    return "incomplete" if offset == length and have_ftyp else "invalid"


def initialized(path):
    return initialization_state(path) == "initialized"


def recoverable_segments(paths, crash):
    """Never skip an interior file, a finalized file, or an initialized corrupt tail."""
    paths = sorted(paths)
    states = {path: initialization_state(path) for path in paths}
    assert "invalid" not in states.values(), "malformed or ambiguous initialization cannot be excluded"
    uninitialized = [path for path in paths if states[path] == "incomplete"]
    if uninitialized:
        assert crash and uninitialized == paths[-1:], "only an uninitialized crash tail may be excluded"
        return paths[:-1], paths[-1].name
    return paths, None


def verify(args, root):
    command = [args.test_exe, "--fixture", args.fixture, "--split-" + args.mode]
    if args.crash:
        result = subprocess.run(command + ["--crash-child", str(root / "complete.mp4")],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        assert result.returncode == 73, "controlled child must exit without finalization"
    else:
        run(command + ["--output-directory", str(root)])
    manifest_path, = root.rglob("input-timestamps.json")
    manifest = json.loads(manifest_path.read_text())
    positions = {"video": 0, "audio": 0}
    reports = []
    paths, incomplete_tail = recoverable_segments(manifest_path.parent.glob("complete-*.mp4"), args.crash)
    for path in paths:
        probe = json.loads(run([args.ffprobe, "-v", "error", "-show_packets", "-show_streams", "-of", "json", str(path)]))
        streams = {s["index"]: s["codec_type"] for s in probe["streams"]}
        tracks = {kind: [p for p in probe["packets"] if streams[p["stream_index"]] == kind] for kind in positions}
        first = tracks["video"][0]
        assert "K" in first["flags"], "segment must start with a keyframe"
        origin = manifest["video"][positions["video"]]["pts"] / 1e7 - float(first["pts_time"])
        errors = {}
        for kind, packets in tracks.items():
            maximum = 0
            for packet in packets:
                expected = manifest[kind][positions[kind]]
                for clock in ("pts", "dts"):
                    maximum = max(maximum, abs(float(packet[clock + "_time"]) + origin - expected[clock] / 1e7))
                positions[kind] += 1
            assert maximum < 0.001, f"{path.name}: {kind} shifted by {maximum}s"
            errors[kind] = maximum * 1000
        # Every file must decode on its own, including reordered HEVC pictures.
        decoded = subprocess.run([args.ffmpeg, "-nostdin", "-v", "error", "-xerror", "-threads", "1",
                                  "-i", str(path), "-map", "0:v:0", "-map", "0:a:0",
                                  "-f", "null", "-"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        assert decoded.returncode == 0 and not decoded.stderr, decoded.stderr.decode(errors="replace")
        counts = json.loads(run([args.ffprobe, "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(path)]))
        video_count = next(s for s in counts["streams"] if s["codec_type"] == "video")["nb_read_frames"]
        assert int(video_count) == len(tracks["video"]), "independent decoding must retain every split video frame"
        reports.append({"file": path.name, "origin": origin, "packets": {k: len(v) for k, v in tracks.items()}, "max_timestamp_error_ms": errors})
    assert len(reports) >= (1 if args.allow_deferred else 3)
    if args.crash:
        assert positions["video"] >= 300 and positions["audio"] >= 470, "completed fragment prefix must survive"
        tails = [manifest[k][positions[k] - 1]["pts"] / 1e7 for k in positions]
        assert abs(tails[0] - tails[1]) < 0.15, "recovered audio/video tails"
    else:
        assert all(positions[k] == len(manifest[k]) for k in positions), "missing or duplicated boundary packets"
    report = {"passed": True, "segments": reports, "total_packets": positions,
              "uninitialized_crash_tail": incomplete_tail}
    (root / "segment-verification.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for key in ("test-exe", "fixture", "ffmpeg", "ffprobe"):
        parser.add_argument("--" + key, required=True)
    parser.add_argument("--mode", choices=("time", "size"), required=True)
    parser.add_argument("--allow-deferred", action="store_true")
    parser.add_argument("--crash", action="store_true")
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()
    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=False)
        verify(args, args.output_directory)
    else:
        with tempfile.TemporaryDirectory(prefix="llcv-segments-") as directory:
            verify(args, Path(directory))
