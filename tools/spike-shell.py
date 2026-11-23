#!/usr/bin/env python3
"""Run shell commands on the Mi Pad 3 mainline spike over its USB serial gadget.

The spike's init keeps a shell on ttyGS0; the host sees it as a /dev/ttyACM* whose
USB vendor is 0525 (Linux g_serial). Only that tty is opened, never the Z-Wave stick
on ttyACM0. Needs root (sudo) for the tty.

usage: spike-shell.py [--wait SEC] [--save DIR] [--timeout SEC] [--upload FILE TARGET] [CMD...]
  Each CMD's output goes to DIR/<n>.txt with --save, else to stdout.
  --upload sends FILE to the spike, which writes it to TARGET ("recovery" for the eMMC
  recovery partition, or a path under /tmp) and reports a CRC-32 of what it wrote and of
  what reads back; both must match the file. Exit status 3 if they do not.
  A CMD given with a leading "!" (or one of the plain reboot commands) is sent without
  waiting for output: use it for commands that restart the tablet.
"""
import argparse
import glob
import os
import re
import select
import subprocess
import sys
import termios
import time
import zlib


def find_tty(wait):
    end = time.time() + wait
    while True:
        for dev in sorted(glob.glob('/dev/ttyACM*')):
            props = subprocess.run(['udevadm', 'info', '-q', 'property', '-n', dev],
                                   capture_output=True, text=True).stdout
            if 'ID_VENDOR_ID=0525' in props.splitlines():
                return dev
        if time.time() >= end:
            return None
        time.sleep(1)


def open_raw(dev):
    # O_NONBLOCK: a plain open() of an ACM tty waits for carrier, which never comes
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attr = termios.tcgetattr(fd)
    attr[0] = 0                      # iflag
    attr[1] = 0                      # oflag
    attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attr[3] = 0                      # lflag: no echo, no canonical mode on our side
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    os.set_blocking(fd, True)
    return fd


def drain(fd, quiet=0.3):
    buf = b''
    while True:
        r, _, _ = select.select([fd], [], [], quiet)
        if not r:
            return buf
        chunk = os.read(fd, 65536)
        if not chunk:
            return buf
        buf += chunk


def run(fd, cmd, timeout):
    tag = 'CAPPU_DONE_%d' % time.time_ns()
    # split the marker in the command so the tty echo of the command never matches
    a, b = tag[:6], tag[6:]
    os.write(fd, ('%s; echo "%s""%s"\n' % (cmd, a, b)).encode())
    buf = b''
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            buf += os.read(fd, 65536)
            idx = buf.find(tag.encode())
            if idx >= 0:
                out = buf[:idx].decode(errors='replace')
                # drop the echoed command line, if the remote tty echoes
                first, _, rest = out.partition('\n')
                return (rest if a in first else out).replace('\r', '')
    raise TimeoutError('no end marker for %r within %ds' % (cmd, timeout))


def upload(fd, path, target):
    data = open(path, 'rb').read()
    os.write(fd, ('@recv %d %s\n' % (len(data), target)).encode())
    buf = b''
    end = time.time() + 10
    while b'READY\n' not in buf:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            buf += os.read(fd, 4096)
        if b'recv:' in buf or b'not found' in buf or time.time() > end:
            raise RuntimeError('spike did not accept the upload: %r' % buf[-200:])
    t0 = time.time()
    off = 0
    while off < len(data):
        off += os.write(fd, data[off:off + 65536])
    buf = b''
    end = time.time() + 120
    while b'read back' not in buf or not buf.endswith(b'\n'):
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            buf += os.read(fd, 4096)
        if time.time() > end:
            raise TimeoutError('no result from the spike after the upload: %r' % buf[-200:])
    # the reply line, or the same result as the kernel log echoes it on this console
    text = buf.decode(errors='replace')
    line = [l for l in text.splitlines() if 'read back' in l][-1]
    want = '%08x' % (zlib.crc32(data) & 0xffffffff)
    m = re.search(r'crc(?:32)? ([0-9a-f]{8}), read back ([0-9a-f]{8})', line)
    ok = bool(m) and m.group(1) == want and m.group(2) == want
    print('[upload] %s (%.1f s, expected crc32 %s): %s' %
          (line.strip(), time.time() - t0, want, 'OK' if ok else 'MISMATCH'), file=sys.stderr)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--wait', type=int, default=60, help='seconds to wait for the gadget tty')
    ap.add_argument('--timeout', type=int, default=60, help='seconds per command')
    ap.add_argument('--save', help='directory for per-command output files')
    ap.add_argument('--upload', nargs=2, metavar=('FILE', 'TARGET'))
    ap.add_argument('cmds', nargs='*')
    args = ap.parse_args()

    dev = find_tty(args.wait)
    if not dev:
        print('no 0525 gadget tty within %ds' % args.wait, file=sys.stderr)
        return 2
    fd = open_raw(dev)
    os.write(fd, b'\n')
    drain(fd, 1.0)
    if args.save:
        os.makedirs(args.save, exist_ok=True)
    # Cancel the dead-man reboot. Retried: the first line after the port opens has been
    # lost at least once (2026-11-22), and then nothing answers until the reboot.
    for attempt in range(6):
        try:
            run(fd, 'touch /tmp/host-attached', 5)
            break
        except TimeoutError:
            os.write(fd, b'\n')
            drain(fd, 0.5)
    else:
        print('the spike does not answer on %s' % dev, file=sys.stderr)
        return 2
    if args.upload:
        try:
            if not upload(fd, args.upload[0], args.upload[1]):
                return 3
        except (RuntimeError, TimeoutError) as e:
            print('[upload] failed: %s' % e, file=sys.stderr)
            return 3
    for n, cmd in enumerate(args.cmds, 1):
        # A command that reboots right away is sent without waiting for output:
        # the known reboot commands, or any command given with a leading "!".
        if cmd.startswith('!') or cmd.strip() in ('reboot', 'reboot -f',
                                                  'echo b > /proc/sysrq-trigger'):
            cmd = cmd.lstrip('!')
            os.write(fd, (cmd + '\n').encode())
            print('[%s] sent: %s' % (dev, cmd), file=sys.stderr)
            continue
        out = run(fd, cmd, args.timeout)
        if args.save:
            with open(os.path.join(args.save, '%02d.txt' % n), 'w') as f:
                f.write('$ %s\n%s' % (cmd, out))
        else:
            sys.stdout.write('$ %s\n%s' % (cmd, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
