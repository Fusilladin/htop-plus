#!/usr/bin/env python3
"""Fixtures stay under a caller-supplied build directory and are archived, never erased."""
import os
from pathlib import Path
import subprocess
import sys
import unittest

PROBE = Path(sys.argv.pop(1)).resolve()
STUB = Path(sys.argv.pop(1)).resolve()
ROOT = Path(sys.argv.pop(1)).resolve()
ROOT.mkdir(parents=True, exist_ok=True)

class Devices(unittest.TestCase):
    def setUp(self):
        self.root = ROOT / self._testMethodName
        self.root.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(('HTOP_PLUS_', 'NVML_TEST_'))}
        self.env.update(HTOP_PLUS_SYSROOT=str(self.root), LD_LIBRARY_PATH=str(STUB),
                        NVML_TEST_TRACE=str(self.root / 'nvml.trace'), NVML_TEST_COUNT='0')

    def put(self, path, text=''):
        target = self.root / path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        return target

    def run_probe(self, *args, code=0):
        result = subprocess.run([str(PROBE), *args], env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result.stdout.splitlines()

    def disk(self, name, stat='1 0 20 0 2 0 30 0 0 40 0'):
        self.put(f'/sys/block/{name}/stat', stat)

    def nvidia(self, count, suspended=()):
        self.env['NVML_TEST_COUNT'] = str(count)
        for i in range(count):
            base = f'/sys/bus/pci/devices/0000:{i+1:02x}:00.0'
            self.put(base + '/vendor', '0x10de\n')
            self.put(base + '/class', '0x030000\n')
            self.put(base + '/power/runtime_status', 'suspended\n' if i in suspended else 'active\n')

    def amd(self, card='card32'):
        base = f'/sys/class/drm/{card}/device'
        self.put(base + '/vendor', '0x1002\n')
        self.put(base + '/power/runtime_status', 'active\n')
        self.put(base + '/mem_info_vram_used', '4096\n')
        self.put(base + '/mem_info_vram_total', '16384\n')

    def test_zero_devices(self):
        self.assertEqual(self.run_probe(), [])
        self.assertFalse((self.root / 'nvml.trace').exists())

    def test_one_disk(self):
        self.disk('sda')
        self.assertEqual(self.run_probe(), ['disk sda'])
        self.assertEqual(self.run_probe('disk', 'sda'), ['20 30 40'])

    def test_three_disks_nvme_sata(self):
        for name in ('sdb', 'nvme2n1', 'sda', 'loop0', 'ram0', 'zram0'):
            self.disk(name)
        self.assertEqual(self.run_probe(), ['disk nvme2n1', 'disk sda', 'disk sdb'])

    def test_mount_filter_decode_deduplicate(self):
        self.put('/proc/self/mounts', '/dev/sda1 / ext4 rw 0 0\n'
                 '/dev/sdb1 /media/My\\040Disk xfs rw 0 0\n'
                 '/dev/sda1 / ext4 rw 0 0\n'
                 'tmpfs /tmp tmpfs rw 0 0\n/dev/loop0 /snap squashfs ro 0 0\n'
                 'overlay /container overlay rw 0 0\nproc /proc proc rw 0 0\n'
                 'tank/main /tank zfs rw 0 0\n')
        self.assertEqual(self.run_probe(), ['mount /', 'mount /media/My Disk', 'mount /tank'])
        self.assertEqual(self.run_probe('mount', '/'), ['1'])

    def test_overrides(self):
        self.disk('sda'); self.disk('sdb')
        self.env.update(HTOP_PLUS_DISK='sdb', HTOP_PLUS_MOUNT='/chosen')
        self.assertEqual(self.run_probe(), ['disk sdb', 'mount /chosen'])

    def test_invalid_disk_override(self):
        self.env['HTOP_PLUS_DISK'] = '../escape'
        self.assertEqual(self.run_probe(), [])

    def test_missing_stats(self):
        self.disk('sda', 'broken')
        self.run_probe('disk', 'sda', code=2)
        self.run_probe('disk', 'sdb', code=2)
        self.run_probe('mount', '/missing', code=2)

    def test_one_nvidia(self):
        self.nvidia(1)
        self.assertEqual(self.run_probe(), ['gpu NVIDIA 0000:01:00.0 1 2048 8192 0'])
        self.assertEqual((self.root / 'nvml.trace').read_text(), 'IHMS')

    def test_two_nvidia(self):
        self.nvidia(2)
        self.assertEqual(self.run_probe(), ['gpu NVIDIA 0000:01:00.0 1 2048 8192 0',
                                          'gpu NVIDIA 0000:02:00.0 1 4096 16384 0'])
        self.assertEqual((self.root / 'nvml.trace').read_text(), 'IHMHMS')

    def test_amd(self):
        self.amd()
        self.assertEqual(self.run_probe(), ['gpu AMD card32 1 4096 16384 0'])

    def test_mixed(self):
        self.nvidia(2); self.amd()
        self.assertEqual(len(self.run_probe()), 3)

    def test_missing_amd_file(self):
        self.amd()
        self.put('/sys/class/drm/card32/device/mem_info_vram_total', '')
        self.assertIn(' 0 ', self.run_probe()[0])

    def test_suspended_peer_never_calls_nvml(self):
        self.nvidia(2, suspended=(1,)); self.amd()
        rows = self.run_probe()
        self.assertEqual(rows[0], 'gpu NVIDIA 0000:01:00.0 0 0 0 0')
        self.assertEqual(rows[1], 'gpu NVIDIA 0000:02:00.0 0 0 0 1')
        self.assertEqual(rows[2], 'gpu AMD card32 1 4096 16384 0')
        self.assertFalse((self.root / 'nvml.trace').exists())

    def test_unknown_power_never_calls_nvml(self):
        self.nvidia(1)
        self.put('/sys/bus/pci/devices/0000:01:00.0/power/runtime_status', '')
        self.assertEqual(self.run_probe(), ['gpu NVIDIA 0000:01:00.0 0 0 0 1'])
        self.assertFalse((self.root / 'nvml.trace').exists())

    def test_nvml_failures(self):
        self.nvidia(1); self.env['NVML_TEST_FAIL'] = '1'
        self.assertEqual(self.run_probe(), ['gpu NVIDIA 0000:01:00.0 0 0 0 0'])
        self.assertEqual((self.root / 'nvml.trace').read_text(), 'I')

    def test_nvml_memory_failure(self):
        self.nvidia(1); self.env['NVML_TEST_MEMORY_FAIL'] = '1'
        self.assertEqual(self.run_probe(), ['gpu NVIDIA 0000:01:00.0 0 0 0 0'])

    def test_nvml_count_zero(self):
        self.nvidia(1); self.env['NVML_TEST_COUNT'] = '0'
        self.assertEqual(self.run_probe(), ['gpu NVIDIA 0000:01:00.0 0 0 0 0'])

unittest.main(verbosity=2)
