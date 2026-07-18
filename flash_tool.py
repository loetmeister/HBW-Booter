#!/usr/bin/env python3
# HBW-Booter Flash-Tool — flasht eine .hex ueber den Bus/USB (Sender-Seite, Meilenstein 2).
# Fahrt: z z -> u (Booter-Einstieg) -> p -> w-Schleife ueber die .hex -> g (App-Start).
#   python flash_tool.py COM12 hbw_testapp_v5_328p.hex
import sys, time, serial

POLY, DEV, CENTRAL = 0x1002, 0x42FFFFFF, 0x00000001
BLK = 64                                   # Bytes pro w-Block

def crc16(frame):
    crc = 0xFFFF
    for b in bytes(frame) + b'\x00\x00':
        for _ in range(8):
            hi = crc & 0x8000; crc = (crc << 1) & 0xFFFF
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
    wl = raw[hdr-1]; dl = wl - 2 if wl >= 2 else 0
    return ctrl, bytes(raw[hdr:hdr+dl])

def parse_hex(path):
    mem = {}; ext = 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(':'): continue
        ll = int(line[1:3],16); a = int(line[3:7],16); tt = int(line[7:9],16); d = line[9:9+ll*2]
        if tt == 0:
            for i in range(ll): mem[ext+a+i] = int(d[i*2:i*2+2],16)
        elif tt == 4: ext = int(d,16) << 16
        elif tt == 2: ext = int(d,16) << 4
    return mem

def collect(s, secs):
    t = time.time(); buf = bytearray()
    while time.time()-t < secs:
        n = s.in_waiting
        if n: buf += s.read(n)
        else: time.sleep(0.01)
    return [d for d in (decode(seg) for seg in bytes(buf).split(b'\xfd')[1:]) if d]

def send_ack(s, frame, tries=4, secs=0.5):
    for _ in range(tries):
        s.write(frame)
        t = time.time(); buf = bytearray()
        while time.time()-t < secs:
            n = s.in_waiting
            if n:
                buf += s.read(n)
                for seg in bytes(buf).split(b'\xfd')[1:]:
                    d = decode(seg)
                    if d and (d[0] & 7) == 1: return True
            else: time.sleep(0.005)
    return False

def appcrc(data):                          # CRC16 (0x1002) ueber die App-Bytes = identisch zu appCrc() im Booter
    crc = 0xFFFF
    for b in data:
        for _ in range(8):
            hi = crc & 0x8000; crc = (crc << 1) & 0xFFFF
            if b & 0x80: crc |= 1
            if hi: crc ^= POLY
            b = (b << 1) & 0xFF
    return crc

def read_flash(s, addr, n, tries=4, secs=0.5):
    frame = build(DEV, 0x18, CENTRAL, [0x72, (addr>>8)&0xFF, addr&0xFF, n])   # r
    for _ in range(tries):
        s.write(frame)
        t = time.time(); buf = bytearray()
        while time.time()-t < secs:
            m = s.in_waiting
            if m:
                buf += s.read(m)
                for seg in bytes(buf).split(b'\xfd')[1:]:
                    d = decode(seg)
                    if d and (d[0] & 7) == 1 and len(d[1]) >= n:
                        return bytes(d[1][:n])            # r-Antwort = ACK-Frame, Payload = die n Flash-Bytes (kein cmd/addr-Echo)
            else: time.sleep(0.005)
    return None

port, hexfile = sys.argv[1], sys.argv[2]
mem = parse_hex(hexfile); maxa = max(mem)
print(f"Flashe {hexfile}: {len(mem)} Bytes, 0x0000..0x{maxa:04X}")

s = serial.Serial(port, 19200, parity=serial.PARITY_EVEN, timeout=0.1)
time.sleep(2); s.reset_input_buffer()

print("z z u  (Booter-Einstieg) ...")
s.write(build(0xFFFFFFFF, 0x98, CENTRAL, [0x7A])); time.sleep(0.05)   # z
s.write(build(0xFFFFFFFF, 0x9C, CENTRAL, [0x7A])); time.sleep(0.05)   # z
s.write(build(DEV, 0x18, CENTRAL, [0x75]))                           # u
time.sleep(1.5)
if any(d[1][:1] == b'\xff' for d in collect(s, 0.5)):
    print("  -> Booter aktiv (StartupReason)")
else:
    print("  !! kein StartupReason — Booter evtl. nicht aktiv, versuche trotzdem weiter")

print("p  (Blockgroesse) ...")
s.write(build(DEV, 0x18, CENTRAL, [0x70]))
for d in collect(s, 0.5):
    if d[1][:1] == b'p': print(f"  -> Booter meldet Blockgroesse {d[1][1]}")

print(f"w  (schreibe {((maxa)//BLK)+1} Bloecke, Page 0 zuletzt) ", end='', flush=True)
allb = list(range(0, maxa+1, BLK))
for base in [b for b in allb if b >= 128] + [b for b in allb if b < 128]:  # Page 0 (Reset-Vektor) ZULETZT
    chunk = bytes(mem.get(base+i, 0xFF) for i in range(min(BLK, maxa+1-base)))
    payload = [0x77, (base>>8)&0xFF, base&0xFF, len(chunk)] + list(chunk)
    if not send_ack(s, build(DEV, 0x18, CENTRAL, payload)):
        print(f"\n  !! FEHLER: kein ACK @0x{base:04X}"); s.close(); sys.exit(1)
    print(".", end='', flush=True)
print(" ok")

print("v  (Verify: zuruecklesen + vergleichen) ", end='', flush=True)
err = 0
for base in range(0, maxa+1, BLK):
    exp = bytes(mem.get(base+i, 0xFF) for i in range(min(BLK, maxa+1-base)))
    got = read_flash(s, base, len(exp))
    if got != exp:
        err += 1
        print(f"\n  !! Mismatch @0x{base:04X}: exp={len(exp)}B got={len(got) if got is not None else 'None'}B  "
              f"{exp[:8].hex()}.. / {(got[:8].hex() if got else '--')}..")
    else:
        print(".", end='', flush=True)
if err:
    print(f"\n  {err} Verify-Fehler -> Abbruch (kein g, App bleibt ungestartet)"); s.close(); sys.exit(1)
print(" ok")

appbytes = bytes(mem.get(i, 0xFF) for i in range(maxa+1))
c = appcrc(appbytes)
print(f"g  (CRC {c:04X} + App starten) ...")
s.write(build(DEV, 0x18, CENTRAL, [0x67, (len(appbytes)>>8)&0xFF, len(appbytes)&0xFF, (c>>8)&0xFF, c&0xFF]))
time.sleep(1.0)
ok = False
for d in collect(s, 3.0):
    if d[1][:1] == b'\x41' and len(d[1]) >= 6:
        ver = (d[1][4] << 8) | d[1][5]
        flag = "  <== NEUE FIRMWARE laeuft!" if ver == 0x0005 else ""
        print(f"  ANNOUNCE FW={ver:04X}{flag}"); ok = True
if not ok: print("  (kein Announce empfangen)")
s.close()
