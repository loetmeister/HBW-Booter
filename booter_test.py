#!/usr/bin/env python3
# HBW-Booter Meilenstein-1-Test (328P/Nano ueber USB-Serial, 8E1 @19200).
# Sendet z z u an die Test-App und dekodiert die Antworten.
# Erwartung: App-ACK auf 'u', dann StartupReason + Announce vom BOOTER.
#   Aufruf:  python booter_test.py COM4
import sys, time, serial   # pip install pyserial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
POLY = 0x1002

def crc16(frame):                      # frame = unescaped bytes inkl. FD, OHNE crc
    crc = 0xFFFF
    for b in bytes(frame) + b'\x00\x00':
        for _ in range(8):
            hi = crc & 0x8000
            crc = (crc << 1) & 0xFFFF
            if b & 0x80: crc |= 1
            if hi: crc ^= POLY
            b = (b << 1) & 0xFF
    return crc

def build(target, control, sender, data):
    body = bytearray(target.to_bytes(4, 'big')); body.append(control)
    if control & 8: body += sender.to_bytes(4, 'big')
    body.append(len(data) + 2); body += bytes(data)
    crc = crc16(b'\xfd' + bytes(body))
    body += bytes([(crc >> 8) & 0xFF, crc & 0xFF])
    out = bytearray([0xFD])
    for b in body:
        out += (bytes([0xFC, b & 0x7F]) if b in (0xFC, 0xFD, 0xFE) else bytes([b]))
    return bytes(out)

def decode(seg):
    raw = bytearray(); esc = False
    for b in seg:
        if b == 0xFC: esc = True; continue
        if esc: b |= 0x80; esc = False
        raw.append(b)
    if len(raw) < 7: return None
    ctrl = raw[4]; isInfo = (ctrl & 1) == 0; isAck = (ctrl & 7) == 1
    hasS = (ctrl & 8) if (isInfo or isAck) else 0
    hdr = 10 if hasS else 6
    if len(raw) < hdr: return None
    tgt = int.from_bytes(raw[0:4], 'big'); snd = int.from_bytes(raw[5:9], 'big') if hasS else 0
    wl = raw[hdr-1]; dl = wl - 2 if wl >= 2 else 0; data = bytes(raw[hdr:hdr+dl])
    if   data and data[0] == 0x41: tag = 'ANNOUNCE'
    elif data and data[0] == 0xFF: tag = 'StartupReason (Booter!)'
    elif isAck and dl == 0:        tag = 'ACK'
    elif data:                     tag = "cmd '%c'" % data[0] if 32 <= data[0] < 127 else 'cmd %02X' % data[0]
    else:                          tag = '?'
    return "   <- tgt=%08X ctrl=%02X snd=%08X data=[%s]  %s" % (tgt, ctrl, snd, data.hex(' '), tag)

def drain(s, secs, label):
    print(label)
    t = time.time(); buf = bytearray()
    while time.time() - t < secs:
        n = s.in_waiting
        if n: buf += s.read(n)
        else: time.sleep(0.02)
    got = False
    for seg in bytes(buf).split(b'\xfd')[1:]:
        d = decode(seg)
        if d: print(d); got = True
    if not got: print("   (nichts empfangen)")

s = serial.Serial(PORT, 19200, parity=serial.PARITY_EVEN, timeout=0.1)
time.sleep(2)                          # Nano resettet beim Oeffnen (DTR) -> App bootet
s.reset_input_buffer()
drain(s, 3.0, "# App-Phase (periodischer Announce erwartet):")

print("\n# --> sende  z  z  u")
s.write(build(0xFFFFFFFF, 0x98, 0x00000001, [0x7A])); time.sleep(0.05)  # z
s.write(build(0xFFFFFFFF, 0x9C, 0x00000001, [0x7A])); time.sleep(0.05)  # z
s.write(build(0x42FFFFFF, 0x18, 0x00000001, [0x75]))                    # u

drain(s, 3.0, "\n# Nach u  (ACK von App -> Reset -> StartupReason+Announce vom BOOTER):")
s.close()
