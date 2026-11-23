#!/usr/bin/env python3
"""Integration tests for mandalac's terminal-owned shellsaver mode."""

from __future__ import annotations

import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path


ALT_SCREEN_ENTER = b"\x1b[?1049h"
ALT_SCREEN_LEAVE = b"\x1b[?1049l"
CURSOR_HIDE = b"\x1b[?25l"
CURSOR_SHOW = b"\x1b[?25h"


def fail(message: str) -> None:
    raise AssertionError(message)


def start_shellsaver(
    executable: Path, seconds: int = 10
) -> tuple[subprocess.Popen[bytes], int, list]:
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 18, 49, 0, 0))
    original_attributes = termios.tcgetattr(slave)
    environment = os.environ.copy()
    environment["TERM"] = "xterm-256color"
    process = subprocess.Popen(
        [str(executable), "--shellsaver", "--seconds", str(seconds)],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        env=environment,
        start_new_session=True,
    )
    os.close(slave)
    return process, master, original_attributes


def read_available(master: int) -> bytes:
    output = bytearray()
    while select.select([master], [], [], 0)[0]:
        try:
            chunk = os.read(master, 1 << 20)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    return bytes(output)


def collect_until_exit(process: subprocess.Popen[bytes], master: int, timeout: float) -> bytes:
    output = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], 0.05)
        if readable:
            try:
                output.extend(os.read(master, 1 << 20))
            except OSError:
                pass
        if process.poll() is not None:
            output.extend(read_available(master))
            return bytes(output)

    process.kill()
    process.wait(timeout=2)
    fail("shellsaver did not exit before the test timeout")


def collect_until_started(
    process: subprocess.Popen[bytes], master: int, timeout: float = 3
) -> bytearray:
    output = bytearray()
    deadline = time.monotonic() + timeout
    while ALT_SCREEN_ENTER not in output and time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], 0.05)
        if readable:
            try:
                output.extend(os.read(master, 1 << 20))
            except OSError:
                break
        if process.poll() is not None:
            break
    if ALT_SCREEN_ENTER not in output:
        process.kill()
        process.wait(timeout=2)
        fail("shellsaver did not enter the alternate screen")
    return output


def assert_terminal_restored(master: int, original_attributes: list) -> None:
    restored = termios.tcgetattr(master)
    if restored != original_attributes:
        fail("shellsaver did not restore terminal attributes")


def assert_visual_lifecycle(output: bytes | bytearray) -> None:
    for sequence in (ALT_SCREEN_ENTER, ALT_SCREEN_LEAVE, CURSOR_HIDE, CURSOR_SHOW):
        if sequence not in output:
            fail(f"missing terminal lifecycle sequence: {sequence!r}")


def test_cli(executable: Path) -> None:
    help_result = subprocess.run(
        [str(executable), "--help"], capture_output=True, check=False, text=True
    )
    if help_result.returncode != 0 or "--shellsaver" not in help_result.stdout:
        fail("--help does not describe shellsaver mode")

    check_result = subprocess.run(
        [str(executable), "--check"], capture_output=True, check=False, text=True
    )
    if check_result.returncode != 0 or "240 E8 roots" not in check_result.stdout:
        fail("existing --check behavior regressed")

    invalid_arguments = (
        ("--shellsaver", "--seconds", "0"),
        ("--seconds", "1"),
        ("--unknown",),
    )
    for arguments in invalid_arguments:
        result = subprocess.run(
            [str(executable), *arguments], capture_output=True, check=False, text=True
        )
        if result.returncode != 2:
            fail(f"invalid arguments {arguments!r} returned {result.returncode}")

    non_tty = subprocess.run(
        [str(executable), "--shellsaver", "--seconds", "1"],
        capture_output=True,
        check=False,
        text=True,
    )
    if non_tty.returncode != 1 or "interactive terminal" not in non_tty.stderr:
        fail("shellsaver did not reject a non-interactive terminal")


def test_timeout(executable: Path) -> None:
    process, master, original_attributes = start_shellsaver(executable, 1)
    try:
        output = collect_until_exit(process, master, 5)
        if process.returncode != 0:
            fail(f"timed shellsaver returned {process.returncode}")
        assert_visual_lifecycle(output)
        assert_terminal_restored(master, original_attributes)
    finally:
        os.close(master)


def test_default_mode_compatibility(executable: Path) -> None:
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 18, 49, 0, 0))
    original_attributes = termios.tcgetattr(slave)
    process = subprocess.Popen(
        [str(executable), "--frames", "1"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        start_new_session=True,
    )
    os.close(slave)
    try:
        output = collect_until_exit(process, master, 3)
        if process.returncode != 0:
            fail(f"default one-frame render returned {process.returncode}")
        assert_visual_lifecycle(output)
        assert_terminal_restored(master, original_attributes)
    finally:
        os.close(master)


def test_any_key(executable: Path) -> None:
    process, master, original_attributes = start_shellsaver(executable)
    try:
        output = collect_until_started(process, master)
        started = time.monotonic()
        os.write(master, b"x")
        output.extend(collect_until_exit(process, master, 3))
        if process.returncode != 0:
            fail(f"key-triggered shellsaver returned {process.returncode}")
        if time.monotonic() - started > 2:
            fail("shellsaver did not react promptly to keyboard input")
        assert_visual_lifecycle(output)
        assert_terminal_restored(master, original_attributes)
    finally:
        os.close(master)


def test_signal_cleanup(executable: Path, signal_number: signal.Signals) -> None:
    process, master, original_attributes = start_shellsaver(executable)
    try:
        output = collect_until_started(process, master)
        process.send_signal(signal_number)
        output.extend(collect_until_exit(process, master, 3))
        if process.returncode != 0:
            fail(f"{signal_number.name} returned {process.returncode}")
        assert_visual_lifecycle(output)
        assert_terminal_restored(master, original_attributes)
    finally:
        os.close(master)


def test_resize_limit(executable: Path) -> None:
    process, master, original_attributes = start_shellsaver(executable)
    try:
        output = collect_until_started(process, master)
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 2000, 2000, 0, 0))
        process.send_signal(signal.SIGWINCH)
        output.extend(collect_until_exit(process, master, 3))
        if process.returncode != 1:
            fail(f"oversized resize returned {process.returncode}")
        if b"exceeds 2000000 cells" not in output:
            fail("oversized resize did not report the renderer safety limit")
        assert_visual_lifecycle(output)
        assert_terminal_restored(master, original_attributes)
    finally:
        os.close(master)


def main() -> None:
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else "./mandala").resolve()
    test_cli(executable)
    test_default_mode_compatibility(executable)
    test_timeout(executable)
    test_any_key(executable)
    for signal_number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT):
        test_signal_cleanup(executable, signal_number)
    test_resize_limit(executable)
    print("shellsaver tests: OK")


if __name__ == "__main__":
    main()
