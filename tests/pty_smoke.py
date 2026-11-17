#!/usr/bin/env python3
"""Two-second real terminal run; isolated config, multiple terminal sizes, clean exit."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

binary = str(Path(sys.argv[1]).resolve())
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
for rows, cols in ((24, 80), (12, 40), (40, 160)):
    config = out / f'config-{rows}-{cols}'
    config.mkdir()
    # Exercise duplicate disk samplers and per-device/all-device VRAM, including missing indices.
    rc = config / 'htoprc'
    rc.write_text('header_layout=two_50_50\n'
                  'column_meters_0=CPU Memory SSD(1) SSD(2) DiskSpace(1) VRAM(1) VRAM\n'
                  'column_meter_modes_0=1 1 1 1 1 1 1\n'
                  'column_meters_1=Tasks LoadAverage Uptime\n'
                  'column_meter_modes_1=2 2 2\n')
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    env = dict(os.environ, TERM=os.environ.get('SMOKE_TERM', 'xterm-256color'),
               HTOPRC=str(rc), HOME=str(config), XDG_CONFIG_HOME=str(config))
    process = subprocess.Popen([binary, '-d', '5'], stdin=slave, stdout=slave,
                               stderr=slave, env=env, start_new_session=True)
    os.close(slave)
    data = bytearray()
    start = time.monotonic()
    sent_setup = sent_meters = sent_escape = sent_quit = False
    try:
        while process.poll() is None:
            elapsed = time.monotonic() - start
            if elapsed >= 0.7 and not sent_setup:
                os.write(master, b'\x1bOQ') # F2: enter setup
                sent_setup = True
            if elapsed >= 0.95 and not sent_meters:
                os.write(master, b'\x1bOB\x1bOB') # select the Meters category
                sent_meters = True
            if elapsed >= 1.3 and not sent_escape:
                os.write(master, b'\x1b')
                sent_escape = True
            if elapsed >= 2.0 and not sent_quit:
                os.write(master, b'q')
                sent_quit = True
            if elapsed > 8:
                os.killpg(process.pid, signal.SIGTERM)
                raise AssertionError('terminal run timed out')
            if select.select([master], [], [], 0.05)[0]:
                try:
                    data.extend(os.read(master, 65536))
                except OSError:
                    break
        code = process.wait(timeout=3)
    finally:
        os.close(master)
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=3)
    (out / f'pty-{rows}-{cols}.bin').write_bytes(data)
    assert code == 0, (rows, cols, code, data[-2000:])
    assert b'AddressSanitizer' not in data and b'runtime error:' not in data, data[-2000:]
    assert b'Error opening terminal' not in data, data
    assert len(data) > 300, 'no terminal drawing'
    if cols >= 80:
        assert b'Available meters' in data, 'F2 meter picker was not drawn'
    assert time.monotonic() - start >= 2.0, 'exited before smoke interval'
    print(f'PTY {rows}x{cols}, F2, 2 seconds, clean exit: PASS')
