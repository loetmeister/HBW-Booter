#!/bin/sh
# HBW-Booter Build — avr-gcc, 2048 Words / 4 KB Boot-Section (BOOTSZ=00).
# Boot-Section-Start je MCU (= FLASHEND+1-0x1000): 32A/328P/328PB @0x7000 (32 KB), 644P @0xF000
# (64 KB), 1284P @0x1F000 (128 KB). Muss zum BOOT_START im Code passen.
# Fuses (einmalig per ISP): BOOTRST aktiv + BOOTSZ = 2048 Words.
# ATmega328PB: eigenes avr-gcc-Target + eigene Signatur (0x1E9516), fuer den Booter aber
# 328P-kompatibel (USART0/MCUSR/TIFR1 gleiche Adressen, FLASHEND 0x7FFF -> Boot @0x7000).
BIN="/c/Users/marku/AppData/Local/Arduino15/packages/arduino/tools/avr-gcc/7.3.0-atmel3.6.1-arduino7/bin"
FCPU=16000000UL

cd "$(dirname "$0")" || exit 1
for MCU in atmega32 atmega328p atmega328pb atmega644p atmega1284p; do
  case $MCU in
    atmega1284p) BOOT=0x1F000 ;;   # 128 KB Flash: Boot-Section im oberen 64-KB-Bereich
    atmega644p)  BOOT=0xF000  ;;   # 64 KB Flash
    *)           BOOT=0x7000  ;;   # 32 KB Flash
  esac
  "$BIN/avr-gcc.exe" -mmcu=$MCU -DF_CPU=$FCPU -Os -Wall -ffreestanding \
     -Wl,--section-start=.text=$BOOT -o hbw_booter_$MCU.elf hbw_booter.c || exit 1
  "$BIN/avr-objcopy.exe" -O ihex -R .eeprom hbw_booter_$MCU.elf hbw_booter_$MCU.hex
  echo "=== $MCU (Boot @$BOOT) ==="
  "$BIN/avr-size.exe" hbw_booter_$MCU.elf
done
