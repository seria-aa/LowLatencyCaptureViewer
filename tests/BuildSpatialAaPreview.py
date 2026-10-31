"""Package lossless GPU PNGs for manual quality inspection; no settings access."""
import argparse
import base64
import json
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--prior", type=Path)
    parser.add_argument("--soft", action="store_true")
    args = parser.parse_args()
    output = args.output
    if args.prior:
        results = []
        for scene in ("antialiased", "aliased"):
            for width in (2560, 3840):
                for mode in ("off", "nis", "cmaa-preserve" if args.soft else "cmaa-current"):
                    name = f"{scene}-{width}-{mode}.png"
                    with Image.open(output / "synthetic" / name) as a, Image.open(args.prior / name) as b:
                        delta = np.abs(np.asarray(a).astype(np.int16) - np.asarray(b).astype(np.int16))
                    result = {"file": name, "maximum": int(delta.max()), "channels_over_one": int((delta > 1).sum())}
                    assert result["maximum"] <= 1, result
                    results.append(result)
        (output / "baseline-parity.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    # Lossless crop only: never scale, sharpen, compress lossily or recolor outputs.
    compact = output / "compact"
    compact.mkdir(exist_ok=True)
    embedded = {}
    for scene in ("antialiased", "aliased", "game"):
        embedded[scene] = {}
        modes = ("nis", "cmaa-preserve", "dlaa50", "dlaa25") if args.soft else ("nis", "cmaa-current", "dlaa", "fxaa")
        for mode in modes:
            if scene == "game":
                with Image.open(output / "saved-game" / f"game-2560-{mode}.png") as image:
                    cropped = image.crop((1056, 104, 1344, 264))
                    target = compact / f"game-{mode}.png"
                    cropped.save(target)
            else:
                target = output / "synthetic" / f"{scene}-2560-{mode}-text.png"
            embedded[scene][mode] = "data:image/png;base64," + base64.b64encode(target.read_bytes()).decode("ascii")
    # Used only by the authoring step; never contains settings or device IDs.
    print(json.dumps(embedded, separators=(",", ":")))


if __name__ == "__main__":
    main()
