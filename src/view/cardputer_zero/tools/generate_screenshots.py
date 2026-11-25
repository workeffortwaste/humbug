#!/usr/bin/env python3
"""Generate the README and store screenshots from the headless UI renderer."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from PIL import Image


ROOT = Path(__file__).resolve().parents[4]
VIEW = ROOT / "src/view/cardputer_zero"
PREVIEW_DIR = ROOT / "assets/preview"
RENDERER = ROOT / "build/desktop/humbug_view_smoke_tests"
THEME_NAMES = list(json.loads((VIEW / "assets/palettes.json").read_text()))
THEME_INDICES = json.loads((VIEW / "tools/embedded-theme-indices.json").read_text())
PLAYBACK_SCREENSHOTS = ("playing-sonos", "playing-cast")


def save_screenshot(source, destination):
    with Image.open(source) as image:
        image.save(destination)
    print(f"Saved {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=PREVIEW_DIR)
    args = parser.parse_args()

    if len(THEME_NAMES) != len(THEME_INDICES):
        raise ValueError("Embedded theme IDs do not match the generated palettes")

    with tempfile.TemporaryDirectory() as temporary_dir:
        subprocess.run([str(RENDERER), temporary_dir, "--theme-screenshots"], check=True)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for name in PLAYBACK_SCREENSHOTS:
            save_screenshot(
                Path(temporary_dir) / f"{name}.ppm",
                args.output_dir / f"{name}.png",
            )

        for theme_index, theme_name in enumerate(THEME_NAMES):
            output_name = f"dashboard-{'-'.join(theme_name.lower().split())}"
            save_screenshot(
                Path(temporary_dir) / f"{theme_index:03}-dashboard.ppm",
                args.output_dir / f"{output_name}.png",
            )

        save_screenshot(
            Path(temporary_dir) / "000-blocked.ppm",
            args.output_dir / "blocked.png",
        )


if __name__ == "__main__":
    main()
