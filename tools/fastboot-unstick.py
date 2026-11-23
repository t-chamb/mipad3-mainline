#!/usr/bin/env python3
"""Un-stick the Mi Pad 3 bootloader's fastboot after a download that stalled mid-transfer.

LK keeps waiting for the rest of the announced data. Feed it filler bytes on the bulk OUT
endpoint until it answers on the IN endpoint; nothing is written to flash by a download
alone, so the filler is harmless. usage (root): fastboot-unstick.py [SERIAL]
"""
import sys
import usb.core
import usb.util

serial = sys.argv[1] if len(sys.argv) > 1 else None
dev = None
for d in usb.core.find(find_all=True, idVendor=0x0bb4, idProduct=0x0c01):
    if serial is None or usb.util.get_string(d, d.iSerialNumber) == serial:
        dev = d
        break
if dev is None:
    sys.exit('no fastboot device')
try:
    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
except Exception:
    pass
usb.util.claim_interface(dev, 0)

def read_reply(timeout):
    try:
        return bytes(dev.read(0x81, 64, timeout)).decode(errors='replace')
    except usb.core.USBTimeoutError:
        return None

r = read_reply(200)
print('pending reply:', r)
sent = 0
chunk = bytes(64 * 1024)
while r is None and sent < 24 * 1024 * 1024:
    try:
        n = dev.write(0x01, chunk, 2000)
    except usb.core.USBTimeoutError:
        print('write timed out after %d bytes: the bootloader is not reading' % sent)
        break
    sent += n
    r = read_reply(20)
print('sent %d filler bytes, reply: %r' % (sent, r))
if r is not None:
    dev.write(0x01, b'getvar:product', 2000)
    print('getvar:product ->', read_reply(2000))
