# HBW-Booter — Over-the-Bus-Firmware-Update für HMW/HBWired-Geräte

Eigener Bootloader für ATmega32A / ATmega328P, mit dem sich HomeMatic-Wired-Eigenbau-Geräte
(HBWired) **über den RS485-Bus bzw. seriell flashen** lassen — ohne ISP, ohne Ausbau. Der Booter
wird **einmalig** per ISP eingespielt; danach läuft jedes weitere Firmware-Update über die Leitung.

**Stand: abgeschlossen.** Ein **eigenes** Gerät (HBW-IO-4-FM, App v3.04) wurde **komplett über die
echte CCU-WebUI** per Bus geflasht — die CCU meldet selbst **„Firmware-Update erfolgreich"**, der
Verify läuft vollständig durch (inkl. Versionsfeld `@0x6FF0`), und das Gerät bootet in die neue
Firmware und meldet `deviceType 0x10` / `firmware_version 3.04` (2026-07-18, HW-verifiziert per
Screenshot + Gateway-Log). Booter-Einstieg, echtes Flashen und alle Absicherungen sind auf ATmega328P
(Arduino Nano) hardware-verifiziert. Der volle Ablauf
`z z → u → u → p → w`-Schleife `→ p → r`-Verify `→ g → Z Z → h/v/Announce` ist aus der OpenCCU-Quelle
nachgebaut (siehe [CCU-Update über die native hs485d](#ccu-update-über-die-native-hs485d)).
Für eigene Geräte mit `.hex` unter `0x7000` flasht der Prozess vollständig; die eq3-Original-Images
bleiben bewusst unvollständig (Boot-Section-Kollision, siehe unten).

---

## Hintergrund

- **eq3-Original-Geräte können es (mit ihrer Firmware) nicht:** Die Disassembly der Original-
  Firmware `hmw_sen_sc_12_dr_hw0.hex` (v3.01, ATmega32A) zeigt: kein `u`/START_BOOTER-Handler,
  kein Watchdog-Reset, kein Sprung in die Boot-Section. Die App kann schlicht nicht in einen
  Bootloader wechseln → Over-the-Bus-Update ist mit dieser Firmware unmöglich (unabhängig von
  Gateway/CCU).
- **Eigene HBWired-Geräte können es:** Sie brauchen (a) diesen Booter in der Boot-Section und
  (b) einen `u`-Handler in der App, der einen Watchdog-Reset auslöst.

## Funktionsprinzip

Der Einstieg in den Booter hängt an der **Reset-Quelle**, nicht an einem Timing-Fenster:

```
Power-on / Brown-out / externer Reset ─▶ App starten (jmp 0)
Watchdog-Reset (von der App per 'u') ─▶ im Booter bleiben  ─▶ Update-Modus
```

Der Booter liegt in der Boot-Section (2048 Words / 4 KB, `BOOTSZ=00`) — beim **32A/328P ab 0x7000**
(App 0x0000–0x6FFF), beim **644P ab 0xF000**, beim **1284P ab 0x1F000** (App unter 0x10000, siehe
unten). Beim Reset läuft
dank `BOOTRST` zuerst der Booter, liest das Reset-Flag `MCUSR`, und entscheidet App-Start oder
Update-Modus.

Auf dem **ATmega1284P** (128 KB Flash) bleibt die App **unter 64 KB** (`< 0x10000`) — das reicht für
alle realen HBWired-Apps und hält das 16-bit-Adressformat des `w`/`r`-Protokolls gültig, sodass
`flash_tool.py` **und die native CCU** ohne Änderung flashen. Der Booter setzt `RAMPZ = 0`, sodass
`LPM`/`SPM` die untere Flash-Hälfte adressieren; ein Bank-Byte (für App > 64 KB) ist bewusst **nicht**
implementiert (das ginge nur über den eigenen Sender, nicht über die native CCU).

## Bus-Protokoll (Sender → Gerät)

19200 Baud **8E1**, Startbyte `0xFD`, `0xFC`-Escaping, CRC16 (Poly `0x1002`, MSB-first) — identisch
zum HMW-Wire-Format.

| Kmd | Bedeutung | Antwort |
|-----|-----------|---------|
| `z` `z` | ZERO_START (Bus still) | — |
| `u` | START_BOOTER → App macht Watchdog-Reset | Booter meldet StartupReason + Announce |
| `p` | Blockgröße abfragen | **ACK + Payload** `[00 40]` (= 64, big-endian) |
| `w` | WRITE_FLASH `[addrHi addrLo len data…]`, Adresse 16-bit | **ACK + Payload** `[00 len]` (empfangene Bytes) |
| `r` | READ_FLASH (Verify) | **ACK + Payload** = **genau die n Flash-Bytes** (kein Echo!) |
| `g` | START_FW `[lenHi lenLo crcHi crcLo]` → CRC-Check, App-Start | ACK |

**Format der Antworten auf `p`/`w`/`r`:** ACK-Frame (control `0x19 | seq<<5`) **mit angehängter
Payload** — kein Info- und kein `0xFE`-System-Frame. Die native `hs485d` wertet im Zustand
`WAIT_ACK` nur echte ACKs (`control & 0x97 == 0x11`) als „Kommando erledigt"; ein Info-/System-Frame
bliebe hängen und liefe in den Response-Timeout. Die Nutzdaten liest sie trotzdem
(`ExtractFrame().GetPayload()`).

> **⚠ `r`-Antwort exakt `blocksize` Bytes, ohne Präfix.** `VerifyFlash` verwirft die Antwort hart:
> `if(response.size() != blocksize) return false` (blocksize = pagesize = 64). Ein `cmd/addr/len`-Echo
> vor den Daten (→ 68 statt 64 Byte) lässt den Verify beim **ersten** Block scheitern. Das `g` steht in
> der OpenCCU-Quelle **außerhalb** des `if(Verify)` → die App startet trotzdem, aber `retval` bleibt
> `false` → die WebUI meldet **„unbekannter Fehler"**, obwohl der Flash korrekt ist. Der Booter sendet
> deshalb genau die n gelesenen Bytes; die Zuordnung läuft über den frameCounter, nicht den Inhalt.
> (`p`/`w` prüft `WriteFlash` nur beim `p` auf `size()==2`, beim `w` gar nicht.)

## Dateien

| Datei | Zweck |
|-------|-------|
| `hbw_booter.c` | der Booter (C, für **avr-gcc**, portabel 32A/328P) |
| `build.sh` | Build-Skript (avr-gcc, `--section-start=.text=0x7000`) |
| `hbw_booter_atmega328p.hex` / `_atmega32.hex` / `_atmega644p.hex` / `_atmega1284p.hex` | kompilierte Booter (~2,6–2,8 KB) |
| `hbw_testapp/` | Test-App (Arduino-Sketch) mit `u`-Handler; meldet FW `0003` |
| `hbw_testapp_v5/` | dieselbe App als Version `0005` (zum Sichtbarmachen des Flash-Erfolgs) |
| `hbw_testapp_328p.hex` / `hbw_testapp_v5_328p.hex` | deren `.hex` |
| `hbw_testapp_644p/` + `hbw_testapp_644p.hex` | dieselbe Test-App als **ATmega644P/644PA**-Sketch (MightyCore) + `.hex` |
| `hbw_testapp_1284p/` + `hbw_testapp_1284p.hex` | dieselbe Test-App als **ATmega1284P**-Sketch (MightyCore) + `.hex` |
| `hbw_combined_328p.hex` | **DIE ISP-Flash-Datei: HBW-IO-4-FM-App + Booter**, jeweils aktueller Stand |
| `hbw_combined_TESTAPP_328p.hex` | Test-App v3 + Booter — eingefrorene Referenz aus dem M1/M2-Test |
| `hbw_combined_644p.hex` | **ISP-Flash-Datei 644P**: Test-App + Booter `@0xF000` (per `merge_hex.py`) |
| `hbw_combined_1284p.hex` | **ISP-Flash-Datei 1284P**: Test-App + Booter `@0x1F000` (per `merge_hex.py`) |
| `hbw_io_4_fm.hex` | HBW-IO-4-FM-App allein (für den Bus-Flash per `flash_tool.py`) |
| `flash_tool.py` | **Sender**: flasht eine `.hex` über Bus/USB (`z z→u→p→w…→v→g`) |
| `merge_hex.py` | App-`.hex` + Booter-`.hex` → eine ISP-`combined.hex` (Overlap-Check, ELA-Records für Booter >64 KB) |
| `booter_test.py` | Einstiegs-Test: schickt `z z u`, dekodiert die Antworten |
| `diag.py` | Diagnose: listet COM-Ports, liest roh (8E1/8N1) |

## Benutzung

### 1. Einmalig: Booter per ISP einspielen

Mit **AVRDUDESS** oder `avrdude` (nicht die Arduino-IDE — die brennt ihren eigenen Optiboot):

```sh
# 328P (Nano/Uno, Entwicklung): SPIEN bleibt an -> nichts brickbar
avrdude -c usbasp -p m328p  -U flash:w:hbw_combined_328p.hex:i  -U hfuse:w:0xD8:m
# 32A / 1284P (Produktivgeraete): NUR hfuse setzen, lfuse (Oszillator) NICHT anfassen
avrdude -c usbasp -p m1284p -U flash:w:hbw_combined_1284p.hex:i -U hfuse:w:0x90:m
```

**Fuses je MCU** — bei allen: *BOOTRST aktiv + BOOTSZ = 2048 Words (4 KB Boot)*, `SPIEN` bleibt an
→ ISP immer möglich.

| MCU | `-p` | `hfuse` Standard | `hfuse` +EESAVE | übrige Fuses |
|-----|------|:----------------:|:---------------:|--------------|
| ATmega328P | `m328p` | **0xD8** | 0xD0 | `lfuse=0xFF`, `efuse=0xFD` unverändert |
| ATmega32A | `m32` | **0x98** | 0x90 | **`lfuse` unverändert** (CKSEL/CKOPT = Oszillator) |
| ATmega644P/644PA | `m644p` | **0x98** | 0x90 | **`lfuse`/`efuse` unverändert** (CKSEL/SUT = Oszillator) |
| ATmega1284P | `m1284p` | **0x98** | 0x90 | **`lfuse`/`efuse` unverändert** (CKSEL/SUT = Oszillator) |

Bei 32A, 644P **und** 1284P hat der Default (`hfuse=0x99`) `BOOTSZ=2048` (Bit 2:1 = 00) **schon gesetzt** —
es ist nur `BOOTRST` (Bit 0) auf 0 zu ziehen (→ `0x98`), optional zusätzlich `EESAVE` (Bit 3, → `0x90`),
damit die Bus-Adresse im EEPROM einen Flash überlebt (**für echte Geräte empfohlen**). Beim 1284P liegt
die Boot-Section dann bei `0x1F000`, beim 644P bei `0xF000`, beim 32A/328P bei `0x7000` — der Booter wird pro MCU passend
gelinkt (`build.sh`). *(Die `hbw_combined_1284p.hex` entsteht wie beim 328P per Merge:
App-`.hex` unter `0x10000` + `hbw_booter_atmega1284p.hex` ab `0x1F000`.)*

> **⚠ 32A/1284P nicht brickbar halten:** Anders als beim 328P steckt der Oszillator hier im `lfuse`.
> Vorher auslesen — `avrdude -c usbasp -p m1284p -U hfuse:r:-:h -U lfuse:r:-:h` — und **nur** das
> BOOTRST-Bit im `hfuse` ändern. Ein falscher `lfuse` (Taktquelle) macht den Chip nur noch mit
> externem Takt oder HV-Programmer erreichbar.

### 2. Ab jetzt: über den Bus flashen — kein ISP mehr

```sh
python flash_tool.py COM12 hbw_testapp_v5_328p.hex
```
```
z z u  →  Booter aktiv (StartupReason)
w  →  … Blöcke ge-ACKt … ok        (Page 0 zuletzt)
v  →  … byte-genau zurückgelesen … ok
g  →  CRC-Check → neue App startet  (ANNOUNCE FW=0005)
```

## Absicherungen

- **ACK pro Block** — jeder `w` wird quittiert (`send_ack`, 3 Retries).
- **Verify** — nach dem Schreiben liest der Sender jeden Block per `r` zurück und vergleicht
  byte-genau (`read_flash` wartet auf den vollständigen Frame).
- **CRC-Gate** — der Booter startet die neue App beim `g` nur, wenn die CRC16 über die ganze App
  stimmt (`appCrc`), sonst bleibt er im Update-Modus.
- **Stromausfall-Schutz** — Page 0 (Reset-Vektor) wird beim **ersten `w`** gelöscht und **zuletzt**
  geschrieben; der Power-on-Pfad startet nur eine App mit gesetztem Reset-Vektor. Bricht der Flash
  ab, bleibt der Booter aktiv statt eine halb-geschriebene App zu booten. (Bewusst **nicht** schon
  beim `p` — sonst zerstörte ein leerer Handshake `p → g` ohne `w`, z. B. wenn die CCU kein Image
  hat, die noch intakte App unnötig.)
- **Selbstschutz** — Schreibadressen ≥ `0x7000` (Boot-Section) werden abgelehnt.
- **Letzte Page committen** — der `p`-Handler flusht eine noch offene Page zuerst. hs485d ruft `p`
  auch am **VerifyFlash-Start** (nach der w-Schleife); ohne das ginge die letzte, noch gepufferte
  w-Page (z. B. mit dem Versionsfeld `@0x6FF0`) verloren → Verify läse dort `0xFF` → Mismatch.
- **Kein Hängenbleiben** — bleibt der Bus ~25 s still (`IDLE_TIMEOUT_OVF`, Timer1) **und** ist die
  App intakt (Reset-Vektor gesetzt), fällt der Booter von selbst in die App zurück. Ein
  abgebrochenes Update legt das Gerät damit nicht mehr bis zum Power-Cycle lahm. Eine halb
  geflashte App startet er dabei nie: dann ist Page 0 gelöscht → Reset-Vektor `0xFFFF` → er
  bleibt im Update-Modus.

## Eigenes HBWired-Gerät bus-update-fähig machen

Jede App, die per Bus updatebar sein soll, **muss auf `u` mit einem Watchdog-Reset reagieren**,
sonst ist sie nach dem ersten Bus-Flash nur noch per ISP erreichbar.

- **Roher Sketch:** die Zeilen aus `hbw_testapp` übernehmen (`sendAck`; `Serial.flush()`;
  `wdt_enable(WDTO_15MS)`; `while(1);`).
- **HBWired-Lib:** `_HAS_BOOTLOADER_` per Build-Flag setzen und den `u`-Handler in
  `HBWired.cpp` von `goto *bootloader_start` auf Watchdog-Reset umstellen:
  ```cpp
  case 'u':                                   // statt goto -> unser Booter erkennt WDRF
     txFrame.targetAddress = senderAddress;   // hs485d WARTET auf dieses ACK, bevor es
     sendAck();                               // den Booter akzeptiert -> erst ACK, dann Reset
     wdt_enable(WDTO_15MS); while(1);
  ```
  Build z. B. per `arduino-cli compile --fqbn arduino:avr:uno
  --build-property "build.extra_flags=-D_HAS_BOOTLOADER_" <sketch>`.
  Die Änderung sitzt im `#if _HAS_BOOTLOADER_`-Block, betrifft also nur Geräte, die das Flag setzen.

## CCU-Update über die native hs485d

Neben `flash_tool.py` (eigener Sender über USB) läuft das Update auch über die **echte CCU**
(OpenCCU + HMW-LGW-Gateway), angestoßen per Klick in *Einstellungen → Geräte-Firmware → Update*.
Den Transport macht dann die native `hs485d`. Choreografie, rekonstruiert aus der OpenCCU-Quelle
(`src/hs485d/HS485Device.cpp::WriteFlash`, `HS485ControllerLGW.cpp`, `HS485CommMessage*`):

```
z z              Broadcast, Bus still
u  → an die App     App: ACK, dann Watchdog-Reset → Booter
u  → an den Booter  Booter: ACK
p                   Booter: ACK + [00 40]          (Blockgröße 64)   ── WriteFlash
w  je Block         Booter: ACK + [00 len]         (Schleife über die ganze .hex)
p                   Booter: ACK + [00 40]                            ── VerifyFlash
r  je Block         Booter: ACK + 64 Flash-Bytes   (liest den Flash zurück, vergleicht)
g                   Booter: CRC-Check → App-Start
Z Z              Broadcast, Bus wieder frei
                 → App bootet, sendet Announce (deviceType, FW-Version)
```

**Vier Dinge mussten dafür stimmen** (alle im Booter bzw. in der HBWired-Lib gelöst):

1. **App-`u`-Handler ACKt erst, dann Reset** — die hs485d wartet auf dieses ACK, bevor sie den
   Booter-Modus akzeptiert.
2. **Booter erkennt die CCU-Booter-Frames** — die hs485d adressiert den Booter mit control
   `0x12`/`0x14`/`0x16` (Bit 4 statt Bit 3 gesetzt, aber **mit** Senderadresse). Der Booter wertet
   `hasSender` daher aus Bit 3 **oder** Bit 4 (außer Discovery); der CRC-Check fängt Fehlgriffe ab.
3. **Antworten als ACK-Frame mit Payload** (siehe [Bus-Protokoll](#bus-protokoll-sender--gerät)) —
   nicht als Info- oder `0xFE`-System-Frame.
4. **`r`-Verify-Antwort exakt `blocksize` Bytes** (siehe ⚠-Kasten oben) — sonst meldet die WebUI
   „unbekannter Fehler", obwohl der Flash sitzt.

**Firmware auf der CCU (`fwmap`):** Ein Eintrag ordnet dem Gerätetyp eine `.hex` zu, z. B.
`H16V0  hmw_io_4_fm_hw0.hex  @0x77F0`. `H16` = HW-Typ `0x10`; `@0x77F0` zeigt auf das 2-Byte-
Versionsfeld `[minor major]` im Image (`06 03` = v3.06). Die CCU bietet das Update an, wenn die
Datei-Version > Geräte-Version ist.

**⚠ Boot-Section-Kollision — eq3-Original vs. eigenes Image:** eq3-Original-Images reichen bis
`0x77FF` und überlappen unseren 4-KB-Booter ab `0x7000`. Beim Test damit zeigt sich: die CCU
flasht/verifiziert **nur bis `0x7000`** (die r-Verify-Schleife endet bei `0x6FC0`), die eq3-Boot-
Section bleibt unangetastet — sonst gäbe es dort einen Verify-Mismatch (Booter-Code ≠ eq3-Boot-Code).
Nach `g` läuft die eq3-App mit **unserem** Booter, aber unvollständig (ihr eigener Bootloader fehlt).
Für ein **vollständig lauffähiges eigenes** Gerät darum die `.hex` unter `0x7000` halten und ein
eigenes `fwmap`/Versionsfeld darunter setzen:
```
H16V0   hbw_io_4_fm_v304.hex   @0x6FF0   #HBW-IO-4-FM v3.04   (Image <0x7000, Versionsfeld @0x6FF0)
```

## Bauen

```sh
sh build.sh                 # Booter für 32A + 328P + 644P + 1284P, .hex + Größe
```
Toolchain = Arduino-avr-gcc 7.3.0. Der Booter ist **kein** Arduino-Sketch (Boot-Section-Linking).
`build.sh` linkt je MCU an die passende Boot-Adresse (32A/328P `0x7000`, 644P `0xF000`, 1284P `0x1F000`).

**1284P-Test-Kandidat** (App + Booter → ISP-Datei für den USB-Serial-Test, wie damals beim 328P):

```sh
arduino-cli compile \
  --fqbn MightyCore:avr:1284:variant=modelP,clock=16MHz_external,pinout=standard,bootloader=no_bootloader \
  --output-dir out hbw_testapp_1284p
python merge_hex.py out/hbw_testapp_1284p.ino.hex hbw_booter_atmega1284p.hex hbw_combined_1284p.hex
```

Dann `hbw_combined_1284p.hex` per ISP einspielen (`-p m1284p`, `hfuse=0x90`, **`lfuse` unverändert**)
und über USB testen: `python booter_test.py COMx` (Einstieg) bzw. `python flash_tool.py COMx
hbw_testapp_1284p.hex` (voller Flash-Durchlauf). Für ein echtes Gerät die Test-App durch deine
Geräte-Firmware (`.hex` < `0x10000`, mit `u`→Watchdog-Reset-Handler) ersetzen und `DEVICE_TYPE` im
Booter-Konfigblock auf den Modultyp setzen.

## Status & Roadmap

| Baustein | Stand |
|----------|-------|
| Booter-Einstieg (Reset-Quelle) | ✅ HW-verifiziert (328P/Nano) |
| Echtes Flashen (`avr/boot.h`) | ✅ HW-verifiziert |
| Verify + CRC + Stromausfall-Schutz | ✅ HW-verifiziert |
| HBW-IO-4-FM per Bus geflasht, läuft an CCU | ✅ (erkannt als Typ 0x10, v3.03) |
| Gerät geht **über die echte CCU** in den Booter | ✅ (App-ACK auf `u` → Reset → Booter-Announce) |
| CCU spricht Booter an (`u`/`p`/`g`), quittiert Antworten | ✅ (Gateway-Log: `p`-Antwort akzeptiert) |
| **CCU-Update vollständig, eigene App läuft danach** | ✅ **HW-verifiziert** — HBW-IO-4-FM **v3.04** über WebUI geflasht, Gerät bootet + meldet Announce FW `0x0304` |
| **WebUI meldet „Firmware-Update erfolgreich"** | ✅ **HW-verifiziert** (Screenshot + Log) — `r`-Verify komplett inkl. Versionsfeld `@0x6FF0`, danach `h`/`v` → FW `3.04` |
| ATmega32A (Code portabel, `hfuse` dokumentiert) | ✅ kompiliert (2646 B) · ⏳ HW-Test am echten RS485 offen |
| ATmega644P/644PA (Boot @0xF000, 64 KB = ganzer Flash adressierbar) | ✅ portiert + kompiliert (2732 B) · ⏳ HW-Test offen |
| ATmega1284P, App < 64 KB (Boot @0x1F000, `RAMPZ=0`) | ✅ portiert + kompiliert (2770 B) · ⏳ HW-Test offen |
| ATmega1284P, App > 64 KB (Bank-Byte, nur eigener Sender) | ⏳ bewusst zurückgestellt — bei Bedarf nachrüstbar |

**Kurz:** Ziel erreicht. `flash_tool.py` flasht über USB ohne ISP, **und** der komplette
Firmware-Update über die native CCU-WebUI läuft am echten Bus durch — ein **eigenes** Gerät
(HBW-IO-4-FM v3.04) wurde so geflasht und **bootet danach in die neue Firmware** (Announce FW `0x0304`).
Voraussetzung für ein *lauffähiges* Ergebnis: das Image bleibt unter `0x7000` (eigene Geräte) — die
eq3-Original-Images überlappen den Booter und bleiben daher unvollständig.

> **Hinweis Gateway-Timing:** Das HMW-LGW-Gateway ist ein reiner LAN↔Bus-Transceiver. Blockierende
> Diagnose-/Nachlausch-Schleifen im Gateway-Loop (z. B. mehrere Sekunden `busReadResponse`) killen
> das enge hs485d-Timing — die `hs485d` hat nur **500 ms** Response-Timeout pro Bootloader-Kommando.
> Genau das (eine 2,5-s-„post-u"-Diagnose) war der letzte Blocker vor dem funktionierenden Update.
