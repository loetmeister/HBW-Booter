#!/usr/bin/env python3
# Diagnose: welche Ports gibt es, und kommt auf dem gewaehlten Port UEBERHAUPT etwas?
#   python diag.py COM4
import sys, time, serial
from serial.tools import list_ports

print("=== Verfuegbare COM-Ports ===")
found = False
for p in list_ports.comports():
    print(f"   {p.device}   {p.description}   [{p.hwid}]"); found = True
if not found:
    print("   (KEINE Ports gefunden!)")

port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'

def raw_read(parity, label):
    print(f"\n=== {label} auf {port} (5 s) ===")
    try:
        s = serial.Serial(port, 19200, parity=parity, timeout=0.1)
    except Exception as e:
        print("   PORT-FEHLER:", e); return
    time.sleep(2); s.reset_input_buffer()   # DTR-Reset abwarten -> App bootet
    t = time.time(); buf = bytearray()
    while time.time() - t < 5:
        n = s.in_waiting
        if n: buf += s.read(n)
        else: time.sleep(0.02)
    s.close()
    if buf: print(f"   >>> {len(buf)} Bytes empfangen:\n   {bytes(buf).hex(' ')}")
    else:   print("   (nichts)")

raw_read(serial.PARITY_EVEN, "8E1  (so sendet die Firmware)")
raw_read(serial.PARITY_NONE, "8N1  (Fallback, falls der USB-Adapter mit Paritaet zickt)")
