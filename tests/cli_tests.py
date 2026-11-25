import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
from pathlib import Path


CLI = sys.argv[1]
SIMULATOR = sys.argv[2]
sys.argv = sys.argv[:1]


class CliTests(unittest.TestCase):
    def assert_terminal_restored(self, actual, original, transient_lflag):
        actual = actual.copy()
        original = original.copy()
        actual[3] &= ~transient_lflag
        original[3] &= ~transient_lflag
        self.assertEqual(actual, original)

    def test_terminal_restoration_allows_only_pending_input_state(self):
        # macOS may add PENDIN; real terminal setting changes must still fail.
        original = [0, 0, 0, 1483, 0, 0, []]
        restored = original.copy()
        restored[3] |= 0x20000000
        self.assert_terminal_restored(restored, original, 0x20000000)
        restored[3] ^= termios.ECHO
        with self.assertRaises(AssertionError):
            self.assert_terminal_restored(restored, original, 0x20000000)

    def test_no_christmas_flag_keeps_custom_rules(self):
        # Disabling the preset must not disable custom rules or redirect history.
        for disable_christmas in (False, True):
            with self.subTest(disable_christmas=disable_christmas), \
                    tempfile.TemporaryDirectory(prefix="humbug-cli-test-") as directory:
                simulator = subprocess.Popen(
                    [SIMULATOR, "--simulate"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, bufsize=1
                )
                cli = None
                try:
                    self.assertIn("Fake Sonos streams:", simulator.stdout.readline())
                    device = simulator.stdout.readline().strip().removeprefix("Set HUMBUG_DEVICE=")
                    self.assertIn("Commands:", simulator.stdout.readline())
                    simulator.stdin.write("track 1 Mariah Carey|All I Want for Christmas Is You\n")
                    simulator.stdin.flush()
                    self.assertIn("Track updated", simulator.stdout.readline())
                    blocklist = Path(directory) / "blocklist"
                    blocklist.write_text("track:Song & one\n")
                    history = Path(directory) / "humbug" / "skips.log"
                    ignored_history = Path(directory) / "ignored-history"
                    env = {key: value for key, value in os.environ.items() if not key.startswith("HUMBUG_")}
                    env.update(
                        HOME=directory, XDG_CONFIG_HOME=directory, XDG_STATE_HOME=directory,
                        HUMBUG_DEVICE=device, HUMBUG_CAST_DISABLED="1", HUMBUG_CHRISTMAS_BLOCKING="1",
                        HUMBUG_HISTORY=str(ignored_history), HUMBUG_LISTEN_ADDRESS="127.0.0.1",
                    )
                    arguments = [CLI]
                    if disable_christmas:
                        arguments.append("--no-christmas")
                    arguments += ["--config", str(blocklist)]
                    cli = subprocess.Popen(
                        arguments, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL, env=env
                    )
                    deadline = time.monotonic() + 10
                    contents = ""
                    while time.monotonic() < deadline:
                        if cli.poll() is not None:
                            self.fail("CLI exited before processing the simulated tracks")
                        contents = history.read_text() if history.exists() else ""
                        if "Song & one" in contents and (
                            disable_christmas or "All I Want for Christmas Is You" in contents
                        ):
                            break
                        time.sleep(0.05)
                    self.assertIn("Song & one", contents)
                    self.assertFalse(ignored_history.exists())
                    if disable_christmas:
                        time.sleep(1)
                        self.assertNotIn("All I Want for Christmas Is You", history.read_text())
                    else:
                        self.assertIn("All I Want for Christmas Is You", contents)
                finally:
                    if cli is not None and cli.poll() is None:
                        cli.terminate()
                        cli.wait(timeout=10)
                    if simulator.poll() is None:
                        simulator.stdin.write("quit\n")
                        simulator.stdin.flush()
                        simulator.wait(timeout=10)
                    simulator.stdin.close()
                    simulator.stdout.close()
                    simulator.stderr.close()

    def test_disconnected_header_at_width_boundary(self):
        # Check both sides of the width where the status header switches layouts.
        with tempfile.TemporaryDirectory(prefix="humbug-cli-header-") as directory:
            master, slave = pty.openpty()
            cli = None
            try:
                env = {key: value for key, value in os.environ.items() if not key.startswith("HUMBUG_")}
                env.update(HOME=directory, XDG_CONFIG_HOME=directory, XDG_STATE_HOME=directory,
                           HUMBUG_SONOS_DISABLED="1", HUMBUG_CAST_DISABLED="1")
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 45, 0, 0))
                cli = subprocess.Popen([CLI], stdin=slave, stdout=slave, stderr=slave, env=env)

                def read_frame(width):
                    output = bytearray()
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        if cli.poll() is not None:
                            self.fail(f"CLI exited early: {output!r}")
                        if select.select([master], [], [], 0.1)[0]:
                            output.extend(os.read(master, 16384))
                            for match in re.finditer(rb"\x1b\[H(.*?)\x1b\[J", output, re.DOTALL):
                                lines = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "",
                                               match.group(1).decode("utf-8")).splitlines()
                                if lines and len(lines[0]) == width:
                                    return match.group(1).decode("utf-8"), lines
                    self.fail(f"Timed out waiting for CLI frame: {output!r}")

                wide_raw, wide = read_frame(45)
                self.assertEqual(wide[0], "┌─ Humbug ┤ Disconnected ├─┤ Not blocking ├─┐")
                self.assertEqual(len(wide[0]), 45)
                self.assertIn("┤ \x1b[2mDisconnected\x1b[2m ├─┤ \x1b[2mNot blocking",
                              wide_raw)
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 44, 0, 0))
                cli.send_signal(signal.SIGWINCH)
                narrow_raw, narrow = read_frame(44)
                self.assertEqual(narrow[1].strip("│ "), "Device Disconnected")
                self.assertEqual(narrow[2].strip("│ "), "Status Not blocking")
                self.assertIn("\x1b[2mDevice \x1b[0mDisconnected", narrow_raw)
                self.assertIn("\x1b[2mStatus \x1b[0mNot blocking", narrow_raw)
                self.assertTrue(all(len(line) == 44 for line in narrow))
            finally:
                if cli is not None and cli.poll() is None:
                    cli.terminate()
                    cli.wait(timeout=10)
                os.close(master)
                os.close(slave)

    def test_terminal_controls_and_restoration(self):
        # A real pseudo-terminal catches input, redraw and terminal cleanup regressions.
        with tempfile.TemporaryDirectory(prefix="humbug-cli-test-") as directory:
            simulator = subprocess.Popen(
                [SIMULATOR, "--simulate"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, bufsize=1
            )
            master, slave = pty.openpty()
            cli = None

            def drain_output(timeout):
                if select.select([master], [], [], timeout)[0]:
                    os.read(master, 16384)

            def wait_for_cli(timeout):
                deadline = time.monotonic() + timeout
                while cli.poll() is None:
                    if time.monotonic() >= deadline:
                        raise subprocess.TimeoutExpired(cli.args, timeout)
                    drain_output(0.05)
                return cli.returncode

            try:
                self.assertIn("Fake Sonos streams:", simulator.stdout.readline())
                device = simulator.stdout.readline().strip().removeprefix("Set HUMBUG_DEVICE=")
                self.assertTrue(device.startswith("http://127.0.0.1:"))
                blocklist = Path(directory) / "blocklist"
                blocklist.touch()
                env = {key: value for key, value in os.environ.items() if not key.startswith("HUMBUG_")}
                env.update(
                    HOME=directory,
                    XDG_CONFIG_HOME=directory,
                    XDG_STATE_HOME=directory,
                    HUMBUG_DEVICE=device,
                    HUMBUG_CAST_DISABLED="1",
                    HUMBUG_CHRISTMAS_BLOCKING="0",
                    HUMBUG_BLOCKLIST=str(blocklist),
                    HUMBUG_LISTEN_ADDRESS="127.0.0.1",
                )
                original_settings = termios.tcgetattr(slave)
                cli = subprocess.Popen([CLI], stdin=slave, stdout=slave, stderr=slave, env=env)

                def read_until(expected, timeout=15):
                    output = bytearray()
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        if cli.poll() is not None:
                            self.fail(f"CLI exited early: {output!r}")
                        if select.select([master], [], [], 0.1)[0]:
                            output.extend(os.read(master, 16384))
                            if expected.encode() in output:
                                return bytes(output).decode("utf-8", errors="replace")
                    self.fail(f"Timed out waiting for {expected!r}: {output!r}")

                initial = read_until("Artist / Song & one")
                plain = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", initial)
                self.assertRegex(plain, r"┌─ Humbug ─*┤ (Connected|Disconnected) ├─┤ (Blocking|Not blocking) ├─┐")
                self.assertIn("┤ \x1b[0mConnected\x1b[2m ├─┤ \x1b[0mBlocking", initial)
                self.assertIn("\x1b[2mPlaying \x1b[0mArtist / Song & one", initial)
                self.assertIn("\x1b[2mLast blocked \x1b[0mNo tracks yet", initial)
                self.assertIn("\x1b[2mBlocked all time \x1b[0m0", initial)
                self.assertIn("\x1b[2mBlocked today \x1b[0m0", initial)
                self.assertRegex(plain, r"│ Blocked all time 0 +│\r?\n│ Blocked today 0 +│")
                self.assertIn("\x1b[2mn\x1b[0m Next stream", initial)
                self.assertIn("\x1b[2mb\x1b[0m Block", initial)
                self.assertIn("\x1b[2mEsc\x1b[0m Exit", initial)
                self.assertEqual(initial.count("\x1b[2J"), 1)
                self.assertNotIn("Device 1/2", plain)
                active_settings = termios.tcgetattr(slave)
                self.assertFalse(active_settings[3] & (termios.ICANON | termios.ECHO))

                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 37, 0, 0))
                cli.send_signal(signal.SIGWINCH)
                narrow = read_until("Status \x1b[0mBlocking")
                self.assertIn("\x1b[2mDevice \x1b[0mConnected", narrow)
                self.assertIn("\x1b[2mStatus \x1b[0mBlocking", narrow)
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 80, 0, 0))
                cli.send_signal(signal.SIGWINCH)

                os.write(master, b"5n")
                next_screen = read_until("2/2  Office")
                self.assertNotIn("\x1b[2J", next_screen)
                os.write(master, b"7")
                deadline = time.monotonic() + 0.4
                while time.monotonic() < deadline:
                    drain_output(0.05)
                self.assertEqual(blocklist.read_text(), "")
                for key in (b"8", b"q"):
                    os.write(master, key)
                    deadline = time.monotonic() + 0.2
                    while time.monotonic() < deadline:
                        drain_output(0.05)
                    self.assertIsNone(cli.poll(), f"{key!r} should not exit")
                os.write(master, b"x")
                self.assertFalse(select.select([master], [], [], 0.2)[0],
                                 "unchanged screen should not redraw")

                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 60, 0, 0))
                cli.send_signal(signal.SIGWINCH)
                resized_screen = read_until("2/2  Office")
                self.assertNotIn("\x1b[2J", resized_screen)
                os.write(master, b"b")
                blocked_screen = read_until("Last blocked \x1b[0mArtist / Song & one")
                self.assertIn("\x1b[2mLast blocked \x1b[0mArtist / Song & one", blocked_screen)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and not blocklist.read_text().strip():
                    drain_output(0.05)
                self.assertIn("Song & one", blocklist.read_text())

                os.write(master, b"\x1b[A")
                deadline = time.monotonic() + 0.2
                while time.monotonic() < deadline:
                    drain_output(0.05)
                self.assertIsNone(cli.poll(), "arrow key should not exit")
                os.write(master, b"n" * 1024 + b"\x1b")
                self.assertEqual(wait_for_cli(10), 0)
                # macOS may set PENDIN when restoring canonical input mode.
                transient_lflag = termios.PENDIN if sys.platform == "darwin" else 0
                self.assert_terminal_restored(termios.tcgetattr(slave), original_settings, transient_lflag)
            finally:
                try:
                    if cli is not None and cli.poll() is None:
                        cli.terminate()
                        try:
                            wait_for_cli(5)
                        except subprocess.TimeoutExpired:
                            cli.kill()
                            cli.wait(timeout=5)
                finally:
                    os.close(master)
                    os.close(slave)
                    try:
                        if simulator.poll() is None:
                            simulator.stdin.write("quit\n")
                            simulator.stdin.flush()
                            try:
                                simulator.wait(timeout=10)
                            except subprocess.TimeoutExpired:
                                simulator.kill()
                                simulator.wait(timeout=5)
                    finally:
                        simulator.stdin.close()
                        simulator.stdout.close()
                        simulator.stderr.close()


if __name__ == "__main__":
    unittest.main()
