#!/bin/sh
# HBW-Booter Build — avr-gcc, Boot-Section @0x7000 (2048 Words / 4 KB Boot).
# Fuses (einmalig per ISP): BOOTRST aktiv + BOOTSZ = 2048 Words.
BIN="/c/Users/marku/AppData/Local/Arduino15/packages/arduino/tools/avr-gcc/7.3.0-atmel3.6.1-arduino7/bin"
BOOT=0x7000
FCPU=16000000UL

cd "$(dirname "$0")" || exit 1
for MCU in atmega32 atmega328p; do
  "$BIN/avr-gcc.exe" -mmcu=$MCU -DF_CPU=$FCPU -Os -Wall -ffreestanding \
     -Wl,--section-start=.text=$BOOT -o hbw_booter_$MCU.elf hbw_booter.c || exit 1
  "$BIN/avr-objcopy.exe" -O ihex -R .eeprom hbw_booter_$MCU.elf hbw_booter_$MCU.hex
  echo "=== $MCU (Boot @$BOOT) ==="
  "$BIN/avr-size.exe" hbw_booter_$MCU.elf
done
