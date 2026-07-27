/*
 * HBW-Booter — Over-the-Bus-Bootloader für HMW/HBWired-Geräte
 * ============================================================
 * Ziel-MCUs: ATmega32A (Produktivgeräte) UND ATmega328P (Entwicklung, Nano/Uno).
 * Meilenstein 1: Reset-Erkennung + Wire-Protokoll + Announce — OHNE echtes Flashen
 *                (WRITE_FLASH quittiert nur, wie DRY_RUN). Flashen = Meilenstein 2.
 *
 * Wire-Logik 1:1 aus HBW-Booter-Sim: Startbyte 0xFD, 0xFC-Escaping, CRC16 Poly 0x1002
 * (MSB-first), Bus 19200 8E1. Auf direkte UART-Register portiert (kein Arduino-Framework).
 *
 * EINSTIEG (der Kern, den der Sim NICHT leisten konnte):
 *   - Power-on / Brown-out / externer Reset  -> App starten (jmp 0)
 *   - Watchdog-Reset (von der App per 'u' ausgeloest) -> im Booter BLEIBEN
 *   Das ist die Reset-QUELLEN-Erkennung (kein Timing-Fenster).
 *
 * Build: avr-gcc, Boot-Section @0x7000 (2048 Words / 4 KB). Siehe build.sh.
 * Fuses (einmalig per ISP): BOOTRST aktiv + BOOTSZ = 2048 Words.
 * (App reicht damit bis 0x6FFF; Versionsfeld/fwmap-Marker entsprechend nach 0x6FF0.)
 */

#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <string.h>

/* ======================= KONFIG (hier anpassen) ======================= */
#ifndef F_CPU
#define F_CPU        16000000UL      /* HMW-Geraete: ext. 16-MHz-Quarz */
#endif
#define BUS_BAUD     19200UL

#define DEVICE_TYPE  0x00            /* NEUTRAL -- der Booter ist geraeteunabhaengig (ein Booter je MCU
                                        gilt fuer ALLE HBW-Geraete des Typs). Wird nur gemeldet, wenn ein
                                        Geraet OHNE laufende App abgefragt wird; die App meldet sonst ihren
                                        eigenen Typ. Fuers Flashen (Adresse kommt aus dem EEPROM) egal. */
#define HW_VERSION   0x00
#define FW_VERSION   0x0001          /* Booter-eigene Version, gemeldet bei 'v' */
#define FALLBACK_ADDR 0x42FFFFFFUL   /* falls EEPROM-Adresse leer (0xFFFFFFFF) */

/* Inaktivitaets-Timeout: nach IDLE_TIMEOUT_OVF Timer1-Ueberlaeufen (je ~4,19 s
   @16 MHz, Prescaler 1024) ohne Bus-Aktivitaet faellt der Booter in eine INTAKTE
   App zurueck (Reset-Vektor gesetzt). So bleibt ein Geraet nach einem abgebrochenen
   Update nicht ewig im Booter haengen. 6 ~ 25 s -- lang genug, dass kein laufendes
   Update faelschlich abbricht (waehrend des Flashs ist die App ohnehin ungueltig). */
#define IDLE_TIMEOUT_OVF 6

/* RS485 Sende-Enable (DE). -1 = Auto-Direction-Modul (kein DE-Pin). */
#define DE_DDR   DDRD
#define DE_PORT  PORTD
#define DE_BIT   2                   /* PD2; auf -1-Verhalten via USE_DE=0 stellen */
#define USE_DE   1

/* ======================= Chip-Portabilitaet ======================= */
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega328PA__) || defined(__AVR_ATmega328__) \
 || defined(__AVR_ATmega644P__)  || defined(__AVR_ATmega644PA__) \
 || defined(__AVR_ATmega644__)   || defined(__AVR_ATmega644A__) \
 || defined(__AVR_ATmega1284P__) || defined(__AVR_ATmega1284__)
  /* 328P(A) / 644P(A) / 1284P sind registergleich (UART0, MCUSR, TIFR1). Nur der 1284P hat
     zusaetzlich RAMPZ (in main() auf 0) -- 644P/328P haben <=64 KB Flash, kein RAMPZ. Die
     Boot-Section-Adresse (BOOT_START) folgt generisch aus FLASHEND. Der 328PA ist eine
     picoPower-Rev des 328P (register- UND signaturgleich): avr-gcc hat KEIN eigenes Target,
     er wird als atmega328p gebaut -- dieses Makro greift nur, falls eine Toolchain es setzt. */
  #define RESET_FLAGS  MCUSR
  #define U_UCSRA UCSR0A
  #define U_UCSRB UCSR0B
  #define U_UCSRC UCSR0C
  #define U_UBRRH UBRR0H
  #define U_UBRRL UBRR0L
  #define U_UDR   UDR0
  #define B_RXC   RXC0
  #define B_UDRE  UDRE0
  #define B_TXC   TXC0
  #define UART_INIT_C()  (U_UCSRC = (1<<UPM01)|(1<<UCSZ01)|(1<<UCSZ00)) /* 8E1 */
  #define T1_IFR  TIFR1              /* Timer1-Overflow-Flag TOV1 sitzt hier */
#elif defined(__AVR_ATmega32A__) || defined(__AVR_ATmega32__)
  #define RESET_FLAGS  MCUCSR
  #define U_UCSRA UCSRA
  #define U_UCSRB UCSRB
  #define U_UDR   UDR
  #define U_UBRRH UBRRH
  #define U_UBRRL UBRRL
  #define B_RXC   RXC
  #define B_UDRE  UDRE
  #define B_TXC   TXC
  /* 32A: UCSRC & UBRRH teilen die Adresse -> Schreiben mit URSEL=1 fuer UCSRC. 8E1. */
  #define UART_INIT_C()  (UCSRC = (1<<URSEL)|(1<<UPM1)|(1<<UCSZ1)|(1<<UCSZ0))
  #define T1_IFR  TIFR               /* 32A: ein gemeinsames Timer-Interrupt-Flag-Register */
#else
  #error "Nicht unterstuetzte MCU (nur ATmega32A / ATmega328P(A) / ATmega644P(A) / ATmega1284P)"
#endif

/* ======================= Frame-Konstanten ======================= */
#define FRAME_START       0xFD
#define FRAME_START_SHORT 0xFE
#define ESCAPE_BYTE       0xFC
#define CRC16_POLY        0x1002
#define MAX_PACKET_SIZE   64
#define RX_BUFSIZE        80

enum { CMD_ANNOUNCE='A', CMD_ZERO_START='z', CMD_ZERO_END='Z',
       CMD_START_BOOTER='u', CMD_GET_PACKET_SIZE='p', CMD_WRITE_FLASH='w',
       CMD_READ_FLASH='r', CMD_START_FW='g', CMD_GET_FW='v', CMD_GET_HW='h',
       CMD_GET_SERIAL='n' };

static uint32_t ownAddr;
static uint8_t  rxSenderNum = 0;

/* ======================= UART ======================= */
static void uartInit(void){
  uint16_t ubrr = (uint16_t)(F_CPU/(16UL*BUS_BAUD) - 1);
  U_UBRRH = (uint8_t)(ubrr>>8);
  U_UBRRL = (uint8_t)ubrr;
  UART_INIT_C();
  U_UCSRB = (1<<3)|(1<<4);           /* TXEN|RXEN (Bit3=TXEN, Bit4=RXEN bei beiden MCUs) */
#if USE_DE
  DE_DDR |= (1<<DE_BIT);
  DE_PORT &= ~(1<<DE_BIT);           /* Empfang */
#endif
}
static void deTx(uint8_t on){
#if USE_DE
  if(on) DE_PORT |= (1<<DE_BIT); else DE_PORT &= ~(1<<DE_BIT);
#else
  (void)on;
#endif
}
static void uartPut(uint8_t b){
  while(!(U_UCSRA & (1<<B_UDRE)));
  U_UDR = b;
}
static void uartFlush(void){        /* warten bis letztes Byte komplett raus ist */
  while(!(U_UCSRA & (1<<B_TXC)));
  U_UCSRA |= (1<<B_TXC);
}
static int16_t uartGet(void){       /* -1 wenn nichts da */
  if(U_UCSRA & (1<<B_RXC)) return U_UDR;
  return -1;
}

/* ======================= CRC (aus Sim) ======================= */
static void crc16Shift(uint8_t b, uint16_t* crc){
  for(uint8_t i=0;i<8;i++){
    uint8_t hi = (*crc & 0x8000)?1:0;
    *crc <<= 1;
    if(b & 0x80) *crc |= 1;
    if(hi)       *crc ^= CRC16_POLY;
    b <<= 1;
  }
}

/* ======================= Wire TX (aus Sim) ======================= */
static void txByteEsc(uint8_t b, uint16_t* crc){
  crc16Shift(b, crc);
  if(b==FRAME_START || b==FRAME_START_SHORT || b==ESCAPE_BYTE){
    uartPut(ESCAPE_BYTE); uartPut((uint8_t)(b & 0x7F));
  } else uartPut(b);
}
static void txRawEsc(uint8_t b){
  if(b==FRAME_START || b==FRAME_START_SHORT || b==ESCAPE_BYTE){
    uartPut(ESCAPE_BYTE); uartPut((uint8_t)(b & 0x7F));
  } else uartPut(b);
}
/* control muss das hasSender-Bit (0x08) korrekt gesetzt haben */
static void sendFrame(uint32_t target, uint8_t control, const uint8_t* data, uint8_t len){
  uint8_t hasSender = (control & 0x08);
  uint16_t crc = 0xFFFF;
  deTx(1);
  uartPut(FRAME_START); crc16Shift(FRAME_START,&crc);
  txByteEsc((target>>24)&0xFF,&crc); txByteEsc((target>>16)&0xFF,&crc);
  txByteEsc((target>>8)&0xFF,&crc);  txByteEsc(target&0xFF,&crc);
  txByteEsc(control,&crc);
  if(hasSender){
    txByteEsc((ownAddr>>24)&0xFF,&crc); txByteEsc((ownAddr>>16)&0xFF,&crc);
    txByteEsc((ownAddr>>8)&0xFF,&crc);  txByteEsc(ownAddr&0xFF,&crc);
  }
  txByteEsc(len+2,&crc);
  for(uint8_t i=0;i<len;i++) txByteEsc(data[i],&crc);
  crc16Shift(0,&crc); crc16Shift(0,&crc);
  txRawEsc((crc>>8)&0xFF); txRawEsc(crc&0xFF);
  uartFlush(); deTx(0);
}
static void sendAck(uint32_t to){ sendFrame(to, 0x19 | (uint8_t)(rxSenderNum<<5), 0, 0); }
static void sendInfo(uint32_t to, const uint8_t* d, uint8_t len){
  sendFrame(to, 0x98 | (uint8_t)(rxSenderNum<<5), d, len);
}
/* Bootloader-Antwort auf p/w/r: Die native OpenCCU-hs485d wertet im Zustand WAIT_ACK nur ein
   ACK-Frame (control & 0x97 == 0x11) als "Kommando erledigt" (ACKED). Ein Info-/system-Frame
   bliebe in WAIT_ACK haengen -> WaitUntilSent laeuft in den response-timeout -> Update bricht ab.
   Also ein ACK-Frame (CTRL_ACK 0x19 | Sendefolgenummer<<5) MIT angehaengter Payload senden --
   die hs485d liest die Nutzdaten ueber ExtractFrame().GetPayload(). (0xFD-Standardframe; die
   frueher vermutete 0xFE-system-Form war falsch, OpenCCU-WriteFlash prueft nur size()==2.) */
static void sendBootReply(uint32_t to, const uint8_t* d, uint8_t len){
  sendFrame(to, 0x19 | (uint8_t)(rxSenderNum<<5), d, len);
}
static void makeSerial(uint32_t a, uint8_t* buf){
  buf[0]='H'; buf[1]='B'; buf[2]='W';
  for(int8_t p=9;p>2;p--){ buf[p]='0'+(a%10); if(a) a/=10; }
}
static void sendAnnounce(void){
  uint8_t d[16];
  d[0]=CMD_ANNOUNCE; d[1]=0; d[2]=DEVICE_TYPE; d[3]=HW_VERSION;
  d[4]=(FW_VERSION>>8)&0xFF; d[5]=FW_VERSION&0xFF;
  makeSerial(ownAddr,&d[6]);
  sendFrame(0xFFFFFFFFUL, 0xF8, d, 16);
}
static void sendStartupReason(void){
  uint8_t d[2]={ 0xFF, 0x08 };       /* 0xFF=STARTUP_REASON, 0x08=gewollter Reset */
  sendFrame(0xFFFFFFFFUL, 0xF8, d, 2);
}

/* ======================= Wire RX (aus Sim) ======================= */
static uint8_t  rxb[RX_BUFSIZE];
static uint8_t  rxIdx=0, rxHeaderLen=0, rxHasSender=0;
static int16_t  rxTotal=-1;
static uint8_t  inFrame=0, pendingEsc=0;
static uint16_t rxCrc=0xFFFF;
static void rxReset(void){ inFrame=0; pendingEsc=0; rxIdx=0; rxTotal=-1; rxHeaderLen=0; }

static uint8_t pollFrame(uint32_t* target, uint8_t* control, uint32_t* sender,
                         uint8_t** data, uint8_t* dlen){
  int16_t v;
  while((v=uartGet())>=0){
    uint8_t b=(uint8_t)v;
    if(b==FRAME_START || b==FRAME_START_SHORT){
      inFrame=1; pendingEsc=0; rxIdx=0; rxTotal=-1; rxHeaderLen=0;
      rxCrc=0xFFFF; crc16Shift(b,&rxCrc); continue;
    }
    if(!inFrame) continue;
    if(b==ESCAPE_BYTE){ pendingEsc=1; continue; }
    if(pendingEsc){ b|=0x80; pendingEsc=0; }
    crc16Shift(b,&rxCrc);
    if(rxIdx<RX_BUFSIZE) rxb[rxIdx]=b;
    if(rxIdx==4){
      /* hasSender-Erkennung: An einen BOOTER setzt die CCU/hs485d teils nur Bit 4 (0x10)
         statt Bit 3 (0x08) -- z.B. control 0x12/0x16 statt 0x18. Beide FUEHREN eine
         Senderadresse (per CRC am echten Bus belegt). Daher Bit 3 ODER Bit 4 werten,
         ausser bei Discovery (Bits 1,0 = 0b11, dort ist Bit 3 kein Sender-Flag). Der
         CRC-Check am Frame-Ende faengt eine falsche Annahme ab -> Frame wird verworfen. */
      uint8_t c=b;
      rxHasSender=((c & 0x18)!=0 && (c & 0x03)!=0x03) ? 1 : 0;
      rxHeaderLen=rxHasSender?10:6;
    }
    if(rxHeaderLen && rxIdx==(rxHeaderLen-1)) rxTotal=rxHeaderLen+b;
    rxIdx++;
    if(rxTotal>0 && rxIdx==rxTotal){
      if(rxCrc!=0){ rxReset(); return 0; }
      *target=((uint32_t)rxb[0]<<24)|((uint32_t)rxb[1]<<16)|((uint32_t)rxb[2]<<8)|rxb[3];
      *control=rxb[4];
      *sender = rxHasSender ? (((uint32_t)rxb[5]<<24)|((uint32_t)rxb[6]<<16)|((uint32_t)rxb[7]<<8)|rxb[8]) : 0;
      uint8_t wireLen=rxb[rxHeaderLen-1];
      *dlen=(wireLen>=2)?wireLen-2:0;
      *data=&rxb[rxHeaderLen];
      rxReset(); return 1;
    }
    if(rxIdx>=RX_BUFSIZE) rxReset();
  }
  return 0;
}

/* ======================= App starten (jmp 0) ======================= */
static void startApp(void){
  U_UCSRB=0;                         /* UART aus */
  cli();
  ((void(*)(void))0)();              /* Sprung zum Reset-Vektor der App */
}

/* ======================= Flash schreiben (avr/boot.h) ======================= */
/* Booter-Section-Start (Byte-Adresse) = oberste 4 KB (2048 Words, BOOTSZ=00) des Flash,
   generisch aus FLASHEND: 32A/328P 0x7000 (32 KB), 644P/644PA 0xF000 (64 KB), 1284P 0x1F000
   (128 KB). Der Booter darf sich hier NIE selbst ueberschreiben. Muss zur Boot-Adresse im
   build.sh-Linkerflag passen (beide = FLASHEND + 1 - 0x1000). */
#define BOOT_START ((uint32_t)(FLASHEND + 1UL - 0x1000UL))
static uint8_t  pageBuf[SPM_PAGESIZE];
static uint16_t pageBase = 0xFFFF;
static uint8_t  pageDirty = 0;
static uint8_t  flashStarted = 0;    /* erst beim ersten 'w' Page 0 (Reset-Vektor) invalidieren */

static void writePage(uint16_t base){
  boot_page_erase(base); boot_spm_busy_wait();
  for(uint16_t i=0;i<SPM_PAGESIZE;i+=2)
    boot_page_fill(base+i, pageBuf[i] | ((uint16_t)pageBuf[i+1]<<8));
  boot_page_write(base); boot_spm_busy_wait();
  boot_rww_enable();                 /* App-Section wieder lesbar machen */
}
static void flushPage(void){ if(pageDirty){ writePage(pageBase); pageDirty=0; } }
static void bufferBytes(uint16_t addr, const uint8_t* src, uint8_t n){
  for(uint8_t i=0;i<n;i++){
    uint16_t a=addr+i, pb=a & ~(SPM_PAGESIZE-1);
    if(pb!=pageBase){ flushPage(); pageBase=pb; memset(pageBuf,0xFF,SPM_PAGESIZE); }
    pageBuf[a-pageBase]=src[i]; pageDirty=1;
  }
}
static uint16_t appCrc(uint16_t len){          /* CRC16 (Poly 0x1002) ueber App-Flash 0..len-1 */
  uint16_t crc=0xFFFF;
  for(uint16_t a=0;a<len;a++) crc16Shift(pgm_read_byte(a), &crc);
  return crc;
}

/* ======================= Kommando-Logik ======================= */
static uint8_t zCount=0, zeroComm=0;

static void handleFrame(uint32_t target, uint8_t control, uint32_t sender,
                        uint8_t* data, uint8_t dlen){
  rxSenderNum=(control>>1)&0x03;
  uint8_t broadcast=(target==0xFFFFFFFFUL);
  if(!broadcast && target!=ownAddr) return;
  if(dlen==0) return;
  uint8_t cmd=data[0];

  if(cmd==CMD_ZERO_START){ if(zCount>=1) zeroComm=1; else zCount++; return; }
  if(cmd==CMD_ZERO_END){ zeroComm=0; zCount=0; return; }
  if(zeroComm && cmd!=CMD_START_BOOTER) return;

  switch(cmd){
    case CMD_START_BOOTER:             /* im Booter sind wir schon -> bestaetigen + melden */
      zeroComm=0; zCount=0;
      sendAck(sender);
      sendStartupReason();
      sendAnnounce();
      break;
    case CMD_GET_PACKET_SIZE:{        /* 'p' = Blockgroesse melden. Reset-Vektor (Page 0) hier NOCH
                                          NICHT loeschen -- erst beim ersten echten 'w'. Sonst
                                          zerstoert ein leerer Handshake (p -> g ohne w, z.B. wenn
                                          die CCU kein Firmware-Image hat) die noch intakte App. */
      flushPage();                     /* WICHTIG: offene Page ZUERST committen. hs485d ruft 'p' auch
                                          am VerifyFlash-Start (NACH der w-Schleife) -- die letzte,
                                          noch nicht geschriebene w-Page (z.B. mit Versionsfeld @0x6FF0)
                                          ginge sonst durch das pageDirty=0 unten verloren -> Verify
                                          liest dort 0xFF -> Mismatch. Beim leeren Handshake ist
                                          pageDirty=0 -> no-op, die intakte App bleibt unberuehrt. */
      pageBase=0xFFFF; pageDirty=0; flashStarted=0;   /* Page-Puffer + Flash-Zustand zuruecksetzen */
      uint8_t r[2]={0x00,MAX_PACKET_SIZE};            /* Blockgroesse als 2-Byte-BIG-ENDIAN, wie hs485d es liest */
      sendBootReply(sender,r,2); break; }
    /* h/v/n im APP-Format beantworten (OHNE cmd-Echo im 1. Byte) -- so liest CCU/Gateway die Werte wie
       bei der laufenden App: h=[Typ,HW], v=[FWhi,FWlo], n=[Serial(10)]. Greift nur im reinen Booter-
       Zustand (ohne App); mit DEVICE_TYPE=0x00 taucht so ein Geraet neutral auf, nicht als Fremdtyp. */
    case CMD_GET_FW:{ uint8_t r[2]={(FW_VERSION>>8)&0xFF,FW_VERSION&0xFF}; sendInfo(sender,r,2); break; }
    case CMD_GET_HW:{ uint8_t r[2]={DEVICE_TYPE,HW_VERSION}; sendInfo(sender,r,2); break; }
    case CMD_GET_SERIAL:{ uint8_t r[10]; makeSerial(ownAddr,r); sendInfo(sender,r,10); break; }
    case CMD_WRITE_FLASH:{             /* 'w' [addrHi addrLo len data..] -> App-Section flashen */
      if(dlen>=4){
        uint16_t addr=((uint16_t)data[1]<<8)|data[2];
        uint8_t  n=data[3]; if(n>(uint8_t)(dlen-4)) n=dlen-4;
        /* ACK ZUERST senden -- BEVOR geflasht wird. An 128-B-Page-Grenzen blockiert boot_page_write
           (via flushPage in bufferBytes) ~7 ms; das darf die ACK NICHT verzoegern, sonst laeuft die
           hs485d evtl. in ihren Response-Timeout und wiederholt genau diesen Block. data[] bleibt in
           rxb gueltig, bis das naechste Frame empfangen wird (waehrend sendBootReply blockiert der
           Booter im Senden, empfaengt also nichts). Bestaetigt auch abgelehnte Boot-Section-Bloecke. */
        uint8_t r[2]={0x00,n};
        sendBootReply(sender,r,2);
        if(!flashStarted){             /* ERSTER Datenblock: Reset-Vektor (Page 0) invalidieren ->
                                          ein Abbruch laesst die App garantiert ungueltig. */
          boot_page_erase(0); boot_spm_busy_wait(); boot_rww_enable();
          flashStarted=1;
        }
        if((uint32_t)addr+n <= BOOT_START) bufferBytes(addr,&data[4],n);  /* Boot-Section schuetzen */
      }
      break; }
    case CMD_READ_FLASH:{              /* 'r' [addrHi addrLo len] -> echte Flash-Bytes (Verify) */
      flushPage();                     /* offene Page committen, damit Verify aktuelle Bytes liest */
      if(dlen>=4){ uint16_t addr=((uint16_t)data[1]<<8)|data[2]; uint8_t n=data[3];
        if(n>MAX_PACKET_SIZE)n=MAX_PACKET_SIZE;
        /* Payload = GENAU die n gelesenen Bytes, KEIN cmd/addr/len-Echo davor. hs485d VerifyFlash
           verwirft die Antwort sonst hart: `if(response.size()!=blocksize)return false` -> die CCU
           meldet "unbekannter Fehler", obwohl der Flash korrekt ist (g laeuft, App startet). Zuordnung
           laeuft ueber den frameCounter, nicht ueber den Payload-Inhalt -> Echo ist unnoetig. */
        uint8_t r[MAX_PACKET_SIZE];
        for(uint8_t i=0;i<n;i++) r[i]=pgm_read_byte(addr+i);
        sendBootReply(sender,r,n); }
      break; }
    case CMD_START_FW:                 /* 'g' [lenHi lenLo crcHi crcLo] -> CRC-Check + App-Start */
      flushPage();
      sendAck(sender);
      _delay_ms(5);
      if(dlen>=5){                     /* mit erwarteter CRC: echte Validierung der ganzen App */
        uint16_t len =((uint16_t)data[1]<<8)|data[2];
        uint16_t want=((uint16_t)data[3]<<8)|data[4];
        if(appCrc(len)==want) startApp();
      } else if(pgm_read_word(0)!=0xFFFF){ /* ohne CRC: schwacher Reset-Vektor-Sanity */
        startApp();
      }
      break;                           /* CRC falsch / keine App -> im Booter bleiben */
    case CMD_ANNOUNCE: sendAnnounce(); break;
    default: sendAck(sender); break;   /* im Booter alles ACKen, damit die CCU weiterlaeuft */
  }
}

/* ======================= main ======================= */
int main(void){
  uint8_t rf = RESET_FLAGS;           /* Reset-Quelle sichern ... */
  RESET_FLAGS = 0;                    /* ... und Flags loeschen */
  wdt_disable();                      /* PFLICHT nach WDRF: sonst Reset-Loop */
#ifdef RAMPZ
  RAMPZ = 0;                          /* 1284P: App <64 KB -> LPM/SPM adressieren die untere
                                         Flash-Haelfte ohne Bank; der Booter macht nie _far-Zugriffe. */
#endif

  /* Bus-Adresse aus den letzten 4 EEPROM-Bytes (E2END-3), big-endian */
  {
    uint16_t a = E2END-3;
    ownAddr = ((uint32_t)eeprom_read_byte((uint8_t*)a)<<24)
            | ((uint32_t)eeprom_read_byte((uint8_t*)(a+1))<<16)
            | ((uint32_t)eeprom_read_byte((uint8_t*)(a+2))<<8)
            |  (uint32_t)eeprom_read_byte((uint8_t*)(a+3));
    if(ownAddr==0xFFFFFFFFUL) ownAddr=FALLBACK_ADDR;
  }

  /* Reset-QUELLEN-Entscheidung:
   * Watchdog-Reset (WDRF, Bit3) = von der App gewollt -> im Booter bleiben.
   * Alles andere (Power-on/Brown-out/extern) -> App starten. */
  if(!(rf & (1<<WDRF)) && pgm_read_word(0) != 0xFFFF){
    startApp();                       /* Power-on + gueltige App (Reset-Vektor gesetzt) -> App.
                                         Reset-Vektor leer (Flash-Abbruch) -> im Booter bleiben. */
  }

  uartInit();
  _delay_ms(20);                      /* Bus kurz beruhigen */
  sendStartupReason();
  _delay_ms(5);
  sendAnnounce();

  /* Inaktivitaets-Timeout gegen "im Booter haengen bleiben": Timer1 frei laufen
   * lassen (Prescaler 1024 -> ~4,19 s je Ueberlauf @16 MHz). Jeder empfangene Frame
   * setzt ihn zurueck. Bleibt der Bus IDLE_TIMEOUT_OVF Ueberlaeufe lang still UND ist
   * die App intakt (Reset-Vektor != 0xFFFF), springen wir zurueck in die App. Waehrend
   * eines echten Flashs ist Page 0 geloescht -> Reset-Vektor 0xFFFF -> der Timeout
   * startet dann NICHTS (halbe App bleibt liegen), und laufende w-Bloecke halten ihn
   * ohnehin frisch. Er wirkt also nur beim echten Haenger. */
  TCCR1A = 0;
  TCCR1B = (1<<CS12)|(1<<CS10);        /* Timer1: clk/1024, Normal-Mode */
  TCNT1  = 0; T1_IFR = (1<<TOV1);
  uint8_t idleOvf = 0;

  for(;;){
    uint32_t target,sender; uint8_t control,*data,dlen;
    if(pollFrame(&target,&control,&sender,&data,&dlen)){
      handleFrame(target,control,sender,data,dlen);
      TCNT1 = 0; idleOvf = 0; T1_IFR = (1<<TOV1);   /* Aktivitaet -> Timeout zuruecksetzen */
    }
    if(T1_IFR & (1<<TOV1)){             /* ~4,19 s vergangen */
      T1_IFR = (1<<TOV1);
      if(++idleOvf >= IDLE_TIMEOUT_OVF && pgm_read_word(0) != 0xFFFF)
        startApp();                    /* lange still + App intakt -> zurueck in die App */
    }
  }
}
