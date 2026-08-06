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
  # STACKTOP = RAMEND-4: Stack beginnt UNTER den obersten 4 RAM-Byte, die als Boot-Marker
  # dienen (bootmagic.h). So killt weder der C-Startup noch der Booter-Stack die Marker-Zelle,
  # bevor main() sie liest. RAMEND je MCU: 32A=0x085F, 328P/PB=0x08FF, 644P=0x10FF, 1284P=0x40FF.
  case $MCU in
    atmega1284p) BOOT=0x1F000; STACKTOP=0x40FB ;;   # 128 KB Flash, 16 KB SRAM
    atmega644p)  BOOT=0xF000;  STACKTOP=0x10FB ;;   # 64 KB Flash, 4 KB SRAM
    atmega32)    BOOT=0x7000;  STACKTOP=0x085B ;;   # 32 KB Flash, 2 KB SRAM (RAMEND 0x085F!)
    *)           BOOT=0x7000;  STACKTOP=0x08FB ;;   # 328P/328PB: 32 KB Flash, 2 KB SRAM
  esac
  "$BIN/avr-gcc.exe" -mmcu=$MCU -DF_CPU=$FCPU -Os -Wall -ffreestanding \
     -Wl,--section-start=.text=$BOOT -Wl,--defsym=__stack=$STACKTOP \
     -o hbw_booter_$MCU.elf hbw_booter.c || exit 1
  "$BIN/avr-objcopy.exe" -O ihex -R .eeprom hbw_booter_$MCU.elf hbw_booter_$MCU.hex
  echo "=== $MCU (Boot @$BOOT) ==="
  "$BIN/avr-size.exe" hbw_booter_$MCU.elf
done
