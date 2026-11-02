#!/usr/bin/env python3
"""qwz-moncap.py: read frames captured by AirportWCN7850's monitor mode.

Drains debug.qwz.moncap (see qwz/qwz_mon.h), prints one line per frame of
interest and writes everything to a pcap (radiotap link type) for Wireshark.

  scripts/qwz-moncap.py                  AWDL frames + a count of the rest
  scripts/qwz-moncap.py -v               every frame
  scripts/qwz-moncap.py -o cap.pcap -t 60

AWDL (what AirDrop runs on) shows up as vendor action frames with Apple's OUI
00:17:f2 and data frames with BSSID 00:25:00:ff:94:73; an iPhone sends them
while its share sheet or AirDrop sheet is open, on channels 6, 44 and 149.
"""
import argparse
import ctypes
import struct
import sys
import time

HDR = struct.Struct('<IIHBBIbBH')       # len, ts_lo(us), flags, decap, chan, seq, rssi, rate, ts_hi
SKIP = 0xffffffff
F_RXDMA_ERR, F_TRUNC, F_NO_MPDU, F_CONT, F_DROPCLASS, F_FCS, F_HDR_SYNTH, F_WMI, F_REO = (
    1, 2, 4, 8, 16, 32, 64, 128, 256)
DECAP = {0: 'raw', 1: 'nwifi', 2: 'eth', 3: '8023'}
AWDL_BSSID = bytes.fromhex('002500ff9473')
APPLE_OUI = bytes.fromhex('0017f2')

libc = ctypes.CDLL(None, use_errno=True)
libc.sysctlbyname.argtypes = [ctypes.c_char_p, ctypes.c_void_p,
                              ctypes.POINTER(ctypes.c_size_t),
                              ctypes.c_void_p, ctypes.c_size_t]


def drain(buf):
    size = ctypes.c_size_t(len(buf))
    rc = libc.sysctlbyname(b'debug.qwz.moncap', buf, ctypes.byref(size), None, 0)
    if rc != 0:
        err = ctypes.get_errno()
        sys.exit(f'sysctl debug.qwz.moncap failed: errno {err} '
                 f'(is AirportWCN7850.kext with monitor support loaded?)')
    return bytes(buf[:size.value])


def records(blob):
    off = 0
    while off + HDR.size <= len(blob):
        length, ts_lo, flags, decap, chan, seq, rssi, rate, ts_hi = HDR.unpack_from(blob, off)
        ts = (ts_hi << 32 | ts_lo) / 1000.0   # milliseconds, from microseconds
        if length == SKIP:
            break
        off += HDR.size
        frame = blob[off:off + length]
        off += (length + 3) & ~3
        yield length, ts, flags, decap, chan, seq, rssi, rate, frame


def mac(b):
    return ':'.join(f'{x:02x}' for x in b)


TYPES = {0: 'mgmt', 1: 'ctrl', 2: 'data', 3: 'ext'}
MGMT = {0: 'assoc-req', 1: 'assoc-resp', 2: 'reassoc-req', 3: 'reassoc-resp',
        4: 'probe-req', 5: 'probe-resp', 8: 'beacon', 9: 'atim', 10: 'disassoc',
        11: 'auth', 12: 'deauth', 13: 'action', 14: 'action-noack'}
CTRL = {8: 'bar', 9: 'ba', 10: 'ps-poll', 11: 'rts', 12: 'cts', 13: 'ack',
        14: 'cf-end', 4: 'bf-report-poll', 5: 'vht-ndpa', 2: 'trigger'}


def describe(frame, flags):
    """(is_awdl, text) for a frame starting at the 802.11 header."""
    if len(frame) < 10:
        return False, f'short ({len(frame)} B)'
    fc0, fc1 = frame[0], frame[1]
    ftype, sub = (fc0 >> 2) & 3, fc0 >> 4
    tds, fds = fc1 & 1, (fc1 >> 1) & 1
    a1 = frame[4:10]
    a2 = frame[10:16] if len(frame) >= 16 else b''
    a3 = frame[16:22] if len(frame) >= 22 else b''
    name = TYPES.get(ftype, '?')
    if ftype == 0:
        name = MGMT.get(sub, f'mgmt-{sub}')
    elif ftype == 1:
        name = CTRL.get(sub, f'ctrl-{sub}')
    elif ftype == 2:
        name = 'qos-data' if sub & 8 else 'data'
        if sub & 4:
            name += '-null'
    awdl = False
    extra = ''
    if ftype == 0 and sub in (13, 14) and len(frame) >= 28:
        cat = frame[24]
        if cat == 127 and frame[25:28] == APPLE_OUI:
            awdl = True
            t = frame[28] if len(frame) > 28 else -1
            sub_t = frame[30] if len(frame) > 30 else -1     # byte 29 is the version
            kind = {0: 'PSF', 3: 'MIF'}.get(sub_t, f'subtype {sub_t}')
            extra = f' AWDL {kind} (type {t})'
        else:
            extra = f' category {cat}'
    if a3 == AWDL_BSSID or (ftype == 2 and not tds and not fds and a3 == AWDL_BSSID):
        awdl = True
        extra += ' bssid=AWDL'
    fl = []
    if flags & F_RXDMA_ERR:
        fl.append('rxdma-err')
    if flags & F_DROPCLASS:
        fl.append('dropclass')
    if flags & F_TRUNC:
        fl.append('trunc')
    if flags & F_NO_MPDU:
        fl.append('no-mpdu-start')
    if flags & F_CONT:
        fl.append('cont')
    if flags & F_WMI:
        fl.append('wmi')
    if flags & F_REO:
        fl.append('reo')
    text = f'{name:12s} a1={mac(a1)}'
    if a2:
        text += f' a2={mac(a2)}'
    if a3:
        text += f' a3={mac(a3)}'
    text += extra
    if fl:
        text += ' [' + ','.join(fl) + ']'
    return awdl, text


def radiotap(chan, flags, rssi, rate):
    freq = 2407 + 5 * chan if chan <= 13 else (2484 if chan == 14 else 5000 + 5 * chan)
    cflags = 0x0080 if chan <= 14 else 0x0100       # CCK/2 GHz vs OFDM/5 GHz
    rflags = 0x10 if flags & F_FCS else 0            # FCS at end
    if flags & F_WMI:                                # flags, rate, channel, antenna signal
        present = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5)
        return struct.pack('<BBHIBBHHb', 0, 0, 15, present, rflags, rate, freq, cflags, rssi)
    present = (1 << 1) | (1 << 3)                    # flags, channel
    return struct.pack('<BBHIBBHH', 0, 0, 14, present, rflags, 0, freq, cflags)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-o', '--out', default='qwz-moncap.pcap', help='pcap to write')
    ap.add_argument('-t', '--seconds', type=float, default=0, help='stop after this long (0: until Ctrl-C)')
    ap.add_argument('-v', '--verbose', action='store_true', help='print every frame, not only AWDL')
    ap.add_argument('-i', '--interval', type=float, default=0.2, help='poll interval')
    args = ap.parse_args()

    buf = ctypes.create_string_buffer(1024 * 1024)
    pcap = open(args.out, 'wb')
    pcap.write(struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 127))
    t0 = time.time()
    total = awdl_n = other = 0
    last_report = t0
    print(f'draining debug.qwz.moncap into {args.out}; Ctrl-C to stop', flush=True)
    try:
        while True:
            blob = drain(buf)
            for length, ts, flags, decap, chan, seq, rssi, rate, frame in records(blob):
                total += 1
                is_awdl, text = describe(frame, flags)
                if flags & F_WMI:
                    text += f' {rssi} dBm {rate / 2:g} Mb/s'
                now = time.time()
                rt = radiotap(chan, flags, rssi, rate)
                pcap.write(struct.pack('<IIII', int(now), int((now % 1) * 1e6),
                                       len(rt) + length, len(rt) + length))
                pcap.write(rt)
                pcap.write(frame)
                if is_awdl:
                    awdl_n += 1
                else:
                    other += 1
                if is_awdl or args.verbose:
                    print(f'{ts/1000:9.3f} #{seq:<6d} ch{chan:<3d} {length:4d}B '
                          f'{DECAP.get(decap, decap):5s} {text}', flush=True)
            pcap.flush()
            now = time.time()
            if not args.verbose and now - last_report >= 5:
                print(f'  ... {total} frames so far ({awdl_n} AWDL, {other} other)', flush=True)
                last_report = now
            if args.seconds and now - t0 >= args.seconds:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    pcap.close()
    print(f'{total} frames written to {args.out}: {awdl_n} AWDL, {other} other')


if __name__ == '__main__':
    main()
