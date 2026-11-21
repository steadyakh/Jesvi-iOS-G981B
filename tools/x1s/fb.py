#!/usr/bin/env python3
"""A minimal fastboot client over libusb, for the Jesvi loader's "Log access (USB)" on Windows.

Windows' fastboot.exe only finds devices that carry Android's USB interface GUID; the loader (18d1:d00d, built on a
Mac) has none, and Windows binds it to its generic WinUSB driver. libusb talks to that driver directly, so nothing in
Windows has to be changed. Needs: pip install pyusb libusb-package.

usage: fb.py getvar NAME | fb.py oem "TEXT" | fb.py cmd "RAW COMMAND" | fb.py get_staged OUT
       fb.py download FILE  (then: fb.py cmd "oem ..." to use it)   fb.py reboot
Every reply line is printed; INFO lines as they come. Exit status 0 on OKAY, 1 on FAIL or no answer.
"""
import sys

import libusb_package
import usb.core
import usb.util

VID, PID = 0x18D1, 0xD00D


class Fastboot:
    def __init__(self, timeout_ms=10000):
        self.dev = usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend())
        if self.dev is None:
            sys.exit('fb: no xnuloader (18d1:d00d) on USB: is the phone in the menu\'s "Log access (USB)"?')
        self.dev.set_configuration()
        intf = self.dev.get_active_configuration()[(0, 0)]
        self.out = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.inp = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        self.timeout = timeout_ms

    def _reply(self):
        while True:
            r = bytes(self.inp.read(4096, self.timeout))
            kind, text = r[:4].decode(errors='replace'), r[4:].decode(errors='replace')
            if kind == 'INFO':
                print('INFO ' + text)
                continue
            return kind, text

    def command(self, text):
        self.out.write(text.encode(), self.timeout)
        return self._reply()

    def download(self, data):
        kind, text = self.command('download:%08x' % len(data))
        if kind != 'DATA':
            return kind, text
        for at in range(0, len(data), 1 << 20):
            self.out.write(data[at:at + (1 << 20)], 60000)
        return self._reply()

    def upload(self):
        kind, text = self.command('upload')
        if kind != 'DATA':
            return kind, text, b''
        size, buf = int(text, 16), bytearray()
        while len(buf) < size:
            buf += bytes(self.inp.read(min(1 << 20, size - len(buf)), 60000))
        kind, text = self._reply()
        return kind, text, bytes(buf)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    fb, op, args = Fastboot(), sys.argv[1], sys.argv[2:]
    if op == 'getvar':
        kind, text = fb.command('getvar:' + args[0])
    elif op == 'oem':
        kind, text = fb.command('oem ' + args[0])
    elif op == 'cmd':
        kind, text = fb.command(args[0])
    elif op == 'reboot':
        kind, text = fb.command('reboot')
    elif op == 'download':
        kind, text = fb.download(open(args[0], 'rb').read())
    elif op == 'get_staged':
        kind, text, data = fb.upload()
        if kind == 'OKAY':
            open(args[0], 'wb').write(data)
            text = '%s (%d bytes to %s)' % (text, len(data), args[0])
    else:
        sys.exit(__doc__)
    print(kind + ' ' + text)
    sys.exit(0 if kind == 'OKAY' else 1)


if __name__ == '__main__':
    main()
