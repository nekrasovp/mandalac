#!/usr/bin/env python3
"""Capture mandalac in a pseudo-terminal and render an animated GIF."""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import termios
import time
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


DEFAULT_FONT = Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")


class Terminal:
    """Minimal ANSI terminal implementing exactly the sequences mandalac emits."""

    def __init__(self, columns: int, rows: int) -> None:
        self.columns = columns
        self.rows = rows
        self.characters = [[" "] * columns for _ in range(rows)]
        self.foreground = [[(255, 255, 255)] * columns for _ in range(rows)]
        self.row = 0
        self.column = 0
        self.current_foreground = (255, 255, 255)
        self.pending = bytearray()

    def clear(self) -> None:
        for row in range(self.rows):
            self.characters[row] = [" "] * self.columns
            self.foreground[row] = [(255, 255, 255)] * self.columns
        self.row = 0
        self.column = 0

    def apply_sgr(self, parameters: list[int]) -> None:
        if not parameters:
            parameters = [0]
        index = 0
        while index < len(parameters):
            value = parameters[index]
            if value in (0, 39):
                self.current_foreground = (255, 255, 255)
                index += 1
            elif value == 38 and index + 4 < len(parameters):
                if parameters[index + 1] == 2:
                    self.current_foreground = tuple(parameters[index + 2 : index + 5])
                    index += 5
                else:
                    index += 1
            else:
                index += 1

    def apply_control(self, parameters: bytes, command: int) -> None:
        text = parameters.decode("ascii").lstrip("?")
        values = [int(value) if value else 0 for value in text.split(";")]
        if command == ord("H"):
            row = values[0] if values and values[0] else 1
            column = values[1] if len(values) > 1 and values[1] else 1
            self.row = min(max(row - 1, 0), self.rows - 1)
            self.column = min(max(column - 1, 0), self.columns - 1)
        elif command == ord("J") and values and values[0] == 2:
            self.clear()
        elif command == ord("m"):
            self.apply_sgr(values)

    def feed(self, data: bytes) -> None:
        self.pending.extend(data)
        position = 0
        while position < len(self.pending):
            byte = self.pending[position]
            if byte == 0x1B:
                if position + 1 >= len(self.pending):
                    break
                if self.pending[position + 1] != ord("["):
                    position += 2
                    continue
                end = position + 2
                while end < len(self.pending) and not (
                    0x40 <= self.pending[end] <= 0x7E
                ):
                    end += 1
                if end == len(self.pending):
                    break
                self.apply_control(bytes(self.pending[position + 2 : end]), self.pending[end])
                position = end + 1
                continue
            if byte == ord("\r"):
                self.column = 0
            elif byte == ord("\n"):
                self.row = min(self.row + 1, self.rows - 1)
            elif 0x20 <= byte <= 0x7E:
                if self.row < self.rows and self.column < self.columns:
                    self.characters[self.row][self.column] = chr(byte)
                    self.foreground[self.row][self.column] = self.current_foreground
                    self.column += 1
            position += 1
        del self.pending[:position]


def render_terminal(
    terminal: Terminal,
    font: ImageFont.FreeTypeFont,
    cell_width: int,
    cell_height: int,
    padding: int,
) -> Image.Image:
    image = Image.new(
        "RGB",
        (
            terminal.columns * cell_width + padding * 2,
            terminal.rows * cell_height + padding * 2,
        ),
        (0, 0, 0),
    )
    draw = ImageDraw.Draw(image)
    for row in range(terminal.rows):
        y = padding + row * cell_height
        for column, character in enumerate(terminal.characters[row]):
            if character != " ":
                draw.text(
                    (padding + column * cell_width, y - 2),
                    character,
                    font=font,
                    fill=terminal.foreground[row][column],
                )
    return image


def capture(
    executable: Path,
    destination: Path,
    columns: int,
    rows: int,
    seconds: float,
    frame_rate: int,
    cycle_seconds: float,
    font_path: Path,
    font_size: int,
) -> None:
    if columns < 1 or rows < 1 or seconds <= 0 or frame_rate < 1 or cycle_seconds < 2:
        raise ValueError("invalid capture dimensions, duration, or frame rate")

    font = ImageFont.truetype(str(font_path), font_size)
    cell_width = max(1, round(font.getlength("M")))
    cell_height = font_size + 3
    padding = 10
    terminal = Terminal(columns, rows)

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    process = subprocess.Popen(
        [
            str(executable.resolve()),
            "--fps",
            "30",
            "--cycle",
            str(cycle_seconds),
        ],
        stdin=subprocess.DEVNULL,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        start_new_session=True,
    )
    os.close(slave)
    os.set_blocking(master, False)

    frames: list[Image.Image] = []
    started = time.monotonic()
    warmup = 0.15
    next_frame = started + warmup
    interval = 1.0 / frame_rate
    expected_frames = round(seconds * frame_rate)
    try:
        while len(frames) < expected_frames:
            readable, _, _ = select.select([master], [], [], 0.01)
            if readable:
                try:
                    terminal.feed(os.read(master, 1 << 20))
                except BlockingIOError:
                    pass
            now = time.monotonic()
            while now >= next_frame and len(frames) < expected_frames:
                frames.append(
                    render_terminal(terminal, font, cell_width, cell_height, padding)
                )
                next_frame += interval
            if process.poll() is not None:
                raise RuntimeError("mandala exited before the capture completed")
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGINT)
            deadline = time.monotonic() + 2.0
            while process.poll() is None and time.monotonic() < deadline:
                readable, _, _ = select.select([master], [], [], 0.05)
                if readable:
                    try:
                        os.read(master, 1 << 20)
                    except (BlockingIOError, OSError):
                        pass
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)
        os.close(master)

    destination.parent.mkdir(parents=True, exist_ok=True)
    frames[0].save(
        destination,
        save_all=True,
        append_images=frames[1:],
        duration=round(1000 / frame_rate),
        loop=0,
        optimize=True,
        disposal=1,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="?", default=Path("./mandala"))
    parser.add_argument(
        "destination",
        type=Path,
        nargs="?",
        default=Path("assets/mandala.gif"),
    )
    parser.add_argument("--columns", type=int, default=84)
    parser.add_argument("--rows", type=int, default=28)
    parser.add_argument("--seconds", type=float, default=6.0)
    parser.add_argument("--frame-rate", type=int, default=10)
    parser.add_argument("--cycle", type=float, default=6.0)
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT)
    parser.add_argument("--font-size", type=int, default=12)
    args = parser.parse_args()

    capture(
        args.executable,
        args.destination,
        args.columns,
        args.rows,
        args.seconds,
        args.frame_rate,
        args.cycle,
        args.font,
        args.font_size,
    )


if __name__ == "__main__":
    main()
