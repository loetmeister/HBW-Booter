/*
 * HBW-Test-App  —  App-Seite fuer HBW-Booter Meilenstein 1
 * ---------------------------------------------------------
 * Normale App-Section (0x0000). Spricht das HMW-Wire-Protokoll (Announce, h/v/n, z/Z).
 * Auf 'u' (START_BOOTER): ACK + ECHTER Watchdog-Reset  ->  uebergibt an den Booter.
 *
 * Ziel: ATmega328P (Nano/Uno). Bus 19200 8E1 — hier direkt ueber USB-Serial testbar
 * (Python schickt z z u, dekodiert die Antworten; kein RS485/Gateway noetig).
 *
 * Test-Ablauf (Meilenstein 1):
 *   Power-on -> Booter sieht kein WDRF -> jmp 0 -> DIESE App laeuft, sendet Announce.
 *   z z u    -> App ACKt 'u' + Watchdog-Reset -> Booter sieht WDRF -> bleibt im Booter
 *            -> Booter sendet StartupReason + Announce.  => Meilenstein 1 erreicht.
 */
#include <avr/wdt.h>

// ============================ OPTIONEN ============================
#define BUS_BAUD     19200
#define RS485_TXEN   2            // DE-Pin; beim USB-Test ohne Funktion (harmlos)
#define USE_DE       1
#define OWN_ADDRESS  0x42FFFFFFUL // = Booter-Fallback bei leerem EEPROM -> beide gleich
#define DEVICE_TYPE  0x10
#define HW_VERSION   0x00
#define FW_VERSION   0x0003       // App-Version (>Booter 0x0001, zur Unterscheidung)
#define LED_PIN      13
// =================================================================

#define FRAME_START        0xFD
#define FRAME_START_SHORT  0xFE
#define ESCAPE_BYTE        0xFC
#define CRC16_POLY         0x1002
#define RX_BUFSIZE         80

enum { CMD_ANNOUNCE='A', CMD_ZERO_START='z', CMD_ZERO_END='Z',
       CMD_START_BOOTER='u', CMD_GET_FW='v', CMD_GET_HW='h', CMD_GET_SERIAL='n' };

static uint8_t rxSenderNum = 0;

// ----------------------------------------------------------- CRC (aus Sim)
static void crc16Shift(uint8_t b, uint16_t* crc){
  for(uint8_t i=0;i<8;i++){
    uint8_t hi=(*crc & 0x8000)?1:0; *crc<<=1;
    if(b&0x80)*crc|=1; if(hi)*crc^=CRC16_POLY; b<<=1;
  }
}
static void deTx(uint8_t on){
#if USE_DE
  digitalWrite(RS485_TXEN, on?HIGH:LOW);
#else
  (void)on;
#endif
}
static void txEsc(uint8_t b, uint16_t* crc){
  if(crc) crc16Shift(b,crc);
  if(b==FRAME_START||b==FRAME_START_SHORT||b==ESCAPE_BYTE){ Serial.write(ESCAPE_BYTE); Serial.write((uint8_t)(b&0x7F)); }
  else Serial.write(b);
}
static void sendFrame(uint32_t target, uint8_t control, const uint8_t* data, uint8_t len){
  uint8_t hasSender=(control&0x08); uint16_t crc=0xFFFF;
  deTx(1);
  Serial.write(FRAME_START); crc16Shift(FRAME_START,&crc);
  txEsc((target>>24)&0xFF,&crc); txEsc((target>>16)&0xFF,&crc);
  txEsc((target>>8)&0xFF,&crc);  txEsc(target&0xFF,&crc);
  txEsc(control,&crc);
  if(hasSender){
    txEsc((OWN_ADDRESS>>24)&0xFF,&crc); txEsc((OWN_ADDRESS>>16)&0xFF,&crc);
    txEsc((OWN_ADDRESS>>8)&0xFF,&crc);  txEsc(OWN_ADDRESS&0xFF,&crc);
  }
  txEsc(len+2,&crc);
  for(uint8_t i=0;i<len;i++) txEsc(data[i],&crc);
  crc16Shift(0,&crc); crc16Shift(0,&crc);
  txEsc((crc>>8)&0xFF,0); txEsc(crc&0xFF,0);
  Serial.flush(); deTx(0);
}
static void sendAck(uint32_t to){ sendFrame(to, 0x19|(uint8_t)(rxSenderNum<<5), 0,0); }
static void sendInfo(uint32_t to,const uint8_t* d,uint8_t len){ sendFrame(to, 0x98|(uint8_t)(rxSenderNum<<5), d,len); }
static void makeSerial(uint32_t a,uint8_t* b){ b[0]='H';b[1]='B';b[2]='W'; for(int8_t p=9;p>2;p--){b[p]='0'+(a%10); if(a)a/=10;} }
static void sendAnnounce(){
  uint8_t d[16]={CMD_ANNOUNCE,0,DEVICE_TYPE,HW_VERSION,(FW_VERSION>>8)&0xFF,FW_VERSION&0xFF};
  makeSerial(OWN_ADDRESS,&d[6]); sendFrame(0xFFFFFFFFUL,0xF8,d,16);
}

// ----------------------------------------------------------- RX (aus Sim)
static uint8_t rxb[RX_BUFSIZE], rxIdx=0, rxHeaderLen=0, rxHasSender=0, inFrame=0, pendingEsc=0;
static int16_t rxTotal=-1; static uint16_t rxCrc=0xFFFF;
static void rxReset(){ inFrame=0;pendingEsc=0;rxIdx=0;rxTotal=-1;rxHeaderLen=0; }
static uint8_t pollFrame(uint32_t* target,uint8_t* control,uint32_t* sender,uint8_t** data,uint8_t* dlen){
  while(Serial.available()){
    uint8_t b=Serial.read();
    if(b==FRAME_START||b==FRAME_START_SHORT){ inFrame=1;pendingEsc=0;rxIdx=0;rxTotal=-1;rxHeaderLen=0;rxCrc=0xFFFF;crc16Shift(b,&rxCrc);continue; }
    if(!inFrame) continue;
    if(b==ESCAPE_BYTE){ pendingEsc=1;continue; }
    if(pendingEsc){ b|=0x80;pendingEsc=0; }
    crc16Shift(b,&rxCrc); if(rxIdx<RX_BUFSIZE) rxb[rxIdx]=b;
    if(rxIdx==4){ uint8_t c=b,isInfo=((c&1)==0),isAck=((c&7)==1); rxHasSender=(isInfo||isAck)?(c&8):0; rxHeaderLen=rxHasSender?10:6; }
    if(rxHeaderLen && rxIdx==(rxHeaderLen-1)) rxTotal=rxHeaderLen+b;
    rxIdx++;
    if(rxTotal>0 && rxIdx==rxTotal){
      if(rxCrc!=0){ rxReset(); return 0; }
      *target=((uint32_t)rxb[0]<<24)|((uint32_t)rxb[1]<<16)|((uint32_t)rxb[2]<<8)|rxb[3];
      *control=rxb[4];
      *sender=rxHasSender?(((uint32_t)rxb[5]<<24)|((uint32_t)rxb[6]<<16)|((uint32_t)rxb[7]<<8)|rxb[8]):0;
      uint8_t wl=rxb[rxHeaderLen-1]; *dlen=(wl>=2)?wl-2:0; *data=&rxb[rxHeaderLen];
      rxReset(); return 1;
    }
    if(rxIdx>=RX_BUFSIZE) rxReset();
  }
  return 0;
}

// ----------------------------------------------------------- Logik
static uint8_t zCount=0, zeroComm=0;
static void handleFrame(uint32_t target,uint8_t control,uint32_t sender,uint8_t* data,uint8_t dlen){
  rxSenderNum=(control>>1)&0x03;
  uint8_t broadcast=(target==0xFFFFFFFFUL);
  if(!broadcast && target!=OWN_ADDRESS) return;
  if(dlen==0) return;
  uint8_t cmd=data[0];
  if(cmd==CMD_ZERO_START){ if(zCount>=1) zeroComm=1; else zCount++; return; }
  if(cmd==CMD_ZERO_END){ zeroComm=0; zCount=0; return; }
  if(zeroComm && cmd!=CMD_START_BOOTER) return;

  switch(cmd){
    case CMD_START_BOOTER:            // 'u' -> ACK, dann ECHTER Watchdog-Reset
      sendAck(sender);
      Serial.flush();                 // ACK vollstaendig raus, BEVOR wir resetten
      digitalWrite(LED_PIN,HIGH);
      wdt_enable(WDTO_15MS);          // Watchdog-Reset in 15 ms -> Booter uebernimmt
      for(;;){}                       // auf Reset warten
    case CMD_GET_FW:{ uint8_t r[3]={cmd,(FW_VERSION>>8)&0xFF,FW_VERSION&0xFF}; sendInfo(sender,r,3); break; }
    case CMD_GET_HW:{ uint8_t r[2]={cmd,HW_VERSION}; sendInfo(sender,r,2); break; }
    case CMD_GET_SERIAL:{ uint8_t r[11]; r[0]=cmd; makeSerial(OWN_ADDRESS,&r[1]); sendInfo(sender,r,11); break; }
    case CMD_ANNOUNCE: sendAnnounce(); break;
    default: break;                   // App ignoriert Booter-Kmds (p/w/r/g)
  }
}

static uint32_t lastAnnounce=0;
void setup(){
  wdt_disable();                      // falls wir per WDRF hier ankaemen: Watchdog aus
  pinMode(RS485_TXEN,OUTPUT); digitalWrite(RS485_TXEN,LOW);
  pinMode(LED_PIN,OUTPUT);    digitalWrite(LED_PIN,LOW);
  Serial.begin(BUS_BAUD, SERIAL_8E1);
  delay(200);
  sendAnnounce();
}
void loop(){
  uint32_t target,sender; uint8_t control,*data,dlen;
  if(pollFrame(&target,&control,&sender,&data,&dlen)) handleFrame(target,control,sender,data,dlen);
  if(millis()-lastAnnounce>3000){ lastAnnounce=millis(); sendAnnounce(); }
}
