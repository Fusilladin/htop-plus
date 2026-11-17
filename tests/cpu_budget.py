#!/usr/bin/env python3
"""Comparable idle terminal CPU samples. Save raw results; this is an observational gate."""
import fcntl
import ctypes
import json
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
from statistics import median

out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True)
binaries = sys.argv[2:]
results = {b: [] for b in binaries}
for attempt in range(3):
    for number, binary in enumerate(binaries):
        config = out / f'config-{attempt}-{number}'
        config.mkdir()
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        env = dict(os.environ, TERM='xterm-256color', HOME=str(config),
                   XDG_CONFIG_HOME=str(config), HTOPRC=str(config / 'htoprc'))
        process = subprocess.Popen([binary, '-d', '15'], stdin=slave, stdout=slave, stderr=slave, env=env)
        os.close(slave)
        clock_id = ctypes.c_int()
        libc = ctypes.CDLL(None)
        assert libc.clock_getcpuclockid(process.pid, ctypes.byref(clock_id)) == 0
        start = time.monotonic()
        before = None
        try:
            while time.monotonic() - start < 10:
                if select.select([master], [], [], 0.05)[0]:
                    os.read(master, 65536)
                assert process.poll() is None
                if before is None and time.monotonic() - start >= 2:
                    before = time.clock_gettime_ns(clock_id.value)
            after = time.clock_gettime_ns(clock_id.value)
            os.write(master, b'q')
            _, status, usage = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
            assert process.returncode == 0
            seconds = (after - before) / 1e9
            results[binary].append(seconds)
            print(binary, attempt + 1, f'{seconds:.6f} CPU seconds', flush=True)
        finally:
            os.close(master)
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.wait()
medians = {b: median(v) for b, v in results.items()}
summary = {'duration_seconds': 8, 'warmup_seconds': 2, 'measurement': 'POSIX process CPU clock', 'delay_tenths': 15, 'samples_cpu_seconds': results,
           'median_cpu_seconds': medians, 'pass': medians[binaries[0]] <= medians[binaries[1]]}
(out / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
sys.exit(0 if summary['pass'] else 1)
