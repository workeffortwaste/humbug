#!/usr/bin/env python3
"""Generate CLI previews using the Cardputer sample tracks and simulated speakers."""

import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[4]
PREVIEW_DIR = ROOT / "assets/images"
CLI = ROOT / "build/core/humbug-cli"
SIMULATOR = ROOT / "build/core/humbug_tests"
SAMPLES = ROOT / "build/desktop/humbug_view_smoke_tests"
FRAME = re.compile(rb"\x1b\[H((?:(?!\x1b\[H).)*?)\x1b\[J", re.DOTALL)
ANSI = re.compile(r"\x1b\[([0-9;]*)([A-Za-z])")


def sample_rooms(renderer):
    output = subprocess.run([str(renderer), "--sample-rooms"], check=True, capture_output=True).stdout
    fields = output.decode("utf-8").split("\0")
    if fields[-1] != "" or len(fields) < 7 or (len(fields) - 1) % 3:
        raise ValueError("Invalid sample room data from the Cardputer renderer")
    return list(zip(fields[0:-1:3], fields[1:-1:3], fields[2:-1:3]))


def read_line(process, timeout=10):
    deadline = time.monotonic() + timeout
    result = bytearray()
    while time.monotonic() < deadline:
        if select.select([process.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
            char = os.read(process.stdout.fileno(), 1)
            if not char:
                break
            if char == b"\n":
                return result.decode("utf-8")
            result.extend(char)
    raise RuntimeError(f"Simulator did not respond: {result!r}")


def wait_for_frame(cli, master, expected, timeout=15):
    output = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if cli.poll() is not None:
            raise RuntimeError(f"CLI exited early with status {cli.returncode}: {output!r}")
        if select.select([master], [], [], 0.1)[0]:
            output.extend(os.read(master, 16384))
            for match in FRAME.finditer(output):
                frame = match.group(1).decode("utf-8")
                plain = ANSI.sub("", frame)
                if all(value in plain for value in expected):
                    return frame
            if len(output) > 1024 * 1024:
                raise RuntimeError("Too much CLI output while waiting for the preview")
    raise RuntimeError(f"Timed out waiting for CLI preview: {output!r}")


def draw_frame(frame, font_path, output, columns=80):
    font = ImageFont.truetype(str(font_path), 18)
    cell_width = round(font.getlength("M"))
    line_height = 24
    padding = 20
    connections = {
        "─": "lr", "│": "ud", "┌": "rd", "┐": "ld", "└": "ru", "┘": "lu",
        "├": "urd", "┤": "uld",
    }
    lines = frame.replace("\r", "").splitlines()
    image = Image.new("RGB", (padding * 2 + columns * cell_width, padding * 2 + len(lines) * line_height),
                      "#242933")
    draw = ImageDraw.Draw(image)
    for row, line in enumerate(lines):
        x = 0
        dim = False
        pos = 0
        for match in ANSI.finditer(line):
            text = line[pos:match.start()]
            color = "#7f8797" if dim else "#e2e6ef"
            for char in text:
                left, top = padding + x * cell_width, padding + row * line_height
                if char in connections:
                    mid_x, mid_y = left + cell_width // 2, top + line_height // 2
                    for direction in connections[char]:
                        end = {"l": (left, mid_y), "r": (left + cell_width, mid_y),
                               "u": (mid_x, top), "d": (mid_x, top + line_height)}[direction]
                        draw.line((mid_x, mid_y, *end), fill=color)
                else:
                    draw.text((left, top), char, font=font, fill=color)
                x += 1
            code, command = match.groups()
            if command == "m" and code in ("0", "2"):
                dim = code == "2"
            elif command != "K":
                raise ValueError(f"Unsupported terminal escape: {match.group()!r}")
            pos = match.end()
        if pos != len(line):
            raise ValueError(f"Unexpected terminal output: {line[pos:]!r}")
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output)
    print(f"Saved {output}")


def stop_cli(cli, master):
    if cli.poll() is not None:
        return
    cli.terminate()
    deadline = time.monotonic() + 10
    while cli.poll() is None and time.monotonic() < deadline:
        if select.select([master], [], [], 0.1)[0]:
            os.read(master, 16384)
    if cli.poll() is None:
        cli.kill()
        cli.wait()
        raise RuntimeError("CLI did not exit after termination")


def capture_disconnected(args, env, directory, history_error=False):
    env = env.copy()
    env.pop("HUMBUG_DEVICE", None)
    env["HUMBUG_SONOS_DISABLED"] = "1"
    if history_error:
        state_dir = directory / "invalid-state"
        history = state_dir / "humbug" / "skips.log"
        history.parent.mkdir(parents=True)
        history.write_text("invalid history entry\n")
        env["XDG_STATE_HOME"] = str(state_dir)

    master, slave = pty.openpty()
    cli = None
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 80, 0, 0))
        cli = subprocess.Popen([str(args.cli), "--no-christmas"], stdin=slave, stdout=slave,
                               stderr=slave, env=env)
        expected = ["┤ Disconnected ├─┤ Not blocking ├─┐", "0/0  No streams found",
                    "Block unavailable"]
        if history_error:
            expected.append("Error: Invalid entry in block history")
        else:
            expected.append("Blocked all time 25185")
        frame = wait_for_frame(cli, master, expected)
        name = "cli-history-error.png" if history_error else "cli-disconnected.png"
        draw_frame(frame, args.font, args.output_dir / name)
        if not history_error:
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 44, 0, 0))
            os.kill(cli.pid, signal.SIGWINCH)
            frame = wait_for_frame(cli, master, ["│ Device Disconnected", "│ Status Not blocking",
                                                "0/0  No streams found", "Blocked all time 25185"])
            draw_frame(frame, args.font, args.output_dir / "cli-disconnected-narrow.png", 44)
    finally:
        try:
            if cli is not None:
                stop_cli(cli, master)
        finally:
            os.close(master)
            os.close(slave)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=CLI)
    parser.add_argument("--simulator", type=Path, default=SIMULATOR)
    parser.add_argument("--samples", type=Path, default=SAMPLES)
    parser.add_argument("--output-dir", type=Path, default=PREVIEW_DIR)
    parser.add_argument("--font", default="DejaVuSansMono.ttf")
    parser.add_argument("--debug", action="store_true", help="capture additional CLI states")
    args = parser.parse_args()
    rooms = sample_rooms(args.samples)

    with tempfile.TemporaryDirectory(prefix="humbug-cli-preview-") as directory:
        directory = Path(directory)
        blocklist = directory / "blocklist"
        blocklist.touch()
        history = directory / "humbug" / "skips.log"
        history.parent.mkdir(parents=True)
        title, artist = rooms[0][1:]
        entry = f"{json.dumps(title, ensure_ascii=False)} {json.dumps(artist, ensure_ascii=False)}\n"
        today = datetime.date.today().isoformat()
        history.write_text(("2000-01-01 " + entry) * 24184 + (today + " " + entry) * 1000)
        env = {key: value for key, value in os.environ.items() if not key.startswith("HUMBUG_")}
        env.update(HOME=str(directory), XDG_CONFIG_HOME=str(directory), XDG_STATE_HOME=str(directory),
                   HUMBUG_CAST_DISABLED="1", HUMBUG_CHRISTMAS_BLOCKING="0",
                   HUMBUG_BLOCKLIST=str(blocklist),
                   HUMBUG_LISTEN_ADDRESS="127.0.0.1")

        with subprocess.Popen([str(args.simulator), "--simulate"], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as simulator:
            master = slave = None
            cli = None
            try:
                if not read_line(simulator).startswith("Fake Sonos streams:"):
                    raise RuntimeError("Unexpected simulator startup")
                device_line = read_line(simulator)
                if not device_line.startswith("Set HUMBUG_DEVICE=http://127.0.0.1:"):
                    raise RuntimeError(f"Unexpected simulator device: {device_line}")
                env["HUMBUG_DEVICE"] = device_line.removeprefix("Set HUMBUG_DEVICE=")
                read_line(simulator)
                for index, (_, title, artist) in enumerate(rooms[:2], 1):
                    if any(c in value for value in (title, artist) for c in "\r\n|"):
                        raise ValueError("Sample track cannot be sent to the simulator")
                    simulator.stdin.write(f"track {index} {artist}|{title}\n".encode())
                    simulator.stdin.flush()
                    if read_line(simulator) != f"Track updated for stream {index}":
                        raise RuntimeError(f"Could not set simulated track {index}")

                master, slave = pty.openpty()
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 80, 0, 0))
                cli = subprocess.Popen([str(args.cli), "--no-christmas"], stdin=slave, stdout=slave,
                                       stderr=slave, env=env)
                for index, (_, title, artist) in enumerate(rooms[:2]):
                    if index:
                        os.write(master, b"n")
                    frame = wait_for_frame(cli, master, [f"{index + 1}/2", f"{artist} / {title}",
                                                        "┤ Connected ├─┤ Blocking ├─┐", "Blocked all time 25184",
                                                        "Blocked today 1000",
                                                        f"Last blocked {rooms[0][2]} / {rooms[0][1]}"])
                    draw_frame(frame, args.font, args.output_dir / f"cli-{index + 1}.png")
                if args.debug:
                    os.write(master, b"b")
                    frame = wait_for_frame(cli, master, ["Blocked all time 25185",
                                                        f"Last blocked {rooms[1][2]} / {rooms[1][1]}"])
                    draw_frame(frame, args.font, args.output_dir / "cli-blocked.png")
            finally:
                try:
                    if cli is not None:
                        stop_cli(cli, master)
                finally:
                    if master is not None:
                        os.close(master)
                    if slave is not None:
                        os.close(slave)
                    if simulator.poll() is None:
                        simulator.stdin.write(b"quit\n")
                        simulator.stdin.flush()
                        try:
                            simulator.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            simulator.kill()
                            simulator.wait()
                            raise RuntimeError("Simulator did not exit")
        if args.debug:
            capture_disconnected(args, env, directory)
            capture_disconnected(args, env, directory, history_error=True)


if __name__ == "__main__":
    main()
