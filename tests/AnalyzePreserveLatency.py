"""Summarize paired GPU/OS-display timings without treating them as optical latency."""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from statistics import mean


def rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--gpu", default="gpu-steady")
    args = parser.parse_args()
    root = args.root
    gpu = rows(root / args.gpu / "gpu-rounds.csv")
    assert len(gpu) == 48 and all(int(r["samples"]) == 120 for r in gpu)
    by_gpu = defaultdict(list)
    for row in gpu:
        by_gpu[(row["scene"], int(row["width"]), row["mode"])].append(row)
    result = {"definition": "GPU: continuous-load timestamps including upload/VP/AA/NIS/output dependency. Display: capture callback to Windows displayed event, not HDMI/optical latency.", "gpu": [], "live": [], "live_pairs": []}
    for (scene, width, mode), values in sorted(by_gpu.items()):
        assert len(values) == 6 and len({r["round"] for r in values}) == 6
        times = [float(r["mean_ms"]) for r in values]
        result["gpu"].append(dict(scene=scene, width=width, mode=mode, samples=720,
                                  mean_ms=mean(times), run_min_ms=min(times), run_max_ms=max(times)))
    live = []
    for round_no in range(4):
        values = rows(root / "live" / f"round-{round_no}" / "summary.csv")
        assert {r["Method"] for r in values} == {"high", "preserve"}
        for row in values:
            row["round"] = round_no
            assert int(row["Unmatched"]) == 0 and int(row["Displayed"]) >= 100
            live.append(row)
        high = next(float(r["CaptureToDisplayMeanMs"]) for r in values if r["Method"] == "high")
        preserve = next(float(r["CaptureToDisplayMeanMs"]) for r in values if r["Method"] == "preserve")
        result["live_pairs"].append(dict(round=round_no, high_ms=high, preserve_ms=preserve, preserve_minus_high_ms=preserve-high))
    present_modes = set()
    for mode in ("high", "preserve"):
        values = [r for r in live if r["Method"] == mode]
        times = [float(r["CaptureToDisplayMeanMs"]) for r in values]
        for row in values:
            present_modes.update(json.loads(row["PresentModes"]))
        result["live"].append(dict(mode=mode, mean_ms=mean(times), run_min_ms=min(times), run_max_ms=max(times),
                                   p95_min_ms=min(float(r["CaptureToDisplayP95Ms"]) for r in values),
                                   p95_max_ms=max(float(r["CaptureToDisplayP95Ms"]) for r in values),
                                   displayed=sum(int(r["Displayed"]) for r in values),
                                   dropped=sum(int(r["Dropped"]) for r in values), unmatched=0))
    assert present_modes == {"Composed: Flip"}, present_modes
    settings = json.loads((root / "live" / "settings-check.json").read_text(encoding="utf-8-sig"))
    assert settings["Equal"]
    result["present_mode"] = "Composed: Flip"
    result["settings_unchanged"] = True
    (root / "summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
