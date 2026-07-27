# Arduino-IDE Custom-Boards (HBW-Booter)

Fertige `boards.local.txt` für die Arduino IDE / arduino-cli. Damit erscheinen die HBW-Booter-Boards
in der Board-Auswahl; *Werkzeuge → Bootloader brennen* brennt dann den Booter **plus die Fuses** per
ISP, und `-D_HAS_BOOTLOADER_` ist beim Kompilieren automatisch gesetzt. Details + Ablauf im
Haupt-[README](../README.md#arduino-ide-eigenes-board-mit-dem-hbw-booter).

## Installation

Jede Datei als `boards.local.txt` in den **jeweiligen Core-Ordner** kopieren und die zugehörige
Booter-`.hex` (aus dem Repo-Root) nach `<core>/bootloaders/hbw/` legen:

| Datei hier | Zielordner (`boards.local.txt`) | Boards | Booter-`.hex` → `bootloaders/hbw/` |
|---|---|---|---|
| `arduino-avr.boards.local.txt` | `…/packages/arduino/hardware/avr/<v>/` | `hbwnano` (328P) | `hbw_booter_atmega328p.hex` |
| `MightyCore.boards.local.txt` | `…/packages/MightyCore/hardware/avr/<v>/` | `hbw32`, `hbw644`, `hbw1284` | `hbw_booter_atmega32.hex`, `_atmega644p.hex`, `_atmega1284p.hex` |
| `MiniCore.boards.local.txt` | `…/packages/MiniCore/hardware/avr/<v>/` | `hbw328pb` | `hbw_booter_atmega328pb.hex` |

`<v>` = installierte Core-Version (z. B. MightyCore `3.1.0`, MiniCore `3.1.2`). Nach dem Kopieren die
IDE neu starten. **`boards.local.txt` geht bei einem Core-Update verloren** — dann diese Datei erneut
kopieren.

## Wichtig

- **„Bootloader brennen" ist der letzte ISP-Schritt.** Danach kommt der Sketch nur noch über den Bus
  (Gateway `/flash`), **nie** per „Hochladen mit Programmer" — das macht einen Chip-Erase und löscht
  den Booter wieder.
- Die Fuses setzen den **Oszillator zwangsweise mit** (16-MHz-Quarz vorausgesetzt). Beim **32A** ist
  `CKOPT=0` (`hfuse=0xC0`) Pflicht für 16 MHz — `0x98` (CKOPT=1) reicht nur bis ~8 MHz.
- Der **328PB** braucht **avrdude ≥ 7.0** (Signatur `0x1E9516`); die avrdude 8.0 aus
  Arduino/MightyCore/MiniCore hat sie.
