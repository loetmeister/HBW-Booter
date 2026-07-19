#!/usr/bin/env python3
# merge_hex.py — fuegt eine App-.hex und die Booter-.hex zu EINER combined.hex zusammen.
# Diese combined.hex wird EINMALIG per ISP (AVRDUDESS/avrdude) geflasht; danach laeuft
# jedes weitere Update ueber den Bus (flash_tool.py oder die native CCU).
#
#   python merge_hex.py <app>.hex <booter>.hex <out>.hex
#   z.B.  python merge_hex.py hbw_testapp_1284p.hex hbw_booter_atmega1284p.hex hbw_combined_1284p.hex
#
# Prueft, dass App und Booter sich NICHT ueberlappen (sonst wuerde die App in die
# Boot-Section ragen bzw. der Booter Teile der App ueberschreiben). Schreibt korrekte
# Extended-Linear-Address-Records (Typ 04) -- noetig, weil der 1284P-Booter bei 0x1F000
# (> 64 KB) liegt und die 16-Bit-Adressfelder allein das nicht ausdruecken koennen.
import sys

def parse_hex(path):
    mem = {}; ext = 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(':'): continue
        ll = int(line[1:3],16); a = int(line[3:7],16); tt = int(line[7:9],16); d = line[9:9+ll*2]
        if tt == 0:
            for i in range(ll): mem[ext+a+i] = int(d[i*2:i*2+2],16)
        elif tt == 4: ext = int(d,16) << 16     # Extended Linear Address
        elif tt == 2: ext = int(d,16) << 4      # Extended Segment Address
    return mem

def emit(rec_type, addr, data):
    body = [len(data), (addr>>8)&0xFF, addr&0xFF, rec_type] + list(data)
    chk = (-sum(body)) & 0xFF
    return ':' + ''.join('%02X'%b for b in body) + '%02X'%chk

def write_hex(mem, path):
    addrs = sorted(mem)
    lines = []; cur_upper = None
    i, n = 0, len(addrs)
    while i < n:
        start = addrs[i]
        seg = [mem[start]]; j = i + 1
        # Lauf aus bis zu 16 zusammenhaengenden Bytes in derselben 64-KB-Bank
        while (j < n and len(seg) < 16
               and addrs[j] == addrs[j-1] + 1
               and (addrs[j] >> 16) == (start >> 16)):
            seg.append(mem[addrs[j]]); j += 1
        upper = start >> 16
        if upper != cur_upper:
            lines.append(emit(4, 0, [(upper>>8)&0xFF, upper&0xFF]))   # neue obere 16 Bit
            cur_upper = upper
        lines.append(emit(0, start & 0xFFFF, seg))
        i = j
    lines.append(':00000001FF')                                       # EOF
    open(path, 'w').write('\n'.join(lines) + '\n')

def main(argv):
    if len(argv) != 4:
        print(__doc__); sys.exit(2)
    app_path, boot_path, out_path = argv[1:4]
    app  = parse_hex(app_path)
    boot = parse_hex(boot_path)

    overlap = set(app) & set(boot)
    if overlap:
        lo, hi = min(overlap), max(overlap)
        print(f"FEHLER: App und Booter ueberlappen ({len(overlap)} Bytes, 0x{lo:05X}..0x{hi:05X}).")
        print("  -> App zu gross oder Booter-Startadresse falsch. Abbruch, nichts geschrieben.")
        sys.exit(1)

    app_max, boot_min = max(app), min(boot)
    if app_max >= boot_min:
        print(f"FEHLER: App reicht bis 0x{app_max:05X}, Booter beginnt bei 0x{boot_min:05X} -> Kollision.")
        sys.exit(1)

    write_hex({**app, **boot}, out_path)
    print(f"OK: {out_path}")
    print(f"  App    0x{min(app):05X}..0x{app_max:05X}  ({len(app)} B)")
    print(f"  Booter 0x{boot_min:05X}..0x{max(boot):05X}  ({len(boot)} B)")
    print(f"  Luecke App->Booter: {boot_min-app_max-1} B frei")

if __name__ == '__main__':
    main(sys.argv)
