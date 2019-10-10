#include "ntsc_240p.h"
#include "pal_240p.h"
#include "ntsc_feedbackclock.h"
#include "pal_feedbackclock.h"
#include "ntsc_1280x720.h"
#include "ntsc_1280x1024.h"
#include "ntsc_1920x1080.h"
#include "pal_1280x720.h"
#include "pal_1280x1024.h"
#include "pal_1920x1080.h"
#include "presetMdSection.h"
#include "presetDeinterlacerSection.h"
#include "presetHdBypassSection.h"
#include "ofw_RGBS.h"

#include <Wire.h>
#include "tv5725.h"
#include "framesync.h"
#include "osd.h"

#include <ESP8266WiFi.h>
// ESPAsyncTCP and ESPAsyncWebServer libraries by me-no-dev
// download (green "Clone or download" button) and extract to Arduino libraries folder
// Windows: "Documents\Arduino\libraries" or full path: "C:\Users\rama\Documents\Arduino\libraries"
// https://github.com/me-no-dev/ESPAsyncTCP
// https://github.com/me-no-dev/ESPAsyncWebServer
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "FS.h"
#include <DNSServer.h>
#include <WiFiUdp.h>
#include <ESP8266mDNS.h>  // mDNS library for finding gbscontrol.local on the local network
#include <ArduinoOTA.h>

// PersWiFiManager library by Ryan Downing
// https://github.com/r-downing/PersWiFiManager
// included in project root folder to allow modifications within limitations of the Arduino framework
// See 3rdparty/PersWiFiManager for unmodified source and license
#include "PersWiFiManager.h"

// WebSockets library by Markus Sattler
// https://github.com/Links2004/arduinoWebSockets
// included in src folder to allow header modifications within limitations of the Arduino framework
// See 3rdparty/WebSockets for unmodified source and license
#include "src/WebSockets.h"
#include "src/WebSocketsServer.h"

// Optional:
// ESP8266-ping library to aid debugging WiFi issues, install via Arduino library manager
//#define HAVE_PINGER_LIBRARY
#ifdef HAVE_PINGER_LIBRARY
#include <Pinger.h>
#include <PingerResponse.h>
unsigned long pingLastTime;
Pinger pinger; // pinger global object to aid debugging WiFi issues
#endif

typedef TV5725<GBS_ADDR> GBS;
const char* ap_ssid = "gbscontrol";
const char* ap_password = "qqqqqqqq";
// change device_hostname_full and device_hostname_partial to rename the device 
// (allows 2 or more on the same network)
// new: only use _partial throughout, comply to standards
const char* device_hostname_full = "gbscontrol.local";
const char* device_hostname_partial = "gbscontrol"; // for MDNS
//
static const char ap_info_string[] PROGMEM =
"(WiFi): AP mode (SSID: gbscontrol, pass 'qqqqqqqq'): Access 'gbscontrol.local' in your browser";
static const char st_info_string[] PROGMEM =
"(WiFi): Access 'http://gbscontrol:80' or 'http://gbscontrol.local' (or device IP) in your browser";

AsyncWebServer server(80);
DNSServer dnsServer;
WebSocketsServer webSocket(81);
//AsyncWebSocket webSocket("/ws");
PersWiFiManager persWM(server, dnsServer);

#define DEBUG_IN_PIN D6 // marked "D12/MISO/D6" (Wemos D1) or D6 (Lolin NodeMCU)
// SCL = D1 (Lolin), D15 (Wemos D1) // ESP8266 Arduino default map: SCL
// SDA = D2 (Lolin), D14 (Wemos D1) // ESP8266 Arduino default map: SDA
#define LEDON \
  pinMode(LED_BUILTIN, OUTPUT); \
  digitalWrite(LED_BUILTIN, LOW)
#define LEDOFF \
  digitalWrite(LED_BUILTIN, HIGH); \
  pinMode(LED_BUILTIN, INPUT)

// fast ESP8266 digitalRead (21 cycles vs 77), *should* work with all possible input pins
// but only "D7" and "D6" have been tested so far
#define digitalRead(x) ((GPIO_REG_READ(GPIO_IN_ADDRESS) >> x) & 1)

// feed the current measurement, get back the moving average
uint8_t getMovingAverage(uint8_t item)
{
  static const uint8_t sz = 16;
  static uint8_t arr[sz] = { 0 };
  static uint8_t pos = 0;

  arr[pos] = item;
  if (pos < (sz - 1)) {
    pos++;
  }
  else {
    pos = 0;
  }

  uint16_t sum = 0;
  for (uint8_t i = 0; i < sz; i++) {
    sum += arr[i];
  }
  return sum >> 4; // for array size 16
}

//
// Sync locking tunables/magic numbers
//
struct FrameSyncAttrs {
  static const uint8_t debugInPin = DEBUG_IN_PIN;
  static const uint32_t lockInterval = 60 * 16; // every 60 frames. good range for this: 30 to 90 (milliseconds)
  static const int16_t syncCorrection = 2; // Sync correction in scanlines to apply when phase lags target
  static const int32_t syncTargetPhase = 90; // Target vsync phase offset (output trails input) in degrees
};
typedef FrameSyncManager<GBS, FrameSyncAttrs> FrameSync;

struct MenuAttrs {
  static const int8_t shiftDelta = 4;
  static const int8_t scaleDelta = 4;
  static const int16_t vertShiftRange = 300;
  static const int16_t horizShiftRange = 400;
  static const int16_t vertScaleRange = 100;
  static const int16_t horizScaleRange = 130;
  static const int16_t barLength = 100;
};
typedef MenuManager<GBS, MenuAttrs> Menu;

// runTimeOptions holds system variables
struct runTimeOptions {
  uint8_t presetVlineShift;
  uint8_t videoStandardInput; // 0 - unknown, 1 - NTSC like, 2 - PAL like, 3 480p NTSC, 4 576p PAL
  uint8_t phaseSP;
  uint8_t phaseADC;
  uint8_t currentLevelSOG;
  uint8_t thisSourceMaxLevelSOG;
  uint8_t syncLockFailIgnore;
  uint8_t applyPresetDoneStage;
  uint8_t continousStableCounter;
  uint8_t noSyncCounter;
  uint8_t failRetryAttempts;
  uint8_t presetID;
  uint8_t HPLLState;
  uint8_t medResLineCount;
  uint8_t osr;
  boolean isInLowPowerMode;
  boolean clampPositionIsSet;
  boolean coastPositionIsSet;
  boolean phaseIsSet;
  boolean inputIsYpBpR;
  boolean syncWatcherEnabled;
  boolean outModeHdBypass;
  boolean printInfos;
  boolean sourceDisconnected;
  boolean webServerEnabled;
  boolean webServerStarted;
  boolean allowUpdatesOTA;
  boolean enableDebugPings;
  boolean autoBestHtotalEnabled;
  boolean videoIsFrozen;
  boolean forceRetime;
  boolean motionAdaptiveDeinterlaceActive;
  boolean deinterlaceAutoEnabled;
  boolean scanlinesEnabled;
  boolean boardHasPower;
  boolean presetIsPalForce60;
  boolean syncTypeCsync;
  boolean isValidForScalingRGBHV;
} rtos;
struct runTimeOptions *rto = &rtos;

// userOptions holds user preferences / customizations
struct userOptions {
  uint8_t presetPreference; // 0 - normal, 1 - feedback clock, 2 - customized, 3 - 1280x720, 4 - 1280x1024, 5 - 1920x1080, 10 - bypass
  uint8_t presetSlot;
  uint8_t enableFrameTimeLock;
  uint8_t frameTimeLockMethod;
  uint8_t enableAutoGain;
  uint8_t wantScanlines;
  uint8_t wantOutputComponent;
  uint8_t deintMode;
  uint8_t wantVdsLineFilter;
  uint8_t wantPeaking;
  uint8_t wantTap6;
  uint8_t preferScalingRgbhv;
  uint8_t PalForce60;
  uint8_t matchPresetSource;
  uint8_t wantStepResponse;
  uint8_t wantFullHeight;
  uint8_t enableCalibrationADC;
} uopts;
struct userOptions *uopt = &uopts;

// remember adc options across presets
struct adcOptions {
  uint8_t r_gain;
  uint8_t g_gain;
  uint8_t b_gain;
  uint8_t r_off;
  uint8_t g_off;
  uint8_t b_off;
} adcopts;
struct adcOptions *adco = &adcopts;

char typeOneCommand; // Serial / Web Server commands
char typeTwoCommand; // Serial / Web Server commands
//uint8_t globalDelay; // used for dev / debug

#if defined(ESP8266)
// serial mirror class for websocket logs
class SerialMirror : public Stream {
  size_t write(const uint8_t *data, size_t size) {
    if (ESP.getFreeHeap() > 20000) {
      webSocket.broadcastTXT(data, size);
    }
    else {
      webSocket.disconnect();
    }
    Serial.write(data, size);
    return size;
  }

  size_t write(const char* data, size_t size) {
    if (ESP.getFreeHeap() > 20000) {
      webSocket.broadcastTXT(data, size);
    }
    else {
      webSocket.disconnect();
    }
    Serial.write(data, size);
    return size;
  }

  size_t write(uint8_t data) {
    if (ESP.getFreeHeap() > 20000) {
      webSocket.broadcastTXT(&data, 1);
    }
    else {
      webSocket.disconnect();
    }
    Serial.write(data);
    return 1;
  }

  size_t write(char data) {
    if (ESP.getFreeHeap() > 20000) {
      webSocket.broadcastTXT(&data, 1);
    }
    else {
      webSocket.disconnect();
    }
    Serial.write(data);
    return 1;
  }

  int available() {
    return 0;
  }
  int read() {
    return -1;
  }
  int peek() {
    return -1;
  }
  void flush() {       }
};

SerialMirror SerialM;
#else
#define SerialM Serial
#endif

static uint8_t lastSegment = 0xFF;

static inline void writeOneByte(uint8_t slaveRegister, uint8_t value)
{
  writeBytes(slaveRegister, &value, 1);
}

static inline void writeBytes(uint8_t slaveRegister, uint8_t* values, uint8_t numValues)
{
  if (slaveRegister == 0xF0 && numValues == 1) {
    lastSegment = *values;
  }
  else
    GBS::write(lastSegment, slaveRegister, values, numValues);
}

void copyBank(uint8_t* bank, const uint8_t* programArray, uint16_t* index)
{
  for (uint8_t x = 0; x < 16; ++x) {
    bank[x] = pgm_read_byte(programArray + *index);
    (*index)++;
  }
}

void zeroAll()
{
  // turn processing units off first
  writeOneByte(0xF0, 0);
  writeOneByte(0x46, 0x00); // reset controls 1
  writeOneByte(0x47, 0x00); // reset controls 2

  // zero out entire register space
  for (int y = 0; y < 6; y++)
  {
    writeOneByte(0xF0, (uint8_t)y);
    for (int z = 0; z < 16; z++)
    {
      uint8_t bank[16];
      for (int w = 0; w < 16; w++)
      {
        bank[w] = 0;
        // exceptions
        //if (y == 5 && z == 0 && w == 2) {
        //  bank[w] = 0x51; // 5_02
        //}
        //if (y == 5 && z == 5 && w == 6) {
        //  bank[w] = 0x01; // 5_56
        //}
        //if (y == 5 && z == 5 && w == 7) {
        //  bank[w] = 0xC0; // 5_57
        //}
      }
      writeBytes(z * 16, bank, 16);
    }
  }
}

void loadHdBypassSection() {
  uint16_t index = 0;
  uint8_t bank[16];
  writeOneByte(0xF0, 1);
  for (int j = 3; j <= 5; j++) { // start at 0x30
    copyBank(bank, presetHdBypassSection, &index);
    writeBytes(j * 16, bank, 16);
  }
}

void loadPresetDeinterlacerSection() {
  uint16_t index = 0;
  uint8_t bank[16];
  writeOneByte(0xF0, 2);
  for (int j = 0; j <= 3; j++) { // start at 0x00
    copyBank(bank, presetDeinterlacerSection, &index);
    writeBytes(j * 16, bank, 16);
  }
}

void loadPresetMdSection() {
  uint16_t index = 0;
  uint8_t bank[16];
  writeOneByte(0xF0, 1);
  for (int j = 6; j <= 7; j++) { // start at 0x60
    copyBank(bank, presetMdSection, &index);
    writeBytes(j * 16, bank, 16);
  }
  bank[0] = pgm_read_byte(presetMdSection + index);
  bank[1] = pgm_read_byte(presetMdSection + index + 1);
  bank[2] = pgm_read_byte(presetMdSection + index + 2);
  bank[3] = pgm_read_byte(presetMdSection + index + 3);
  writeBytes(8 * 16, bank, 4); // MD section ends at 0x83, not 0x90
}

// programs all valid registers (the register map has holes in it, so it's not straight forward)
// 'index' keeps track of the current preset data location.
void writeProgramArrayNew(const uint8_t* programArray, boolean skipMDSection)
{
  uint16_t index = 0;
  uint8_t bank[16];
  uint8_t y = 0;

  //GBS::PAD_SYNC_OUT_ENZ::write(1);
  //GBS::DAC_RGBS_PWDNZ::write(0);    // no DAC
  //GBS::SFTRST_MEM_FF_RSTZ::write(0);  // stop mem fifos

  // should only be possible if previously was in RGBHV bypass, then hit a manual preset switch
  if (rto->videoStandardInput == 15) {
    rto->videoStandardInput = 0;
  }

  rto->outModeHdBypass = 0; // the default at this stage
  if (GBS::ADC_INPUT_SEL::read() == 0) {
    //if (rto->inputIsYpBpR == 0) SerialM.println("rto->inputIsYpBpR was 0, fixing to 1");
    rto->inputIsYpBpR = 1; // new: update the var here, allow manual preset loads
  }
  else {
    //if (rto->inputIsYpBpR == 1) SerialM.println("rto->inputIsYpBpR was 1, fixing to 0");
    rto->inputIsYpBpR = 0;
  }

  uint8_t reset46 = GBS::RESET_CONTROL_0x46::read(); // for keeping these as they are now
  uint8_t reset47 = GBS::RESET_CONTROL_0x47::read();

  for (; y < 6; y++)
  {
    writeOneByte(0xF0, (uint8_t)y);
    switch (y) {
    case 0:
      for (int j = 0; j <= 1; j++) { // 2 times
        for (int x = 0; x <= 15; x++) {
          if (j == 0 && x == 4) {
            // keep DAC off
            bank[x] = pgm_read_byte(programArray + index); // &~(1 << 0);
          }
          else if (j == 0 && x == 6) {
            bank[x] = reset46;
          }
          else if (j == 0 && x == 7) {
            bank[x] = reset47;
          }
          else if (j == 0 && x == 9) {
            // keep sync output off
            bank[x] = pgm_read_byte(programArray + index); // | (1 << 2);
          }
          else {
            // use preset values
            bank[x] = pgm_read_byte(programArray + index);
          }

          index++;
        }
        writeBytes(0x40 + (j * 16), bank, 16);
      }
      copyBank(bank, programArray, &index);
      writeBytes(0x90, bank, 16);
      break;
    case 1:
      for (int j = 0; j <= 2; j++) { // 3 times
        copyBank(bank, programArray, &index);
        if (j == 0) {
          bank[0] = bank[0] & ~(1 << 5); // clear 1_00 5
          bank[1] = bank[1] | (1 << 0);  // set 1_01 0
          bank[12] = bank[12] & 0x0f;    // clear 1_0c upper bits
          bank[13] = 0;                  // clear 1_0d
        }
        writeBytes(j * 16, bank, 16);
      }
      if (!skipMDSection) {
        loadPresetMdSection();
        if (rto->inputIsYpBpR)  GBS::MD_SEL_VGA60::write(0);  // EDTV possible
        else                    GBS::MD_SEL_VGA60::write(1);  // VGA 640x480 more likely

        GBS::MD_HD1250P_CNTRL::write(rto->medResLineCount); // patch med res support
      }
      break;
    case 2:
      loadPresetDeinterlacerSection();
      break;
    case 3:
      for (int j = 0; j <= 7; j++) { // 8 times
        copyBank(bank, programArray, &index);
        writeBytes(j * 16, bank, 16);
      }
      // blank out VDS PIP registers, otherwise they can end up uninitialized
      for (int x = 0; x <= 15; x++) {
        writeOneByte(0x80 + x, 0x00);
      }
      break;
    case 4:
      for (int j = 0; j <= 5; j++) { // 6 times
        copyBank(bank, programArray, &index);
        writeBytes(j * 16, bank, 16);
      }
      break;
    case 5:
      for (int j = 0; j <= 6; j++) { // 7 times
        for (int x = 0; x <= 15; x++) {
          bank[x] = pgm_read_byte(programArray + index);
          if (index == 322) { // s5_02 bit 6+7 = input selector (only bit 6 is relevant)
            if (rto->inputIsYpBpR) bitClear(bank[x], 6);
            else bitSet(bank[x], 6);
          }
          if (index == 323) { // s5_03 set clamps according to input channel
            if (rto->inputIsYpBpR) {
              bitClear(bank[x], 2); // G bottom clamp
              bitSet(bank[x], 1);   // R mid clamp
              bitSet(bank[x], 3);   // B mid clamp
            }
            else {
              bitClear(bank[x], 2); // G bottom clamp
              bitClear(bank[x], 1); // R bottom clamp
              bitClear(bank[x], 3); // B bottom clamp
            }
          }
          //if (index == 324) { // s5_04 reset(0) for ADC REF init
          //  bank[x] = 0x00;
          //}
          if (index == 382) { // s5_3e
            bitSet(bank[x], 5); // SP_DIS_SUB_COAST = 1
          }
          if (index == 407) { // s5_57
            bitSet(bank[x], 0); // SP_NO_CLAMP_REG = 1
          }
          index++;
        }
        writeBytes(j * 16, bank, 16);
      }
      break;
    }
  }

  // scaling RGBHV mode
  if (uopt->preferScalingRgbhv && rto->isValidForScalingRGBHV) {
    GBS::GBS_OPTION_SCALING_RGBHV::write(1);
    rto->videoStandardInput = 3;
  }
}

void resetInterruptSogBadBit() {
  GBS::INT_CONTROL_RST_SOGBAD::write(1);
  GBS::INT_CONTROL_RST_SOGBAD::write(0);
}

void resetInterruptNoHsyncBadBit() {
  GBS::INT_CONTROL_RST_NOHSYNC::write(1);
  GBS::INT_CONTROL_RST_NOHSYNC::write(0);
}

void setResetParameters() {
  SerialM.println("<reset>");
  rto->videoStandardInput = 0;
  rto->videoIsFrozen = false;
  rto->applyPresetDoneStage = 0;
  rto->presetVlineShift = 0;
  rto->sourceDisconnected = true;
  rto->outModeHdBypass = 0;
  rto->clampPositionIsSet = 0;
  rto->coastPositionIsSet = 0;
  rto->phaseIsSet = 0;
  rto->continousStableCounter = 0;
  rto->noSyncCounter = 0;
  rto->isInLowPowerMode = false;
  rto->currentLevelSOG = 5;
  rto->thisSourceMaxLevelSOG = 31;  // 31 = auto sog has not (yet) run
  rto->failRetryAttempts = 0;
  rto->HPLLState = 0;
  rto->motionAdaptiveDeinterlaceActive = false;
  rto->scanlinesEnabled = false;
  rto->syncTypeCsync = false;
  rto->isValidForScalingRGBHV = false;
  rto->medResLineCount = 0x33; // 51*8=408
  rto->osr = 0;

  adco->r_gain = 0;
  adco->g_gain = 0;
  adco->b_gain = 0;

  // clear temp storage
  GBS::ADC_UNUSED_64::write(0); GBS::ADC_UNUSED_65::write(0);
  GBS::ADC_UNUSED_66::write(0); GBS::ADC_UNUSED_67::write(0);
  GBS::GBS_PRESET_CUSTOM::write(0);
  GBS::GBS_PRESET_ID::write(0);
  GBS::GBS_OPTION_SCALING_RGBHV::write(0);
  GBS::GBS_OPTION_PALFORCED60_ENABLED::write(0);

  GBS::OUT_SYNC_CNTRL::write(0);          // no H / V sync out to PAD
  GBS::DAC_RGBS_PWDNZ::write(0);          // disable DAC
  GBS::ADC_TA_05_CTRL::write(0x02);       // 5_05 1 // minor SOG clamp effect
  GBS::ADC_TEST_04::write(0x02);          // 5_04
  GBS::ADC_TEST_0C::write(0x12);          // 5_0c 1 4
  GBS::ADC_CLK_PA::write(0);              // 5_00 0/1 PA_ADC input clock = PLLAD CLKO2
  GBS::ADC_SOGEN::write(1);
  GBS::SP_SOG_MODE::write(1);
  GBS::ADC_INPUT_SEL::write(1);           // 1 = RGBS / RGBHV adc data input
  GBS::ADC_POWDZ::write(1);               // ADC on
  setAndUpdateSogLevel(rto->currentLevelSOG);
  GBS::RESET_CONTROL_0x46::write(0x00);   // all units off
  GBS::RESET_CONTROL_0x47::write(0x00);
  GBS::GPIO_CONTROL_00::write(0x67);      // most GPIO pins regular GPIO
  GBS::GPIO_CONTROL_01::write(0x00);      // all GPIO outputs disabled
  GBS::DAC_RGBS_PWDNZ::write(0);          // disable DAC (output)
  GBS::PLL648_CONTROL_01::write(0x00);    // VCLK(1/2/4) display clock // needs valid for debug bus
  GBS::PAD_CKOUT_ENZ::write(1);           // clock output disable
  GBS::IF_SEL_ADC_SYNC::write(1);         // ! 1_28 2
  GBS::PLLAD_VCORST::write(1);            // reset = 1
  GBS::PLL_ADS::write(1); // When = 1, input clock is from ADC ( otherwise, from unconnected clock at pin 40 )
  GBS::PLL_CKIS::write(0); // PLL use OSC clock
  GBS::PLL_MS::write(2); // fb memory clock can go lower power
  GBS::PAD_CONTROL_00_0x48::write(0x2b); //disable digital inputs, enable debug out pin
  GBS::PAD_CONTROL_01_0x49::write(0x1f); //pad control pull down/up transistors on
  loadHdBypassSection(); // 1_30 to 1_55
  loadPresetMdSection(); // 1_60 to 1_83
  setAdcParametersGainAndOffset();
  GBS::SP_PRE_COAST::write(9);        // was 0x07 // need pre / poast to allow all sources to detect
  GBS::SP_POST_COAST::write(18);      // was 0x10 // ps2 1080p 18
  GBS::SP_NO_COAST_REG::write(0);     // can be 1 in some soft reset situations, will prevent sog vsync decoding
  GBS::SP_CS_CLP_ST::write(32);       // define it to something at start
  GBS::SP_CS_CLP_SP::write(48);
  GBS::SP_SOG_SRC_SEL::write(0);      // SOG source = ADC
  GBS::SP_EXT_SYNC_SEL::write(0);     // connect HV input ( 5_20 bit 3 )
  GBS::SP_NO_CLAMP_REG::write(1);
  GBS::PLLAD_ICP::write(0);           // lowest charge pump current
  GBS::PLLAD_FS::write(0);            // low gain (have to deal with cold and warm startups)
  GBS::PLLAD_5_16::write(0x1f);
  GBS::PLLAD_MD::write(0x700);
  resetPLL(); // cycles PLL648
  delay(2);
  resetPLLAD(); // same for PLLAD
  GBS::PLL_VCORST::write(1); // reset on
  GBS::PLLAD_CONTROL_00_5x11::write(0x01); // reset on
  resetDebugPort(); 

  //GBS::RESET_CONTROL_0x47::write(0x16);
  GBS::RESET_CONTROL_0x46::write(0x41);     // new 23.07.19
  GBS::RESET_CONTROL_0x47::write(0x17);     // new 23.07.19 (was 0x16)
  GBS::INTERRUPT_CONTROL_01::write(0xff);   // enable interrupts
  GBS::INTERRUPT_CONTROL_00::write(0xff);   // reset irq status
  GBS::INTERRUPT_CONTROL_00::write(0x00);
  GBS::PAD_SYNC_OUT_ENZ::write(0);          // sync output enabled, will be low (HC125 fix)
  rto->clampPositionIsSet = 0;              // some functions override these, so make sure
  rto->coastPositionIsSet = 0;
  rto->phaseIsSet = 0;
  rto->continousStableCounter = 0;
  typeOneCommand = '@';
  typeTwoCommand = '@';
}

void OutputComponentOrVGA() {
  
  boolean isCustomPreset = GBS::GBS_PRESET_CUSTOM::read();
  if (uopt->wantOutputComponent) {
    SerialM.println(F("Output Format: Component"));
    GBS::VDS_SYNC_LEV::write(0x80); // 0.25Vpp sync (leave more room for Y)
    GBS::VDS_CONVT_BYPS::write(1); // output YUV
    GBS::OUT_SYNC_CNTRL::write(0); // no H / V sync out to PAD
    // patch up some presets
    uint8_t id = GBS::GBS_PRESET_ID::read();
    if (!isCustomPreset) {
      if (id == 0x02 || id == 0x12 || id == 0x01 || id == 0x11) { // 1280x1024, 1280x960 presets
        set_vtotal(1090); // 1080 is enough lines to trick my tv into "1080p" mode
        if (id == 0x02 || id == 0x01) { // 60
          GBS::IF_VB_SP::write(2); // GBS::IF_VB_SP::read() - 16 // better fix this
          GBS::IF_VB_ST::write(0); // GBS::IF_VB_ST::read() - 16
          GBS::VDS_HS_SP::write(10);
        }
        else { // 50
          GBS::VDS_DIS_HB_ST::write(GBS::VDS_DIS_HB_ST::read() - 70);
          GBS::VDS_HSCALE::write(724);
          GBS::IF_VB_SP::write(2); // GBS::IF_VB_SP::read() - 18
          GBS::IF_VB_ST::write(0); // GBS::IF_VB_ST::read() - 18
        }
        rto->forceRetime = true;
      }
    }
  }
  else {
    GBS::VDS_SYNC_LEV::write(0);
    GBS::VDS_CONVT_BYPS::write(0); // output RGB
    GBS::OUT_SYNC_CNTRL::write(1); // H / V sync out enable
  }

  if (!isCustomPreset) {
    if (rto->inputIsYpBpR == true) {
      applyYuvPatches();
    }
    else {
      applyRGBPatches();
    }
  }
}

void applyComponentColorMixing() {
  GBS::VDS_Y_GAIN::write(0x64); // 3_35
  GBS::VDS_UCOS_GAIN::write(0x19); // 3_36
  GBS::VDS_VCOS_GAIN::write(0x19); // 3_37
  GBS::VDS_Y_OFST::write(0xfe); // 3_3a
  GBS::VDS_U_OFST::write(0x01); // 3_3b
}

void applyYuvPatches() {
  GBS::ADC_RYSEL_R::write(1); // midlevel clamp red
  GBS::ADC_RYSEL_B::write(1); // midlevel clamp blue
  GBS::ADC_RYSEL_G::write(0); // gnd clamp green
  GBS::DEC_MATRIX_BYPS::write(1); // ADC
  GBS::IF_MATRIX_BYPS::write(1);

  if (GBS::GBS_PRESET_CUSTOM::read() == 0) {
    // colors
    GBS::VDS_Y_GAIN::write(0x80); // 3_25 0x7f
    GBS::VDS_UCOS_GAIN::write(0x1c); // 3_26 blue
    GBS::VDS_VCOS_GAIN::write(0x27); // 3_27 red
    GBS::VDS_Y_OFST::write(0x00); // 3_3a // fe
    GBS::VDS_U_OFST::write(0x02); // 3_3b // with new adc offset calibration
    GBS::VDS_V_OFST::write(0x02); // 3_3c
  }

  if (uopt->wantOutputComponent) {
    applyComponentColorMixing();
  }
}

void applyRGBPatches() {
  GBS::ADC_RYSEL_R::write(0); // gnd clamp red
  GBS::ADC_RYSEL_B::write(0); // gnd clamp blue
  GBS::ADC_RYSEL_G::write(0); // gnd clamp green
  GBS::DEC_MATRIX_BYPS::write(0); // 5_1f 2 = 1 for YUV / 0 for RGB
  GBS::IF_MATRIX_BYPS::write(1);

  if (GBS::GBS_PRESET_CUSTOM::read() == 0) {
    // colors
    GBS::VDS_Y_GAIN::write(0x80); // 0x80 = 0
    GBS::VDS_UCOS_GAIN::write(0x1c); // blue
    GBS::VDS_VCOS_GAIN::write(0x28); // red
    GBS::VDS_Y_OFST::write(0x00); // 3_3a
    GBS::VDS_U_OFST::write(0x02); // 3_3b // with new adc offset calibration
    GBS::VDS_V_OFST::write(0x02); // 3_3c
  }

  if (uopt->wantOutputComponent) {
    applyComponentColorMixing();
  }
}

void setAdcParametersGainAndOffset() {
  GBS::ADC_ROFCTRL::write(0x3F);
  GBS::ADC_GOFCTRL::write(0x3F);
  GBS::ADC_BOFCTRL::write(0x3F);
  GBS::ADC_RGCTRL::write(0x7B);
  GBS::ADC_GGCTRL::write(0x7B);
  GBS::ADC_BGCTRL::write(0x7B);
}

void updateHVSyncEdge() {
  static uint8_t printHS = 0, printVS = 0;
  uint16_t temp = 0;

  if (GBS::STATUS_INT_SOG_BAD::read() == 1) {
    resetInterruptSogBadBit();
    return;
  }

  uint8_t syncStatus = GBS::STATUS_16::read();
  if (rto->syncTypeCsync) {
    // sog check, only check H
    if ((syncStatus & 0x02) != 0x02) return;
  }
  else {
    // HV check, check H + V
    if ((syncStatus & 0x0a) != 0x0a) return;
  }

  if ((syncStatus & 0x02) != 0x02) // if !hs active
  {
    //SerialM.println("(SP) can't detect sync edge");
  }
  else
  {
    if ((syncStatus & 0x01) == 0x00)
    {
      if (printHS != 1) { SerialM.println(F("(SP) HS active low")); }
      printHS = 1;

      temp = GBS::HD_HS_SP::read();
      if (GBS::HD_HS_ST::read() < temp) { // if out sync = ST < SP
        GBS::HD_HS_SP::write(GBS::HD_HS_ST::read());
        GBS::HD_HS_ST::write(temp);
        GBS::SP_HS2PLL_INV_REG::write(1);
        //GBS::SP_SOG_P_INV::write(0); // 5_20 2 //could also use 5_20 1 "SP_SOG_P_ATO"
      }
    }
    else
    {
      if (printHS != 2) { SerialM.println(F("(SP) HS active high")); }
      printHS = 2;

      temp = GBS::HD_HS_SP::read();
      if (GBS::HD_HS_ST::read() > temp) { // if out sync = ST > SP
        GBS::HD_HS_SP::write(GBS::HD_HS_ST::read());
        GBS::HD_HS_ST::write(temp);
        GBS::SP_HS2PLL_INV_REG::write(0);
        //GBS::SP_SOG_P_INV::write(1); // 5_20 2 //could also use 5_20 1 "SP_SOG_P_ATO"
      }
    }

    // VS check, but only necessary for separate sync (CS should have VS always active low)
    if (rto->syncTypeCsync == false)
    {
      if ((syncStatus & 0x08) != 0x08) // if !vs active
      {
        Serial.println("VS can't detect sync edge");
      }
      else
      {
        if ((syncStatus & 0x04) == 0x00)
        {
          if (printVS != 1) { SerialM.println(F("(SP) VS active low")); }
          printVS = 1;

          temp = GBS::HD_VS_SP::read();
          if (GBS::HD_VS_ST::read() < temp) { // if out sync = ST < SP
            GBS::HD_VS_SP::write(GBS::HD_VS_ST::read());
            GBS::HD_VS_ST::write(temp);
          }
        }
        else
        {
          if (printVS != 2) { SerialM.println(F("(SP) VS active high")); }
          printVS = 2;

          temp = GBS::HD_VS_SP::read();
          if (GBS::HD_VS_ST::read() > temp) { // if out sync = ST > SP
            GBS::HD_VS_SP::write(GBS::HD_VS_ST::read());
            GBS::HD_VS_ST::write(temp);
          }
        }
      }
    }
  }
}

void prepareSyncProcessor() {
  writeOneByte(0xF0, 5);
  GBS::SP_SOG_P_ATO::write(0); // 5_20 disable sog auto polarity // hpw can be > ht, but auto is worse
  GBS::SP_JITTER_SYNC::write(0);
  // H active detect control
  writeOneByte(0x21, 0x18); // SP_SYNC_TGL_THD    H Sync toggle times threshold  0x20; lower than 5_33(not always); 0 to turn off (?) 0x18 for 53.69 system @ 33.33
  writeOneByte(0x22, 0x0F); // SP_L_DLT_REG       Sync pulse width different threshold (little than this as equal)
  writeOneByte(0x23, 0x00); // UNDOCUMENTED       range from 0x00 to at least 0x1d
  writeOneByte(0x24, 0x40); // SP_T_DLT_REG       H total width different threshold rgbhv: b // range from 0x02 upwards
  writeOneByte(0x25, 0x00); // SP_T_DLT_REG
  writeOneByte(0x26, 0x04); // SP_SYNC_PD_THD     H sync pulse width threshold // from 0(?) to 0x50 // in yuv 720p range only to 0x0a!
  writeOneByte(0x27, 0x00); // SP_SYNC_PD_THD
  writeOneByte(0x2a, 0x0F); // SP_PRD_EQ_THD      How many legal lines as valid; scales with 5_33 (needs to be below)
  // V active detect control
  // these 4 only have effect with HV input; test:  s5s2ds34 s5s2es24 s5s2fs16 s5s31s84
  writeOneByte(0x2d, 0x03); // SP_VSYNC_TGL_THD   V sync toggle times threshold // at 5 starts to drop many 0_16 vsync events
  writeOneByte(0x2e, 0x00); // SP_SYNC_WIDTH_DTHD V sync pulse width threshod
  writeOneByte(0x2f, 0x02); // SP_V_PRD_EQ_THD    How many continue legal v sync as valid // at 4 starts to drop 0_16 vsync events
  writeOneByte(0x31, 0x2f); // SP_VT_DLT_REG      V total different threshold
  // Timer value control
  writeOneByte(0x33, 0x3a); // SP_H_TIMER_VAL     H timer value for h detect (was 0x28)
  writeOneByte(0x34, 0x06); // SP_V_TIMER_VAL     V timer for V detect // 0_16 vsactive // was 0x05
  // Sync separation control
  if (rto->videoStandardInput == 0) GBS::SP_DLT_REG::write(0x70); // 5_35  130 too much for ps2 1080i, 0xb0 for 1080p
  else if (rto->videoStandardInput <= 4) GBS::SP_DLT_REG::write(0x130); // would be best to measure somehow
  else if (rto->videoStandardInput <= 6) GBS::SP_DLT_REG::write(0x110);
  else if (rto->videoStandardInput == 7) GBS::SP_DLT_REG::write(0x70);
  else GBS::SP_DLT_REG::write(0x70);
  GBS::SP_H_PULSE_IGNOR::write(0x02); // test with MS / Genesis mode (wsog 2) vs ps2 1080p (0x13 vs 0x05)

  // leave out pre / post coast here
  // 5_3a  attempted 2 for 1chip snes 239 mode intermittency, works fine except for MD in MS mode
  // make sure this is stored in the presets as well, as it affects sync time
  GBS::SP_H_TOTAL_EQ_THD::write(3);
  //  test NTSC: s5s3bs11 s5s3fs09 s5s40s0b
  //  test PAL: s5s3bs11 s5s3fs38 s5s40s3c
  GBS::SP_SDCS_VSST_REG_H::write(0);
  GBS::SP_SDCS_VSSP_REG_H::write(0);
  GBS::SP_SDCS_VSST_REG_L::write(12); // 5_3f test with bypass mode: t0t4ft7 t0t4bt2 t5t56t4 t5t11t3
  GBS::SP_SDCS_VSSP_REG_L::write(11); // 5_40  // was 11
  //GBS::SP_SDCS_VSST_REG_L::write(2); // S5_3F
  //GBS::SP_SDCS_VSSP_REG_L::write(0); // S5_40

  GBS::SP_CS_HS_ST::write(0);    // 5_45
  GBS::SP_CS_HS_SP::write(0x40); // 5_47 720p source needs ~20 range, may be necessary to adjust at runtime, based on source res

  writeOneByte(0x49, 0x00); // retime HS start for RGB+HV rgbhv: 20
  writeOneByte(0x4a, 0x00); //
  writeOneByte(0x4b, 0x44); // retime HS stop rgbhv: 50
  writeOneByte(0x4c, 0x00); //

  writeOneByte(0x51, 0x02); // 0x00 rgbhv: 2
  writeOneByte(0x52, 0x00); // 0xc0
  writeOneByte(0x53, 0x00); // 0x05 rgbhv: 6
  writeOneByte(0x54, 0x00); // 0xc0

  if (rto->videoStandardInput != 15 && (GBS::GBS_OPTION_SCALING_RGBHV::read() != 1)) {
    GBS::SP_CLAMP_MANUAL::write(0); // 0 = automatic on/off possible
    GBS::SP_CLP_SRC_SEL::write(0);  // clamp source 1: pixel clock, 0: 27mhz // was 1 but the pixel clock isn't available at first
    GBS::SP_NO_CLAMP_REG::write(1); // 5_57_0 unlock clamp
    GBS::SP_SOG_MODE::write(1);
    GBS::SP_H_CST_ST::write(0x18);   // 5_4d
    GBS::SP_H_CST_SP::write(0x80);   // 5_4f // how low (high) may this go? source dependant
    GBS::SP_DIS_SUB_COAST::write(1); // coast initially off 5_3e 5
    GBS::SP_H_PROTECT::write(0);     // SP_H_PROTECT off
    GBS::SP_HCST_AUTO_EN::write(0);
  }

  GBS::SP_HS_REG::write(1);          // 5_57 7
  GBS::SP_HS_PROC_INV_REG::write(0); // no SP sync inversion
  GBS::SP_VS_PROC_INV_REG::write(0); //

  writeOneByte(0x58, 0x05); //rgbhv: 0
  writeOneByte(0x59, 0x00); //rgbhv: 0
  writeOneByte(0x5a, 0x01); //rgbhv: 0 // was 0x05 but 480p ps2 doesnt like it
  writeOneByte(0x5b, 0x00); //rgbhv: 0
  writeOneByte(0x5c, 0x03); //rgbhv: 0
  writeOneByte(0x5d, 0x02); //rgbhv: 0 // range: 0 - 0x20 (how long should clamp stay off)
}

// Sync detect resolution: 5bits; comparator voltage range 10mv~320mv.
// -> 10mV per step; if cables and source are to standard (level 6 = 60mV)
void setAndUpdateSogLevel(uint8_t level) {
  rto->currentLevelSOG = level & 0x1f;
  GBS::ADC_SOGCTRL::write(level);
  setAndLatchPhaseSP();
  setAndLatchPhaseADC();
  latchPLLAD();
  GBS::INTERRUPT_CONTROL_00::write(0xff); // reset irq status
  GBS::INTERRUPT_CONTROL_00::write(0x00);
  //Serial.print("sog: "); Serial.println(rto->currentLevelSOG);
}

// in operation: t5t04t1 for 10% lower power on ADC
// also: s0s40s1c for 5% (lower memclock of 108mhz)
// for some reason: t0t45t2 t0t45t4 (enable SDAC, output max voltage) 5% lower  done in presets
// t0t4ft4 clock out should be off
// s4s01s20 (was 30) faster latency // unstable at 108mhz
// both phase controls off saves some power 506ma > 493ma
// oversample ratio can save 10% at 1x
// t3t24t3 VDS_TAP6_BYPS on can save 2%

// Generally, the ADC has to stay enabled to perform SOG separation and thus "see" a source.
// It is possible to run in low power mode.
// Function should not further nest, so it can be called in syncwatcher
void goLowPowerWithInputDetection() {
  GBS::OUT_SYNC_CNTRL::write(0); // no H / V sync out to PAD
  GBS::DAC_RGBS_PWDNZ::write(0); // direct disableDAC()
  //zeroAll();
  setResetParameters(); // includes rto->videoStandardInput = 0
  prepareSyncProcessor();
  delay(100);
  rto->isInLowPowerMode = true;
  SerialM.println(F("Scanning inputs for sources ..."));
  LEDOFF;
}

boolean optimizePhaseSP() {
  uint16_t pixelClock = GBS::PLLAD_MD::read();
  uint8_t badHt = 0, prevBadHt = 0, worstBadHt = 0, worstPhaseSP = 0, prevPrevBadHt = 0, goodHt = 0;
  boolean runTest = 1;

  if (GBS::STATUS_SYNC_PROC_HTOTAL::read() < (pixelClock - 8)) {
    return 0;
  }
  if (GBS::STATUS_SYNC_PROC_HTOTAL::read() > (pixelClock + 8)) {
    return 0;
  }

  if (rto->currentLevelSOG <= 2) {
    // not very stable, use fixed values
    rto->phaseSP  = 16;
    rto->phaseADC = 16;
    if (rto->videoStandardInput > 0 && rto->videoStandardInput <= 4) {
      if (rto->osr == 4) {
        rto->phaseADC += 16; rto->phaseADC &= 0x1f;
      }
    }
    delay(8);      // a bit longer, to match default run time
    runTest = 0;    // skip to end
  }

  //unsigned long startTime = millis();

  if (runTest) {
    // 32 distinct phase settings, 3 average samples (missing 2 phase steps) > 34
    for (uint8_t u = 0; u < 34; u++) {
      rto->phaseSP++;
      rto->phaseSP &= 0x1f;
      setAndLatchPhaseSP();
      badHt = 0;
      delayMicroseconds(256);
      for (uint8_t i = 0; i < 20; i++) {
        if (GBS::STATUS_SYNC_PROC_HTOTAL::read() != pixelClock) {
          badHt++;
          delayMicroseconds(384);
        }
      }
      // if average 3 samples has more badHt than seen yet, this phase step is worse
      if ((badHt + prevBadHt + prevPrevBadHt) > worstBadHt) {
        worstBadHt = (badHt + prevBadHt + prevPrevBadHt);
        worstPhaseSP = (rto->phaseSP - 1) & 0x1f; // medium of 3 samples
      }

      if (badHt == 0) {
        // count good readings as well, to know whether the entire run is valid
        goodHt++;
      }

      prevPrevBadHt = prevBadHt;
      prevBadHt = badHt;
      //Serial.print(rto->phaseSP); Serial.print(" badHt: "); Serial.println(badHt);
    }

    //Serial.println(goodHt);

    if (goodHt < 17) {
      //Serial.println("pxClk unstable");
      return 0;
    }

    // adjust global phase values according to test results
    if (worstBadHt != 0) {
      rto->phaseSP = (worstPhaseSP + 16) & 0x1f;
      // assume color signals arrive at same time: phase adc = phase sp
      // test in hdbypass mode shows this is more related to sog.. the assumptions seem fine at sog = 8
      rto->phaseADC = 16; //(rto->phaseSP) & 0x1f;

      // different OSR require different phase angles, also depending on bypass, etc
      // shift ADC phase 180 degrees for the following
      if (rto->videoStandardInput >= 5 && rto->videoStandardInput <= 7) {
        if (rto->osr == 2) {
          //Serial.println("shift adc phase");
          rto->phaseADC += 16; rto->phaseADC &= 0x1f;
        }
      }
      else if (rto->videoStandardInput > 0 && rto->videoStandardInput <= 4) {
        if (rto->osr == 4) {
          //Serial.println("shift adc phase");
          rto->phaseADC += 16; rto->phaseADC &= 0x1f;
        }
      }
    }
    else {
      // test was always good, so choose any reasonable value
      rto->phaseSP = 16;
      rto->phaseADC = 16;
      if (rto->videoStandardInput > 0 && rto->videoStandardInput <= 4) {
        if (rto->osr == 4) {
          rto->phaseADC += 16; rto->phaseADC &= 0x1f;
        }
      }
    }
  }

  //Serial.println(millis() - startTime);
  //Serial.print("worstPhaseSP: "); Serial.println(worstPhaseSP);
  SerialM.print("Phase: "); SerialM.print(rto->phaseSP);
  SerialM.print(" SOG: ");  SerialM.print(rto->currentLevelSOG);
  SerialM.println();
  setAndLatchPhaseSP();
  delay(1);
  setAndLatchPhaseADC();

  return 1;
}

void optimizeSogLevel() {
  if (rto->boardHasPower == false) // checkBoardPower is too invasive now
  {
    rto->thisSourceMaxLevelSOG = rto->currentLevelSOG = 13;
    return;
  }
  if (rto->videoStandardInput == 15 || GBS::SP_SOG_MODE::read() != 1 || rto->syncTypeCsync == false) {
    rto->thisSourceMaxLevelSOG = rto->currentLevelSOG = 13;
    return;
  }

  if (rto->inputIsYpBpR) {
    rto->thisSourceMaxLevelSOG = rto->currentLevelSOG = 14;
  }
  else {
    rto->thisSourceMaxLevelSOG = rto->currentLevelSOG = 13;  // similar to yuv, allow variations
  }
  setAndUpdateSogLevel(rto->currentLevelSOG);

  resetModeDetect();
  delay(100);
  //unfreezeVideo();
  delay(160);

  while (1) {
    uint8_t syncGoodCounter = 0;
    unsigned long timeout = millis();
    while ((millis() - timeout) < 370) {
      if ((getStatus16SpHsStable() == 1) && (GBS::STATUS_SYNC_PROC_HLOW_LEN::read() > 10))
      {
        syncGoodCounter++;
        if (syncGoodCounter >= 30) { break; }
      }
    }

    if (syncGoodCounter >= 30) {
      delay(180); // to recognize video mode
      if (getVideoMode() != 0) {
        syncGoodCounter = 0;
        delay(20);
        for (int a = 0; a < 50; a++) {
          syncGoodCounter++;
          if (GBS::STATUS_SYNC_PROC_HLOW_LEN::read() < 10) {
            syncGoodCounter = 0;
            break;
          }
        }
        if (syncGoodCounter >= 49) {
          //Serial.print(" @SOG "); Serial.print(rto->currentLevelSOG);
          //Serial.print(" STATUS_00: ");
          //uint8_t status00 = GBS::STATUS_00::read();
          //Serial.println(status00, HEX);
          break; // found, exit
        }
        else {
          //Serial.print(" inner test failed syncGoodCounter: "); Serial.println(syncGoodCounter);
        }
      }
      else { // getVideoMode() == 0
        //Serial.print("sog-- syncGoodCounter: "); Serial.println(syncGoodCounter);
      }
    }
    else { // syncGoodCounter < 40
      //Serial.print("outer test failed syncGoodCounter: "); Serial.println(syncGoodCounter);
    }

    if (rto->currentLevelSOG >= 4) {
      rto->currentLevelSOG -= 2;
      setAndUpdateSogLevel(rto->currentLevelSOG);
      delay(200); // time for sog to settle
    }
    else { 
      rto->currentLevelSOG = 2;
      setAndUpdateSogLevel(rto->currentLevelSOG);
      delay(200);
      break; // break and exit
    } 
  }

  rto->thisSourceMaxLevelSOG = rto->currentLevelSOG;
}

// GBS boards have 2 potential sync sources:
// - RCA connectors
// - VGA input / 5 pin RGBS header / 8 pin VGA header (all 3 are shared electrically)
// This routine looks for sync on the currently active input. If it finds it, the input is returned.
// If it doesn't find sync, it switches the input and returns 0, so that an active input will be found eventually.
uint8_t detectAndSwitchToActiveInput() { // if any
  uint8_t currentInput = GBS::ADC_INPUT_SEL::read();
  unsigned long timeout = millis();
  while (millis() - timeout < 450) {
    delay(10);
    handleWiFi(0);
    
    boolean stable = getStatus16SpHsStable();
    if (stable) 
    {
      currentInput = GBS::ADC_INPUT_SEL::read();
      SerialM.print(F("Activity detected, input: "));
      if(currentInput == 1) SerialM.println("RGB");
      else SerialM.println("Component");

      if (currentInput == 1) { // RGBS or RGBHV
        boolean vsyncActive = 0;
        rto->inputIsYpBpR = false;  // declare for MD
        GBS::MD_SEL_VGA60::write(1);  // VGA 640x480 more likely than EDTV
        rto->currentLevelSOG = 13;   // test startup with MD and MS separately!
        setAndUpdateSogLevel(rto->currentLevelSOG);

        unsigned long timeOutStart = millis();
        // vsync test
        // 360ms good up to 5_34 SP_V_TIMER_VAL = 0x0b
        while (!vsyncActive && ((millis() - timeOutStart) < 360)) { 
          vsyncActive = GBS::STATUS_SYNC_PROC_VSACT::read();
          handleWiFi(0); // wifi stack
          delay(1);
        }

        // if VSync is active, it's RGBHV or RGBHV with CSync on HS pin
        if (vsyncActive) {
          SerialM.println("VSync: present");
          boolean hsyncActive = 0;

          timeOutStart = millis();
          while (!hsyncActive && millis() - timeOutStart < 400) {
            hsyncActive = GBS::STATUS_SYNC_PROC_HSACT::read();
            handleWiFi(0); // wifi stack
            delay(1);
          }

          if (hsyncActive) {
            SerialM.println("HSync: present");
            // The HSync and SOG pins are setup to detect CSync, if present 
            // (SOG mode on, coasting setup, debug bus setup, etc)
            // SP_H_PROTECT is needed for CSync with a VS source present as well
            GBS::SP_H_PROTECT::write(1);
            delay(120);

            short decodeSuccess = 0;
            for (int i = 0; i < 2; i++)
            {
              // no success if: no signal at all (returns 0.0f), no embedded VSync (returns ~18.5f)
              if (getSourceFieldRate(1) > 40.0f) decodeSuccess++; // properly decoded vsync from 40 to xx Hz
            }

            if (decodeSuccess >= 2) { rto->syncTypeCsync = true; }
            else { rto->syncTypeCsync = false; }

            // check for 25khz, all regular SOG modes first
            // if source is HS+VS, can't detect via MD unit, need to set 5_11=0x92 and look at vt: counter
            for (uint8_t i = 0; i < 8; i++) {
              //printInfo();
              uint8_t innerVideoMode = getVideoMode();
              if (innerVideoMode > 0 && innerVideoMode != 8) {
                return 1;
              }
              if (innerVideoMode == 8) {
                setAndUpdateSogLevel(rto->currentLevelSOG);
                rto->medResLineCount = GBS::MD_HD1250P_CNTRL::read();
                SerialM.println("25khz mixed rgbs");

                return 1;
              }
              // update 25khz detection 
              GBS::MD_HD1250P_CNTRL::write(GBS::MD_HD1250P_CNTRL::read() + 1);
              //Serial.println(GBS::MD_HD1250P_CNTRL::read(), HEX);
              delay(10);
            }

            rto->videoStandardInput = 15;
            // exception: apply preset here, not later in syncwatcher
            applyPresets(rto->videoStandardInput);
            delay(100);

            return 3;
          }
          else {
            // need to continue looking
            SerialM.println("but no HSync!");
          }
        }

        if (!vsyncActive) { // then do RGBS check
          uint16_t testCycle = 0;
          timeOutStart = millis();
          while ((millis() - timeOutStart) < 6000) 
          {
            delay(2);
            if (getVideoMode() > 0) {
              if (getVideoMode() != 8) {  // if it's mode 8, need to set stuff first
                return 1;
              }
            }
            testCycle++;
            // post coast 18 can mislead occasionally (SNES 239 mode)
            // but even then it still detects the video mode pretty well
            if ((testCycle % 150) == 0) {
              if (rto->currentLevelSOG == 1) {
                rto->currentLevelSOG = 2;
              }
              else {
                rto->currentLevelSOG += 2;
              }
              if (rto->currentLevelSOG >= 15) { rto->currentLevelSOG = 1; }
              setAndUpdateSogLevel(rto->currentLevelSOG);
            }

            // new: check for 25khz, use regular scaling route for those
            if (getVideoMode() == 8) {
              rto->currentLevelSOG = rto->thisSourceMaxLevelSOG = 13;
              setAndUpdateSogLevel(rto->currentLevelSOG);
              rto->medResLineCount = GBS::MD_HD1250P_CNTRL::read();
              SerialM.println("25khz pure rgbs");
              return 1;
            }

            uint8_t currentMedResLineCount = GBS::MD_HD1250P_CNTRL::read();
            if (currentMedResLineCount < 0x3c) {
              GBS::MD_HD1250P_CNTRL::write(currentMedResLineCount + 1);
            }
            else {
              GBS::MD_HD1250P_CNTRL::write(0x33);
            }
            //Serial.println(GBS::MD_HD1250P_CNTRL::read(), HEX);

          }

          rto->currentLevelSOG = rto->thisSourceMaxLevelSOG = 13;
          setAndUpdateSogLevel(rto->currentLevelSOG);

          return 1; //anyway, let later stage deal with it
        }

        GBS::SP_SOG_MODE::write(1);
        resetSyncProcessor();
        resetModeDetect(); // there was some signal but we lost it. MD is stuck anyway, so reset
        delay(40);
      }
      else if (currentInput == 0) { // YUV
        uint16_t testCycle = 0;
        rto->inputIsYpBpR = true;  // declare for MD
        GBS::MD_SEL_VGA60::write(0);  // EDTV more likely than VGA 640x480

        unsigned long timeOutStart = millis();
        while ((millis() - timeOutStart) < 6000)
        {
          delay(2);
          if (getVideoMode() > 0) {
            return 2;
          }

          testCycle++;
          if ((testCycle % 180) == 0) {
            if (rto->currentLevelSOG == 1) {
              rto->currentLevelSOG = 2;
            }
            else {
              rto->currentLevelSOG += 2;
            }
            if (rto->currentLevelSOG >= 16) { rto->currentLevelSOG = 1; }
            setAndUpdateSogLevel(rto->currentLevelSOG);
            rto->thisSourceMaxLevelSOG = rto->currentLevelSOG;
          }
        }

        rto->currentLevelSOG = rto->thisSourceMaxLevelSOG = 14;
        setAndUpdateSogLevel(rto->currentLevelSOG);
        
        return 2; //anyway, let later stage deal with it
      }

      SerialM.println(" lost..");
      rto->currentLevelSOG = 2;
      setAndUpdateSogLevel(rto->currentLevelSOG);
    }
    
    GBS::ADC_INPUT_SEL::write(!currentInput); // can only be 1 or 0
    delay(200);

    return 0; // don't do the check on the new input here, wait till next run
  }

  return 0;
}

uint8_t inputAndSyncDetect() {
  uint8_t debug_backup = GBS::TEST_BUS_SEL::read();
  uint8_t debug_backup_SP = GBS::TEST_BUS_SP_SEL::read();
  if (debug_backup != 0xa) {
    GBS::TEST_BUS_SEL::write(0xa); delay(1);
  }
  if (debug_backup_SP != 0x0f) {
    GBS::TEST_BUS_SP_SEL::write(0x0f); delay(1);
  }

  uint8_t syncFound = detectAndSwitchToActiveInput();

  if (debug_backup != 0xa) {
    GBS::TEST_BUS_SEL::write(debug_backup);
  }
  if (debug_backup_SP != 0x0f) {
    GBS::TEST_BUS_SP_SEL::write(debug_backup_SP);
  }

  if (syncFound == 0) {
    if (!getSyncPresent()) {
      if (rto->isInLowPowerMode == false) {
        rto->sourceDisconnected = true;
        rto->videoStandardInput = 0;
        // reset to base settings, then go to low power
        GBS::SP_SOG_MODE::write(1);
        goLowPowerWithInputDetection();
        rto->isInLowPowerMode = true;
      }
    }
    return 0;
  }
  else if (syncFound == 1) { // input is RGBS
    rto->inputIsYpBpR = 0;
    rto->sourceDisconnected = false;
    rto->isInLowPowerMode = false;
    resetDebugPort();
    applyRGBPatches();
    LEDON;
    return 1;
  }
  else if (syncFound == 2) {
    rto->inputIsYpBpR = 1;
    rto->sourceDisconnected = false;
    rto->isInLowPowerMode = false;
    resetDebugPort();
    applyYuvPatches();
    LEDON;
    return 2;
  }
  else if (syncFound == 3) { // input is RGBHV
    //already applied
    rto->isInLowPowerMode = false;
    rto->inputIsYpBpR = 0;
    rto->sourceDisconnected = false;
    rto->videoStandardInput = 15;
    resetDebugPort();
    LEDON;
    return 3;
  }

  return 0;
}

uint8_t getSingleByteFromPreset(const uint8_t* programArray, unsigned int offset) {
  return pgm_read_byte(programArray + offset);
}

static inline void readFromRegister(uint8_t reg, int bytesToRead, uint8_t* output)
{
  return GBS::read(lastSegment, reg, output, bytesToRead);
}

void printReg(uint8_t seg, uint8_t reg) {
  uint8_t readout;
  readFromRegister(reg, 1, &readout);
  // didn't think this HEX trick would work, but it does! (?)
  SerialM.print("0x"); SerialM.print(readout, HEX); SerialM.print(", // s"); SerialM.print(seg); SerialM.print("_"); SerialM.println(reg, HEX);
  // old:
  //SerialM.print(readout); SerialM.print(", // s"); SerialM.print(seg); SerialM.print("_"); SerialM.println(reg, HEX);
}

// dumps the current chip configuration in a format that's ready to use as new preset :)
void dumpRegisters(byte segment)
{
  if (segment > 5) return;
  writeOneByte(0xF0, segment);

  switch (segment) {
  case 0:
    for (int x = 0x40; x <= 0x5F; x++) {
      printReg(0, x);
    }
    for (int x = 0x90; x <= 0x9F; x++) {
      printReg(0, x);
    }
    break;
  case 1:
    for (int x = 0x0; x <= 0x2F; x++) {
      printReg(1, x);
    }
    break;
  case 2:
    // not needed anymore, code kept for debug
    /*for (int x = 0x0; x <= 0x3F; x++) {
      printReg(2, x);
    }*/
    break;
  case 3:
    for (int x = 0x0; x <= 0x7F; x++) {
      printReg(3, x);
    }
    break;
  case 4:
    for (int x = 0x0; x <= 0x5F; x++) {
      printReg(4, x);
    }
    break;
  case 5:
    for (int x = 0x0; x <= 0x6F; x++) {
      printReg(5, x);
    }
    break;
  }
}

void resetPLLAD() {
  GBS::PLLAD_VCORST::write(1);
  GBS::PLLAD_PDZ::write(1); // in case it was off
  latchPLLAD();
  GBS::PLLAD_VCORST::write(0);
  delay(1);
  latchPLLAD();
  rto->clampPositionIsSet = 0; // test, but should be good
  rto->continousStableCounter = 1;
}

void latchPLLAD() {
  GBS::PLLAD_LAT::write(0);
  delayMicroseconds(128);
  GBS::PLLAD_LAT::write(1);
}

void resetPLL() {
  GBS::PLL_VCORST::write(1);
  delay(1);
  GBS::PLL_VCORST::write(0);
  delay(1);
  rto->clampPositionIsSet = 0; // test, but should be good
  rto->continousStableCounter = 1;
}

void ResetSDRAM() {
  //GBS::SDRAM_RESET_CONTROL::write(0x87); // enable "Software Control SDRAM Idle Period" and "SDRAM_START_INITIAL_CYCLE"
  //GBS::SDRAM_RESET_SIGNAL::write(1);
  //GBS::SDRAM_RESET_SIGNAL::write(0);
  //GBS::SDRAM_START_INITIAL_CYCLE::write(0);
  GBS::SDRAM_RESET_CONTROL::write(0x02);
  GBS::SDRAM_RESET_SIGNAL::write(1);
  GBS::SDRAM_RESET_SIGNAL::write(0);
  GBS::SDRAM_RESET_CONTROL::write(0x82);
}

// soft reset cycle
// This restarts all chip units, which is sometimes required when important config bits are changed.
void resetDigital() {
  boolean keepBypassActive = 0;
  if (GBS::SFTRST_HDBYPS_RSTZ::read() == 1) { // if HDBypass enabled
    keepBypassActive = 1;
  }

  //GBS::RESET_CONTROL_0x47::write(0x00);
  GBS::RESET_CONTROL_0x47::write(0x17); // new, keep 0,1,2,4 on (DEC,MODE,SYNC,INT) //MODE okay?

  if (rto->outModeHdBypass) { // if currently in bypass
    GBS::RESET_CONTROL_0x46::write(0x00);
    GBS::RESET_CONTROL_0x47::write(0x1F);
    return;  // 0x46 stays all 0
  }

  GBS::RESET_CONTROL_0x46::write(0x41); // keep VDS (6) + IF (0) enabled, reset rest
  if (keepBypassActive == 1) { // if HDBypass enabled
    GBS::RESET_CONTROL_0x47::write(0x1F);
  }
  //else {
  //  GBS::RESET_CONTROL_0x47::write(0x17);
  //}
  GBS::RESET_CONTROL_0x46::write(0x7f);
}

void resetSyncProcessor() {
  GBS::SFTRST_SYNC_RSTZ::write(0);
  delayMicroseconds(10);
  GBS::SFTRST_SYNC_RSTZ::write(1);
  //rto->clampPositionIsSet = false;  // resetSyncProcessor is part of autosog
  //rto->coastPositionIsSet = false;
}

void resetModeDetect() {
  GBS::SFTRST_MODE_RSTZ::write(0);
  delay(1); // needed
  GBS::SFTRST_MODE_RSTZ::write(1);
  //rto->clampPositionIsSet = false;
  //rto->coastPositionIsSet = false;
}

void shiftHorizontal(uint16_t amountToShift, bool subtracting) {
  uint16_t hrst = GBS::VDS_HSYNC_RST::read();
  uint16_t hbst = GBS::VDS_HB_ST::read();
  uint16_t hbsp = GBS::VDS_HB_SP::read();

  // Perform the addition/subtraction
  if (subtracting) {
    if ((int16_t)hbst - amountToShift >= 0) {
      hbst -= amountToShift;
    }
    else {
      hbst = hrst - (amountToShift - hbst);
    }
    if ((int16_t)hbsp - amountToShift >= 0) {
      hbsp -= amountToShift;
    }
    else {
      hbsp = hrst - (amountToShift - hbsp);
    }
  }
  else {
    if ((int16_t)hbst + amountToShift <= hrst) {
      hbst += amountToShift;
      // also extend hbst_d to maximum hrst-1
      if (hbst > GBS::VDS_DIS_HB_ST::read()) {
        GBS::VDS_DIS_HB_ST::write(hbst);
      }
    }
    else {
      hbst = 0 + (amountToShift - (hrst - hbst));
    }
    if ((int16_t)hbsp + amountToShift <= hrst) {
      hbsp += amountToShift;
    }
    else {
      hbsp = 0 + (amountToShift - (hrst - hbsp));
    }
  }

  GBS::VDS_HB_ST::write(hbst);
  GBS::VDS_HB_SP::write(hbsp);
  //Serial.print("hbst: "); Serial.println(hbst);
  //Serial.print("hbsp: "); Serial.println(hbsp);
}

void shiftHorizontalLeft() {
  shiftHorizontal(4, true);
}

void shiftHorizontalRight() {
  shiftHorizontal(4, false);
}

// unused but may become useful
void shiftHorizontalLeftIF(uint8_t amount) {
  uint16_t IF_HB_ST2 = GBS::IF_HB_ST2::read() + amount;
  uint16_t IF_HB_SP2 = GBS::IF_HB_SP2::read() + amount;
  uint16_t PLLAD_MD = GBS::PLLAD_MD::read();

  if (rto->videoStandardInput <= 2) {
    GBS::IF_HSYNC_RST::write(PLLAD_MD / 2); // input line length from pll div
  }
  else if (rto->videoStandardInput <= 7) {
    GBS::IF_HSYNC_RST::write(PLLAD_MD);
  }
  uint16_t IF_HSYNC_RST = GBS::IF_HSYNC_RST::read();

  GBS::IF_LINE_SP::write(IF_HSYNC_RST + 1);

  // start
  if (IF_HB_ST2 < IF_HSYNC_RST) {
    GBS::IF_HB_ST2::write(IF_HB_ST2);
  }
  else {
    GBS::IF_HB_ST2::write(IF_HB_ST2 - IF_HSYNC_RST);
  }
  //SerialM.print("IF_HB_ST2:  "); SerialM.println(GBS::IF_HB_ST2::read());

  // stop
  if (IF_HB_SP2 < IF_HSYNC_RST) {
    GBS::IF_HB_SP2::write(IF_HB_SP2);
  }
  else {
    GBS::IF_HB_SP2::write((IF_HB_SP2 - IF_HSYNC_RST) + 1);
  }
  //SerialM.print("IF_HB_SP2:  "); SerialM.println(GBS::IF_HB_SP2::read());
}

// unused but may become useful
void shiftHorizontalRightIF(uint8_t amount) {
  int16_t IF_HB_ST2 = GBS::IF_HB_ST2::read() - amount;
  int16_t IF_HB_SP2 = GBS::IF_HB_SP2::read() - amount;
  uint16_t PLLAD_MD = GBS::PLLAD_MD::read();

  if (rto->videoStandardInput <= 2) {
    GBS::IF_HSYNC_RST::write(PLLAD_MD / 2); // input line length from pll div
  }
  else if (rto->videoStandardInput <= 7) {
    GBS::IF_HSYNC_RST::write(PLLAD_MD);
  }
  int16_t IF_HSYNC_RST = GBS::IF_HSYNC_RST::read();

  GBS::IF_LINE_SP::write(IF_HSYNC_RST + 1);

  if (IF_HB_ST2 > 0) {
    GBS::IF_HB_ST2::write(IF_HB_ST2);
  }
  else {
    GBS::IF_HB_ST2::write(IF_HSYNC_RST - 1);
  }
  //SerialM.print("IF_HB_ST2:  "); SerialM.println(GBS::IF_HB_ST2::read());

  if (IF_HB_SP2 > 0) {
    GBS::IF_HB_SP2::write(IF_HB_SP2);
  }
  else {
    GBS::IF_HB_SP2::write(IF_HSYNC_RST - 1);
    //GBS::IF_LINE_SP::write(GBS::IF_LINE_SP::read() - 2);
  }
  //SerialM.print("IF_HB_SP2:  "); SerialM.println(GBS::IF_HB_SP2::read());
}

void scaleHorizontal(uint16_t amountToScale, bool subtracting)
{
    uint16_t hscale = GBS::VDS_HSCALE::read();

    // smooth out areas of interest
    if (subtracting  && (hscale == 513 || hscale == 512)) amountToScale = 1;
    if (!subtracting && (hscale == 511 || hscale == 512)) amountToScale = 1;

    if (subtracting && (((int)hscale - amountToScale) <= 256)) {
        hscale = 256;
        GBS::VDS_HSCALE::write(hscale);
        SerialM.println("limit");
        return;
    }

    if (subtracting && (hscale - amountToScale > 255)) {
        hscale -= amountToScale;
    } else if (hscale + amountToScale < 1023) {
        hscale += amountToScale;
    } else if (hscale + amountToScale == 1023) { // exact max > bypass but apply VDS fetch changes
        hscale = 1023;
        GBS::VDS_HSCALE::write(hscale);
        GBS::VDS_HSCALE_BYPS::write(1);
    } else if (hscale + amountToScale > 1023) { // max + overshoot > bypass and no VDS fetch adjust
        hscale = 1023;
        GBS::VDS_HSCALE::write(hscale);
        GBS::VDS_HSCALE_BYPS::write(1);
        SerialM.println("limit");
        return;
    }

    // will be scaling
    GBS::VDS_HSCALE_BYPS::write(0);

    // move within VDS VB fetch area (within reason)
    uint16_t htotal = GBS::VDS_HSYNC_RST::read();
    uint16_t toShift = 0;
    if (hscale < 540) toShift = 4;
    else if (hscale < 640) toShift = 3;
    else toShift = 2;

    if (subtracting) {
      shiftHorizontal(toShift, true);
      if ((GBS::VDS_HB_ST::read() + 5) < GBS::VDS_DIS_HB_ST::read()) {
        GBS::VDS_HB_ST::write(GBS::VDS_HB_ST::read() + 5);
      }
      else if ((GBS::VDS_DIS_HB_ST::read() + 5) < htotal) {
        GBS::VDS_DIS_HB_ST::write(GBS::VDS_DIS_HB_ST::read() + 5);
        GBS::VDS_HB_ST::write(GBS::VDS_DIS_HB_ST::read()); // dis_hbst = hbst
      }

      // fix HB_ST > HB_SP conditions
      if (GBS::VDS_HB_SP::read() < (GBS::VDS_HB_ST::read() + 16)) { // HB_SP < HB_ST
        if ((GBS::VDS_HB_SP::read()) > (htotal / 2)) {              // but HB_SP > some small value
          GBS::VDS_HB_ST::write(GBS::VDS_HB_SP::read() - 16);
        }
      }
    }

    // !subtracting check just for readability
    if (!subtracting) {
      shiftHorizontal(toShift, false);
      if ((GBS::VDS_HB_ST::read() - 5) > 0) {
        GBS::VDS_HB_ST::write(GBS::VDS_HB_ST::read() - 5);
      }
    }

    // fix scaling < 512 glitch: factor even, htotal even: hbst / hbsp should be even, etc
    if (hscale < 512) {
        if (hscale % 2 == 0) { // hscale 512 / even
            if (GBS::VDS_HB_ST::read() % 2 == 1) {
                GBS::VDS_HB_ST::write(GBS::VDS_HB_ST::read() + 1);
            }
            if (htotal % 2 == 1) {
                if (GBS::VDS_HB_SP::read() % 2 == 0) {
                    GBS::VDS_HB_SP::write(GBS::VDS_HB_SP::read() - 1);
                }
            } else {
                if (GBS::VDS_HB_SP::read() % 2 == 1) {
                    GBS::VDS_HB_SP::write(GBS::VDS_HB_SP::read() - 1);
                }
            }
        } else { // hscale 499 / uneven
            if (GBS::VDS_HB_ST::read() % 2 == 1) {
                GBS::VDS_HB_ST::write(GBS::VDS_HB_ST::read() + 1);
            }
            if (htotal % 2 == 0) {
                if (GBS::VDS_HB_SP::read() % 2 == 1) {
                    GBS::VDS_HB_SP::write(GBS::VDS_HB_SP::read() - 1);
                }
            } else {
                if (GBS::VDS_HB_SP::read() % 2 == 0) {
                    GBS::VDS_HB_SP::write(GBS::VDS_HB_SP::read() - 1);
                }
            }
        }
        // if scaling was < 512 and HB_ST moved, align with VDS_DIS_HB_ST
        if (GBS::VDS_DIS_HB_ST::read() < GBS::VDS_HB_ST::read()) {
          GBS::VDS_DIS_HB_ST::write(GBS::VDS_HB_ST::read());
        }
    }

    //SerialM.print("HB_ST: "); SerialM.println(GBS::VDS_HB_ST::read());
    //SerialM.print("HB_SP: "); SerialM.println(GBS::VDS_HB_SP::read());
    SerialM.print("HScale: "); SerialM.println(hscale);
    GBS::VDS_HSCALE::write(hscale);
}

void moveHS(uint16_t amountToAdd, bool subtracting) {
  uint16_t VDS_HS_ST = GBS::VDS_HS_ST::read();
  uint16_t VDS_HS_SP = GBS::VDS_HS_SP::read();
  uint16_t VDS_DIS_HB_SP = GBS::VDS_DIS_HB_SP::read();
  uint16_t htotal = GBS::VDS_HSYNC_RST::read();
  
  if (htotal == 0) return; // safety
  int16_t amount = subtracting ? (0 - amountToAdd) : amountToAdd;

  if ((VDS_HS_ST + amount) >= 0 && (VDS_HS_SP + amount) >= 0)
  {
    if (amount > 0) { // is HS_SP going to be to the left of display blank?
      if ((VDS_HS_SP + amount) < (VDS_DIS_HB_SP - 8)) {
        GBS::VDS_HS_ST::write((VDS_HS_ST + amount) % htotal);
        GBS::VDS_HS_SP::write((VDS_HS_SP + amount) % htotal);
      }
      else {
        SerialM.println("limit");
      }
    }
    else { // amount <= 0
      if ((VDS_HS_ST + amount) > 4) { // is HS_ST going to be to the right of 0?
        GBS::VDS_HS_ST::write((VDS_HS_ST + amount) % htotal);
        GBS::VDS_HS_SP::write((VDS_HS_SP + amount) % htotal);
      }
      else {
        SerialM.println("limit");
      }
    }
  }
  else if ((VDS_HS_ST + amount) < 0)
  {
    GBS::VDS_HS_ST::write((htotal + amount) % htotal);
    GBS::VDS_HS_SP::write((VDS_HS_SP + amount) % htotal);
  }
  else if ((VDS_HS_SP + amount) < 0)
  {
    GBS::VDS_HS_ST::write((VDS_HS_ST + amount) % htotal);
    GBS::VDS_HS_SP::write((htotal + amount) % htotal);
  }

  SerialM.print("HSST: "); SerialM.print(GBS::VDS_HS_ST::read());
  SerialM.print(" HSSP: "); SerialM.println(GBS::VDS_HS_SP::read());
}

void moveVS(uint16_t amountToAdd, bool subtracting) {
  uint16_t vtotal = GBS::VDS_VSYNC_RST::read();
  if (vtotal == 0) return; // safety
  uint16_t VDS_DIS_VB_ST = GBS::VDS_DIS_VB_ST::read();
  uint16_t newVDS_VS_ST = GBS::VDS_VS_ST::read();
  uint16_t newVDS_VS_SP = GBS::VDS_VS_SP::read();

  if (subtracting) {
    if ((newVDS_VS_ST - amountToAdd) > VDS_DIS_VB_ST) {
      newVDS_VS_ST -= amountToAdd;
      newVDS_VS_SP -= amountToAdd;
    }
    else SerialM.println("limit");
  }
  else {
    if ((newVDS_VS_SP + amountToAdd) < vtotal) {
      newVDS_VS_ST += amountToAdd;
      newVDS_VS_SP += amountToAdd;
    }
    else SerialM.println("limit");
  }
  //SerialM.print("VSST: "); SerialM.print(newVDS_VS_ST);
  //SerialM.print(" VSSP: "); SerialM.println(newVDS_VS_SP);

  GBS::VDS_VS_ST::write(newVDS_VS_ST);
  GBS::VDS_VS_SP::write(newVDS_VS_SP);
}

void invertHS() {
  uint8_t high, low;
  uint16_t newST, newSP;

  writeOneByte(0xf0, 3);
  readFromRegister(0x0a, 1, &low);
  readFromRegister(0x0b, 1, &high);
  newST = ((((uint16_t)high) & 0x000f) << 8) | (uint16_t)low;
  readFromRegister(0x0b, 1, &low);
  readFromRegister(0x0c, 1, &high);
  newSP = ((((uint16_t)high) & 0x00ff) << 4) | ((((uint16_t)low) & 0x00f0) >> 4);

  uint16_t temp = newST;
  newST = newSP;
  newSP = temp;

  writeOneByte(0x0a, (uint8_t)(newST & 0x00ff));
  writeOneByte(0x0b, ((uint8_t)(newSP & 0x000f) << 4) | ((uint8_t)((newST & 0x0f00) >> 8)));
  writeOneByte(0x0c, (uint8_t)((newSP & 0x0ff0) >> 4));
}

void invertVS() {
  uint8_t high, low;
  uint16_t newST, newSP;

  writeOneByte(0xf0, 3);
  readFromRegister(0x0d, 1, &low);
  readFromRegister(0x0e, 1, &high);
  newST = ((((uint16_t)high) & 0x000f) << 8) | (uint16_t)low;
  readFromRegister(0x0e, 1, &low);
  readFromRegister(0x0f, 1, &high);
  newSP = ((((uint16_t)high) & 0x00ff) << 4) | ((((uint16_t)low) & 0x00f0) >> 4);

  uint16_t temp = newST;
  newST = newSP;
  newSP = temp;

  writeOneByte(0x0d, (uint8_t)(newST & 0x00ff));
  writeOneByte(0x0e, ((uint8_t)(newSP & 0x000f) << 4) | ((uint8_t)((newST & 0x0f00) >> 8)));
  writeOneByte(0x0f, (uint8_t)((newSP & 0x0ff0) >> 4));
}

void scaleVertical(uint16_t amountToScale, bool subtracting) {
  uint16_t vscale = GBS::VDS_VSCALE::read();

  // least invasive "is vscaling enabled" check
  if (vscale == 1023) {
    GBS::VDS_VSCALE_BYPS::write(0);
  }

  // smooth out areas of interest
  if (subtracting && (vscale == 513 || vscale == 512)) amountToScale = 1;
  if (subtracting && (vscale == 684 || vscale == 683)) amountToScale = 1;
  if (!subtracting && (vscale == 511 || vscale == 512)) amountToScale = 1;
  if (!subtracting && (vscale == 682 || vscale == 683)) amountToScale = 1;

  if (subtracting && (vscale - amountToScale > 128)) {
    vscale -= amountToScale;
  }
  else if (subtracting) {
    vscale = 128;
  }
  else if (vscale + amountToScale <= 1023) {
    vscale += amountToScale;
  }
  else if (vscale + amountToScale > 1023) {
    vscale = 1023;
    // don't enable vscale bypass here, since that disables ie line filter
  }

  SerialM.print("VScale: "); SerialM.println(vscale);
  GBS::VDS_VSCALE::write(vscale);
}

void shiftVertical(uint16_t amountToAdd, bool subtracting) {
  typedef GBS::Tie<GBS::VDS_VB_ST, GBS::VDS_VB_SP> Regs;
  uint16_t vrst = GBS::VDS_VSYNC_RST::read() - FrameSync::getSyncLastCorrection();
  uint16_t vbst = 0, vbsp = 0;
  int16_t newVbst = 0, newVbsp = 0;

  Regs::read(vbst, vbsp);
  newVbst = vbst; newVbsp = vbsp;

  if (subtracting) {
    newVbst -= amountToAdd;
    newVbsp -= amountToAdd;
  }
  else {
    newVbst += amountToAdd;
    newVbsp += amountToAdd;
  }

  // handle the case where hbst or hbsp have been decremented below 0
  if (newVbst < 0) {
    newVbst = vrst + newVbst;
  }
  if (newVbsp < 0) {
    newVbsp = vrst + newVbsp;
  }

  // handle the case where vbst or vbsp have been incremented above vrstValue
  if (newVbst > (int16_t)vrst) {
    newVbst = newVbst - vrst;
  }
  if (newVbsp > (int16_t)vrst) {
    newVbsp = newVbsp - vrst;
  }

  Regs::write(newVbst, newVbsp);
  //SerialM.print("VSST: "); SerialM.print(newVbst); SerialM.print(" VSSP: "); SerialM.println(newVbsp);
}

void shiftVerticalUp() {
  shiftVertical(1, true);
}

void shiftVerticalDown() {
  shiftVertical(1, false);
}

void shiftVerticalUpIF() {
  // -4 to allow variance in source line count
  uint8_t offset = rto->videoStandardInput == 2 ? 4 : 1;
  uint16_t sourceLines = GBS::VPERIOD_IF::read() - offset;
  // add an override for sourceLines, in case where the IF data is not available
  if ((GBS::GBS_OPTION_SCALING_RGBHV::read() == 1) && rto->videoStandardInput == 14) {
    sourceLines = GBS::STATUS_SYNC_PROC_VTOTAL::read();
  }
  int16_t stop = GBS::IF_VB_SP::read();
  int16_t start = GBS::IF_VB_ST::read();

  if (stop < (sourceLines-1) && start < (sourceLines-1)) { stop += 2; start += 2; }
  else {
    start = 0; stop = 2;
  }
  GBS::IF_VB_SP::write(stop);
  GBS::IF_VB_ST::write(start);
}

void shiftVerticalDownIF() {
  uint8_t offset = rto->videoStandardInput == 2 ? 4 : 1;
  uint16_t sourceLines = GBS::VPERIOD_IF::read() - offset;
  // add an override for sourceLines, in case where the IF data is not available
  if ((GBS::GBS_OPTION_SCALING_RGBHV::read() == 1) && rto->videoStandardInput == 14) {
    sourceLines = GBS::STATUS_SYNC_PROC_VTOTAL::read();
  }

  int16_t stop = GBS::IF_VB_SP::read();
  int16_t start = GBS::IF_VB_ST::read();

  if (stop > 1 && start > 1) { stop -= 2; start -= 2; }
  else {
    start = sourceLines - 2; stop = sourceLines;
  }
  GBS::IF_VB_SP::write(stop);
  GBS::IF_VB_ST::write(start);
}

void setHSyncStartPosition(uint16_t value) {
  GBS::VDS_HS_ST::write(value);
}

void setHSyncStopPosition(uint16_t value) {
  GBS::VDS_HS_SP::write(value);
}

void setMemoryHblankStartPosition(uint16_t value) {
  GBS::VDS_HB_ST::write(value);
}

void setMemoryHblankStopPosition(uint16_t value) {
  GBS::VDS_HB_SP::write(value);
}

void setDisplayHblankStartPosition(uint16_t value) {
  GBS::VDS_DIS_HB_ST::write(value);
}

void setDisplayHblankStopPosition(uint16_t value) {
  GBS::VDS_DIS_HB_SP::write(value);
}

void setVSyncStartPosition(uint16_t value) {
  GBS::VDS_VS_ST::write(value);
}

void setVSyncStopPosition(uint16_t value) {
  GBS::VDS_VS_SP::write(value);
}

void setMemoryVblankStartPosition(uint16_t value) {
  GBS::VDS_VB_ST::write(value);
}

void setMemoryVblankStopPosition(uint16_t value) {
  GBS::VDS_VB_SP::write(value);
}

void setDisplayVblankStartPosition(uint16_t value) {
  GBS::VDS_DIS_VB_ST::write(value);
}

void setDisplayVblankStopPosition(uint16_t value) {
  GBS::VDS_DIS_VB_SP::write(value);
}

void getVideoTimings() {
  SerialM.println("");
  SerialM.print("HT / scale   : "); SerialM.print(GBS::VDS_HSYNC_RST::read());
  SerialM.print(" "); SerialM.println(GBS::VDS_HSCALE::read());
  SerialM.print("HS ST/SP     : "); SerialM.print(GBS::VDS_HS_ST::read());
  SerialM.print(" "); SerialM.println(GBS::VDS_HS_SP::read());
  SerialM.print("HB ST/SP(d)  : "); SerialM.print(GBS::VDS_DIS_HB_ST::read());
  SerialM.print(" "); SerialM.println(GBS::VDS_DIS_HB_SP::read());
  SerialM.print("HB ST/SP     : "); SerialM.print(GBS::VDS_HB_ST::read());
  SerialM.print(" "); SerialM.println(GBS::VDS_HB_SP::read());
  SerialM.println("------");
  // vertical 
  SerialM.print("VT / scale   : "); SerialM.print(GBS::VDS_VSYNC_RST::read());
  SerialM.print(" "); SerialM.println(GBS::VDS_VSCALE::read());
  SerialM.print("VS ST/SP     : "); SerialM.print(GBS::VDS_VS_ST::read());
  SerialM.print(" "); SerialM.println(GBS::VDS_VS_SP::read());
  SerialM.print("VB ST/SP(d)  : "); SerialM.print(GBS::VDS_DIS_VB_ST::read());
  SerialM.print(" "); SerialM.println(GBS::VDS_DIS_VB_SP::read());
  SerialM.print("VB ST/SP     : "); SerialM.print(GBS::VDS_VB_ST::read());
  SerialM.print(" "); SerialM.println(GBS::VDS_VB_SP::read());
  // IF V offset
  SerialM.print("IF VB ST/SP  : "); SerialM.print(GBS::IF_VB_ST::read());
  SerialM.print(" "); SerialM.println(GBS::IF_VB_SP::read());
}

void set_htotal(uint16_t htotal) {
  // ModeLine "1280x960" 108.00 1280 1376 1488 1800 960 961 964 1000 +HSync +VSync
  // front porch: H2 - H1: 1376 - 1280
  // back porch : H4 - H3: 1800 - 1488
  // sync pulse : H3 - H2: 1488 - 1376

  uint16_t h_blank_display_start_position = htotal - 1;
  uint16_t h_blank_display_stop_position = htotal - ((htotal * 3) / 4);
  uint16_t center_blank = ((h_blank_display_stop_position / 2) * 3) / 4; // a bit to the left
  uint16_t h_sync_start_position = center_blank - (center_blank / 2);
  uint16_t h_sync_stop_position = center_blank + (center_blank / 2);
  uint16_t h_blank_memory_start_position = h_blank_display_start_position - 1;
  uint16_t h_blank_memory_stop_position = h_blank_display_stop_position - (h_blank_display_stop_position / 50);

  GBS::VDS_HSYNC_RST::write(htotal);
  GBS::VDS_HS_ST::write(h_sync_start_position);
  GBS::VDS_HS_SP::write(h_sync_stop_position);
  GBS::VDS_DIS_HB_ST::write(h_blank_display_start_position);
  GBS::VDS_DIS_HB_SP::write(h_blank_display_stop_position);
  GBS::VDS_HB_ST::write(h_blank_memory_start_position);
  GBS::VDS_HB_SP::write(h_blank_memory_stop_position);
}

void set_vtotal(uint16_t vtotal) {
  uint16_t VDS_DIS_VB_ST = vtotal - 2; // just below vtotal
  uint16_t VDS_DIS_VB_SP = (vtotal >> 6) + 8; // positive, above new sync stop position
  uint16_t VDS_VB_ST = ((uint16_t)(vtotal * 0.016f)) & 0xfffe; // small fraction of vtotal
  uint16_t VDS_VB_SP = VDS_VB_ST + 2; // always VB_ST + 2
  uint16_t v_sync_start_position = 1;
  uint16_t v_sync_stop_position = 5;
  // most low line count formats have negative sync!
  // exception: 1024x768 (1344x806 total) has both sync neg. // also 1360x768 (1792x795 total)
  if ((vtotal < 530) || (vtotal >=803 && vtotal <= 809) || (vtotal >=793 && vtotal <= 798)) {
    uint16_t temp = v_sync_start_position;
    v_sync_start_position = v_sync_stop_position;
    v_sync_stop_position = temp;
  }

  GBS::VDS_VSYNC_RST::write(vtotal);
  GBS::VDS_VS_ST::write(v_sync_start_position);
  GBS::VDS_VS_SP::write(v_sync_stop_position);
  GBS::VDS_VB_ST::write(VDS_VB_ST);
  GBS::VDS_VB_SP::write(VDS_VB_SP);
  GBS::VDS_DIS_VB_ST::write(VDS_DIS_VB_ST);
  GBS::VDS_DIS_VB_SP::write(VDS_DIS_VB_SP);
}

void resetDebugPort() {
  GBS::PAD_BOUT_EN::write(1); // output to pad enabled
  GBS::IF_TEST_EN::write(1);
  GBS::IF_TEST_SEL::write(3); // IF vertical period signal
  GBS::TEST_BUS_SEL::write(0xa); // test bus to SP
  GBS::TEST_BUS_EN::write(1);
  GBS::TEST_BUS_SP_SEL::write(0x0f); // SP test signal select (vsync in, after SOG separation)
  GBS::MEM_FF_TOP_FF_SEL::write(1); // g0g13/14 shows FIFO stats (capture, rff, wff, etc)
  // SP test bus enable bit is included in TEST_BUS_SP_SEL
  GBS::VDS_TEST_EN::write(1); // VDS test enable
}

void readEeprom() {
  int addr = 0;
  const uint8_t eepromAddr = 0x50;
  Wire.beginTransmission(eepromAddr);
  //if (addr >= 0x1000) { addr = 0; }
  Wire.write(addr >> 8); // high addr byte, 4 bits +
  Wire.write((uint8_t)addr); // low addr byte, 8 bits = 12 bits (0xfff max)
  Wire.endTransmission();
  Wire.requestFrom(eepromAddr, (uint8_t)128);
  uint8_t readData = 0;
  uint8_t i = 0;
  while (Wire.available())
  {
    Serial.print("addr 0x"); Serial.print(i, HEX);
    Serial.print(": 0x"); readData = Wire.read();
    Serial.println(readData, HEX); 
    //addr++;
    i++;
  }
}

void fastGetBestHtotal() {
  uint32_t inStart, inStop;
  signed long inPeriod = 1;
  double inHz = 1.0;
  GBS::TEST_BUS_SEL::write(0xa);
  if (FrameSync::vsyncInputSample(&inStart, &inStop)) {
    inPeriod = (inStop - inStart) >> 1;
    if (inPeriod > 1) {
      inHz = (double)1000000 / (double)inPeriod;
    }
    SerialM.print("inPeriod: "); SerialM.println(inPeriod);
    SerialM.print("in hz: "); SerialM.println(inHz);
  }
  else {
    SerialM.println("error");
  }

  uint16_t newVtotal = GBS::VDS_VSYNC_RST::read();
  double bestHtotal = 108000000 / ((double)newVtotal * inHz);  // 107840000
  double bestHtotal50 = 108000000 / ((double)newVtotal * 50);  
  double bestHtotal60 = 108000000 / ((double)newVtotal * 60); 
  SerialM.print("newVtotal: "); SerialM.println(newVtotal);
  // display clock probably not exact 108mhz
  SerialM.print("bestHtotal: "); SerialM.println(bestHtotal);
  SerialM.print("bestHtotal50: "); SerialM.println(bestHtotal50);
  SerialM.print("bestHtotal60: "); SerialM.println(bestHtotal60);
  if (bestHtotal > 800 && bestHtotal < 3200) {
    //applyBestHTotal((uint16_t)bestHtotal);
    //FrameSync::resetWithoutRecalculation();
  }
}

boolean runAutoBestHTotal() {
  if (!FrameSync::ready() 
    && rto->autoBestHtotalEnabled == true 
    && rto->videoStandardInput > 0  
    && rto->videoStandardInput < 15)
  {
    
    //Serial.println("running");
    //unsigned long startTime = millis();

    boolean stableNow = 1;

    for (uint8_t i = 0; i < 64; i++) {
      if (!getStatus16SpHsStable()) {
        stableNow = 0;
        //Serial.println("prevented: !getStatus16SpHsStable");
        break;
      }
    }

    if (stableNow)
    {
      if (GBS::STATUS_INT_SOG_BAD::read()) {
        //Serial.println("prevented_2!");
        resetInterruptSogBadBit();
        delay(40);
        stableNow = false;
      }
      resetInterruptSogBadBit();

      if (stableNow) {
        
        uint8_t testBusSelBackup  = GBS::TEST_BUS_SEL::read();
        uint8_t vdsBusSelBackup   = GBS::VDS_TEST_BUS_SEL::read();
        uint8_t ifBusSelBackup    = GBS::IF_TEST_SEL::read();

        if (testBusSelBackup != 0)  GBS::TEST_BUS_SEL::write(0);     // needs decimation + if
        if (vdsBusSelBackup != 0)   GBS::VDS_TEST_BUS_SEL::write(0); // VDS test # 0 = VBlank
        if (ifBusSelBackup != 3)    GBS::IF_TEST_SEL::write(3);      // IF averaged frame time

        yield();
        uint16_t bestHTotal = FrameSync::init();  // critical task
        yield();

        GBS::TEST_BUS_SEL::write(testBusSelBackup); // always restore from backup (TB has changed)
        if (vdsBusSelBackup != 0)   GBS::VDS_TEST_BUS_SEL::write(vdsBusSelBackup);
        if (ifBusSelBackup != 3)    GBS::IF_TEST_SEL::write(ifBusSelBackup);

        if (GBS::STATUS_INT_SOG_BAD::read()) {
          //Serial.println("prevented_5 INT_SOG_BAD!");
          stableNow = false;
        }
        for (uint8_t i = 0; i < 16; i++) {
          if (!getStatus16SpHsStable()) {
            stableNow = 0;
            //Serial.println("prevented_5: !getStatus16SpHsStable");
            break;
          }
        }
        resetInterruptSogBadBit();

        if (bestHTotal > 4095) {
          if (!rto->forceRetime) {
            stableNow = false;
          }
          else {
            // roll with it
            bestHTotal = 4095;
          }
        }

        if (stableNow) {
          for (uint8_t i = 0; i < 24; i++) {
            delay(1);
            if (!getStatus16SpHsStable()) {
              stableNow = false;
              //Serial.println("prevented_3!");
              break;
            }
          }
        }

        if (bestHTotal > 0 && stableNow) {
          boolean success = applyBestHTotal(bestHTotal);
          if (success) {
            rto->syncLockFailIgnore = 16;
            //Serial.print("ok, took: ");
            //Serial.println(millis() - startTime);
            return true; // success
          }
        }
      }
    }

    // if we reach here, it failed
    FrameSync::reset();

    if (rto->syncLockFailIgnore > 0) {
      rto->syncLockFailIgnore--;
      if (rto->syncLockFailIgnore == 0) {
        GBS::DAC_RGBS_PWDNZ::write(1); // xth chance
        if (!uopt->wantOutputComponent)
        {
          GBS::PAD_SYNC_OUT_ENZ::write(0); // enable sync out // xth chance
        }
        rto->autoBestHtotalEnabled = false;
      }
    }
    Serial.print(F("bestHtotal retry ("));
    Serial.print(rto->syncLockFailIgnore);
    Serial.println(")");

    //SerialM.print("FAIL!, took: ");
    //SerialM.println(millis() - startTime);
  }
  else if (FrameSync::ready()) {
    // FS ready but mode is 0 or 15 or autoBestHtotal is off
    return true;
  }

  if (rto->continousStableCounter != 0 && rto->continousStableCounter != 255) {
    rto->continousStableCounter++; // stop repetitions
  }

  return false;
}

boolean applyBestHTotal(uint16_t bestHTotal) {
  if (rto->outModeHdBypass) {
    return true; // false? doesn't matter atm
  }
  
  uint16_t orig_htotal = GBS::VDS_HSYNC_RST::read();
  int diffHTotal = bestHTotal - orig_htotal;
  uint16_t diffHTotalUnsigned = abs(diffHTotal);
  if (diffHTotalUnsigned < 1 && !rto->forceRetime) {
    if (!uopt->enableFrameTimeLock) { // FTL can double throw this when it resets to adjust
      float sfr = getSourceFieldRate(0);
      delay(0); // wifi
      float ofr = getOutputFrameRate();
      if (sfr < 1.0f) {
        delay(1);
        sfr = getSourceFieldRate(0);  // retry
      }
      if (ofr < 1.0f) {
        delay(1);
        sfr = getOutputFrameRate();  // retry
      }
      SerialM.print("HTotal Adjust: "); 
      if (diffHTotal >= 0) {
        SerialM.print(" "); // formatting to align with negative value readouts
      }
      SerialM.print(diffHTotal);
      SerialM.print(", source Hz: ");
      SerialM.print(sfr, 3); // prec. 3
      SerialM.print(", output Hz: ");
      SerialM.println(ofr, 3); // prec. 3
      delay(0);
    }
    return true; // nothing to do
  }
  if (GBS::GBS_OPTION_PALFORCED60_ENABLED::read() == 1) {
    // source is 50Hz, preset has to stay at 60Hz: return
    return true;
  }
  boolean isLargeDiff = (diffHTotalUnsigned > (orig_htotal * 0.06f)) ? true : false; // typical diff: 1802 to 1794 (=8)

  if (isLargeDiff && (getVideoMode() == 8 || rto->videoStandardInput == 14)) {
    // arcade stuff syncs down from 60 to 52 Hz..
    isLargeDiff = (diffHTotalUnsigned > (orig_htotal * 0.16f)) ? true : false;
  }

  if (isLargeDiff) {
    SerialM.println(F("ABHT: large diff"));
  }

  // rto->forceRetime = true means the correction should be forced (command '.')
  if (isLargeDiff && (rto->forceRetime == false)) {
    if (rto->videoStandardInput != 14) {
      rto->failRetryAttempts++;
      if (rto->failRetryAttempts < 8) {
        SerialM.println("retry");
        FrameSync::reset();
        delay(60);
      }
      else {
        SerialM.println("give up");
        rto->autoBestHtotalEnabled = false;
      }
    }
    return false; // large diff, no forced
  }

  // bestHTotal 0? could be an invald manual retime
  if (bestHTotal == 0) {
    Serial.println("bestHTotal 0");
    return false;
  }

  if (rto->forceRetime == false) {
    if (GBS::STATUS_INT_SOG_BAD::read() == 1) {
      //Serial.println("prevented in apply");
      return false;
    }
  }

  rto->failRetryAttempts = 0; // else all okay!, reset to 0
  rto->forceRetime = false;

  // move blanking (display)
  uint16_t h_blank_display_start_position = GBS::VDS_DIS_HB_ST::read();
  uint16_t h_blank_display_stop_position = GBS::VDS_DIS_HB_SP::read();
  uint16_t h_blank_memory_start_position = GBS::VDS_HB_ST::read();
  uint16_t h_blank_memory_stop_position = GBS::VDS_HB_SP::read();
 
  // h_blank_memory_start_position usually is == h_blank_display_start_position
  if (h_blank_memory_start_position == h_blank_display_start_position) {
    h_blank_display_start_position += (diffHTotal / 2);
    h_blank_display_stop_position += (diffHTotal / 2);
    h_blank_memory_start_position = h_blank_display_start_position; // normal case
    h_blank_memory_stop_position += (diffHTotal / 2);
  }
  else {
    h_blank_display_start_position += (diffHTotal / 2);
    h_blank_display_stop_position += (diffHTotal / 2);
    h_blank_memory_start_position += (diffHTotal / 2); // the exception (currently 1280x1024)
    h_blank_memory_stop_position += (diffHTotal / 2);
  }

  if (diffHTotal < 0) {
    h_blank_display_start_position &= 0xfffe;
    h_blank_display_stop_position &= 0xfffe;
    h_blank_memory_start_position &= 0xfffe;
    h_blank_memory_stop_position &= 0xfffe;
  }
  else if (diffHTotal > 0) {
    h_blank_display_start_position += 1; h_blank_display_start_position &= 0xfffe;
    h_blank_display_stop_position += 1; h_blank_display_stop_position &= 0xfffe;
    h_blank_memory_start_position += 1; h_blank_memory_start_position &= 0xfffe;
    h_blank_memory_stop_position += 1; h_blank_memory_stop_position &= 0xfffe;
  }

  // don't move HSync with small diffs
  uint16_t h_sync_start_position = GBS::VDS_HS_ST::read();
  uint16_t h_sync_stop_position = GBS::VDS_HS_SP::read();

  // fix over / underflows
  if (h_blank_display_start_position > (bestHTotal - 8)) {
    h_blank_display_start_position = bestHTotal - 8;
  }
  if (h_blank_display_stop_position > bestHTotal) {
    h_blank_display_stop_position = bestHTotal * 0.178f;
  }
  if ((h_blank_memory_start_position > bestHTotal) || (h_blank_memory_start_position > h_blank_display_start_position)) {
    h_blank_memory_start_position = h_blank_display_start_position;
  }
  if (h_blank_memory_stop_position > bestHTotal) {
    h_blank_memory_stop_position = h_blank_display_stop_position * 0.64f;
  }

  // finally, fix forced timings with large diff
  if (isLargeDiff) {
    h_blank_display_start_position = bestHTotal * 0.996f;
    h_blank_display_stop_position = bestHTotal * 0.08f;
    h_blank_memory_start_position = h_blank_display_start_position;
    h_blank_memory_stop_position = h_blank_display_stop_position * 0.2f;

    if (h_sync_start_position > h_sync_stop_position) { // is neg HSync
      h_sync_stop_position = 0;
      // stop = at least start, then a bit outwards
      h_sync_start_position = 16 + (h_blank_display_stop_position * 0.4f);
    }
    else {
      h_sync_start_position = 0;
      h_sync_stop_position = 16 + (h_blank_display_stop_position * 0.4f);
    }
  }

  if (diffHTotal != 0) { // apply
    //Serial.println("                        !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    GBS::VDS_HSYNC_RST::write(bestHTotal); // instant apply
    GBS::VDS_DIS_HB_ST::write(h_blank_display_start_position);
    GBS::VDS_DIS_HB_SP::write(h_blank_display_stop_position);
    GBS::VDS_HB_ST::write(h_blank_memory_start_position);
    GBS::VDS_HB_SP::write(h_blank_memory_stop_position);
    GBS::VDS_HS_ST::write(h_sync_start_position);
    GBS::VDS_HS_SP::write(h_sync_stop_position);
  }

  delay(2); // wifi
  float sfr = getSourceFieldRate(0);
  delay(0);
  float ofr = getOutputFrameRate();
  if (sfr < 1.0f) {
    delay(1);
    sfr = getSourceFieldRate(0);  // retry
  }
  if (ofr < 1.0f) {
    delay(1);
    sfr = getOutputFrameRate();  // retry
  }
  SerialM.print("HTotal Adjust: ");
  if (diffHTotal >= 0) {
    SerialM.print(" "); // formatting to align with negative value readouts
  }
  SerialM.print(diffHTotal);
  SerialM.print(", source Hz: ");
  SerialM.print(sfr, 3); // prec. 3
  SerialM.print(", output Hz: ");
  SerialM.println(ofr, 3); // prec. 3
  delay(0);

  return true;
}

float getSourceFieldRate(boolean useSPBus) {
  double esp8266_clock_freq = ESP.getCpuFreqMHz() * 1000000;
  uint8_t testBusSelBackup =  GBS::TEST_BUS_SEL::read();
  uint8_t spBusSelBackup =    GBS::TEST_BUS_SP_SEL::read();
  uint8_t ifBusSelBackup =    GBS::IF_TEST_SEL::read();
  uint8_t debugPinBackup =    GBS::PAD_BOUT_EN::read();

  if (debugPinBackup != 1) GBS::PAD_BOUT_EN::write(1); // enable output to pin for test

  if (ifBusSelBackup != 3) GBS::IF_TEST_SEL::write(3);  // IF averaged frame time

  if (useSPBus)
  {
    if (testBusSelBackup != 0xa)  GBS::TEST_BUS_SEL::write(0xa);
    if (spBusSelBackup != 0x0f)   GBS::TEST_BUS_SP_SEL::write(0x0f);
  }
  else
  {
    if (testBusSelBackup != 0) GBS::TEST_BUS_SEL::write(0); // needs decimation + if
  }

  delay(1); // wifi
  uint32_t fieldTimeTicks = FrameSync::getPulseTicks();

  float retVal = 0;
  if (fieldTimeTicks > 0) {
    retVal = esp8266_clock_freq / (double)fieldTimeTicks;
  }
  
  GBS::TEST_BUS_SEL::write(testBusSelBackup);
  GBS::PAD_BOUT_EN::write(debugPinBackup);
  if (spBusSelBackup != 0x0f) GBS::TEST_BUS_SP_SEL::write(spBusSelBackup);
  if (ifBusSelBackup != 3)    GBS::IF_TEST_SEL::write(ifBusSelBackup);

  return retVal;
}

float getOutputFrameRate() {
  double esp8266_clock_freq = ESP.getCpuFreqMHz() * 1000000;
  uint8_t testBusSelBackup = GBS::TEST_BUS_SEL::read();
  uint8_t debugPinBackup = GBS::PAD_BOUT_EN::read();

  if (debugPinBackup != 1) GBS::PAD_BOUT_EN::write(1); // enable output to pin for test

  if (testBusSelBackup != 2) GBS::TEST_BUS_SEL::write(2); // 0x4d = 0x22 VDS test

  delay(1); // wifi
  uint32_t fieldTimeTicks = FrameSync::getPulseTicks();

  float retVal = 0;
  if (fieldTimeTicks > 0) {
    retVal = esp8266_clock_freq / (double)fieldTimeTicks;
  }

  GBS::TEST_BUS_SEL::write(testBusSelBackup);
  GBS::PAD_BOUT_EN::write(debugPinBackup);

  return retVal;
}

// used for RGBHV to determine the ADPLL speed "level" / can jitter with SOG Sync
uint32_t getPllRate() {
  uint32_t esp8266_clock_freq = ESP.getCpuFreqMHz() * 1000000;
  uint8_t testBusSelBackup =  GBS::TEST_BUS_SEL::read();
  uint8_t spBusSelBackup =    GBS::TEST_BUS_SP_SEL::read();
  uint8_t debugPinBackup =    GBS::PAD_BOUT_EN::read();

  if (testBusSelBackup != 0xa) {
    GBS::TEST_BUS_SEL::write(0xa);
  }
  if (rto->syncTypeCsync) {
    if (spBusSelBackup != 0x6b) GBS::TEST_BUS_SP_SEL::write(0x6b);
  }
  else {
    if (spBusSelBackup != 0x09) GBS::TEST_BUS_SP_SEL::write(0x09);
  }
  GBS::PAD_BOUT_EN::write(1); // enable output to pin for test
  delay(1); // BOUT signal and wifi
  uint32_t ticks = FrameSync::getPulseTicks();

  // restore
  GBS::PAD_BOUT_EN::write(debugPinBackup);
  if (testBusSelBackup != 0xa) {
    GBS::TEST_BUS_SEL::write(testBusSelBackup);
  }
  GBS::TEST_BUS_SP_SEL::write(spBusSelBackup);

  uint32_t retVal = 0;
  if (ticks > 0) {
    retVal = esp8266_clock_freq / ticks;
  }
  
  return retVal;
}

void doPostPresetLoadSteps() {
  //unsigned long postLoadTimer = millis();

  //GBS::PAD_SYNC_OUT_ENZ::write(1);  // no sync out
  //GBS::DAC_RGBS_PWDNZ::write(0);    // no DAC
  //GBS::SFTRST_MEM_FF_RSTZ::write(0);  // mem fifos keep in reset

  if (rto->videoStandardInput == 0) 
  {
    uint8_t videoMode = getVideoMode();
    SerialM.print(F("post preset: rto->videoStandardInput 0 > "));
    SerialM.println(videoMode);
    if (videoMode > 0) { rto->videoStandardInput = videoMode; }
  }
  rto->presetID = GBS::GBS_PRESET_ID::read();
  boolean isCustomPreset = GBS::GBS_PRESET_CUSTOM::read();
  
  GBS::ADC_UNUSED_64::write(0); GBS::ADC_UNUSED_65::write(0); // clear temp storage
  GBS::ADC_UNUSED_66::write(0); GBS::ADC_UNUSED_67::write(0); // clear temp storage

  prepareSyncProcessor(); // todo: handle modes 14 and 15 better, now that they support scaling
  updateSpDynamic();      // remember: rto->videoStandardInput for RGB(C/HV) in scaling is 1, 2 or 3 here

  GBS::SP_HCST_AUTO_EN::write(0);
  if (GBS::GBS_OPTION_SCALING_RGBHV::read() == 0) // maybe more conditions, but only scaling rgbhv excluded from coast now
  {
    // for potentially faster detection
    GBS::SP_DIS_SUB_COAST::write(1); // SUB_COAST not yet
    GBS::SP_H_PROTECT::write(1);     // enable H_PROTECT temporarily
  }
  GBS::SP_NO_CLAMP_REG::write(1);  // (keep) clamp disabled, to be enabled when position determined
  GBS::OUT_SYNC_CNTRL::write(1);   // prepare sync out to PAD

  // auto offset adc prep
  GBS::ADC_AUTO_OFST_PRD::write(1);   // by line (0 = by frame)
  GBS::ADC_AUTO_OFST_DELAY::write(2); // sample delay 2 (max 4)
  GBS::ADC_AUTO_OFST_STEP::write(2);  // 0 = abs diff, then 1 to 3 steps
  GBS::ADC_AUTO_OFST_TEST::write(1);
  GBS::ADC_AUTO_OFST_RANGE_REG::write(0xff); // 5_0f U/V ranges = 15 (0 to 15)

  if (rto->inputIsYpBpR == true) {
    applyYuvPatches();
  }
  else {
    applyRGBPatches();
  }

  if (rto->outModeHdBypass) {
    GBS::OUT_SYNC_SEL::write(1); // 0_4f 1=sync from HDBypass, 2=sync from SP
    //GBS::DAC_RGBS_PWDNZ::write(1); // enable DAC
    rto->autoBestHtotalEnabled = false;
  }
  else {
    rto->autoBestHtotalEnabled = true;
  }

  rto->phaseADC = GBS::PA_ADC_S::read();  // we can't know which is right, get from preset
  rto->phaseSP = 8;                       // get phase into global variables early: before latching

  if (rto->inputIsYpBpR) {
    rto->thisSourceMaxLevelSOG = rto->currentLevelSOG = 14;
  }
  else {
    rto->thisSourceMaxLevelSOG = rto->currentLevelSOG = 13;  // similar to yuv, allow variations
  }

  setAndUpdateSogLevel(rto->currentLevelSOG);
  
  if (!isCustomPreset) {
    setAdcParametersGainAndOffset();
  }

  GBS::GPIO_CONTROL_00::write(0x67); // most GPIO pins regular GPIO
  GBS::GPIO_CONTROL_01::write(0x00); // all GPIO outputs disabled
  rto->clampPositionIsSet = 0;
  rto->coastPositionIsSet = 0;
  rto->phaseIsSet = 0;
  rto->continousStableCounter = 0;
  rto->noSyncCounter = 0;
  rto->motionAdaptiveDeinterlaceActive = false;
  rto->scanlinesEnabled = false;
  rto->failRetryAttempts = 0;
  rto->videoIsFrozen = true; // ensures unfreeze
  rto->sourceDisconnected = false; // this must be true if we reached here (no syncwatcher operation)
  rto->boardHasPower = true; //same

  if (!isCustomPreset) {
    GBS::IF_INI_ST::write(0); // 16.08.19: don't calculate, use fixed to 0
    //GBS::IF_INI_ST::write(GBS::IF_HSYNC_RST::read() * 0.68); // see update above

    GBS::IF_HS_INT_LPF_BYPS::write(0); // // 1_02 2
    // 0 allows int/lpf for smoother scrolling with non-ideal scaling, also reduces jailbars and even noise
    // interpolation or lpf available, lpf looks better
    GBS::IF_HS_SEL_LPF::write(1); // 1_02 1
    GBS::IF_HS_PSHIFT_BYPS::write(1); // 1_02 3 nonlinear scale phase shift bypass
    // 1_28 1 1:hbin generated write reset 0:line generated write reset
    GBS::IF_LD_WRST_SEL::write(1); // at 1 fixes output position regardless of 1_24
    //GBS::MADPT_Y_DELAY_UV_DELAY::write(0); // 2_17 default: 0 // don't overwrite

    GBS::SP_RT_HS_ST::write(0); // 5_49 // retiming hs ST, SP
    GBS::SP_RT_HS_SP::write(GBS::PLLAD_MD::read() * 0.93f);

    GBS::VDS_PK_LB_CORE::write(0);    // 3_44 0-3 // 1 for anti source jailbars
    GBS::VDS_PK_LH_CORE::write(0);    // 3_46 0-3 // 1 for anti source jailbars
    GBS::VDS_PK_LB_GAIN::write(0x16); // 3_45 // peaking HF
    GBS::VDS_PK_LH_GAIN::write(0x18); // 3_47
    GBS::VDS_PK_VL_HL_SEL::write(0);  // 3_43 0 if 1 then 3_45 HF almost no effect (coring 0xf9)
    GBS::VDS_PK_VL_HH_SEL::write(0);  // 3_43 1

    //GBS::VDS_STEP_DLY_CNTRL::write(1);  // leave up to preset
    GBS::VDS_STEP_GAIN::write(1);     // max 15
    //GBS::VDS_UV_STEP_BYPS::write(0);  // enable step response

    // DAC filters / keep in presets for now
    //GBS::VDS_1ST_INT_BYPS::write(1); // disable RGB stage interpolator
    //GBS::VDS_2ND_INT_BYPS::write(1); // disable YUV stage interpolator

    // most cases will use osr 2
    setOverSampleRatio(2, true); // prepare only = true

    // full height option
    if (rto->videoStandardInput == 1 || rto->videoStandardInput == 3) {
      if (rto->presetID == 0x5)
      { // out 1080p 60
        if (uopt->wantFullHeight) {
          GBS::VDS_VSCALE::write(455);
          GBS::VDS_DIS_VB_ST::write(GBS::VDS_VSYNC_RST::read() - 2);
          GBS::VDS_DIS_VB_SP::write(40);
          GBS::VDS_VB_SP::write(GBS::VDS_DIS_VB_SP::read() - 2); // 38
          GBS::IF_VB_SP::write(0x10);
          GBS::IF_VB_ST::write(0xE);
        }
      }
    }
    else if (rto->videoStandardInput == 2 || rto->videoStandardInput == 4) {
      if (rto->presetID == 0x15)
      { // out 1080p 50
        if (uopt->wantFullHeight) {
          GBS::VDS_VSCALE::write(455);
          GBS::VDS_DIS_VB_ST::write(GBS::VDS_VSYNC_RST::read() - 2);
          GBS::VDS_DIS_VB_SP::write(38);
          GBS::IF_VB_SP::write(0x3C);
          GBS::IF_VB_ST::write(0x3A);
        }
      }
    }

    if (rto->videoStandardInput == 1 || rto->videoStandardInput == 2)
    {
      GBS::PLLAD_ICP::write(6);           // 5 looks worse
      GBS::ADC_FLTR::write(3);            // 5_03 4/5 ADC filter 3=40, 2=70, 1=110, 0=150 Mhz
      GBS::PLLAD_KS::write(2);            // 5_16
      setOverSampleRatio(2, true);        // prepare only = true
      GBS::IF_SEL_WEN::write(0);          // 1_02 0; 0 for SD, 1 for EDTV
      if (rto->inputIsYpBpR) {            // todo: check other videoStandardInput in component vs rgb
        GBS::IF_HS_TAP11_BYPS::write(0);  // 1_02 4 Tap11 LPF bypass in YUV444to422 
        GBS::IF_HS_Y_PDELAY::write(2);    // 1_02 5+6 delays
        GBS::VDS_V_DELAY::write(0);       // 3_24 2 
        GBS::VDS_Y_DELAY::write(3);       // 3_24 4/5 delays
      }
    }
    if (rto->videoStandardInput == 3 || rto->videoStandardInput == 4 || rto->videoStandardInput == 8)
    {
      // EDTV p-scan, need to either double adc data rate and halve vds scaling
      // or disable line doubler (better) (50 / 60Hz shared)
      GBS::IF_LD_RAM_BYPS::write(1);    // 1_0c 0 no LD
      GBS::ADC_FLTR::write(3);          // 5_03 4/5
      GBS::PLLAD_KS::write(1);          // 5_16
      setOverSampleRatio(2, true);      // with KS = 1 for modes 3, 4, 8
      GBS::IF_HS_DEC_FACTOR::write(0);  // 1_0b 4+5
      GBS::IF_LD_SEL_PROV::write(1);    // 1_0b 7
      GBS::IF_PRGRSV_CNTRL::write(1);   // 1_00 6
      GBS::IF_SEL_WEN::write(1);        // 1_02 0
      GBS::IF_HS_SEL_LPF::write(0);     // 1_02 1   0 = use interpolator not lpf for EDTV
      GBS::IF_HS_TAP11_BYPS::write(0);  // 1_02 4 filter
      GBS::IF_HS_Y_PDELAY::write(3);    // 1_02 5+6 delays (ps2 test on one board clearly says 3, not 2)
      GBS::IF_HB_SP::write(0);          // 1_12 deinterlace offset, fixes colors
      GBS::VDS_V_DELAY::write(1);       // 3_24 2 // new 24.07.2019 : 1, also set 2_17 to 1
      GBS::MADPT_Y_DELAY_UV_DELAY::write(1); // 2_17 : 1
      GBS::VDS_Y_DELAY::write(3);       // 3_24 4/5 delays (ps2 test saying 3 for 1_02 goes with 3 here)
    }
    if (rto->videoStandardInput == 3) 
    { // ED YUV 60
      //GBS::VDS_VSCALE::write(512); // remove
      GBS::IF_HB_ST::write(30);     // 1_10; magic number
      GBS::IF_HB_ST2::write(0x60);  // 1_18
      GBS::IF_HB_SP2::write(0x88);  // 1_1a
      GBS::IF_HBIN_SP::write(0x60); // 1_26 works for all output presets
      if (rto->presetID == 0x5) 
      { // out 1080p
        GBS::IF_HB_SP2::write(GBS::IF_HB_SP2::read() + 12);  // 1_1a  = 0x94
      }
      else if (rto->presetID == 0x3) 
      { // out 720p
        GBS::VDS_VSCALE::write(683); // same as base preset
        GBS::IF_HB_SP2::write(0x8c);  // 1_1a
      }
      else if (rto->presetID == 0x2) 
      { // out x1024
        GBS::VDS_VB_SP::write(GBS::VDS_VB_SP::read() - 8);
        GBS::IF_HBIN_ST::write(0x20); // 1_24
      }
      else if (rto->presetID == 0x1) 
      { // out x960
        GBS::IF_HBIN_ST::write(0x20); // 1_24
        GBS::IF_HB_SP2::write(GBS::IF_HB_SP2::read() + 10);  // 1_1a
      }
    }
    else if (rto->videoStandardInput == 4) 
    { // ED YUV 50
      GBS::IF_HB_ST2::write(0x60);  // 1_18
      GBS::IF_HB_SP2::write(0x88);  // 1_1a for hshift (now only left for out 640x480)
      GBS::IF_HBIN_SP::write(0x40); // 1_26 was 0x80 test: ps2 videomodetester 576p mode
      GBS::IF_HBIN_ST::write(0x20); // 1_24, odd but need to set this here (blue bar)
      GBS::IF_HB_ST::write(0x30); // 1_10
      if (rto->presetID == 0x15) 
      { // out 1080p
        GBS::VDS_DIS_HB_SP::write(GBS::VDS_DIS_HB_SP::read() - 10); // extend left blank
      }
      else if (rto->presetID == 0x13) 
      { // out 720p

      }
      else if (rto->presetID == 0x12) 
      { // out x1024
        GBS::VDS_VB_SP::write(GBS::VDS_VB_SP::read() - 12);
      }
      else if (rto->presetID == 0x11) 
      { // out x960

      }
    }
    else if (rto->videoStandardInput == 5) 
    { // 720p
      GBS::ADC_FLTR::write(1);        // 5_03
      GBS::IF_PRGRSV_CNTRL::write(1); // progressive
      GBS::IF_HS_DEC_FACTOR::write(0);
      GBS::INPUT_FORMATTER_02::write(0x74);
      GBS::VDS_Y_DELAY::write(3);
    }
    else if (rto->videoStandardInput == 6 || rto->videoStandardInput == 7) 
    { // 1080i/p
      GBS::ADC_FLTR::write(1);        // 5_03
      GBS::PLLAD_KS::write(0);        // 5_16
      GBS::IF_PRGRSV_CNTRL::write(1);
      GBS::IF_HS_DEC_FACTOR::write(0);
      GBS::INPUT_FORMATTER_02::write(0x74);
      GBS::VDS_Y_DELAY::write(3);
    }
    else if (rto->videoStandardInput == 8)
    { // 25khz
      // todo: this mode for HV sync
      GBS::PLLAD_ICP::write(6);     // all 25khz submodes have more lines than NTSC
      GBS::ADC_FLTR::write(1);      // 5_03
      GBS::IF_HB_ST::write(30);     // 1_10; magic number
      GBS::IF_HB_ST2::write(0x60);  // 1_18
      GBS::IF_HB_SP2::write(0x88);  // 1_1a
      GBS::IF_HBIN_SP::write(0x60); // 1_26 works for all output presets
      if (rto->presetID == 0x1)
      { // out x960
        GBS::VDS_VSCALE::write(410);
      }
      else if (rto->presetID == 0x2)
      { // out x1024
        GBS::VDS_VSCALE::write(402);
      }
      else if (rto->presetID == 0x3)
      { // out 720p
        GBS::VDS_VSCALE::write(546);
      }
      else if (rto->presetID == 0x5)
      { // out 1080p
        GBS::VDS_VSCALE::write(400);
      }
    }
  }

  latchPLLAD(); // besthtotal reliable with this (EDTV modes, possibly others)

  if (isCustomPreset) {
    // patch in segments not covered in custom preset files (currently seg 2)
    if (rto->videoStandardInput == 3 || rto->videoStandardInput == 4 || rto->videoStandardInput == 8)
    {
      GBS::MADPT_Y_DELAY_UV_DELAY::write(1); // 2_17 : 1
    }
  }

  if (rto->presetIsPalForce60) {
    if (GBS::GBS_OPTION_PALFORCED60_ENABLED::read() != 1) {
      SerialM.println(F("pal forced 60hz: apply vshift"));
      uint16_t vshift = 56; // default shift
      if (rto->presetID == 0x5) { GBS::IF_VB_SP::write(4); } // out 1080p
      else {
        GBS::IF_VB_SP::write(GBS::IF_VB_SP::read() + vshift);
      }
      GBS::IF_VB_ST::write(GBS::IF_VB_SP::read() - 2);
      GBS::GBS_OPTION_PALFORCED60_ENABLED::write(1);
    }
  }

  freezeVideo();

  GBS::ADC_TEST_04::write(0x02);    // 5_04
  GBS::ADC_TEST_0C::write(0x12);    // 5_0c 1 4
  GBS::ADC_TA_05_CTRL::write(0x02);   // 5_05

  // auto ADC gain
  if (uopt->enableAutoGain == 1 && adco->r_gain == 0) {
    //SerialM.println(F("ADC gain: reset"));
    GBS::ADC_RGCTRL::write(0x48);
    GBS::ADC_GGCTRL::write(0x48);
    GBS::ADC_BGCTRL::write(0x48);
    GBS::DEC_TEST_ENABLE::write(1);
  }
  else if (uopt->enableAutoGain == 1 && adco->r_gain != 0) {
    //SerialM.println(F("ADC gain: keep previous"));
    //SerialM.print(adco->r_gain, HEX); SerialM.print(" ");
    //SerialM.print(adco->g_gain, HEX); SerialM.print(" ");
    //SerialM.print(adco->b_gain, HEX); SerialM.println(" ");
    GBS::ADC_RGCTRL::write(adco->r_gain);
    GBS::ADC_GGCTRL::write(adco->g_gain);
    GBS::ADC_BGCTRL::write(adco->b_gain);
    GBS::DEC_TEST_ENABLE::write(1);
  }
  else {
    GBS::DEC_TEST_ENABLE::write(0); // no need for decimation test to be enabled
  }

  // ADC offset if measured
  if (adco->r_off != 0 && adco->g_off != 0 && adco->b_off != 0) {
    GBS::ADC_ROFCTRL::write(adco->r_off);
    GBS::ADC_GOFCTRL::write(adco->g_off);
    GBS::ADC_BOFCTRL::write(adco->b_off);
  }

  SerialM.print("ADC offset: R:"); SerialM.print(GBS::ADC_ROFCTRL::read(), HEX);
  SerialM.print(" G:"); SerialM.print(GBS::ADC_GOFCTRL::read(), HEX);
  SerialM.print(" B:"); SerialM.println(GBS::ADC_BOFCTRL::read(), HEX);

  GBS::IF_AUTO_OFST_U_RANGE::write(1);
  GBS::IF_AUTO_OFST_V_RANGE::write(1);
  GBS::IF_AUTO_OFST_PRD::write(0);  // 0 = by line, 1 = by frame
  GBS::IF_AUTO_OFST_EN::write(0);   // not reliable yet

  if (uopt->wantVdsLineFilter) { GBS::VDS_D_RAM_BYPS::write(0); }
  else { GBS::VDS_D_RAM_BYPS::write(1); }

  if (uopt->wantPeaking) { GBS::VDS_PK_Y_H_BYPS::write(0); }
  else { GBS::VDS_PK_Y_H_BYPS::write(1); }

  if (uopt->wantTap6) { GBS::VDS_TAP6_BYPS::write(0); }
  else { 
    GBS::VDS_TAP6_BYPS::write(1); 
    //GBS::MADPT_Y_DELAY_UV_DELAY::write(GBS::MADPT_Y_DELAY_UV_DELAY::read() + 1);
  }

  if (uopt->wantStepResponse) {
    // step response requested, but only apply if not feedback clock presets
    if (rto->presetID != 0x04 && rto->presetID != 0x14) {
      GBS::VDS_UV_STEP_BYPS::write(0);
    }
  }
  else { 
    GBS::VDS_UV_STEP_BYPS::write(1); 
  }

  resetDebugPort();
  Menu::init();
  FrameSync::reset();
  rto->syncLockFailIgnore = 16;

  if (GBS::GBS_OPTION_SCALING_RGBHV::read() == 0) {
    delay(30);
    updateCoastPosition(0);
    if (rto->coastPositionIsSet) {
      if (GBS::GBS_OPTION_SCALING_RGBHV::read() == 0) // maybe more conditions, but only scaling rgbhv excluded from coast now
      {
        GBS::SP_DIS_SUB_COAST::write(0); // enable SUB_COAST
        GBS::SP_H_PROTECT::write(1);     // enable H_PROTECT temporarily
      }
    }

    // autobesthtotal
    delay(50);  // minimum delay without which random failures happen: ~40
    //boolean autoBestHtotalSuccess = 0;
    if (rto->autoBestHtotalEnabled && rto->videoStandardInput != 0 && !rto->outModeHdBypass) {
      for (uint8_t i = 0; i < 20; i++) {
        if (GBS::STATUS_INT_SOG_BAD::read() == 1) {
          SerialM.println("*");
          optimizeSogLevel();
          resetInterruptSogBadBit();
        }
        else if (getStatus16SpHsStable()) {
          delay(1); // wifi
          float sfr = getSourceFieldRate(0);
          if (sfr > 45.0f && sfr < 87.0f) {
            //autoBestHtotalSuccess = 
            runAutoBestHTotal();
            delay(1); // wifi
            break;
          }
        }
        delay(5);
      }
    }
  }
  else {
    // scaling rgbhv
    delay(80);
    updateCoastPosition(0);
    updateClampPosition();
  }
  //SerialM.print("pp time: "); SerialM.println(millis() - postLoadTimer);

  // noise starts here!
  resetDigital();
  delay(8);
  resetPLLAD(); // also turns on pllad
  GBS::PLLAD_LEN::write(1); // 5_11 1

  if (!isCustomPreset) {
    GBS::VDS_IN_DREG_BYPS::write(0); // 3_40 2 // 0 = input data triggered on falling clock edge, 1 = bypass
    GBS::PLLAD_R::write(3);
    GBS::PLLAD_S::write(3);
    GBS::PLL_R::write(1); // PLL lock detector skew
    GBS::PLL_S::write(2);
    GBS::DEC_IDREG_EN::write(1); // 5_1f 7
    GBS::DEC_WEN_MODE::write(1); // 5_1e 7 // 1 keeps ADC phase consistent. around 4 lock positions vs totally random

    // 4 segment 
    GBS::CAP_SAFE_GUARD_EN::write(0); // 4_21_5 // does more harm than good
    GBS::MADPT_PD_RAM_BYPS::write(1); // 2_24_2 vertical scale down line buffer bypass (not the vds one, the internal one for reduction)
    // memory timings, anti noise
    GBS::PB_CUT_REFRESH::write(1); // test, helps with PLL=ICLK mode artefacting
    GBS::RFF_LREQ_CUT::write(0); // was in motionadaptive toggle function but on, off seems nicer
    GBS::CAP_REQ_OVER::write(1); // 4_22 0  1=capture stop at hblank 0=free run
    GBS::PB_REQ_SEL::write(3); // PlayBack 11 High request Low request
    //GBS::PB_GENERAL_FLAG_REG::write(0x3d); // 4_2D should be set by preset
    GBS::RFF_WFF_OFFSET::write(0x0); // scanline fix
    //GBS::PB_MAST_FLAG_REG::write(0x16); // 4_2c should be set by preset
    // 4_12 should be set by preset
  }

  if (!rto->outModeHdBypass) {
    ResetSDRAM();
  }

  {
    // prepare ideal vline shift for PAL / NTSC SD sources
    // best test for upper content is snes 239 mode (use mainly for setting IF_VB_ST/SP first (1_1c/1e))
    // rto->presetVlineShift = 26; // for ntsc_240p, 1280x1024 ntsc

    // gonsky
  }

  setAndUpdateSogLevel(rto->currentLevelSOG); // use this to cycle SP / ADPLL latches
  
  // IF_VS_SEL = 1 for SD/HD SP mode in HD mode (5_3e 1)
  GBS::IF_VS_SEL::write(0); // 0 = "VCR" IF sync, requires VS_FLIP to be on, more stable?
  GBS::IF_VS_FLIP::write(1);

  GBS::SP_CLP_SRC_SEL::write(0); // 0: 27Mhz clock; 1: pixel clock
  GBS::SP_CS_CLP_ST::write(32); GBS::SP_CS_CLP_SP::write(48); // same as reset parameters

  GBS::DAC_RGBS_PWDNZ::write(1);  // DAC on if needed

  if (rto->outModeHdBypass) {
    GBS::INTERRUPT_CONTROL_01::write(0xff); // enable interrupts
    GBS::INTERRUPT_CONTROL_00::write(0xff); // reset irq status
    GBS::INTERRUPT_CONTROL_00::write(0x00);
    unfreezeVideo();  // eventhough not used atm
    GBS::SP_H_PROTECT::write(0);     // disable H_PROTECT
    // DAC and Sync out will be enabled later
    return; // to setOutModeHdBypass();
  }

  if (GBS::GBS_OPTION_SCALING_RGBHV::read() == 1) {
    rto->videoStandardInput = 14;
  }

  if (GBS::GBS_OPTION_SCALING_RGBHV::read() == 0) {
    unsigned long timeout = millis();
    while ((!getStatus16SpHsStable()) && (millis() - timeout < 2002)) { delay(4); handleWiFi(0); }
    while ((getVideoMode() == 0) && (millis() - timeout < 1505)) { delay(4); handleWiFi(0); }

    timeout = millis() - timeout;
    if (timeout > 1000) {
      Serial.print("to1 is: ");
      Serial.println(timeout);
    }
    if (timeout >= 1500) {
      optimizeSogLevel();
      delay(300);
    }
  }

  // early attempt
  updateClampPosition();
  if (rto->clampPositionIsSet) {
    if (GBS::SP_NO_CLAMP_REG::read() == 1) {
      GBS::SP_NO_CLAMP_REG::write(0);
    }
  }

  updateSpDynamic(); // !
  
  if (!rto->syncWatcherEnabled) {
    GBS::SP_NO_CLAMP_REG::write(0);
  }

  // this was used with ADC write enable, producing about (exactly?) 4 lock positions
  // cycling through the phase let it land favorably
  //for (uint8_t i = 0; i < 8; i++) {
  //  advancePhase();
  //}

  setAndUpdateSogLevel(rto->currentLevelSOG); // use this to cycle SP / ADPLL latches
  optimizePhaseSP();

  GBS::SP_H_PROTECT::write(0);            // disable H_PROTECT
  GBS::INTERRUPT_CONTROL_01::write(0xff); // enable interrupts
  GBS::INTERRUPT_CONTROL_00::write(0xff); // reset irq status
  GBS::INTERRUPT_CONTROL_00::write(0x00);
  
  OutputComponentOrVGA();
  
  // presetPreference 10 means the user prefers bypass mode at startup
  // it's best to run a normal format detect > apply preset loop, then enter bypass mode
  // this can lead to an endless loop, so applyPresetDoneStage = 10 applyPresetDoneStage = 11 
  // are introduced to break out of it.
  // also need to check for mode 15
  // also make sure to turn off autoBestHtotal
  if (uopt->presetPreference == 10 && rto->videoStandardInput != 15) 
  {
    rto->autoBestHtotalEnabled = 0;
    if (rto->applyPresetDoneStage == 11) {
      // we were here before, stop the loop
      rto->applyPresetDoneStage = 1;
    }
    else {
      rto->applyPresetDoneStage = 10;
    }
  }
  else {
    // normal modes
    rto->applyPresetDoneStage = 1;
  }

  unfreezeVideo();

  if (uopt->enableFrameTimeLock) {
    SerialM.println(F("FrameTime Lock is enabled. Disable if display goes blank!"));
  }

  SerialM.print(F("post preset done (preset id: ")); SerialM.print(rto->presetID, HEX);
  if (isCustomPreset) {
    rto->presetID = 9; // overwrite to "custom" after printing original id (for webui)
  }
  if (rto->outModeHdBypass)
  {
    SerialM.print(F(") (bypass)"));
  }
  else if (isCustomPreset) {
    SerialM.print(F(") (custom)"));
  }
  else
  {
    SerialM.print(F(")"));
  }

  SerialM.print(F(" for "));
  if (rto->videoStandardInput == 1)       SerialM.print(F("60Hz "));
  else if (rto->videoStandardInput == 2)  SerialM.print(F("50Hz "));
  else if (rto->videoStandardInput == 3)  SerialM.print(F("60Hz EDTV "));
  else if (rto->videoStandardInput == 4)  SerialM.print(F("50Hz EDTV "));
  else if (rto->videoStandardInput == 5)  SerialM.print(F("720p 60Hz HDTV "));
  else if (rto->videoStandardInput == 6)  SerialM.print(F("1080i 60Hz HDTV "));
  else if (rto->videoStandardInput == 7)  SerialM.print(F("1080p 60Hz HDTV "));
  else if (rto->videoStandardInput == 8)  SerialM.print(F("Medium Res "));
  else if (rto->videoStandardInput == 13) SerialM.print(F("VGA/SVGA/XGA/SXGA"));
  else if (rto->videoStandardInput == 14 || rto->videoStandardInput == 15) {
    if (rto->syncTypeCsync) SerialM.print(F("RGB Bypass (CSync)"));
    else                    SerialM.print(F("RGB Bypass (HV Sync)"));
  }
  else if (rto->videoStandardInput == 0)  SerialM.print(F("!should not go here!"));
  // presetPreference = 2 may fail to load (missing) preset file and arrive here with defaults
  //if (uopt->presetPreference == 2) SerialM.println(F("(custom)"));
  SerialM.println();
}

void applyPresets(uint8_t result) {
  // if RGBHV scaling and invoked through web ui for preset change
  if (result == 14) {
    bypassModeSwitch_RGBHV();
    return;
  }
  
  boolean waitExtra = 0;
  if (rto->outModeHdBypass || rto->videoStandardInput == 15 || rto->videoStandardInput == 0) {
    if (result <= 4 || result == 14) {
      GBS::SFTRST_IF_RSTZ::write(1); // early init
      GBS::SFTRST_VDS_RSTZ::write(1);
      GBS::SFTRST_DEC_RSTZ::write(1);
    }
    waitExtra = 1;
  }
  rto->presetIsPalForce60 = 0;        // the default
  rto->outModeHdBypass = 0;           // the default at this stage
  GBS::GBS_PRESET_CUSTOM::write(0);   // in case it is set; will get set appropriately later 

  // carry over debug view if possible
  if (GBS::ADC_UNUSED_62::read() != 0x00) { 
    // only if the preset to load isn't custom
    // (else the command will instantly disable debug view)
    if (uopt->presetPreference != 2) {
      typeOneCommand = 'D';
    }
  }

  if (uopt->PalForce60 == 1) {
    if (uopt->presetPreference != 2) { // != custom. custom saved as pal preset has ntsc customization
      if (result == 2 || result == 4) { Serial.println("PAL@50 to 60Hz"); rto->presetIsPalForce60 = 1; }
      if (result == 2) { result = 1; }
      if (result == 4) { result = 3; }
    }
  }

  if (result == 1 || result == 3 || result == 8) {
    if (uopt->presetPreference == 0) {
      if (uopt->wantOutputComponent) {
        writeProgramArrayNew(ntsc_1280x1024, false); // override to x1024, later to be patched to 1080p
      }
      else {
        writeProgramArrayNew(ntsc_240p, false);
      }
    }
    else if (uopt->presetPreference == 1) {
      writeProgramArrayNew(ntsc_feedbackclock, false);
    }
    else if (uopt->presetPreference == 3) {
      writeProgramArrayNew(ntsc_1280x720, false);
    }
#if defined(ESP8266)
    else if (uopt->presetPreference == 2) {
      const uint8_t* preset = loadPresetFromSPIFFS(result);
      writeProgramArrayNew(preset, false);
    }
    else if (uopt->presetPreference == 4) {
      if (uopt->matchPresetSource && (result != 8) && (GBS::GBS_OPTION_SCALING_RGBHV::read() == 0)) {
        SerialM.println("matched preset override > 1280x960");
        writeProgramArrayNew(ntsc_240p, false); // pref = x1024 override to x960
      }
      else {
        writeProgramArrayNew(ntsc_1280x1024, false);
      }
    }
#endif
    else if (uopt->presetPreference == 5) {
      writeProgramArrayNew(ntsc_1920x1080, false);
    }
  }
  else if (result == 2 || result == 4) {
    if (uopt->presetPreference == 0) {
      if (uopt->wantOutputComponent) {
        writeProgramArrayNew(pal_1280x1024, false); // override to x1024, later to be patched to 1080p
      }
      else {
        if (uopt->matchPresetSource) {
          SerialM.println("matched preset override > 1280x1024");
          writeProgramArrayNew(pal_1280x1024, false); // pref = x960 override to x1024
        }
        else {
          writeProgramArrayNew(pal_240p, false);
        }
      }
    }
    else if (uopt->presetPreference == 1) {
      writeProgramArrayNew(pal_feedbackclock, false);
    }
    else if (uopt->presetPreference == 3) {
      writeProgramArrayNew(pal_1280x720, false);
    }
#if defined(ESP8266)
    else if (uopt->presetPreference == 2) {
      const uint8_t* preset = loadPresetFromSPIFFS(result);
      writeProgramArrayNew(preset, false);
    }
    else if (uopt->presetPreference == 4) {
      writeProgramArrayNew(pal_1280x1024, false);
    }
#endif
    else if (uopt->presetPreference == 5) {
      writeProgramArrayNew(pal_1920x1080, false);
    }
  }
  else if (result == 5 || result == 6 || result == 7 || result == 13) {
    // use bypass mode for these HD sources
    rto->videoStandardInput = result;
    setOutModeHdBypass();
    return;
  }
  else if (result == 15) {
    SerialM.print("RGB/HV bypass ");
    if (rto->syncTypeCsync) { SerialM.print("(CSync) "); }
    //if (uopt->preferScalingRgbhv) {
    //  SerialM.print("(prefer scaling mode)");
    //}
    SerialM.println();
    bypassModeSwitch_RGBHV();
    // don't go through doPostPresetLoadSteps
    return;
  }
  else {
    SerialM.println("Unknown timing! ");
    rto->videoStandardInput = 0; // mark as "no sync" for syncwatcher
    inputAndSyncDetect();
    delay(300);
    return;
  }

  // get auto gain prefs
  if (uopt->presetPreference == 2 && uopt->enableAutoGain) {
    adco->r_gain = GBS::ADC_RGCTRL::read();
    adco->g_gain = GBS::ADC_GGCTRL::read();
    adco->b_gain = GBS::ADC_BGCTRL::read();
  }

  rto->videoStandardInput = result;
  if (waitExtra) {
    // extra time needed for digital resets, so that autobesthtotal works first attempt
    //Serial.println("waitExtra 400ms");
    delay(400); // min ~ 300
  }
  doPostPresetLoadSteps();
}

void unfreezeVideo() {
  //if (GBS::CAP_REQ_FREEZ::read() == 1)
  if (rto->videoIsFrozen == 1)
  {
    /*GBS::CAP_REQ_FREEZ::write(0);
    delay(60);
    GBS::CAPTURE_ENABLE::write(1);*/
    
    //GBS::IF_VB_ST::write(4);
    GBS::IF_VB_ST::write(GBS::IF_VB_SP::read() - 2);
  }
  rto->videoIsFrozen = false;
}

void freezeVideo() {
  if (rto->videoIsFrozen == false) {
    /*GBS::CAP_REQ_FREEZ::write(1);
    delay(1);
    GBS::CAPTURE_ENABLE::write(0);*/
    GBS::IF_VB_ST::write(GBS::IF_VB_SP::read());
  }
  rto->videoIsFrozen = true;
}

static uint8_t getVideoMode() {
  uint8_t detectedMode = 0;

  if (rto->videoStandardInput >= 14) { // check RGBHV first // not mode 13 here, else mode 13 can't reliably exit
    detectedMode = GBS::STATUS_16::read();
    if ((detectedMode & 0x0a) > 0) { // bit 1 or 3 active?
      return rto->videoStandardInput; // still RGBHV bypass, 14 or 15
    }
    else {
      return 0;
    }
  }

  detectedMode = GBS::STATUS_00::read();

  // note: if stat0 == 0x07, it's supposedly stable. if we then can't find a mode, it must be an MD problem
  if ((detectedMode & 0x07) == 0x07)
  {
    if ((detectedMode & 0x80) == 0x80) { // bit 7: SD flag (480i, 480P, 576i, 576P)
      if ((detectedMode & 0x08) == 0x08) return 1; // ntsc interlace
      if ((detectedMode & 0x20) == 0x20) return 2; // pal interlace
      if ((detectedMode & 0x10) == 0x10) return 3; // edtv 60 progressive
      if ((detectedMode & 0x40) == 0x40) return 4; // edtv 50 progressive
    }

    detectedMode = GBS::STATUS_03::read();
    if ((detectedMode & 0x10) == 0x10) { return 5; } // hdtv 720p

    if (rto->videoStandardInput == 4) {
      detectedMode = GBS::STATUS_04::read();
      if ((detectedMode & 0xFF) == 0x80) {
        return 4; // still edtv 50 progressive
      }
    }
  }

  detectedMode = GBS::STATUS_04::read();
  if ((detectedMode & 0x20) == 0x20) { // hd mode on
    if ((detectedMode & 0x61) == 0x61) {
      // hdtv 1080i // 576p mode tends to get misdetected as this, even with all the checks
      // real 1080i (PS2): h:199 v:1124
      // misdetected 576p (PS2): h:215 v:1249
      if (GBS::VPERIOD_IF::read() < 1160) {
        return 6;
      }
    }
    if ((detectedMode & 0x10) == 0x10) {
      if ((detectedMode & 0x04) == 0x04) { // normally HD2376_1250P (PAL FHD?), but using this for 24k
        return 8;
      }
      return 7; // hdtv 1080p
    }
  }

  // graphic modes, mostly used for ps2 doing rgb over yuv with sog
  if ((GBS::STATUS_05::read() & 0x0c) == 0x00) // 2: Horizontal unstable AND 3: Vertical unstable are 0?
  {
    if (GBS::STATUS_00::read() == 0x07) { // the 3 stat0 stable indicators on, none of the SD indicators on
      if ((GBS::STATUS_03::read() & 0x02) == 0x02) // Graphic mode bit on (any of VGA/SVGA/XGA/SXGA at all detected Hz)
      {
        if (rto->inputIsYpBpR)  return 13;
        else                    return 15; // switch to RGBS/HV handling
      }
      else {
        // this mode looks like it wants to be graphic mode, but the horizontal counter target in MD is very strict
        static uint8_t XGA_60HZ = GBS::MD_XGA_60HZ_CNTRL::read();
        static uint8_t XGA_70HZ = GBS::MD_XGA_70HZ_CNTRL::read();
        static uint8_t XGA_75HZ = GBS::MD_XGA_75HZ_CNTRL::read();
        static uint8_t XGA_85HZ = GBS::MD_XGA_85HZ_CNTRL::read();

        static uint8_t SXGA_60HZ = GBS::MD_SXGA_60HZ_CNTRL::read();
        static uint8_t SXGA_75HZ = GBS::MD_SXGA_75HZ_CNTRL::read();
        static uint8_t SXGA_85HZ = GBS::MD_SXGA_85HZ_CNTRL::read();

        static uint8_t SVGA_60HZ = GBS::MD_SVGA_60HZ_CNTRL::read();
        static uint8_t SVGA_75HZ = GBS::MD_SVGA_75HZ_CNTRL::read();
        static uint8_t SVGA_85HZ = GBS::MD_SVGA_85HZ_CNTRL::read();

        static uint8_t VGA_75HZ = GBS::MD_VGA_75HZ_CNTRL::read();
        static uint8_t VGA_85HZ = GBS::MD_VGA_85HZ_CNTRL::read();

        short hSkew = random(-2, 2);  // skew the target a little
        //Serial.println(XGA_60HZ + hSkew, HEX);
        GBS::MD_XGA_60HZ_CNTRL::write(XGA_60HZ + hSkew);
        GBS::MD_XGA_70HZ_CNTRL::write(XGA_70HZ + hSkew);
        GBS::MD_XGA_75HZ_CNTRL::write(XGA_75HZ + hSkew);
        GBS::MD_XGA_85HZ_CNTRL::write(XGA_85HZ + hSkew);
        GBS::MD_SXGA_60HZ_CNTRL::write(SXGA_60HZ + hSkew);
        GBS::MD_SXGA_75HZ_CNTRL::write(SXGA_75HZ + hSkew);
        GBS::MD_SXGA_85HZ_CNTRL::write(SXGA_85HZ + hSkew);
        GBS::MD_SVGA_60HZ_CNTRL::write(SVGA_60HZ + hSkew);
        GBS::MD_SVGA_75HZ_CNTRL::write(SVGA_75HZ + hSkew);
        GBS::MD_SVGA_85HZ_CNTRL::write(SVGA_85HZ + hSkew);
        GBS::MD_VGA_75HZ_CNTRL::write(VGA_75HZ + hSkew);
        GBS::MD_VGA_85HZ_CNTRL::write(VGA_85HZ + hSkew);
      }
    }
  }

  return 0; // unknown mode
}

// if testbus has 0x05, sync is present and line counting active. if it has 0x04, sync is present but no line counting
boolean getSyncPresent() {
  uint8_t debug_backup = GBS::TEST_BUS_SEL::read();
  uint8_t debug_backup_SP = GBS::TEST_BUS_SP_SEL::read();
  if (debug_backup != 0xa) {
    GBS::TEST_BUS_SEL::write(0xa);
  }
  if (debug_backup_SP != 0x0f) {
    GBS::TEST_BUS_SP_SEL::write(0x0f);
  }
  uint16_t readout = GBS::TEST_BUS::read();
  //if (((readout & 0x0500) == 0x0500) || ((readout & 0x0500) == 0x0400)) {
  if (readout > 0x0180) {
    if (debug_backup != 0xa) {
      GBS::TEST_BUS_SEL::write(debug_backup);
    }
    if (debug_backup_SP != 0x0f) {
      GBS::TEST_BUS_SP_SEL::write(debug_backup_SP);
    }
    return true;
  }

  if (debug_backup != 0xa) {
    GBS::TEST_BUS_SEL::write(debug_backup);
  }
  if (debug_backup_SP != 0x0f) {
    GBS::TEST_BUS_SP_SEL::write(debug_backup_SP);
  }
  return false;
}

// returns 0_00 bit 2 = H+V both stable (for the IF, not SP)
boolean getStatus00IfHsVsStable() {
  return ((GBS::STATUS_00::read() & 0x04) == 0x04) ? 1 : 0;
}

// used to be a check for the length of the debug bus readout of 5_63 = 0x0f
// now just checks the chip status at 0_16 HS active (and Interrupt bit4 HS active for RGBHV)
boolean getStatus16SpHsStable() {
  if (rto->videoStandardInput == 15) { // check RGBHV first
    if (GBS::STATUS_INT_INP_NO_SYNC::read() == 0) {
      return true;
    }
    else {
      resetInterruptNoHsyncBadBit();
      return false;
    }
  }

  // STAT_16 bit 1 is the "hsync active" flag, which appears to be a reliable indicator
  // checking the flag replaces checking the debug bus pulse length manually
  uint8_t status16 = GBS::STATUS_16::read();
  if ((status16 & 0x02) == 0x02)
  {
    if (rto->videoStandardInput == 1 || rto->videoStandardInput == 2) 
    {
      if ((status16 & 0x01) != 0x01) 
      {  // pal / ntsc should be sync active low
        return true;
      }
    }
    else 
    {
      return true;  // not pal / ntsc
    }
  }

  return false;
}

void setOverSampleRatio(uint8_t newRatio, boolean prepareOnly) {
  uint8_t ks = GBS::PLLAD_KS::read();

  switch (newRatio) {
  case 1:
    if (ks == 0) GBS::PLLAD_CKOS::write(0);
    if (ks == 1) GBS::PLLAD_CKOS::write(1);
    if (ks == 2) GBS::PLLAD_CKOS::write(2);
    if (ks == 3) GBS::PLLAD_CKOS::write(3);
    GBS::ADC_CLK_ICLK2X::write(0);
    GBS::ADC_CLK_ICLK1X::write(0);
    GBS::DEC1_BYPS::write(1); // dec1 couples to ADC_CLK_ICLK2X
    GBS::DEC2_BYPS::write(1);

    if (rto->videoStandardInput == 8 || rto->videoStandardInput == 4 || rto->videoStandardInput == 3) {
      GBS::ADC_CLK_ICLK1X::write(1);
      //GBS::DEC2_BYPS::write(0);
    }

    rto->osr = 1;
    //if (!prepareOnly) SerialM.println("OSR 1x");

    break;
  case 2:
    if (ks == 0) { setOverSampleRatio(1, false); return; } // 2x impossible
    if (ks == 1) GBS::PLLAD_CKOS::write(0);
    if (ks == 2) GBS::PLLAD_CKOS::write(1);
    if (ks == 3) GBS::PLLAD_CKOS::write(2);
    GBS::ADC_CLK_ICLK2X::write(0);
    GBS::ADC_CLK_ICLK1X::write(1);
    GBS::DEC2_BYPS::write(0);
    GBS::DEC1_BYPS::write(1); // dec1 couples to ADC_CLK_ICLK2X

    if (rto->videoStandardInput == 8 || rto->videoStandardInput == 4 || rto->videoStandardInput == 3) {
      //GBS::ADC_CLK_ICLK2X::write(1);
      //GBS::DEC1_BYPS::write(0);
      // instead increase CKOS by 1 step
      GBS::PLLAD_CKOS::write(GBS::PLLAD_CKOS::read() + 1);
    }

    rto->osr = 2;
    //if (!prepareOnly) SerialM.println("OSR 2x");

    break;
  case 4:
    if (ks == 0) { setOverSampleRatio(1, false); return; } // 4x impossible
    if (ks == 1) { setOverSampleRatio(1, false); return; } // 4x impossible
    if (ks == 2) GBS::PLLAD_CKOS::write(0);
    if (ks == 3) GBS::PLLAD_CKOS::write(1);
    GBS::ADC_CLK_ICLK2X::write(1);
    GBS::ADC_CLK_ICLK1X::write(1);
    GBS::DEC1_BYPS::write(0); // dec1 couples to ADC_CLK_ICLK2X
    GBS::DEC2_BYPS::write(0);

    rto->osr = 4;
    //if (!prepareOnly) SerialM.println("OSR 4x");

    break;
  default:
    break;
  }

  if (!prepareOnly) latchPLLAD();
}

void togglePhaseAdjustUnits() {
  GBS::PA_SP_BYPSZ::write(0); // yes, 0 means bypass on here
  GBS::PA_SP_BYPSZ::write(1);
  delay(2);
  GBS::PA_ADC_BYPSZ::write(0);
  GBS::PA_ADC_BYPSZ::write(1);
  delay(2);
}

void advancePhase() {
  rto->phaseADC = (rto->phaseADC + 1) & 0x1f;
  setAndLatchPhaseADC();
}

void movePhaseThroughRange() {
  for (uint8_t i = 0; i < 128; i++) { // 4x for 4x oversampling?
    advancePhase();
  }
}

void setAndLatchPhaseSP() {
  GBS::PA_SP_LAT::write(0); // latch off
  GBS::PA_SP_S::write(rto->phaseSP);
  GBS::PA_SP_LAT::write(1); // latch on
}

void setAndLatchPhaseADC() {
  GBS::PA_ADC_LAT::write(0);
  GBS::PA_ADC_S::write(rto->phaseADC);
  GBS::PA_ADC_LAT::write(1);
}

void updateSpDynamic() {
  if ((rto->videoStandardInput == 0) || !rto->boardHasPower || rto->sourceDisconnected)
  {
    return;
  }
  
  uint8_t vidModeReadout = getVideoMode();
  // reset condition, allow most formats to detect
  if (vidModeReadout == 0 || (vidModeReadout != rto->videoStandardInput)) {
      GBS::SP_PRE_COAST::write(0xA);    //10
      GBS::SP_POST_COAST::write(0x12);  //18 // ps2 1080p
      GBS::SP_DLT_REG::write(0x70);     //5_35 to 0x70 (0x80 for 1080p)
      GBS::SP_H_PULSE_IGNOR::write(0x02);
      return;
  }
  
  if (rto->videoStandardInput <= 2) { // SD interlaced
    GBS::SP_PRE_COAST::write(10); // psx: 9,9 (5,5 in 240p)
    GBS::SP_POST_COAST::write(9);
    GBS::SP_DLT_REG::write(0x130);
    GBS::SP_H_PULSE_IGNOR::write(0x14);
  }
  else if (rto->videoStandardInput <= 4) {
    GBS::SP_PRE_COAST::write(11); // these two were 7 and 6
    GBS::SP_POST_COAST::write(11); // and last 11 and 11
    // 3,3 fixes the ps2 issue but these are too low for format change detects
    // update: seems to be an SP bypass only problem (t5t57t6 to 0 also fixes it)
    GBS::SP_DLT_REG::write(0x130);
    GBS::SP_H_PULSE_IGNOR::write(0x0b);
  }
  else if (rto->videoStandardInput == 5) { // 720p
    GBS::SP_PRE_COAST::write(8); // down to 4 ok with ps2
    GBS::SP_POST_COAST::write(8); // down to 6 ok with ps2
    GBS::SP_DLT_REG::write(0x110);
    GBS::SP_H_PULSE_IGNOR::write(0x06);
  }
  else if (rto->videoStandardInput <= 7) { // 1080i,p
    GBS::SP_PRE_COAST::write(9);
    GBS::SP_POST_COAST::write(22); // of 1124 input lines
    GBS::SP_DLT_REG::write(0x70);
    GBS::SP_H_PULSE_IGNOR::write(0x02);
  }
  else if (rto->videoStandardInput >= 13) { // 13, 14 and 15 (was just 13 and 15)
    if (rto->syncTypeCsync == false)
    {
      GBS::SP_PRE_COAST::write(0x00);
      GBS::SP_POST_COAST::write(0x00);
      //GBS::SP_H_PULSE_IGNOR::write(0x02);
      GBS::SP_H_PULSE_IGNOR::write(0xff); // todo: test in mode 14 and 13, 15 requires this because 5_02 0 is on
    }
    else { // csync
      GBS::SP_PRE_COAST::write(0x04);   // as in bypass mode set function
      GBS::SP_POST_COAST::write(0x07);  // as in bypass mode set function
      GBS::SP_DLT_REG::write(0x70);
      GBS::SP_H_PULSE_IGNOR::write(0x02);
    }
  }

  if (rto->syncTypeCsync == true) {
    if (GBS::STATUS_SYNC_PROC_VSACT::read() == 1) {
      GBS::SP_H_PROTECT::write(1);
    }
    else {
      GBS::SP_H_PROTECT::write(0);
    }
  }

}

void updateCoastPosition(boolean autoCoast) {
  if (((rto->videoStandardInput == 0) || (rto->videoStandardInput > 14)) ||
    !rto->boardHasPower || rto->sourceDisconnected)
  {
    return;
  }

  uint32_t accInHlength = 0;
  uint16_t prevInHlength = GBS::HPERIOD_IF::read();
  for (uint8_t i = 0; i < 8; i++) {
    // psx jitters between 427, 428
    uint16_t thisInHlength = GBS::HPERIOD_IF::read();
    if ((thisInHlength > (prevInHlength - 3)) && (thisInHlength < (prevInHlength + 3))) {
      accInHlength += thisInHlength;
    }
    else {
      return;
    }
    if (!getStatus16SpHsStable()) {
      return;
    }

    prevInHlength = thisInHlength;
  }
  accInHlength = (accInHlength * 4) / 8;

  // 30.09.19 new: especially in low res VGA input modes, it can clip at "511 * 4 = 2044"
  // limit to more likely actual value of 430
  if (accInHlength >= 2040) {
    accInHlength = 1716;
  }

  if (accInHlength > 32) {
    if (autoCoast) {
      // autoCoast (5_55 7 = on)
      GBS::SP_H_CST_ST::write((uint16_t)(accInHlength * 0.004f));
      GBS::SP_H_CST_SP::write((uint16_t)(accInHlength * 0.060f));
      GBS::SP_HCST_AUTO_EN::write(1);
    }
    else {
      // regular coast (5_55 7 = off): maximize length
      // test: psx pal black license screen, then ntsc SMPTE color bars 100%; or MS
      GBS::SP_H_CST_ST::write((uint16_t)(accInHlength * 0.014f)); // 0.07f
      GBS::SP_H_CST_SP::write((uint16_t)(accInHlength * 0.95)); // 0.978f // also test with t5t57t2
      GBS::SP_HCST_AUTO_EN::write(0);
    }
    rto->coastPositionIsSet = 1;

    // also set SP regenerated HS position, in case it is to be used
    // this appears to be used in Mode Detect so mind interlace / progressive switches (halved h period)
    GBS::SP_CS_HS_ST::write(32);
    GBS::SP_CS_HS_SP::write(0);

    Serial.print("coast ST: "); Serial.print("0x"); Serial.print(GBS::SP_H_CST_ST::read(), HEX);
    Serial.print(", ");
    Serial.print("SP: "); Serial.print("0x"); Serial.print(GBS::SP_H_CST_SP::read(), HEX);
    Serial.print("  total: "); Serial.print("0x"); Serial.print(accInHlength, HEX);
    Serial.print(" ~ "); Serial.println(accInHlength / 4);
  }
}

void updateClampPosition() {
  if ((rto->videoStandardInput == 0) || !rto->boardHasPower || rto->sourceDisconnected) 
  {
    return;
  }
  // this is required especially on mode changes with ypbpr
  if (getVideoMode() == 0) { return; }

  if (rto->inputIsYpBpR) {
    GBS::SP_CLAMP_MANUAL::write(0);
  }
  else {
    GBS::SP_CLAMP_MANUAL::write(1); // no auto clamp for RGB
  }

  // STATUS_SYNC_PROC_HTOTAL is "ht: " value; use with SP_CLP_SRC_SEL = 1 pixel clock
  // GBS::HPERIOD_IF::read()  is "h: " value; use with SP_CLP_SRC_SEL = 0 osc clock
  // update: in RGBHV bypass it seems both clamp source modes use pixel clock for calculation
  // but with sog modes, it uses HPERIOD_IF ... k
  // update2: if the clamp is already short, yet creeps into active video, check sog invert (t5t20t2)
  uint32_t accInHlength = 0;
  uint16_t prevInHlength = 0;
  uint16_t thisInHlength = 0;
  if (rto->syncTypeCsync) prevInHlength = GBS::HPERIOD_IF::read();
  else                    prevInHlength = GBS::STATUS_SYNC_PROC_HTOTAL::read();
  for (uint8_t i = 0; i < 16; i++) {
    if (rto->syncTypeCsync) thisInHlength = GBS::HPERIOD_IF::read();
    else                    thisInHlength = GBS::STATUS_SYNC_PROC_HTOTAL::read();
    if ((thisInHlength > (prevInHlength - 3)) && (thisInHlength < (prevInHlength + 3))) {
      accInHlength += thisInHlength;
    }
    else {
      //Serial.println("updateClampPosition unstable");
      return;
    }
    if (!getStatus16SpHsStable()) {
      return;
    }

    prevInHlength = thisInHlength;
    delayMicroseconds(100);
  }
  accInHlength = accInHlength / 16;  // for the 16x loop

  // HPERIOD_IF: 9 bits (0-511, represents actual scanline time / 4)
  // STATUS_SYNC_PROC_HTOTAL: 12 bits (0-4095)
  if (accInHlength < 16 || accInHlength > 4095) {
      return;
  }

  uint16_t oldClampST = GBS::SP_CS_CLP_ST::read();
  uint16_t oldClampSP = GBS::SP_CS_CLP_SP::read();
  float multiSt = rto->syncTypeCsync == 1 ? 0.032f : 0.010f;
  float multiSp = rto->syncTypeCsync == 1 ? 0.190f : 0.058f;
  uint16_t start = 1 + (accInHlength * multiSt);   // HPERIOD_IF: *0.04 seems good
  uint16_t stop =  2 + (accInHlength * multiSp);   // HPERIOD_IF: *0.21 starts to creep into ps2 worst mode, all else is good long after 

  if (rto->inputIsYpBpR) {
    // YUV: // ST shift forward to pass blacker than black HSync, sog: min * 0.08
    multiSt = rto->syncTypeCsync == 1 ? 0.089f : 0.032f;
    start = 1 + (accInHlength * multiSt);   
  }

  if ((start < (oldClampST - 1) || start > (oldClampST + 1)) ||
      (stop < (oldClampSP - 1) || stop > (oldClampSP + 1)))
  {
    GBS::SP_CS_CLP_ST::write(start);
    GBS::SP_CS_CLP_SP::write(stop);
    Serial.print("clamp ST: "); Serial.print("0x"); Serial.print(start, HEX);
    Serial.print(", ");
    Serial.print("SP: "); Serial.print("0x"); Serial.print(stop, HEX);
    Serial.print("   total: "); Serial.print("0x"); Serial.print(accInHlength, HEX);
    Serial.print(" / "); Serial.println(accInHlength);
  }

  rto->clampPositionIsSet = true;
}

// use t5t00t2 and adjust t5t11t5 to find this sources ideal sampling clock for this preset (affected by htotal)
// 2431 for psx, 2437 for MD
// in this mode, sampling clock is free to choose
void setOutModeHdBypass() {
  rto->autoBestHtotalEnabled = false;   // disable while in this mode
  rto->outModeHdBypass = 1;             // skips waiting at end of doPostPresetLoadSteps

  loadHdBypassSection();                // this would be ignored otherwise
  GBS::ADC_UNUSED_62::write(0x00);      // clear debug view
  GBS::RESET_CONTROL_0x46::write(0x00); // 0_46 all off, nothing needs to be enabled for bp mode
  GBS::RESET_CONTROL_0x47::write(0x00);
  GBS::PA_ADC_BYPSZ::write(1);          // enable phase unit ADC
  GBS::PA_SP_BYPSZ::write(1);           // enable phase unit SP

  doPostPresetLoadSteps();
  resetDebugPort();

  rto->autoBestHtotalEnabled = false; // need to re-set this
  GBS::OUT_SYNC_SEL::write(1); // 0_4f 1=sync from HDBypass, 2=sync from SP, (00 = from VDS)

  GBS::PLL_CKIS::write(0); // 0_40 0 //  0: PLL uses OSC clock | 1: PLL uses input clock
  GBS::PLL_DIVBY2Z::write(0); // 0_40 1 // 1= no divider (full clock, ie 27Mhz) 0 = halved
  //GBS::PLL_ADS::write(0); // 0_40 3 test:  input clock is from PCLKIN (disconnected, not ADC clock)
  GBS::PAD_OSC_CNTRL::write(1); // test: noticed some wave pattern in 720p source, this fixed it
  GBS::PLL648_CONTROL_01::write(0x35);
  GBS::PLL648_CONTROL_03::write(0x00); GBS::PLL_LEN::write(1);  // 0_43
  GBS::DAC_RGBS_R0ENZ::write(1); GBS::DAC_RGBS_G0ENZ::write(1); // 0_44
  GBS::DAC_RGBS_B0ENZ::write(1); GBS::DAC_RGBS_S1EN::write(1);  // 0_45
  // from RGBHV tests: the memory bus can be tri stated for noise reduction
  GBS::PAD_TRI_ENZ::write(1); // enable tri state
  GBS::PLL_MS::write(2); // select feedback clock (but need to enable tri state!)
  GBS::MEM_PAD_CLK_INVERT::write(0); // helps also
  GBS::RESET_CONTROL_0x47::write(0x1f);

  // update: found the real use of HDBypass :D
  GBS::DAC_RGBS_BYPS2DAC::write(1);
  GBS::SP_HS_LOOP_SEL::write(1);
  GBS::SP_HS_PROC_INV_REG::write(0); // (5_56_5) do not invert HS

  GBS::PB_BYPASS::write(1);
  GBS::PLLAD_MD::write(2345); // 2326 looks "better" on my LCD but 2345 looks just correct on scope
  GBS::PLLAD_KS::write(2);    // 5_16 post divider 0 : FCKO1 > 87MHz, 3 : FCKO1<23MHz
  setOverSampleRatio(2, true);
  GBS::PLLAD_ICP::write(5);
  GBS::PLLAD_FS::write(1);
  
  if (rto->inputIsYpBpR) {
    GBS::DEC_MATRIX_BYPS::write(1); // 5_1f 2 = 1 for YUV / 0 for RGB
    GBS::HD_MATRIX_BYPS::write(0);  // 1_30 1 / input to jacks is yuv, adc leaves it as yuv > convert to rgb for output here
    GBS::HD_DYN_BYPS::write(0);     // don't bypass color expansion
    //GBS::HD_U_OFFSET::write(3);     // color adjust via scope
    //GBS::HD_V_OFFSET::write(3);     // color adjust via scope
  }
  else {
    GBS::DEC_MATRIX_BYPS::write(1); // assuming RGB to the jack, then adc should still leave it as yuv
    GBS::HD_MATRIX_BYPS::write(1);  // 1_30 1 / input to jacks is rgb, adc leaves it as rgb > bypass yuv to rgb here
    GBS::HD_DYN_BYPS::write(1);     // bypass as well
  }
  
  GBS::HD_SEL_BLK_IN::write(0);   // 0 enables HDB blank timing (1 would be DVI, not working atm)

  GBS::SP_SDCS_VSST_REG_H::write(0); // S5_3B
  GBS::SP_SDCS_VSSP_REG_H::write(0); // S5_3B
  GBS::SP_SDCS_VSST_REG_L::write(0); // S5_3F // 3 for SP sync
  GBS::SP_SDCS_VSSP_REG_L::write(2); // S5_40 // 10 for SP sync // check with interlaced sources

  GBS::HD_HSYNC_RST::write(0x3ff); // max 0x7ff
  GBS::HD_INI_ST::write(0);    // todo: test this at 0 / was 0x298
  // timing into HDB is PLLAD_MD with PLLAD_KS divider: KS = 0 > full PLLAD_MD
  if (rto->videoStandardInput <= 2) {
    //GBS::SP_HS2PLL_INV_REG::write(1); //5_56 1 lock to falling sync edge // seems wrong, sync issues with MD
    GBS::ADC_FLTR::write(3);     // 5_03 4/5 ADC filter 3=40, 2=70, 1=110, 0=150 Mhz
    //GBS::HD_INI_ST::write(0x76); // 1_39
    GBS::HD_HB_ST::write(0x878); // 1_3B
    GBS::HD_HB_SP::write(0x90);  // 1_3D
    GBS::HD_HS_ST::write(0x08);  // 1_3F
    GBS::HD_HS_SP::write(0x8b0); // 1_41
    GBS::HD_VB_ST::write(0x00);  // 1_43
    GBS::HD_VB_SP::write(0x1d);  // 1_45
    GBS::HD_VS_ST::write(0x07);  // 1_47 // VS neg
    GBS::HD_VS_SP::write(0x02);  // 1_49
  }
  else if (rto->videoStandardInput == 3 || rto->videoStandardInput == 4) { // 480p, 576p
    GBS::ADC_FLTR::write(2);     // 5_03 4/5 ADC filter 3=40, 2=70, 1=110, 0=150 Mhz
    GBS::PLLAD_KS::write(1);     // 5_16 post divider
    GBS::PLLAD_CKOS::write(0);   // 5_16 2x OS (with KS=1)
    //GBS::HD_INI_ST::write(0x76); // 1_39
    GBS::HD_HB_ST::write(0x878); // 1_3B
    GBS::HD_HB_SP::write(0xa0);  // 1_3D
    GBS::HD_HS_ST::write(0x10);  // 1_3F
    GBS::HD_HS_SP::write(0x8b0); // 1_41
    GBS::HD_VB_ST::write(0x00);  // 1_43
    GBS::HD_VB_SP::write(0x40);  // 1_45
    GBS::HD_VS_ST::write(0x16);  // 1_47 // VS neg
    GBS::HD_VS_SP::write(0x10);  // 1_49
    GBS::SP_SDCS_VSST_REG_L::write(2); // S5_3F // invert CS separation VS to output earlier
    GBS::SP_SDCS_VSSP_REG_L::write(0); // S5_40
  }
  else if (rto->videoStandardInput <= 7 || rto->videoStandardInput == 13) {
    //GBS::SP_HS2PLL_INV_REG::write(0); // 5_56 1 use rising edge of tri-level sync // always 0 now
    GBS::ADC_FLTR::write(1);            // 5_03 4/5 ADC filter 3=40, 2=70, 1=110, 0=150 Mhz
    if (rto->videoStandardInput == 5) { // 720p
      GBS::HD_HSYNC_RST::write(550); // 1_37
      //GBS::HD_INI_ST::write(78);     // 1_39
      // 720p has high pllad vco output clock, so don't do oversampling
      GBS::PLLAD_KS::write(0);      // 5_16 post divider 0 : FCKO1 > 87MHz, 3 : FCKO1<23MHz
      GBS::PLLAD_CKOS::write(0);    // 5_16 1x OS (with KS=CKOS=0)
      GBS::ADC_CLK_ICLK1X::write(0);// 5_00 4 (OS=1)
      GBS::DEC2_BYPS::write(1);     // 5_1f 1 // dec2 disabled (OS=1)
      GBS::PLLAD_ICP::write(6);     // fine at 7 as well, FS is 0
      GBS::PLLAD_FS::write(0);
      GBS::HD_HB_ST::write(0);      // 1_3B
      GBS::HD_HB_SP::write(0x140);  // 1_3D
      GBS::HD_HS_ST::write(0x18);   // 1_3F
      GBS::HD_HS_SP::write(0x80);   // 1_41
      GBS::HD_VB_ST::write(0x00);   // 1_43
      GBS::HD_VB_SP::write(0x40);   // 1_45
      GBS::HD_VS_ST::write(0x08);   // 1_47
      GBS::HD_VS_SP::write(0x0d);   // 1_49
      GBS::SP_SDCS_VSST_REG_L::write(2); // S5_3F // invert CS separation VS to output earlier
      GBS::SP_SDCS_VSSP_REG_L::write(0); // S5_40
    }
    if (rto->videoStandardInput == 6) { // 1080i
      // interl. source
      GBS::HD_HSYNC_RST::write(0x710); // 1_37
      //GBS::HD_INI_ST::write(2);    // 1_39
      GBS::PLLAD_KS::write(1);     // 5_16 post divider
      GBS::PLLAD_CKOS::write(0);   // 5_16 2x OS (with KS=1)
      GBS::HD_HB_ST::write(0);     // 1_3B
      GBS::HD_HB_SP::write(0xb8);  // 1_3D
      GBS::HD_HS_ST::write(0x10);  // 1_3F
      GBS::HD_HS_SP::write(0x50);  // 1_41
      GBS::HD_VB_ST::write(0x00);  // 1_43
      GBS::HD_VB_SP::write(0x1e);  // 1_45
      GBS::HD_VS_ST::write(0x04);  // 1_47
      GBS::HD_VS_SP::write(0x09);  // 1_49
      GBS::SP_SDCS_VSST_REG_L::write(2); // S5_3F // invert CS separation VS to output earlier
      GBS::SP_SDCS_VSSP_REG_L::write(0); // S5_40
    }
    if (rto->videoStandardInput == 7) { // 1080p
      GBS::HD_HSYNC_RST::write(0x710); // 1_37
      //GBS::HD_INI_ST::write(0xf0);     // 1_39
      // 1080p has highest pllad vco output clock, so don't do oversampling
      GBS::PLLAD_KS::write(0); // 5_16 post divider 0 : FCKO1 > 87MHz, 3 : FCKO1<23MHz
      GBS::PLLAD_CKOS::write(0);    // 5_16 1x OS (with KS=CKOS=0)
      GBS::ADC_CLK_ICLK1X::write(0);// 5_00 4 (OS=1)
      GBS::DEC2_BYPS::write(1);     // 5_1f 1 // dec2 disabled (OS=1)
      GBS::PLLAD_ICP::write(5);     // fine at 6 as well, FS is 1
      GBS::PLLAD_FS::write(1);
      GBS::HD_HB_ST::write(0x00); // 1_3B
      GBS::HD_HB_SP::write(0xb0); // 1_3D // d0
      GBS::HD_HS_ST::write(0x10); // 1_3F
      GBS::HD_HS_SP::write(0x70); // 1_41
      GBS::HD_VB_ST::write(0x00); // 1_43
      GBS::HD_VB_SP::write(0x2f); // 1_45
      GBS::HD_VS_ST::write(0x10);  // 1_47
      GBS::HD_VS_SP::write(0x16);  // 1_49
    }
    if (rto->videoStandardInput == 13) { // odd HD mode (PS2 "VGA" over Component)
      applyRGBPatches(); // treat mostly as RGB, clamp R/B to gnd
      rto->syncTypeCsync = true; // used in loop to set clamps and SP dynamic
      GBS::DEC_MATRIX_BYPS::write(1); // overwrite for this mode 
      GBS::SP_PRE_COAST::write(4);
      GBS::SP_POST_COAST::write(4);
      GBS::SP_DLT_REG::write(0x70);
      GBS::HD_MATRIX_BYPS::write(1); // bypass since we'll treat source as RGB
      GBS::HD_DYN_BYPS::write(1); // bypass since we'll treat source as RGB
      GBS::SP_VS_PROC_INV_REG::write(0); // don't invert
      // same as with RGBHV, the ps2 resolution can vary widely
      GBS::PLLAD_KS::write(0); // 5_16 post divider 0 : FCKO1 > 87MHz, 3 : FCKO1<23MHz
      GBS::PLLAD_CKOS::write(0);    // 5_16 1x OS (with KS=CKOS=0)
      GBS::ADC_CLK_ICLK1X::write(0);// 5_00 4 (OS=1)
      GBS::ADC_CLK_ICLK2X::write(0);// 5_00 3 (OS=1)
      GBS::DEC1_BYPS::write(1);     // 5_1f 1 // dec1 disabled (OS=1)
      GBS::DEC2_BYPS::write(1);     // 5_1f 1 // dec2 disabled (OS=1)
      GBS::PLLAD_MD::write(512);    // could try 856
    }
  }

  if (rto->videoStandardInput == 13) {
    // section is missing HD_HSYNC_RST and HD_INI_ST adjusts
    uint16_t vtotal = GBS::STATUS_SYNC_PROC_VTOTAL::read();
    if (vtotal < 532) { // 640x480 or less
      GBS::PLLAD_KS::write(3);
      GBS::PLLAD_FS::write(1);
    }
    else if (vtotal >= 532 && vtotal < 810) { // 800x600, 1024x768
      //GBS::PLLAD_KS::write(3); // just a little too much at 1024x768
      GBS::PLLAD_FS::write(0);
      GBS::PLLAD_KS::write(2);
    }
    else { //if (vtotal > 1058 && vtotal < 1074) { // 1280x1024
      GBS::PLLAD_KS::write(2);
      GBS::PLLAD_FS::write(1);
    }
  }

  rto->outModeHdBypass = 1;
  rto->presetID = 0x21; // bypass flavor 1, used to signal buttons in web ui

  updateSpDynamic(); // !
  GBS::DEC_IDREG_EN::write(1); // 5_1f 7
  GBS::DEC_WEN_MODE::write(1); // 5_1e 7 // 1 keeps ADC phase consistent. around 4 lock positions vs totally random
  rto->phaseSP = 8;
  rto->phaseADC = 24; // fix value // works best with yuv input in tests
  setAndUpdateSogLevel(rto->currentLevelSOG); // also re-latch everything

  // these used to go after the wait, but should be fine here
  GBS::DAC_RGBS_PWDNZ::write(1);   // enable DAC
  GBS::PAD_SYNC_OUT_ENZ::write(0); // enable sync out

  unsigned long timeout = millis();
  while ((!getStatus16SpHsStable()) && (millis() - timeout < 2002)) {
    delay(1);
  }
  while ((getVideoMode() == 0) && (millis() - timeout < 1502)) {
    delay(1);
  }
  while (millis() - timeout < 600) { delay(1); } // minimum delay for pt: 600

  optimizePhaseSP();
  SerialM.println("pass-through on");
}

void bypassModeSwitch_RGBHV() {
  GBS::DAC_RGBS_PWDNZ::write(0);    // disable DAC
  GBS::PAD_SYNC_OUT_ENZ::write(1);  // disable sync out
  
  loadHdBypassSection();
  GBS::ADC_UNUSED_62::write(0x00);      // clear debug view
  GBS::PA_ADC_BYPSZ::write(1);          // enable phase unit ADC
  GBS::PA_SP_BYPSZ::write(1);           // enable phase unit SP
  applyRGBPatches();
  resetDebugPort();
  rto->videoStandardInput = 15;       // making sure
  rto->autoBestHtotalEnabled = false; // not necessary, since VDS is off / bypassed // todo: mode 14 (works anyway)
  rto->clampPositionIsSet = false;
  rto->HPLLState = 0;

  GBS::PLL_CKIS::write(0);    // 0_40 0 //  0: PLL uses OSC clock | 1: PLL uses input clock
  GBS::PLL_DIVBY2Z::write(0); // 0_40 1 // 1= no divider (full clock, ie 27Mhz) 0 = halved clock
  GBS::PLL_ADS::write(0);     // 0_40 3 test:  input clock is from PCLKIN (disconnected, not ADC clock)
  GBS::PLL_MS::write(2);      // 0_40 4-6 select feedback clock (but need to enable tri state!)
  GBS::PAD_TRI_ENZ::write(1); // enable some pad's tri state (they become high-z / inputs), helps noise
  GBS::MEM_PAD_CLK_INVERT::write(0); // helps also
  GBS::PLL648_CONTROL_01::write(0x35);
  GBS::PLL648_CONTROL_03::write(0x00); // 0_43
  GBS::PLL_LEN::write(1);              // 0_43
  
  GBS::DAC_RGBS_ADC2DAC::write(1);
  GBS::OUT_SYNC_SEL::write(1);    // 0_4f 1=sync from HDBypass, 2=sync from SP, (00 = from VDS)

  GBS::SFTRST_HDBYPS_RSTZ::write(1);  // enable
  GBS::HD_INI_ST::write(0);           // needs to be some small value or apparently 0 works
    //GBS::DAC_RGBS_BYPS2DAC::write(1);
    //GBS::OUT_SYNC_SEL::write(2); // 0_4f sync from SP
    //GBS::SFTRST_HDBYPS_RSTZ::write(1); // need HDBypass
    //GBS::SP_HS_LOOP_SEL::write(1); // (5_57_6) // can bypass since HDBypass does sync
  GBS::HD_MATRIX_BYPS::write(1);  // bypass since we'll treat source as RGB
  GBS::HD_DYN_BYPS::write(1);     // bypass since we'll treat source as RGB
    //GBS::HD_SEL_BLK_IN::write(1); // "DVI", not regular

  GBS::PAD_SYNC1_IN_ENZ::write(0); // filter H/V sync input1 (0 = on)
  GBS::PAD_SYNC2_IN_ENZ::write(0); // filter H/V sync input2 (0 = on)
  
  GBS::SP_SOG_P_ATO::write(1);    // 5_20 1 corrects hpw readout and slightly affects sync
  if (rto->syncTypeCsync == false)
  {
    GBS::SP_SOG_SRC_SEL::write(0);  // 5_20 0 | 0: from ADC 1: from hs // use ADC and turn it off = no SOG
    GBS::ADC_SOGEN::write(1); // 5_02 0 ADC SOG // having it 0 drags down the SOG (hsync) input; = 1: need to supress SOG decoding
    GBS::SP_EXT_SYNC_SEL::write(0); // connect HV input ( 5_20 bit 3 )
    GBS::SP_SOG_MODE::write(0); // 5_56 bit 0 // 0: normal, 1: SOG
    GBS::SP_NO_COAST_REG::write(1); // coasting off
    GBS::SP_PRE_COAST::write(0);
    GBS::SP_POST_COAST::write(0);
    GBS::SP_H_PULSE_IGNOR::write(0xff); // cancel out SOG decoding
    GBS::SP_SYNC_BYPS::write(0);    // external H+V sync for decimator (+ sync out) | 1 to mirror in sync, 0 to output processed sync
    GBS::SP_HS_POL_ATO::write(1);   // 5_55 4 auto polarity for retiming
    GBS::SP_VS_POL_ATO::write(1);   // 5_55 6
    GBS::SP_HS_LOOP_SEL::write(1);  // 5_57_6 | 0 enables retiming on SP | 1 to bypass input to HDBYPASS
    GBS::SP_H_PROTECT::write(0);    // 5_3e 4 disable for H/V
    rto->phaseADC = 16;
    rto->phaseSP = 8;
  }
  else
  {
    // todo: SOG SRC can be ADC or HS input pin. HS requires TTL level, ADC can use lower levels
    // HS seems to have issues at low PLL speeds
    // maybe add detection whether ADC Sync is needed
    GBS::SP_SOG_SRC_SEL::write(0); // 5_20 0 | 0: from ADC 1: hs is sog source
    GBS::SP_EXT_SYNC_SEL::write(1); // disconnect HV input
    GBS::ADC_SOGEN::write(1); // 5_02 0 ADC SOG
    GBS::SP_SOG_MODE::write(1); // apparently needs to be off for HS input (on for ADC)
    GBS::SP_NO_COAST_REG::write(0); // coasting on
    GBS::SP_PRE_COAST::write(4);  // 5_38, > 4 can be seen with clamp invert on the lower lines
    GBS::SP_POST_COAST::write(7);
    GBS::SP_SYNC_BYPS::write(0); // use regular sync for decimator (and sync out) path
    GBS::SP_HS_LOOP_SEL::write(1); // 5_57_6 | 0 enables retiming on SP | 1 to bypass input to HDBYPASS
    GBS::SP_H_PROTECT::write(1); // 5_3e 4 enable for SOG
    rto->currentLevelSOG = 24;
    rto->phaseADC = 16;
    rto->phaseSP = 8;
  }
  GBS::SP_CLAMP_MANUAL::write(1); // needs to be 1

  GBS::SP_DIS_SUB_COAST::write(1); // 5_3e 5 
  GBS::SP_HS_PROC_INV_REG::write(0); // 5_56 5
  GBS::SP_VS_PROC_INV_REG::write(0); // 5_56 6
  GBS::PLLAD_KS::write(1); // 0 - 3
  setOverSampleRatio(2, true);    // prepare only = true
  GBS::DEC_MATRIX_BYPS::write(1); // 5_1f with adc to dac mode
  GBS::ADC_FLTR::write(0);        // 5_03 4/5 ADC filter 3=40, 2=70, 1=110, 0=150 Mhz

  GBS::PLLAD_ICP::write(4);
  GBS::PLLAD_FS::write(0); // low gain
  GBS::PLLAD_MD::write(1856); // 1349 perfect for for 1280x+ ; 1856 allows lower res to detect

  // T4R0x2B Bit: 3 (was 0x7) is now: 0xF
  // S0R0x4F (was 0x80) is now: 0xBC
  // 0_43 1a
  // S5R0x2 (was 0x48) is now: 0x54
  // s5s11sb2
  //0x25, // s0_44
  //0x11, // s0_45
  // new: do without running default preset first
  GBS::ADC_TA_05_CTRL::write(0x02); // 5_05 1 // minor SOG clamp effect
  GBS::ADC_TEST_04::write(0x02);    // 5_04
  GBS::ADC_TEST_0C::write(0x12);    // 5_0c 1 4
  GBS::DAC_RGBS_R0ENZ::write(1);
  GBS::DAC_RGBS_G0ENZ::write(1);
  GBS::DAC_RGBS_B0ENZ::write(1);
  GBS::OUT_SYNC_CNTRL::write(1);
  //resetPLL();   // try to avoid this
  resetDigital(); // this will leave 0_46 all 0
  resetSyncProcessor(); // required to initialize SOG status
  delay(2);ResetSDRAM();delay(2);
  resetPLLAD();
  togglePhaseAdjustUnits();
  delay(20);
  GBS::PLLAD_LEN::write(1); // 5_11 1
  GBS::DAC_RGBS_PWDNZ::write(1); // enable DAC
  GBS::PAD_SYNC_OUT_ENZ::write(0); // enable sync out

  // todo: detect if H-PLL parameters fit the source before aligning clocks (5_11 etc)

  setAndLatchPhaseSP(); // different for CSync and pure HV modes
  setAndLatchPhaseADC();
  latchPLLAD();

  if (uopt->enableAutoGain == 1 && adco->r_gain == 0) {
    //SerialM.println("ADC gain: reset");
    GBS::ADC_RGCTRL::write(0x48);
    GBS::ADC_GGCTRL::write(0x48);
    GBS::ADC_BGCTRL::write(0x48);
    GBS::DEC_TEST_ENABLE::write(1);
  }
  else if (uopt->enableAutoGain == 1 && adco->r_gain != 0) {
    /*SerialM.println("ADC gain: keep previous");
    SerialM.print(adco->r_gain, HEX); SerialM.print(" ");
    SerialM.print(adco->g_gain, HEX); SerialM.print(" ");
    SerialM.print(adco->b_gain, HEX); SerialM.println(" ");*/
    GBS::ADC_RGCTRL::write(adco->r_gain);
    GBS::ADC_GGCTRL::write(adco->g_gain);
    GBS::ADC_BGCTRL::write(adco->b_gain);
    GBS::DEC_TEST_ENABLE::write(1);
  }
  else {
    GBS::DEC_TEST_ENABLE::write(0); // no need for decimation test to be enabled
  }

  rto->presetID = 0x22; // bypass flavor 2, used to signal buttons in web ui
  delay(200);
}

void runAutoGain()
{
    uint8_t limit_found = 0;
    uint8_t status00reg = GBS::STATUS_00::read(); // confirm no mode changes happened

    //GBS::DEC_TEST_SEL::write(5);

    //for (uint8_t i = 0; i < 14; i++) {
    //  uint8_t greenValue = GBS::TEST_BUS_2E::read();
    //  if (greenValue >= 0x28 && greenValue <= 0x2f) {  // 0x2c seems to be "highest" (haven't seen 0x2b yet)
    //    if (getStatus16SpHsStable() && (GBS::STATUS_00::read() == status00reg)) {
    //      limit_found++;
    //    }
    //    else return;
    //  }
    //}

    GBS::DEC_TEST_SEL::write(1); // luma and G channel

    for (uint8_t i = 0; i < 20; i++) {
        limit_found = 0;
        uint8_t greenValue = GBS::TEST_BUS_2F::read() & 0x7f;
        uint8_t blueValue = GBS::TEST_BUS_2E::read() & 0x7f;
        if ((greenValue >= 0x7c && greenValue <= 0x7f) || (blueValue >= 0x7c && blueValue <= 0x7f)) {
            for (uint8_t a = 0; a < 2; a++) {
                delayMicroseconds(22);
                greenValue = GBS::TEST_BUS_2F::read() & 0x7f;
                blueValue = GBS::TEST_BUS_2E::read() & 0x7f;
                if ((greenValue >= 0x7c && greenValue <= 0x7f) || (blueValue >= 0x7c && blueValue <= 0x7f)) {
                    if (getStatus16SpHsStable() && (GBS::STATUS_00::read() == status00reg)) {
                        limit_found++;
                    } else
                        return;
                }
            }
            if (limit_found == 2) {
                limit_found = 0;
                uint8_t level = GBS::ADC_GGCTRL::read();
                if (level < 0xff) {
                    GBS::ADC_GGCTRL::write(level + 1);
                    GBS::ADC_RGCTRL::write(level + 1);
                    GBS::ADC_BGCTRL::write(level + 1);

                    // remember these gain settings
                    adco->r_gain = GBS::ADC_RGCTRL::read();
                    adco->g_gain = GBS::ADC_GGCTRL::read();
                    adco->b_gain = GBS::ADC_BGCTRL::read();

                    //printInfo();
                    delayMicroseconds(100); // let it settle a little
                }
            }
        }
    }
}

void enableScanlines() {
  if (GBS::GBS_OPTION_SCANLINES_ENABLED::read() == 0) {
    //SerialM.println("enableScanlines())");

    //GBS::RFF_ADR_ADD_2::write(0);
    //GBS::RFF_REQ_SEL::write(1);
    //GBS::RFF_MASTER_FLAG::write(0x3f);
    //GBS::RFF_WFF_OFFSET::write(0); // scanline fix
    //GBS::RFF_FETCH_NUM::write(0);
    //GBS::RFF_ENABLE::write(1); //GBS::WFF_ENABLE::write(1);
    //delay(10);
    //GBS::RFF_ENABLE::write(0); //GBS::WFF_ENABLE::write(0);

    GBS::MADPT_PD_RAM_BYPS::write(0);
    GBS::RFF_YUV_DEINTERLACE::write(1); // scanline fix 2
    GBS::MADPT_Y_MI_DET_BYPS::write(1); // make sure, so that mixing works
    //GBS::VDS_Y_GAIN::write(GBS::VDS_Y_GAIN::read() + 0x30); // more luma gain
    GBS::VDS_Y_OFST::write(GBS::VDS_Y_OFST::read() + 3);
    GBS::VDS_WLEV_GAIN::write(0x14);
    GBS::VDS_W_LEV_BYPS::write(0); // brightness test
    GBS::MADPT_VIIR_COEF::write(0x14); // set up VIIR filter 2_27
    GBS::MADPT_Y_MI_OFFSET::write(0x28); // 2_0b offset (mixing factor here)
    GBS::MADPT_VIIR_BYPS::write(0); // enable VIIR 
    GBS::RFF_LINE_FLIP::write(1); // clears potential garbage in rff buffer

    GBS::MAPDT_VT_SEL_PRGV::write(0);
    GBS::GBS_OPTION_SCANLINES_ENABLED::write(1);
  }
  rto->scanlinesEnabled = 1;
}

void disableScanlines() {
  if (GBS::GBS_OPTION_SCANLINES_ENABLED::read() == 1) {
    //SerialM.println("disableScanlines())");
    GBS::MAPDT_VT_SEL_PRGV::write(1);
    //GBS::VDS_Y_GAIN::write(GBS::VDS_Y_GAIN::read() - 0x30);
    GBS::VDS_Y_OFST::write(GBS::VDS_Y_OFST::read() - 3);
    GBS::VDS_W_LEV_BYPS::write(1); // brightness test
    GBS::MADPT_Y_MI_OFFSET::write(0xff); // 2_0b offset 0xff disables mixing
    GBS::MADPT_VIIR_BYPS::write(1); // disable VIIR
    GBS::MADPT_PD_RAM_BYPS::write(1);
    GBS::RFF_LINE_FLIP::write(0); // back to default

    GBS::GBS_OPTION_SCANLINES_ENABLED::write(0);
  }
  rto->scanlinesEnabled = 0;
}

void enableMotionAdaptDeinterlace() {
  GBS::DEINT_00::write(0x19);         // 2_00 // bypass angular (else 0x00)
  GBS::MADPT_Y_MI_OFFSET::write(0x00); // 2_0b  // also used for scanline mixing
  //GBS::MADPT_STILL_NOISE_EST_EN::write(1); // 2_0A 5 (was 0 before)
  GBS::MADPT_Y_MI_DET_BYPS::write(0); //2_0a_7  // switch to automatic motion indexing
  //GBS::MADPT_UVDLY_PD_BYPS::write(0); // 2_35_5 // UVDLY
  //GBS::MADPT_EN_UV_DEINT::write(0);   // 2_3a 0
  //GBS::MADPT_EN_STILL_FOR_NRD::write(1); // 2_3a 3 (new)

  //GBS::RFF_WFF_STA_ADDR_A::write(0);
  //GBS::RFF_WFF_STA_ADDR_B::write(1);
  GBS::RFF_ADR_ADD_2::write(1);
  GBS::RFF_REQ_SEL::write(3);
  //GBS::RFF_MASTER_FLAG::write(0x24);  // use preset's value
  //GBS::WFF_SAFE_GUARD::write(0); // 4_42 3
  GBS::RFF_FETCH_NUM::write(0x80); // part of RFF disable fix, could leave 0x80 always otherwise
  GBS::RFF_WFF_OFFSET::write(0x100); // scanline fix
  GBS::RFF_YUV_DEINTERLACE::write(0); // scanline fix 2
  GBS::WFF_FF_STA_INV::write(0); // 4_42_2 // 22.03.19 : turned off // update: only required in PAL?
  //GBS::WFF_LINE_FLIP::write(0); // 4_4a_4 // 22.03.19 : turned off // update: only required in PAL?
  GBS::WFF_ENABLE::write(1); // 4_42 0 // enable before RFF
  GBS::RFF_ENABLE::write(1); // 4_4d 7
  delay(60); // 55 first good
  GBS::MAPDT_VT_SEL_PRGV::write(0);   // 2_16_7
  rto->motionAdaptiveDeinterlaceActive = true;
}

void disableMotionAdaptDeinterlace() {
  GBS::MAPDT_VT_SEL_PRGV::write(1);   // 2_16_7
  GBS::DEINT_00::write(0xff); // 2_00

  GBS::RFF_FETCH_NUM::write(0x1);  // RFF disable fix
  GBS::RFF_WFF_OFFSET::write(0x1); // RFF disable fix
  delay(2);
  GBS::WFF_ENABLE::write(0);
  GBS::RFF_ENABLE::write(0); // can cause mem reset requirement, procedure above should fix it

  //GBS::WFF_ADR_ADD_2::write(0);
  GBS::WFF_FF_STA_INV::write(1); // 22.03.19 : turned off // update: only required in PAL?
  //GBS::WFF_LINE_FLIP::write(1); // 22.03.19 : turned off // update: only required in PAL?
  GBS::MADPT_Y_MI_OFFSET::write(0x7f);
  //GBS::MADPT_STILL_NOISE_EST_EN::write(0); // new
  GBS::MADPT_Y_MI_DET_BYPS::write(1);
  //GBS::MADPT_UVDLY_PD_BYPS::write(1); // 2_35_5
  //GBS::MADPT_EN_UV_DEINT::write(0); // 2_3a 0
  //GBS::MADPT_EN_STILL_FOR_NRD::write(0); // 2_3a 3 (new)
  rto->motionAdaptiveDeinterlaceActive = false;
}

void printInfo() {
  static char print[112]; // 105 + 1 minimum
  uint8_t lockCounter = 0;

  for (uint8_t i = 0; i < 20; i++) {
    if (GBS::STATUS_MISC_PLLAD_LOCK::read() == 1) {
      lockCounter++;
    }
    else {
      delay(1);
    }
  }
  lockCounter = getMovingAverage(lockCounter); // stores first, then updates with average

  int32_t wifi = 0;
  if ((WiFi.status() == WL_CONNECTED) || (WiFi.getMode() == WIFI_AP))
  {
    wifi = WiFi.RSSI();
  }

  uint16_t hperiod = GBS::HPERIOD_IF::read();
  uint16_t vperiod = GBS::VPERIOD_IF::read();

  //int charsToPrint = 
  sprintf(print, "h:%4u v:%4u PLL:%02u A:%02x%02x%02x S:%02x.%02x.%02x I:%02x D:%04x m:%hu ht:%4d vt:%4d hpw:%4d u:%2x s:%2d TF:%04x W:%2d",
    hperiod, vperiod, lockCounter,
    GBS::ADC_RGCTRL::read(), GBS::ADC_GGCTRL::read(), GBS::ADC_BGCTRL::read(),
    GBS::STATUS_00::read(), GBS::STATUS_05::read(), GBS::STATUS_16::read(), 
    GBS::STATUS_0F::read(), GBS::TEST_BUS::read(), getVideoMode(),
    GBS::STATUS_SYNC_PROC_HTOTAL::read(), GBS::STATUS_SYNC_PROC_VTOTAL::read() /*+ 1*/,   // emucrt: without +1 is correct line count 
    GBS::STATUS_SYNC_PROC_HLOW_LEN::read(), rto->noSyncCounter,
    rto->currentLevelSOG, GBS::TEST_FF_STATUS::read(), wifi);

  //SerialM.print("charsToPrint: "); SerialM.println(charsToPrint);
  SerialM.println(print);

  if (rto->webServerEnabled && rto->webServerStarted) {
    if (webSocket.connectedClients() > 0) {
      delay(2); handleWiFi(0); delay(1);
    }
  }
}

void stopWire() {
  pinMode(SCL, INPUT);
  pinMode(SDA, INPUT);
  delayMicroseconds(80);
}

void startWire() {
  Wire.begin();
  // The i2c wire library sets pullup resistors on by default. Disable this so that 5V MCUs aren't trying to drive the 3.3V bus.
#if defined(ESP8266)
  pinMode(SCL, OUTPUT_OPEN_DRAIN);
  pinMode(SDA, OUTPUT_OPEN_DRAIN);
  // no issues at 700k, requires ESP8266 160Mhz CPU clock, else (80Mhz) falls back to 400k via library
  //Wire.setClock(700000);
  Wire.setClock(400000);
#else
  digitalWrite(SCL, LOW);
  digitalWrite(SDA, LOW);
  Wire.setClock(100000);
#endif
  delayMicroseconds(80);
  {
    // run some dummy commands to reinit I2C
    GBS::SP_SOG_MODE::read(); GBS::SP_SOG_MODE::read();
    writeOneByte(0xF0, 0); writeOneByte(0x00, 0); // update cached segment
    GBS::STATUS_00::read();
  }
}

void fastSogAdjust()
{
  if (rto->noSyncCounter <= 5) {
    uint8_t debug_backup = GBS::TEST_BUS_SEL::read();
    uint8_t debug_backup_SP = GBS::TEST_BUS_SP_SEL::read();
    if (debug_backup != 0xa) {
      GBS::TEST_BUS_SEL::write(0xa);
    }
    if (debug_backup_SP != 0x0f) {
      GBS::TEST_BUS_SP_SEL::write(0x0f);
    }

    if ((GBS::TEST_BUS_2F::read() & 0x05) != 0x05) {
      while ((GBS::TEST_BUS_2F::read() & 0x05) != 0x05) {
        if (rto->currentLevelSOG >= 4) {
          rto->currentLevelSOG -= 2;
        }
        else {
          rto->currentLevelSOG = 13;
          setAndUpdateSogLevel(rto->currentLevelSOG);
          delay(40);
          break; // abort / restart next round
        }
        setAndUpdateSogLevel(rto->currentLevelSOG);
        delay(28); // 4
      }
      delay(10);
    }

    if (debug_backup != 0xa) {
      GBS::TEST_BUS_SEL::write(debug_backup);
    }
    if (debug_backup_SP != 0x0f) {
      GBS::TEST_BUS_SP_SEL::write(debug_backup_SP);
    }
  }
  else if (rto->noSyncCounter >= 24 && rto->noSyncCounter <= 27) {
    // reset sog to midscale
    rto->currentLevelSOG = 8;
    setAndUpdateSogLevel(rto->currentLevelSOG);
    delay(20);
  }
}

void runSyncWatcher()
{
  static uint8_t newVideoModeCounter = 0;
  static unsigned long lastSyncDrop = millis();

  uint8_t detectedVideoMode = getVideoMode();
  boolean status16SpHsStable = getStatus16SpHsStable();

  if (rto->videoStandardInput == 13) {  // using flaky graphic modes
    if (detectedVideoMode == 0) {
      if (GBS::STATUS_INT_SOG_BAD::read() == 0) {
        detectedVideoMode = 13;         // then keep it
      }
    }
  }

  static unsigned long preemptiveSogWindowStart = millis();
  static const uint16_t sogWindowLen = 3000; // ms
  static uint8_t badLowLen = 0;

  // look for SOG instability
  if (rto->syncTypeCsync) {
    if (newVideoModeCounter == 0) {
      // start window only if the sog bad interrupt bit is set
      if (GBS::STATUS_INT_SOG_BAD::read() == 1) {
        //Serial.print("^");
        resetInterruptSogBadBit();
        preemptiveSogWindowStart = millis();
      }
    }

    //if (detectedVideoMode == rto->videoStandardInput) {

    if (rto->videoStandardInput == 1 || rto->videoStandardInput == 2) {
      if ((millis() - preemptiveSogWindowStart) < sogWindowLen) {
        //Serial.print("-");
        boolean exit = 0;
        for (uint8_t i = 0; i < 16; i++) {
          if (exit) { break; }
          if (GBS::STATUS_SYNC_PROC_HLOW_LEN::read() < 14) {
            // sog may be bad but need to determine whether the source is even active, too
            uint16_t hlowStart = GBS::STATUS_SYNC_PROC_HLOW_LEN::read();
            for (int a = 0; a < 20; a++) {
              if (GBS::STATUS_SYNC_PROC_HLOW_LEN::read() != hlowStart) {
                // okay, source still there so count this one
                badLowLen++;
                //Serial.print("badLowLen "); Serial.println(badLowLen);
                // and break back to outer for loop
                exit = 1;
                break;
              }
              delay(0);
            }
          }
          delay(0);
        }
      }


      if ((millis() - preemptiveSogWindowStart) < sogWindowLen) {
        if (badLowLen >= 4) {
          if (rto->currentLevelSOG >= 1) {
            rto->currentLevelSOG -= 1;
            setAndUpdateSogLevel(rto->currentLevelSOG);
          }
          else {
            rto->currentLevelSOG = rto->thisSourceMaxLevelSOG;
            setAndUpdateSogLevel(rto->currentLevelSOG);
          }
          badLowLen = 0;
          delay(60);
          rto->phaseIsSet = optimizePhaseSP();
          delay(60);
        }
      }
      else {
        // window expired, reset
        badLowLen = 0;
        // don't set thisSourceMaxLevelSOG here, let it go to 14 again
      }
    }
  }

  if ((detectedVideoMode == 0 || !status16SpHsStable) && rto->videoStandardInput != 15) 
  {
    //freezeVideo();
    rto->noSyncCounter++;
    rto->continousStableCounter = 0;
    rto->phaseIsSet = 0;
    /*if (rto->videoStandardInput == 1 || rto->videoStandardInput == 2) {
      if (rto->syncTypeCsync) {
        fastSogAdjust();
      }
    }*/
    
    if (newVideoModeCounter == 0) {
      LEDOFF; // LEDOFF on sync loss

      if (rto->printInfos == false) {
        static unsigned long timeToLineBreak = millis();
        if (rto->noSyncCounter == 1) {
          if ((millis() - timeToLineBreak) > 3000) { SerialM.print("\n."); timeToLineBreak = millis(); }
          else { SerialM.print("."); }
        }
      }

      if (rto->noSyncCounter == 1) {                 // this usually repeats
        if ((millis() - lastSyncDrop) > 1500) { // minimum space between runs
          updateSpDynamic();
        }
        lastSyncDrop = millis(); // restart timer
      }
    }

    if (rto->inputIsYpBpR && (rto->noSyncCounter == 36 || rto->noSyncCounter == 37)) {
      GBS::SP_NO_CLAMP_REG::write(1); // unlock clamp
      rto->clampPositionIsSet = false;
    }
    if (rto->videoStandardInput != 15) {
      if (rto->noSyncCounter % 40 == 0 || rto->noSyncCounter % 43 == 0) {
        // the * check needs to be first (go before auto sog level) to support SD > HDTV detection
        SerialM.print("*");
        updateSpDynamic();
      }
    }

    if (rto->noSyncCounter == 122) {
      // worst case, sometimes necessary, will be unstable but may at least detect
      rto->currentLevelSOG = 0;
      setAndUpdateSogLevel(rto->currentLevelSOG);
      handleWiFi(0); delay(400);
      detectedVideoMode = getVideoMode();
    }

    // modulo needs to hit 120
    if (rto->noSyncCounter % 60 == 0) {
      SerialM.print("\nno signal\n");
      printInfo();
      updateSpDynamic();
      delay(80);

      // prepare optimizeSogLevel
      // use STATUS_SYNC_PROC_HLOW_LEN changes to determine whether source is still active
      uint16_t hlowStart = GBS::STATUS_SYNC_PROC_HLOW_LEN::read();
      for (int a = 0; a < 128; a++) {
        if (GBS::STATUS_SYNC_PROC_HLOW_LEN::read() != hlowStart) {
          // source still there
          if (rto->noSyncCounter % 120 == 0) {
            rto->currentLevelSOG = 0; // worst case, sometimes necessary, will be unstable but at least detect
            setAndUpdateSogLevel(rto->currentLevelSOG);
          }
          else {
            optimizeSogLevel();
          }
          break;
        }
        delay(0);
      }
    }

    newVideoModeCounter = 0;
    // sog unstable check end
    delay(10);
  }

  // if format changed to valid, potentially new video mode
  if (((detectedVideoMode != 0 && detectedVideoMode != rto->videoStandardInput) ||
    (detectedVideoMode != 0 && rto->videoStandardInput == 0)) &&
    rto->videoStandardInput != 15)
  {
    // before thoroughly checking for a mode change, use delay via newVideoModeCounter
    if (newVideoModeCounter < 255) { 
      newVideoModeCounter++;
      updateSpDynamic();
      if (newVideoModeCounter == 2) {
        freezeVideo();
        rto->continousStableCounter = 0;  // usually already 0, but occasionally not
      }
      if (newVideoModeCounter >= 3) {
        if (rto->coastPositionIsSet) {
          GBS::SP_DIS_SUB_COAST::write(1);  // turn SUB_COAST off to see if the format change is good
          GBS::SP_H_PROTECT::write(0);      // H_PROTECT off
          rto->coastPositionIsSet = 0;
          delay(40);
        }
      }
    }

    if (newVideoModeCounter >= 6)
    {
      SerialM.print("\nFormat change:");
      for (int a = 0; a < 30; a++) {
        if (getVideoMode() == 13) { newVideoModeCounter = 5; } // treat ps2 quasi rgb as stable
        if (getVideoMode() != detectedVideoMode) { newVideoModeCounter = 0; }
      }
      if (newVideoModeCounter != 0) {
        // apply new mode
        SerialM.println(" <stable>");
        Serial.print("Old: "); Serial.print(rto->videoStandardInput);
        Serial.print(" New: "); Serial.println(detectedVideoMode);
        rto->videoIsFrozen = false;

        if (GBS::SP_SOG_MODE::read() == 1) { rto->syncTypeCsync = true; }
        else { rto->syncTypeCsync = false; }
        boolean wantPassThroughMode = uopt->presetPreference == 10;
        if (!wantPassThroughMode)
        {
          // needs to know the sync type for early updateclamp
          applyPresets(detectedVideoMode);
        }
        else
        {
          rto->videoStandardInput = detectedVideoMode;
          setOutModeHdBypass();
        }
        rto->videoStandardInput = detectedVideoMode;
        rto->noSyncCounter = 0;
        rto->continousStableCounter = 0; // also in postloadsteps
        newVideoModeCounter = 0;
        delay(2); // post delay
        badLowLen = 0;
        preemptiveSogWindowStart = millis();
      }
      else {
        unfreezeVideo();  // (whops)
        SerialM.println(" <not stable>");
        for (int i = 0; i < 2; i++) { printInfo(); }
        newVideoModeCounter = 0;
        if (rto->videoStandardInput == 0) {
          // if we got here from standby mode, return there soon
          // but occasionally, this is a regular new mode that needs a SP parameter change to work
          // ie: 1080p needs longer post coast, which the syncwatcher loop applies at some point
          rto->noSyncCounter = 180; // 254 = no sync, give some time in normal loop
        }
      }
    }
  }
  else if (getStatus16SpHsStable() && detectedVideoMode != 0 && rto->videoStandardInput != 15
    && (rto->videoStandardInput == detectedVideoMode))
  { 
    // last used mode reappeared / stable again
    if (rto->continousStableCounter < 255) {
      rto->continousStableCounter++;
      unfreezeVideo();
    }
    rto->noSyncCounter = 0;
    newVideoModeCounter = 0;

    if (rto->continousStableCounter == 4) {
      updateSpDynamic();
    }

    if (rto->continousStableCounter == 5) {
      LEDON;
    }

    if (!rto->phaseIsSet) {
      if (rto->continousStableCounter >= 8) {
        if ((rto->continousStableCounter % 8) == 0) { // will give up at 255
          rto->phaseIsSet = optimizePhaseSP();
        }
      }
    }

    if (rto->continousStableCounter == 160) {
      resetInterruptSogBadBit();
    }

    if (rto->continousStableCounter == 60) {
      GBS::ADC_UNUSED_67::write(0); // clear sync fix temp registers (67/68)
      //rto->coastPositionIsSet = 0; // leads to a flicker
      rto->clampPositionIsSet = 0;  // run updateClampPosition occasionally
    }

    if (rto->continousStableCounter >= 3) {
      if (rto->videoIsFrozen) { unfreezeVideo(); }

      if ((rto->videoStandardInput == 1 || rto->videoStandardInput == 2) &&
        !rto->outModeHdBypass && rto->noSyncCounter == 0)
      {
        // deinterlacer and scanline code
        boolean preventScanlines = 0;
        if (rto->deinterlaceAutoEnabled) {
          if (uopt->deintMode == 0) // else it's BOB which works by not using Motion Adaptive it at all
          {
            uint16_t VPERIOD_IF = GBS::VPERIOD_IF::read();
            static uint8_t filteredLineCountMotionAdaptiveOn = 0, filteredLineCountMotionAdaptiveOff = 0;
            static uint16_t VPERIOD_IF_OLD = VPERIOD_IF; // for glitch filter

            if (VPERIOD_IF_OLD != VPERIOD_IF) {
              //freezeVideo(); // glitch filter
              preventScanlines = 1;
              filteredLineCountMotionAdaptiveOn = 0;
              filteredLineCountMotionAdaptiveOff = 0;
            }
            if (!rto->motionAdaptiveDeinterlaceActive && VPERIOD_IF % 2 == 0) { // ie v:524, even counts > enable
              filteredLineCountMotionAdaptiveOn++;
              filteredLineCountMotionAdaptiveOff = 0;
              if (filteredLineCountMotionAdaptiveOn >= 3) // at least >= 3
              {
                if (GBS::GBS_OPTION_SCANLINES_ENABLED::read() == 1) { // don't rely on rto->scanlinesEnabled
                  disableScanlines();
                }
                enableMotionAdaptDeinterlace();
                preventScanlines = 1;
                filteredLineCountMotionAdaptiveOn = 0;
              }
            }
            else if (rto->motionAdaptiveDeinterlaceActive && VPERIOD_IF % 2 == 1) { // ie v:523, uneven counts > disable
              filteredLineCountMotionAdaptiveOff++;
              filteredLineCountMotionAdaptiveOn = 0;
              if (filteredLineCountMotionAdaptiveOff >= 3) // at least >= 3
              {
                disableMotionAdaptDeinterlace();
                filteredLineCountMotionAdaptiveOff = 0;
              }
            }

            VPERIOD_IF_OLD = VPERIOD_IF; // part of glitch filter
          }
          else {
            // using bob
            if (rto->motionAdaptiveDeinterlaceActive) {
              disableMotionAdaptDeinterlace();
            }
          }
        }

        // scanlines
        if (uopt->wantScanlines) {
          if (!rto->scanlinesEnabled && !rto->motionAdaptiveDeinterlaceActive && !preventScanlines)
          {
            enableScanlines();
          }
          else if (!uopt->wantScanlines && rto->scanlinesEnabled) {
            disableScanlines();
          }
        }

        // the test can't get stability status when the MD glitch filters are too long
        static uint8_t filteredLineCountShouldShiftDown = 0, filteredLineCountShouldShiftUp = 0;
        uint16_t sourceVlines = GBS::STATUS_SYNC_PROC_VTOTAL::read();
        if ((rto->videoStandardInput == 1 && (sourceVlines >= 260 && sourceVlines <= 264)) ||
          (rto->videoStandardInput == 2 && (sourceVlines >= 310 && sourceVlines <= 314)))
        {
          filteredLineCountShouldShiftUp = 0;
          if (GBS::IF_AUTO_OFST_RESERVED_2::read() == 0)
          {
            filteredLineCountShouldShiftDown++;
            if (filteredLineCountShouldShiftDown >= 3) // 2 or more // less = less jaring when action should be done
            {
              //Serial.println("down");
              for (uint8_t a = 0; a <= 5; a++) {
                shiftVerticalDownIF();
              }

              //GBS::SP_SDCS_VSSP_REG_L::write(3);  // 5_4f first
              //GBS::SP_SDCS_VSST_REG_L::write(5);  // 5_3f

              GBS::IF_AUTO_OFST_RESERVED_2::write(1); // mark as adjusted
              filteredLineCountShouldShiftDown = 0;
            }
          }
        }
        else if ((rto->videoStandardInput == 1 && (sourceVlines >= 269 && sourceVlines <= 274)) ||
          (rto->videoStandardInput == 2 && (sourceVlines >= 319 && sourceVlines <= 324)))
        {
          filteredLineCountShouldShiftDown = 0;
          if (GBS::IF_AUTO_OFST_RESERVED_2::read() == 1)
          {
            filteredLineCountShouldShiftUp++;
            if (filteredLineCountShouldShiftUp >= 3) // 2 or more // less = less jaring when action should be done
            {
              //Serial.println("up");
              for (uint8_t a = 0; a <= 5; a++) {
                shiftVerticalUpIF();
              }

              //GBS::SP_SDCS_VSSP_REG_L::write(3);  // 5_4f first
              //GBS::SP_SDCS_VSST_REG_L::write(12); // 5_3f

              GBS::IF_AUTO_OFST_RESERVED_2::write(0); // mark as regular
              filteredLineCountShouldShiftUp = 0;
            }
          }
        }
      }
    }
  }

  if (rto->videoStandardInput >= 14) { // RGBHV checks
    static uint16_t RGBHVNoSyncCounter = 0;

    if (uopt->preferScalingRgbhv)
    {
      // is the source in range for scaling RGBHV and is it currently in mode 15?
      uint16 sourceLines = GBS::STATUS_SYNC_PROC_VTOTAL::read();
      if ((sourceLines <= 535) && rto->videoStandardInput == 15) {
        uint16_t firstDetectedSourceLines = sourceLines;
        boolean moveOn = 1;
        boolean needPostAdjust = 0;
        for (int i = 0; i < 5; i++) { // not the best check, but we don't want to try if this is not stable (usually is though)
          sourceLines = GBS::STATUS_SYNC_PROC_VTOTAL::read();
          // range needed for interlace
          if ((sourceLines < firstDetectedSourceLines - 1) || (sourceLines > firstDetectedSourceLines + 1)) {
            moveOn = 0;
            break;
          }
          delay(10);
        }
        if (moveOn) {
          rto->isValidForScalingRGBHV = true;
          GBS::SP_SOG_MODE::write(0);
          GBS::GBS_OPTION_SCALING_RGBHV::write(1);
          rto->autoBestHtotalEnabled = 1;

          if (sourceLines < 280) { // this is an "NTSC like?" check, seen 277 lines in "512x512 interlaced (emucrt)"
            rto->videoStandardInput = 1;
          }
          else if (sourceLines < 380) { // this is an "PAL like?" check, seen vt:369 (MDA mode)
            rto->videoStandardInput = 2;
          }
          else {
            rto->videoStandardInput = 3;
            needPostAdjust = 1;
          }

          applyPresets(rto->videoStandardInput); // NTSC/PAL (with LineDouble) or 480p as base
          GBS::GBS_OPTION_SCALING_RGBHV::write(1);
          GBS::SP_SOG_P_ATO::write(1);          // 5_20 1 auto SOG polarity (now "hpw" should never be close to "ht")

          // adjust vposition
          GBS::SP_SDCS_VSST_REG_L::write(2); // 5_3f
          GBS::SP_SDCS_VSSP_REG_L::write(0); // 5_40
          setMemoryVblankStartPosition(2);
          setMemoryVblankStopPosition(4);
          GBS::IF_VB_SP::write(8);
          GBS::IF_VB_ST::write(6);

          rto->coastPositionIsSet = rto->clampPositionIsSet = 0;
          rto->videoStandardInput = 14;

          if (needPostAdjust) {
            // base preset was "3" / no line doubling
            // info: actually the position needs to be adjusted based on hor. freq or "h:" value (todo!)
            GBS::IF_HB_ST2::write(0x08); // patches
            GBS::IF_HB_SP2::write(0x68); // image
            GBS::IF_HBIN_SP::write(0x50);// position
          }

          if (GBS::PLLAD_ICP::read() >= 6) {
            GBS::PLLAD_ICP::write(5); // reduce charge pump current for more general use
            latchPLLAD();
          }

          updateSpDynamic();
          if (rto->syncTypeCsync == false)
          {
            GBS::SP_SOG_MODE::write(0);
            GBS::SP_CLAMP_MANUAL::write(1);
            GBS::SP_NO_COAST_REG::write(1);
          }
          else {
            GBS::SP_SOG_MODE::write(1);
            GBS::SP_H_CST_ST::write(0x18);    // 5_4d  // set some default values
            GBS::SP_H_CST_SP::write(0x80);    // will be updated later
            GBS::SP_H_PROTECT::write(1);      // some modes require this (or invert SOG)
          }
          delay(300);
        }
      }
      if ((sourceLines > 535) && rto->videoStandardInput == 14) {
        uint16_t firstDetectedSourceLines = sourceLines;
        boolean moveOn = 1;
        for (int i = 0; i < 10; i++) {
          sourceLines = GBS::STATUS_SYNC_PROC_VTOTAL::read();
          if (sourceLines != firstDetectedSourceLines) {
            moveOn = 0;
            break;
          }
          delay(20);
        }
        if (moveOn) {
          rto->videoStandardInput = 15;
          rto->isValidForScalingRGBHV = false;
          applyPresets(rto->videoStandardInput); // exception: apply preset here, not later in syncwatcher
          delay(300);
        }
      }
    }
    else if (!uopt->preferScalingRgbhv && rto->videoStandardInput == 14) {
      // user toggled the web ui button / revert scaling rgbhv
      rto->videoStandardInput = 15;
      rto->isValidForScalingRGBHV = false;
      applyPresets(rto->videoStandardInput);
      delay(300);
    }

    uint16_t limitNoSync = 0;
    uint8_t VSHSStatus = 0;
    boolean stable = 0;
    if (rto->syncTypeCsync == true) {
      if (GBS::STATUS_INT_SOG_BAD::read() == 1) {
        // STATUS_INT_SOG_BAD = 0x0f bit 0, interrupt reg
        resetModeDetect();
        stable = 0;
        SerialM.print("`");
        delay(10);
        resetInterruptSogBadBit();
      }
      else {
        stable = 1;
        VSHSStatus = GBS::STATUS_00::read();
        // this status can get stuck (regularly does)
        stable = ((VSHSStatus & 0x04) == 0x04); // RGBS > check h+v from 0_00
      }
      limitNoSync = 200; // 100
    }
    else {
      VSHSStatus = GBS::STATUS_16::read();
      // this status usually updates when a source goes off
      stable = ((VSHSStatus & 0x0a) == 0x0a); // RGBHV > check h+v from 0_16
      limitNoSync = 300;
    }

    if (!stable) {
      LEDOFF;
      RGBHVNoSyncCounter++;
      rto->continousStableCounter = 0;
      if (RGBHVNoSyncCounter % 20 == 0) {
        SerialM.print("`");
      }
    }
    else {
      RGBHVNoSyncCounter = 0;
      LEDON;
      if (rto->continousStableCounter < 255) {
        rto->continousStableCounter++;
        if (rto->continousStableCounter == 6) {
          updateSpDynamic();
        }
      }
    }

    if (RGBHVNoSyncCounter > limitNoSync) {
      RGBHVNoSyncCounter = 0;
      setResetParameters();
      prepareSyncProcessor();
      resetSyncProcessor(); // todo: fix MD being stuck in last mode when sync disappears
      //resetModeDetect();
      rto->noSyncCounter = 0;
      //Serial.println("RGBHV limit no sync");
    }

    static unsigned long lastTimeSogAndPllRateCheck = millis();
    if ((millis() - lastTimeSogAndPllRateCheck) > 900)
    {
      if (rto->videoStandardInput != 14) {
        // start out by adjusting sync polarity, may reset sog unstable irq
        updateHVSyncEdge();
        delay(100);
        // next do some stuff, if sync is unstable, the irq generator will have picked it up

        static uint8_t runsWithSogBadStatus = 0;
        static uint8_t oldHPLLState = 0;

        if (rto->syncTypeCsync == false)
        {
          if (GBS::STATUS_INT_SOG_BAD::read()) { // SOG source unstable indicator
            runsWithSogBadStatus++;
            //SerialM.print("test: "); SerialM.println(runsWithSogBadStatus);
            if (runsWithSogBadStatus >= 4) {
              SerialM.println("RGB/HV < > SOG");
              rto->syncTypeCsync = true;
              rto->HPLLState = runsWithSogBadStatus = RGBHVNoSyncCounter = 0;
              rto->noSyncCounter = 254; // will cause a return
            }
          }
          else { runsWithSogBadStatus = 0; }
        }

        uint32_t currentPllRate = 0;
        static uint32_t oldPllRate = 10;

        // how fast is the PLL running? needed to set charge pump and gain
        // typical: currentPllRate: 1560, currentPllRate: 3999 max seen the pll reach: 5008 for 1280x1024@75
        if (GBS::STATUS_INT_SOG_BAD::read() == 0) {
          currentPllRate = getPllRate();
          //Serial.println(currentPllRate);
          if (currentPllRate > 100 && currentPllRate < 7500) {
            if ((currentPllRate < (oldPllRate - 3)) || (currentPllRate > (oldPllRate + 3))) {
              delay(40);
              if (GBS::STATUS_INT_SOG_BAD::read() == 1) delay(100);
              currentPllRate = getPllRate();  // test again, guards against random spurs
              // but don't force currentPllRate to = 0 if these inner checks fail, 
              // prevents csync <> hvsync changes
              if ((currentPllRate < (oldPllRate - 3)) || (currentPllRate > (oldPllRate + 3))) {
                oldPllRate = currentPllRate; // okay, it changed
              }
            }
          }
          else { currentPllRate = 0; }
        }

        resetInterruptSogBadBit();

        //short activeChargePumpLevel = GBS::PLLAD_ICP::read();
        //short activeGainBoost = GBS::PLLAD_FS::read();
        //SerialM.print(" rto->HPLLState: "); SerialM.println(rto->HPLLState);
        //SerialM.print(" currentPllRate: "); SerialM.println(currentPllRate);
        //SerialM.print(" CPL: "); SerialM.print(activeChargePumpLevel);
        //SerialM.print(" Gain: "); SerialM.print(activeGainBoost);
        //SerialM.print(" KS: "); SerialM.print(GBS::PLLAD_KS::read());

        oldHPLLState = rto->HPLLState; // do this first, else it can miss events
        if (currentPllRate != 0)
        {
          if (currentPllRate < 1030) // ~ 970 to 1030 for 15kHz stuff
          {
            if (rto->HPLLState != 1) {
              GBS::PLLAD_KS::write(2);          // KS = 2 okay
              GBS::PLLAD_FS::write(0);
              GBS::PLLAD_ICP::write(6);
              rto->HPLLState = 1;               // check: 640x200@60
            }
          }
          else if (currentPllRate < 2300)       // KS = 1 okay
          {
            if (rto->HPLLState != 2) {
              GBS::PLLAD_KS::write(1);
              GBS::PLLAD_FS::write(0);
              GBS::PLLAD_ICP::write(6);
              rto->HPLLState = 2;               // check: 640x480
            }
          }
          else if (currentPllRate < 3200)
          {
            if (rto->HPLLState != 3) {          // KS = 1 okay
              GBS::PLLAD_KS::write(1);
              GBS::PLLAD_FS::write(1);
              GBS::PLLAD_ICP::write(6); // would need 7 but this is risky
              rto->HPLLState = 3;
            }
          }
          else if (currentPllRate < 3800)
          {
            if (rto->HPLLState != 4) {
              GBS::PLLAD_KS::write(0);        // KS = 0 from here on
              GBS::PLLAD_FS::write(0);
              GBS::PLLAD_ICP::write(6);
              rto->HPLLState = 4;
            }
          }
          else // >= 3800
          {
            if (rto->HPLLState != 5) {
              GBS::PLLAD_KS::write(0);          // KS = 0
              GBS::PLLAD_FS::write(1);
              GBS::PLLAD_ICP::write(6);
              rto->HPLLState = 5;
            }
          }
        }
        if (oldHPLLState != rto->HPLLState) {
          latchPLLAD();
          delay(2);
          setOverSampleRatio(4, false);  // false = do apply // will auto decrease to max possible factor
          SerialM.print("(H-PLL) state: "); SerialM.println(rto->HPLLState);
          delay(100);
        }
      }

      if (rto->videoStandardInput == 14) {
        // scanlines
        if (uopt->wantScanlines) {
          if (!rto->scanlinesEnabled && !rto->motionAdaptiveDeinterlaceActive)
          {
            if (GBS::IF_LD_RAM_BYPS::read() == 0) {   // line doubler on?
              enableScanlines();
            }
          }
          else if (!uopt->wantScanlines && rto->scanlinesEnabled) {
            disableScanlines();
          }
        }
      }

      rto->clampPositionIsSet = false; // RGBHV should regularly check clamp position
      lastTimeSogAndPllRateCheck = millis();
    }
  }

  // couldn't recover, source is lost
  // restore initial conditions and move to input detect
  uint8_t giveUpCount = 254;
  // some modes can return earlier
  if (rto->videoStandardInput == 8) {
    giveUpCount = 127;
  }

  if (rto->noSyncCounter >= giveUpCount) {
    GBS::DAC_RGBS_PWDNZ::write(0); // 0 = disable DAC
    rto->noSyncCounter = 0;
    goLowPowerWithInputDetection(); // does not further nest, so it can be called here // sets reset parameters
  }
}

boolean checkBoardPower()
{
    GBS::ADC_UNUSED_69::write(0x6a); // 0110 1010
    if (GBS::ADC_UNUSED_69::read() == 0x6a) {
        GBS::ADC_UNUSED_69::write(0);
        return 1;
    }

    GBS::ADC_UNUSED_69::write(0); // attempt to clear
    if (rto->boardHasPower == true) {
        Serial.println("! power / i2c lost !");
    }
    rto->boardHasPower = false;
    rto->continousStableCounter = 0;
    rto->syncWatcherEnabled = false;
    return 0;

    //stopWire(); // sets pinmodes SDA, SCL to INPUT
    //uint8_t SCL_SDA = 0;
    //for (int i = 0; i < 2; i++) {
    //  SCL_SDA += digitalRead(SCL);
    //  SCL_SDA += digitalRead(SDA);
    //}

    //if (SCL_SDA != 6)
    //{
    //  if (rto->boardHasPower == true) {
    //    Serial.println("! power / i2c lost !");
    //  }
    //  rto->boardHasPower = false;
    //  rto->continousStableCounter = 0;
    //  rto->syncWatcherEnabled = false;
    //  // I2C stays off and pins are INPUT
    //  return 0;
    //}

    //startWire();
    //return 1;
}

void calibrateAdcOffset()
{
  GBS::PAD_BOUT_EN::write(0);           // disable output to pin for test
  GBS::PLL648_CONTROL_01::write(0xA5);  // display clock to adc = 162mhz
  GBS::ADC_INPUT_SEL::write(2);         // 10 > R2/G2/B2 as input (not connected, so to isolate ADC)
  GBS::DEC_MATRIX_BYPS::write(1);
  GBS::DEC_TEST_ENABLE::write(1);
  GBS::ADC_5_03::write(0x31);           // bottom clamps, filter max (40mhz)
  GBS::ADC_TEST_04::write(0x00);        // disable bit 1
  GBS::SP_CS_CLP_ST::write(0x00);
  GBS::SP_CS_CLP_SP::write(0x00);
  GBS::SP_5_56::write(0x05);            // SP_SOG_MODE needs to be 1
  GBS::SP_5_57::write(0x80);
  GBS::ADC_5_00::write(0x02);
  GBS::TEST_BUS_SEL::write(0x0b);   // 0x2b
  GBS::TEST_BUS_EN::write(1);
  resetDigital();

  uint16_t hitTargetCounter = 0;
  uint16_t readout16 = 0;
  uint8_t missTargetCounter = 0;
  uint8_t readout = 0;

  GBS::ADC_RGCTRL::write(0x7F);
  GBS::ADC_GGCTRL::write(0x7F);
  GBS::ADC_BGCTRL::write(0x7F);
  GBS::ADC_ROFCTRL::write(0x7F);
  GBS::ADC_GOFCTRL::write(0x3D);  // start
  GBS::ADC_BOFCTRL::write(0x7F);
  GBS::DEC_TEST_SEL::write(1);    // 5_1f = 0x1c

  //unsigned long overallTimer = millis();
  unsigned long startTimer = 0;
  for (uint8_t i = 0; i < 3; i++) {
    missTargetCounter = 0; hitTargetCounter = 0;
    delay(20);
    startTimer = millis();

    // loop breaks either when the timer runs out, or hitTargetCounter reaches target
    while ((millis() - startTimer) < 800) {
      readout16 = GBS::TEST_BUS::read() & 0x7fff;
      //Serial.println(readout16, HEX);

      if (readout16 >= 0 && readout16 < 7) {
        hitTargetCounter++;
        missTargetCounter = 0;
      }
      else if (missTargetCounter++ > 2) {
        if (i == 0) {
          GBS::ADC_GOFCTRL::write(GBS::ADC_GOFCTRL::read() + 1); // incr. offset
          readout = GBS::ADC_GOFCTRL::read();
          Serial.print(" G: ");
        }
        else if (i == 1) {
          GBS::ADC_ROFCTRL::write(GBS::ADC_ROFCTRL::read() + 1);
          readout = GBS::ADC_ROFCTRL::read();
          Serial.print(" R: ");
        }
        else if (i == 2) {
          GBS::ADC_BOFCTRL::write(GBS::ADC_BOFCTRL::read() + 1);
          readout = GBS::ADC_BOFCTRL::read();
          Serial.print(" B: ");
        }
        Serial.print(readout, HEX);

        if (readout >= 0x52) {
          // some kind of failure
          break;
        }

        delay(10);
        hitTargetCounter = 0;
        missTargetCounter = 0;
        startTimer = millis(); // extend timer
      }
      if (hitTargetCounter > 1500) {
        break;
      }

    }
    if (i == 0) {
      // G done, prep R
      adco->g_off = GBS::ADC_GOFCTRL::read();
      GBS::ADC_GOFCTRL::write(0x7F);
      GBS::ADC_ROFCTRL::write(0x3D);
      GBS::DEC_TEST_SEL::write(2);    // 5_1f = 0x2c
    }
    if (i == 1) {
      adco->r_off = GBS::ADC_ROFCTRL::read();
      GBS::ADC_ROFCTRL::write(0x7F);
      GBS::ADC_BOFCTRL::write(0x3D);
      GBS::DEC_TEST_SEL::write(3);    // 5_1f = 0x3c
    }
    if (i == 2) {
      adco->b_off = GBS::ADC_BOFCTRL::read();
    }
    Serial.println("");
  }

  if (readout >= 0x52) {
    // there was a problem; revert
    adco->r_off = adco->g_off = adco->b_off = 0x40;
  }

  GBS::ADC_GOFCTRL::write(adco->g_off);
  GBS::ADC_ROFCTRL::write(adco->r_off);
  GBS::ADC_BOFCTRL::write(adco->b_off);

  //Serial.println(millis() - overallTimer);
}

void loadDefaultUserOptions() {
  uopt->presetPreference = 0; // #1
  uopt->enableFrameTimeLock = 0; // permanently adjust frame timing to avoid glitch vertical bar. does not work on all displays!
  uopt->presetSlot = 1; //
  uopt->frameTimeLockMethod = 0; // compatibility with more displays
  uopt->enableAutoGain = 0;
  uopt->wantScanlines = 0;
  uopt->wantOutputComponent = 0;
  uopt->deintMode = 0;
  uopt->wantVdsLineFilter = 1;
  uopt->wantPeaking = 1;
  uopt->preferScalingRgbhv = 1;
  uopt->wantTap6 = 1;
  uopt->PalForce60 = 0;
  uopt->matchPresetSource = 1;  // #14
  uopt->wantStepResponse = 1;   // #15
  uopt->wantFullHeight = 1;     // #16
  uopt->enableCalibrationADC = 1;  // #17
}

//void preinit() {
//  //system_phy_set_powerup_option(3); // 0 = default, use init byte; 3 = full calibr. each boot, extra 200ms
//  system_phy_set_powerup_option(0);
//}

void setup() {
  rto->webServerEnabled = true;
  rto->webServerStarted = false; // make sure this is set

  Serial.begin(115200); // Arduino IDE Serial Monitor requires the same 115200 bauds!
  Serial.setTimeout(10);

  // start web services as early in boot as possible
  WiFi.hostname(device_hostname_partial); // was _full
  if (rto->webServerEnabled) {
    rto->allowUpdatesOTA = false; // need to initialize for handleWiFi()
    WiFi.setSleepMode(WIFI_NONE_SLEEP); // low latency responses, less chance for missing packets
    startWebserver();
    WiFi.setOutputPower(20.0f); // float: min 0.0f, max 20.5f // reduced from max, but still strong
    rto->webServerStarted = true;
  }
  else {
    //WiFi.disconnect(); // deletes credentials
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
  }
#ifdef HAVE_PINGER_LIBRARY
  pingLastTime = millis();
#endif

  SerialM.println("\nstartup");

  loadDefaultUserOptions();
  //globalDelay = 0;

  rto->allowUpdatesOTA = false; // ESP over the air updates. default to off, enable via web interface
  rto->enableDebugPings = false;
  rto->autoBestHtotalEnabled = true;  // automatically find the best horizontal total pixel value for a given input timing
  rto->syncLockFailIgnore = 16; // allow syncLock to fail x-1 times in a row before giving up (sync glitch immunity)
  rto->forceRetime = false;
  rto->syncWatcherEnabled = true;  // continously checks the current sync status. required for normal operation
  rto->phaseADC = 16;
  rto->phaseSP = 8;
  rto->failRetryAttempts = 0;
  rto->presetID = 0;
  rto->HPLLState = 0;
  rto->motionAdaptiveDeinterlaceActive = false;
  rto->deinterlaceAutoEnabled = true;
  rto->scanlinesEnabled = false;
  rto->boardHasPower = true;
  rto->presetIsPalForce60 = false;
  rto->syncTypeCsync = false;
  rto->isValidForScalingRGBHV = false;
  rto->medResLineCount = 0x33; // 51*8=408
  rto->osr = 0;

  // more run time variables
  rto->inputIsYpBpR = false;
  rto->videoStandardInput = 0;
  rto->outModeHdBypass = false;
  rto->videoIsFrozen = false;
  if (!rto->webServerEnabled) rto->webServerStarted = false;
  rto->printInfos = false;
  rto->sourceDisconnected = true;
  rto->isInLowPowerMode = false;
  rto->applyPresetDoneStage = 0;
  rto->presetVlineShift = 0;
  rto->clampPositionIsSet = 0;
  rto->coastPositionIsSet = 0;
  rto->continousStableCounter = 0;
  rto->currentLevelSOG = 5;
  rto->thisSourceMaxLevelSOG = 31; // 31 = auto sog has not (yet) run

  adco->r_gain = 0;
  adco->g_gain = 0;
  adco->b_gain = 0;
  adco->r_off = 0;
  adco->g_off = 0;
  adco->b_off = 0;

  typeOneCommand = '@'; // ASCII @ = 0
  typeTwoCommand = '@';

  pinMode(DEBUG_IN_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  LEDON; // enable the LED, lets users know the board is starting up

  //Serial.setDebugOutput(true); // if you want simple wifi debug info

  unsigned long initDelay1 = millis();
  while (millis() - initDelay1 < 500) {
    handleWiFi(0);
    delay(1);
  }

  // file system (web page, custom presets, ect)
  if (!SPIFFS.begin()) {
    SerialM.println(F("SPIFFS mount failed! ((1M SPIFFS) selected?)"));
  }
  else {
    // load user preferences file
    File f = SPIFFS.open("/preferencesv2.txt", "r");
    if (!f) {
      SerialM.println(F("no preferences file yet, create new"));
      loadDefaultUserOptions();
      saveUserPrefs(); // if this fails, there must be a spiffs problem
    }
    else {
      //on a fresh / spiffs not formatted yet MCU:  userprefs.txt open ok //result = 207
      uopt->presetPreference = (uint8_t)(f.read() - '0'); // #1
      if (uopt->presetPreference > 10) uopt->presetPreference = 0; // fresh spiffs ?

      uopt->enableFrameTimeLock = (uint8_t)(f.read() - '0');
      if (uopt->enableFrameTimeLock > 1) uopt->enableFrameTimeLock = 0;

      uopt->presetSlot = (uint8_t)(f.read() - '0');
      if (uopt->presetSlot > 5) uopt->presetSlot = 1;

      uopt->frameTimeLockMethod = (uint8_t)(f.read() - '0');
      if (uopt->frameTimeLockMethod > 1) uopt->frameTimeLockMethod = 0;

      uopt->enableAutoGain = (uint8_t)(f.read() - '0');
      if (uopt->enableAutoGain > 1) uopt->enableAutoGain = 0;

      uopt->wantScanlines = (uint8_t)(f.read() - '0');
      if (uopt->wantScanlines > 1) uopt->wantScanlines = 0;

      uopt->wantOutputComponent = (uint8_t)(f.read() - '0');
      if (uopt->wantOutputComponent > 1) uopt->wantOutputComponent = 0;

      uopt->deintMode = (uint8_t)(f.read() - '0');
      if (uopt->deintMode > 2) uopt->deintMode = 0;
      
      uopt->wantVdsLineFilter = (uint8_t)(f.read() - '0');
      if (uopt->wantVdsLineFilter > 1) uopt->wantVdsLineFilter = 1;

      uopt->wantPeaking = (uint8_t)(f.read() - '0');
      if (uopt->wantPeaking > 1) uopt->wantPeaking = 1;

      uopt->preferScalingRgbhv = (uint8_t)(f.read() - '0');
      if (uopt->preferScalingRgbhv > 1) uopt->preferScalingRgbhv = 1;

      uopt->wantTap6 = (uint8_t)(f.read() - '0');
      if (uopt->wantTap6 > 1) uopt->wantTap6 = 1;

      uopt->PalForce60 = (uint8_t)(f.read() - '0');
      if (uopt->PalForce60 > 1) uopt->PalForce60 = 0;
      
      uopt->matchPresetSource = (uint8_t)(f.read() - '0'); // #14
      if (uopt->matchPresetSource > 1) uopt->matchPresetSource = 1;

      uopt->wantStepResponse = (uint8_t)(f.read() - '0'); // #15
      if (uopt->wantStepResponse > 1) uopt->wantStepResponse = 1;

      uopt->wantFullHeight = (uint8_t)(f.read() - '0'); // #16
      if (uopt->wantFullHeight > 1) uopt->wantFullHeight = 1;

      uopt->enableCalibrationADC = (uint8_t)(f.read() - '0'); // #17
      if (uopt->enableCalibrationADC > 1) uopt->enableCalibrationADC = 1;

      f.close();
    }
  }
  
  unsigned long initDelay2 = millis();
  while (millis() - initDelay2 < 1000) {
    handleWiFi(0);
    delay(1);
  }

  if (WiFi.status() == WL_CONNECTED) {
    // nothing
  }
  else if (WiFi.SSID().length() == 0) {
    SerialM.println(FPSTR(ap_info_string));
  }
  else {
    SerialM.println(F("(WiFi): still connecting.."));
  }

  startWire();
  boolean powerOrWireIssue = 0;  
  if ( !checkBoardPower() )
  {
    stopWire(); // sets pinmodes SDA, SCL to INPUT
    for (int i = 0; i < 40; i++) {
      // I2C SDA probably stuck, attempt recovery (max attempts in tests was around 10)
      startWire();
      digitalWrite(SCL, 0); delayMicroseconds(12);
      stopWire();
      if (digitalRead(SDA) == 1) { break; } // unstuck
      if ((i % 7) == 0) { delay(1); }
    }

    startWire();

    if (!checkBoardPower()) {
      stopWire();
      powerOrWireIssue = 1; // fail
      rto->boardHasPower = false;
      rto->syncWatcherEnabled = false;
    }
    else { // recover success
      rto->syncWatcherEnabled = true;
      rto->boardHasPower = true;
      SerialM.println("recovered");
    }
  }

  if (powerOrWireIssue == 0)
  {
    zeroAll();
    setResetParameters();
    prepareSyncProcessor();

    uint8_t productId = GBS::CHIP_ID_PRODUCT::read();
    uint8_t revisionId = GBS::CHIP_ID_REVISION::read();
    SerialM.print("Chip ID: "); 
    SerialM.print(productId,HEX); 
    SerialM.print(" ");
    SerialM.println(revisionId,HEX);

    if (uopt->enableCalibrationADC) {
      // enabled by default
      calibrateAdcOffset();
    }
    setResetParameters();

    delay(4); // help wifi (presets are unloaded now)
    handleWiFi(1);
    delay(4);
    //rto->syncWatcherEnabled = false; // allows passive operation by disabling syncwatcher here
    //inputAndSyncDetect();
    //if (rto->syncWatcherEnabled == true) {
    //  rto->isInLowPowerMode = true; // just for initial detection; simplifies code later
    //  for (uint8_t i = 0; i < 3; i++) {
    //    if (inputAndSyncDetect()) {
    //      break;
    //    }
    //  }
    //  rto->isInLowPowerMode = false;
    //}
  }
  else {
    SerialM.println(F("Please check board power and cabling or restart!"));
  }
  
  LEDOFF; // LED behaviour: only light LED on active sync

  // some debug tools leave garbage in the serial rx buffer 
  if (Serial.available()) {
    discardSerialRxData();
  }
}

#ifdef HAVE_BUTTONS
#define INPUT_SHIFT 0
#define DOWN_SHIFT 1
#define UP_SHIFT 2
#define MENU_SHIFT 3

static const uint8_t historySize = 32;
static const uint16_t buttonPollInterval = 100; // microseconds
static uint8_t buttonHistory[historySize];
static uint8_t buttonIndex;
static uint8_t buttonState;
static uint8_t buttonChanged;

uint8_t readButtons(void) {
  return ~((digitalRead(INPUT_PIN) << INPUT_SHIFT) |
    (digitalRead(DOWN_PIN) << DOWN_SHIFT) |
    (digitalRead(UP_PIN) << UP_SHIFT) |
    (digitalRead(MENU_PIN) << MENU_SHIFT));
}

void debounceButtons(void) {
  buttonHistory[buttonIndex++ % historySize] = readButtons();
  buttonChanged = 0xFF;
  for (uint8_t i = 0; i < historySize; ++i)
    buttonChanged &= buttonState ^ buttonHistory[i];
  buttonState ^= buttonChanged;
}

bool buttonDown(uint8_t pos) {
  return (buttonState & (1 << pos)) && (buttonChanged & (1 << pos));
}

void handleButtons(void) {
  debounceButtons();
  if (buttonDown(INPUT_SHIFT))
    Menu::run(MenuInput::BACK);
  if (buttonDown(DOWN_SHIFT))
    Menu::run(MenuInput::DOWN);
  if (buttonDown(UP_SHIFT))
    Menu::run(MenuInput::UP);
  if (buttonDown(MENU_SHIFT))
    Menu::run(MenuInput::FORWARD);
}
#endif

void discardSerialRxData() {
  uint16_t maxThrowAway = 0x1fff;
  while (Serial.available() && maxThrowAway > 0) {
    Serial.read();
    maxThrowAway--;
  }
}

void updateWebSocketData() {
  if (rto->webServerEnabled && rto->webServerStarted) {
    if (webSocket.connectedClients() > 0) {

      char toSend[7] = { 0 };
      toSend[0] = '#'; // makeshift ping in slot 0

      switch (rto->presetID) {
      case 0x01:
      case 0x11:
        toSend[1] = '1';
        break;
      case 0x02:
      case 0x12:
        toSend[1] = '2';
        break;
      case 0x03:
      case 0x13:
        toSend[1] = '3';
        break;
      case 0x04:
      case 0x14:
        toSend[1] = '4';
        break;
      case 0x05:
      case 0x15:
        toSend[1] = '5';
        break;
      case 0x09: // custom
        toSend[1] = '9';
        break;
      case 0x21: // bypass 1
      case 0x22: // bypass 2
        toSend[1] = '8';
        break;
      default:
        toSend[1] = '0';
        break;
      }

      switch (uopt->presetSlot) {
      case 1:
        toSend[2] = '1';
        break;
      case 2:
        toSend[2] = '2';
        break;
      case 3:
        toSend[2] = '3';
        break;
      case 4:
        toSend[2] = '4';
        break;
      case 5:
        toSend[2] = '5';
        break;
      default:
        toSend[2] = '1';
        break;
      }

      // '@' = 0x40, used for "byte is present" detection; 0x80 not in ascii table
      toSend[3] = '@';
      toSend[4] = '@';
      toSend[5] = '@';

      if (uopt->enableAutoGain) { toSend[3] |= (1 << 0); }
      if (uopt->wantScanlines) { toSend[3] |= (1 << 1); }
      if (uopt->wantVdsLineFilter) { toSend[3] |= (1 << 2); }
      if (uopt->wantPeaking) { toSend[3] |= (1 << 3); }
      if (uopt->PalForce60) { toSend[3] |= (1 << 4); }
      if (uopt->wantOutputComponent) { toSend[3] |= (1 << 5); }

      if (uopt->matchPresetSource) { toSend[4] |= (1 << 0); }
      if (uopt->enableFrameTimeLock) { toSend[4] |= (1 << 1); }
      if (uopt->deintMode) { toSend[4] |= (1 << 2); }
      if (uopt->wantTap6) { toSend[4] |= (1 << 3); }
      if (uopt->wantStepResponse) { toSend[4] |= (1 << 4); }
      if (uopt->wantFullHeight) { toSend[4] |= (1 << 5); }

      if (uopt->enableCalibrationADC) { toSend[5] |= (1 << 0); }
      if (uopt->preferScalingRgbhv) { toSend[5] |= (1 << 1); }

      // send ping and stats
      if (ESP.getFreeHeap() > 14000) {
        webSocket.broadcastTXT(toSend);
      }
      else {
        webSocket.disconnect();
      }
    }

  }
}

void handleWiFi(boolean instant) {
  static unsigned long lastTimePing = millis();
  yield();
#if defined(ESP8266)
  if (rto->webServerEnabled && rto->webServerStarted) {
    MDNS.update();
    persWM.handleWiFi(); // if connected, returns instantly. otherwise it reconnects or opens AP
    dnsServer.processNextRequest();

    //webSocket.loop(); // checks _runnning internally, skips all work if not

    if ((millis() - lastTimePing > 973) || instant) { // slightly odd value so not everything happens at once
      //if (webSocket.connectedClients(true) > 0) { // true = with compliant ping
        webSocket.broadcastPing();
        updateWebSocketData();
        delay(1);
      //}
      lastTimePing = millis();
    }
  }

  if (rto->allowUpdatesOTA) {
    ArduinoOTA.handle();
  }
  yield();
#endif
}

void loop() {
  static uint8_t readout = 0;
  static uint8_t segmentCurrent = 255;
  static uint8_t registerCurrent = 255;
  static uint8_t inputToogleBit = 0;
  static uint8_t inputStage = 0;
  static unsigned long lastTimeSyncWatcher = millis();
  static unsigned long lastVsyncLock = millis();
  static unsigned long lastTimeSourceCheck = 500; // 500 to start right away (after setup it will be 2790ms when we get here)
  static unsigned long lastTimeAutoGain = millis();
  static unsigned long lastTimeInterruptClear = millis();
#ifdef HAVE_BUTTONS
  static unsigned long lastButton = micros();
#endif

  handleWiFi(0); // ESP8266 check, WiFi + OTA updates, checks for server enabled + started

#ifdef HAVE_BUTTONS
  if (micros() - lastButton > buttonPollInterval) {
    lastButton = micros();
    handleButtons();
  }
#endif

  // is there a command from Terminal or web ui?
  // Serial takes precedence
  if (Serial.available()) {
    typeOneCommand = Serial.read();
  }
  else if (inputStage > 0) {
    // multistage with no more data
    SerialM.println(" abort");
    discardSerialRxData();
    typeOneCommand = ' ';
  }
  if (typeOneCommand != '@') 
  {
    // multistage with bad characters?
    if (inputStage > 0) {
      // need 's', 't' or 'g'
      if (typeOneCommand != 's' && typeOneCommand != 't' && typeOneCommand != 'g') {
        discardSerialRxData();
        SerialM.println(" abort");
        typeOneCommand = ' ';
      }
    }
    switch (typeOneCommand) {
    case ' ':
      // skip spaces
      inputStage = segmentCurrent = registerCurrent = 0; // and reset these
    break;
    case 'd':
    {
      // check for vertical adjust and undo if necessary
      if (GBS::IF_AUTO_OFST_RESERVED_2::read() == 1)
      {
        //GBS::VDS_VB_ST::write(GBS::VDS_VB_ST::read() - rto->presetVlineShift);
        //GBS::VDS_VB_SP::write(GBS::VDS_VB_SP::read() - rto->presetVlineShift);
        for (uint8_t a = 0; a <= 5; a++) {
          shiftVerticalUpIF();
        }
        GBS::IF_AUTO_OFST_RESERVED_2::write(0);
      }
      // don't store scanlines
      if (GBS::GBS_OPTION_SCANLINES_ENABLED::read() == 1) {
        disableScanlines();
      }
      // pal forced 60hz: no need to revert here. let the voffset function handle it

      // dump
      for (int segment = 0; segment <= 5; segment++) {
        dumpRegisters(segment);
      }
      SerialM.println("};");
    }
    break;
    case '+':
      SerialM.println("hor. +");
      shiftHorizontalRight();
      //shiftHorizontalRightIF(4);
    break;
    case '-':
      SerialM.println("hor. -");
      shiftHorizontalLeft();
      //shiftHorizontalLeftIF(4);
    break;
    case '*':
      shiftVerticalUpIF();
    break;
    case '/':
      shiftVerticalDownIF();
    break;
    case 'z':
      SerialM.println("scale+");
      scaleHorizontal(2, true);
    break;
    case 'h':
      SerialM.println("scale-");
      scaleHorizontal(2, false);
    break;
    case 'q':
      resetDigital(); delay(2);
      ResetSDRAM(); delay(2);
      togglePhaseAdjustUnits();
      //enableVDS();
    break;
    case 'D':
      SerialM.print("debug view: ");
      if (GBS::ADC_UNUSED_62::read() == 0x00) { // "remembers" debug view 
        //if (uopt->wantPeaking == 0) { GBS::VDS_PK_Y_H_BYPS::write(0); } // 3_4e 0 // enable peaking but don't store
        GBS::VDS_PK_LB_GAIN::write(0x3f); // 3_45
        GBS::VDS_PK_LH_GAIN::write(0x3f); // 3_47
        GBS::ADC_UNUSED_60::write(GBS::VDS_Y_OFST::read()); // backup
        GBS::ADC_UNUSED_61::write(GBS::HD_Y_OFFSET::read());
        GBS::ADC_UNUSED_62::write(1); // remember to remove on restore
        GBS::VDS_Y_OFST::write(GBS::VDS_Y_OFST::read() + 0x24);
        GBS::HD_Y_OFFSET::write(GBS::HD_Y_OFFSET::read() + 0x24);
        if (!rto->inputIsYpBpR) {
          // RGB input that has HD_DYN bypassed, use it now
          GBS::HD_DYN_BYPS::write(0);
          GBS::HD_U_OFFSET::write(GBS::HD_U_OFFSET::read() + 0x24);
          GBS::HD_V_OFFSET::write(GBS::HD_V_OFFSET::read() + 0x24);
        }
        //GBS::IF_IN_DREG_BYPS::write(1); // enhances noise from not delaying IF processing properly
        SerialM.println("on");
      }
      else {
        //if (uopt->wantPeaking == 0) { GBS::VDS_PK_Y_H_BYPS::write(1); } // 3_4e 0
        GBS::VDS_PK_LB_GAIN::write(0x16); // 3_45
        GBS::VDS_PK_LH_GAIN::write(0x18); // 3_47
        GBS::VDS_Y_OFST::write(GBS::ADC_UNUSED_60::read()); // restore
        GBS::HD_Y_OFFSET::write(GBS::ADC_UNUSED_61::read());
        if (!rto->inputIsYpBpR) {
          // RGB input, HD_DYN_BYPS again
          GBS::HD_DYN_BYPS::write(1);
          GBS::HD_U_OFFSET::write(0); // probably just 0 by default
          GBS::HD_V_OFFSET::write(0); // probably just 0 by default
        }
        //GBS::IF_IN_DREG_BYPS::write(0);
        GBS::ADC_UNUSED_60::write(0); // .. and clear
        GBS::ADC_UNUSED_61::write(0);
        GBS::ADC_UNUSED_62::write(0);
        SerialM.println("off");
      }
    break;
    case 'C':
      SerialM.println("PLL: ICLK");
      GBS::PLL_MS::write(0); // required again (108Mhz)
      //GBS::MEM_ADR_DLY_REG::write(0x03); GBS::MEM_CLK_DLY_REG::write(0x03); // memory subtimings
      GBS::PLLAD_FS::write(1); // gain high
      GBS::PLLAD_ICP::write(3); // CPC was 5, but MD works with as low as 0 and it removes a glitch
      GBS::PLL_CKIS::write(1); // PLL use ICLK (instead of oscillator)
      latchPLLAD();
      GBS::VDS_HSCALE::write(512);
      rto->syncLockFailIgnore = 16;
      //FrameSync::reset(); // adjust htotal to new display clock
      //rto->forceRetime = true;
      applyBestHTotal(FrameSync::init()); // adjust htotal to new display clock
      applyBestHTotal(FrameSync::init()); // twice
      //GBS::VDS_FLOCK_EN::write(1); //risky
      delay(100);
    break;
    case 'Y':
      writeProgramArrayNew(ntsc_1280x720, false);
      doPostPresetLoadSteps();
    break;
    case 'y':
      writeProgramArrayNew(pal_1280x720, false);
      doPostPresetLoadSteps();
    break;
    case 'P':
      SerialM.print("auto deinterlace: ");
      rto->deinterlaceAutoEnabled = !rto->deinterlaceAutoEnabled;
      if (rto->deinterlaceAutoEnabled) {
        SerialM.println("on");
      }
      else {
        SerialM.println("off");
      }
    break;
    case 'p':
      if (!rto->motionAdaptiveDeinterlaceActive) {
        if (GBS::GBS_OPTION_SCANLINES_ENABLED::read() == 1) { // don't rely on rto->scanlinesEnabled
          disableScanlines();
        }
        enableMotionAdaptDeinterlace();
      }
      else {
        disableMotionAdaptDeinterlace();
      }
    break;
    case 'k':
      bypassModeSwitch_RGBHV();
    break;
    case 'K':
      setOutModeHdBypass();
      uopt->presetPreference = 10;
      saveUserPrefs();
    break;
    case 'T':
      SerialM.print("auto gain ");
      if (uopt->enableAutoGain == 0) {
        uopt->enableAutoGain = 1;
        if (!rto->outModeHdBypass) { // no readout possible
          GBS::ADC_RGCTRL::write(0x48);
          GBS::ADC_GGCTRL::write(0x48);
          GBS::ADC_BGCTRL::write(0x48);
          GBS::DEC_TEST_ENABLE::write(1);
        }
        SerialM.println("on");
      }
      else {
        uopt->enableAutoGain = 0;
        GBS::DEC_TEST_ENABLE::write(0);
        SerialM.println("off");
      }
      saveUserPrefs();
    break;
    case 'e':
      writeProgramArrayNew(ntsc_240p, false);
      doPostPresetLoadSteps();
    break;
    case 'r':
      writeProgramArrayNew(pal_240p, false);
      doPostPresetLoadSteps();
    break;
    case '.':
    {
      // bestHtotal recalc
      rto->autoBestHtotalEnabled = true;
      rto->syncLockFailIgnore = 16;
      rto->forceRetime = true;
      FrameSync::reset();

      if (!rto->syncWatcherEnabled) {
        boolean autoBestHtotalSuccess = 0;
        delay(30);
        autoBestHtotalSuccess = runAutoBestHTotal();
        if (!autoBestHtotalSuccess) {
          SerialM.println(F("(unchanged)"));
        }
      }
    }
    break;
    case '!':
      //fastGetBestHtotal();
      //readEeprom();
      Serial.println(getPllRate());
    break;
    case '$':
      {
      // EEPROM write protect pin (7, next to Vcc) is under original MCU control
      // MCU drags to Vcc to write, EEPROM drags to Gnd normally
      // This routine only works with that "WP" pin disconnected
      // 0x17 = input selector? // 0x18 = input selector related?
      // 0x54 = coast start 0x55 = coast end
      uint16_t writeAddr = 0x54;
      const uint8_t eepromAddr = 0x50;
      for (; writeAddr < 0x56; writeAddr++)
      {
        Wire.beginTransmission(eepromAddr);
        Wire.write(writeAddr >> 8); // high addr byte, 4 bits +
        Wire.write((uint8_t)writeAddr); // mlow addr byte, 8 bits = 12 bits (0xfff max)
        Wire.write(0x10); // coast end value ?
        Wire.endTransmission();
        delay(5);
      }

      //Wire.beginTransmission(eepromAddr);
      //Wire.write((uint8_t)0); Wire.write((uint8_t)0);
      //Wire.write(0xff); // force eeprom clear probably
      //Wire.endTransmission();
      //delay(5);

      Serial.println("done");
      }
    break;
    case 'j':
      //resetPLL();
      latchPLLAD();
    break;
    case 'J':
      resetPLLAD();
    break;
    case 'v':
      rto->phaseSP += 1; rto->phaseSP &= 0x1f;
      SerialM.print("SP: "); SerialM.println(rto->phaseSP);
      setAndLatchPhaseSP();
      //setAndLatchPhaseADC();
    break;
    case 'b':
      advancePhase(); latchPLLAD();
      SerialM.print("ADC: "); SerialM.println(rto->phaseADC);
    break;
    case '#':
      rto->videoStandardInput = 13;
      applyPresets(13);
      //Serial.println(getStatus00IfHsVsStable());
      //globalDelay++;
      //SerialM.println(globalDelay);
    break;
    case 'n':
    {
      uint16_t pll_divider = GBS::PLLAD_MD::read();
      if (pll_divider < 4095) {
        pll_divider += 1;
        GBS::PLLAD_MD::write(pll_divider);
        if (!rto->outModeHdBypass) {
          uint16_t newHT = (GBS::PLLAD_MD::read() >> 1) + 1;
          GBS::IF_HSYNC_RST::write(newHT); // 1_0e
          GBS::IF_LINE_SP::write(newHT + 1); // 1_22
          
          //GBS::IF_INI_ST::write(newHT * 0.68f); // fixed to 0 now

          // s1s03sff s1s04sff s1s05sff s1s06sff s1s07sff s1s08sff s1s09sff s1s0asff s1s0bs4f
          // s1s03s00 s1s04s00 s1s05s00 s1s06s00 s1s07s00 s1s08s00 s1s09s00 s1s0as00 s1s0bs50
          // when using nonlinear scale then remember to zero 1_02 bit 3 (IF_HS_PSHIFT_BYPS)
        }
        latchPLLAD();
        //applyBestHTotal(GBS::VDS_HSYNC_RST::read());
        SerialM.print("PLL div: "); SerialM.println(pll_divider, HEX);
        rto->clampPositionIsSet = 0;
        rto->coastPositionIsSet = 0;
        rto->continousStableCounter = 1; // necessary for clamp test
      }
    }
    break;
    case 'N':
    {
      //if (GBS::RFF_ENABLE::read()) {
      //  disableMotionAdaptDeinterlace();
      //}
      //else {
      //  enableMotionAdaptDeinterlace();
      //}

      //GBS::RFF_ENABLE::write(!GBS::RFF_ENABLE::read());

      if (rto->scanlinesEnabled) {
        rto->scanlinesEnabled = false;
        disableScanlines();
      }
      else {
        rto->scanlinesEnabled = true;
        enableScanlines();
      }
    }
    break;
    case 'a':
      SerialM.print("HTotal++: "); SerialM.println(GBS::VDS_HSYNC_RST::read());
      if (GBS::VDS_HSYNC_RST::read() < 4095) {
        applyBestHTotal(GBS::VDS_HSYNC_RST::read() + 1);
      }
    break;
    case 'A':
      SerialM.print("HTotal--: "); SerialM.println(GBS::VDS_HSYNC_RST::read());
      if (GBS::VDS_HSYNC_RST::read() > 0) {
        applyBestHTotal(GBS::VDS_HSYNC_RST::read() - 1);
      }
    break;
    case 'M':
    {
      /*for (int a = 0; a < 10000; a++) {
        GBS::VERYWIDEDUMMYREG::read();
      }*/

      calibrateAdcOffset();

      //optimizeSogLevel();
      /*rto->clampPositionIsSet = false;
      rto->coastPositionIsSet = false;
      updateClampPosition();
      updateCoastPosition();*/
    }
    break;
    case 'm':
      SerialM.print("syncwatcher ");
      if (rto->syncWatcherEnabled == true) {
        rto->syncWatcherEnabled = false;
        if (rto->videoIsFrozen) { unfreezeVideo(); }
        SerialM.println("off");
      }
      else {
        rto->syncWatcherEnabled = true;
        SerialM.println("on");
      }
    break;
    case ',':
      getVideoTimings();
    break;
    case 'i':
      rto->printInfos = !rto->printInfos;
    break;
    case 'c':
      SerialM.println("OTA Updates on");
      initUpdateOTA();
      rto->allowUpdatesOTA = true;
    break;
    case 'G':
      SerialM.print("Debug Pings ");
      if (!rto->enableDebugPings) {
        SerialM.println("on");
        rto->enableDebugPings = 1;
      }
      else {
        SerialM.println("off");
        rto->enableDebugPings = 0;
      }
    break;
    case 'u':
      ResetSDRAM();
    break;
    case 'f':
      SerialM.print("peaking ");
      if (uopt->wantPeaking == 0) {
        uopt->wantPeaking = 1;
        GBS::VDS_PK_Y_H_BYPS::write(0);
        SerialM.println("on");
      }
      else {
        uopt->wantPeaking = 0;
        GBS::VDS_PK_Y_H_BYPS::write(1);
        SerialM.println("off");
      }
      saveUserPrefs();
    break;
    case 'F':
      SerialM.print("ADC filter ");
      if (GBS::ADC_FLTR::read() > 0) {
        GBS::ADC_FLTR::write(0);
        SerialM.println("off");
      }
      else {
        GBS::ADC_FLTR::write(3);
        SerialM.println("on");
      }
    break;
    case 'L':
    {
      // Component / VGA Output
      uopt->wantOutputComponent = !uopt->wantOutputComponent;
      OutputComponentOrVGA();
      saveUserPrefs();
      // apply 1280x720 preset now, otherwise a reboot would be required
      uint8_t videoMode = getVideoMode();
      if (videoMode == 0) videoMode = rto->videoStandardInput;
      uint8_t backup = uopt->presetPreference;
      uopt->presetPreference = 3;
      rto->videoStandardInput = 0; // force hard reset
      applyPresets(videoMode);
      uopt->presetPreference = backup;
    }
    break;
    case 'l':
      SerialM.println("resetSyncProcessor");
      //freezeVideo();
      resetSyncProcessor();
      //delay(10);
      //unfreezeVideo();
    break;
    case 'Z':
    {
      uopt->matchPresetSource = !uopt->matchPresetSource;
      saveUserPrefs();
    }
    break;
    case 'W':
      uopt->enableFrameTimeLock = !uopt->enableFrameTimeLock;
    break;
    case 'E':
      writeProgramArrayNew(ntsc_1280x1024, false);
      doPostPresetLoadSteps();
    break;
    case 'R':
      writeProgramArrayNew(pal_1280x1024, false);
      doPostPresetLoadSteps();
    break;
    case '0':
      moveHS(4, true);
    break;
    case '1':
      moveHS(4, false);
    break;
    case '2':
      writeProgramArrayNew(pal_feedbackclock, false); // ModeLine "720x576@50" 27 720 732 795 864 576 581 586 625 -hsync -vsync
      doPostPresetLoadSteps();
    break;
    case '3':
      //
    break;
    case '4':
      scaleVertical(2, true);
    break;
    case '5':
      scaleVertical(2, false);
    break;
    case '6':
      if (rto->videoStandardInput == 3 || rto->videoStandardInput == 4) {
        shiftHorizontalRight(); // use VDS mem move for EDTV presets
      }
      else {
        if (GBS::IF_HBIN_SP::read() >= 10) { // IF_HBIN_SP: min 2
          GBS::IF_HBIN_SP::write(GBS::IF_HBIN_SP::read() - 8); // canvas move right
        }
        else {
          SerialM.println("limit");
        }
      }
    break;
    case '7':
      if (rto->videoStandardInput == 3 || rto->videoStandardInput == 4) {
        shiftHorizontalLeft();
      }
      else {
        GBS::IF_HBIN_SP::write(GBS::IF_HBIN_SP::read() + 8); // canvas move left
      }
    break;
    case '8':
      //SerialM.println("invert sync");
      invertHS(); invertVS();
      //optimizePhaseSP();
    break;
    case '9':
      writeProgramArrayNew(ntsc_feedbackclock, false);
      doPostPresetLoadSteps();
    break;
    case 'o':
    {
      if (rto->osr == 1) {
        setOverSampleRatio(2, false);
      }
      else if (rto->osr == 2) {
        setOverSampleRatio(4, false);
      }
      else if (rto->osr == 4) {
        setOverSampleRatio(1, false);
      }
      delay(4);
      optimizePhaseSP();
      SerialM.print("OSR "); SerialM.print(rto->osr); SerialM.println("x");
      rto->phaseIsSet = 0;  // do it again in modes applicable
    }
    break;
    case 'g':
      inputStage++;
      // we have a multibyte command
      if (inputStage > 0) {
        if (inputStage == 1) {
          segmentCurrent = Serial.parseInt();
          SerialM.print("G");
          SerialM.print(segmentCurrent);
        }
        else if (inputStage == 2) {
          char szNumbers[3];
          szNumbers[0] = Serial.read(); szNumbers[1] = Serial.read(); szNumbers[2] = '\0';
          //char * pEnd;
          registerCurrent = strtol(szNumbers, NULL, 16);
          SerialM.print("R");
          SerialM.print(registerCurrent, HEX);
          if (segmentCurrent <= 5) {
            writeOneByte(0xF0, segmentCurrent);
            readFromRegister(registerCurrent, 1, &readout);
            SerialM.print(" value: 0x"); SerialM.println(readout, HEX);
          }
          else {
            discardSerialRxData();
            SerialM.println("abort");
          }
          inputStage = 0;
        }
      }
    break;
    case 's':
      inputStage++;
      // we have a multibyte command
      if (inputStage > 0) {
        if (inputStage == 1) {
          segmentCurrent = Serial.parseInt();
          SerialM.print("S");
          SerialM.print(segmentCurrent);
        }
        else if (inputStage == 2) {
          char szNumbers[3];
          for (uint8_t a = 0; a <= 1; a++) {
            // ascii 0x30 to 0x39 for '0' to '9'
            if ((Serial.peek() >= '0' && Serial.peek() <= '9') ||
                (Serial.peek() >= 'a' && Serial.peek() <= 'f') ||
                (Serial.peek() >= 'A' && Serial.peek() <= 'F'))
            {
              szNumbers[a] = Serial.read();
            }
            else {
              szNumbers[a] = 0;  // NUL char
              Serial.read();     // but consume the char
            }
          }
          szNumbers[2] = '\0';
          //char * pEnd;
          registerCurrent = strtol(szNumbers, NULL, 16);
          SerialM.print("R");
          SerialM.print(registerCurrent, HEX);
        }
        else if (inputStage == 3) {
          char szNumbers[3];
          for (uint8_t a = 0; a <= 1; a++) {
            if ((Serial.peek() >= '0' && Serial.peek() <= '9') ||
                (Serial.peek() >= 'a' && Serial.peek() <= 'f') ||
                (Serial.peek() >= 'A' && Serial.peek() <= 'F'))
            {
              szNumbers[a] = Serial.read();
            }
            else {
              szNumbers[a] = 0;  // NUL char
              Serial.read();     // but consume the char
            }
          }
          szNumbers[2] = '\0';
          //char * pEnd;
          inputToogleBit = strtol(szNumbers, NULL, 16);
          if (segmentCurrent <= 5) {
            writeOneByte(0xF0, segmentCurrent);
            readFromRegister(registerCurrent, 1, &readout);
            SerialM.print(" (was 0x"); SerialM.print(readout, HEX); SerialM.print(")");
            writeOneByte(registerCurrent, inputToogleBit);
            readFromRegister(registerCurrent, 1, &readout);
            SerialM.print(" is now: 0x"); SerialM.println(readout, HEX);
          }
          else {
            discardSerialRxData();
            SerialM.println("abort");
          }
          inputStage = 0;
        }
      }
    break;
    case 't':
      inputStage++;
      // we have a multibyte command
      if (inputStage > 0) {
        if (inputStage == 1) {
          segmentCurrent = Serial.parseInt();
          SerialM.print("T");
          SerialM.print(segmentCurrent);
        }
        else if (inputStage == 2) {
          char szNumbers[3];
          for (uint8_t a = 0; a <= 1; a++) {
            // ascii 0x30 to 0x39 for '0' to '9'
            if ((Serial.peek() >= '0' && Serial.peek() <= '9') || 
                (Serial.peek() >= 'a' && Serial.peek() <= 'f') || 
                (Serial.peek() >= 'A' && Serial.peek() <= 'F'))
            {
              szNumbers[a] = Serial.read(); 
            }
            else {
              szNumbers[a] = 0;  // NUL char
              Serial.read();     // but consume the char
            }
          }
          szNumbers[2] = '\0';
          //char * pEnd;
          registerCurrent = strtol(szNumbers, NULL, 16);
          SerialM.print("R");
          SerialM.print(registerCurrent, HEX);
        }
        else if (inputStage == 3) {
          if (Serial.peek() >= '0' && Serial.peek() <= '7') { 
            inputToogleBit = Serial.parseInt(); 
          }
          else {
            inputToogleBit = 255; // will get discarded next step
          }
          SerialM.print(" Bit: "); SerialM.print(inputToogleBit);
          inputStage = 0;
          if ((segmentCurrent <= 5) && (inputToogleBit <= 7)) {
            writeOneByte(0xF0, segmentCurrent);
            readFromRegister(registerCurrent, 1, &readout);
            SerialM.print(" (was 0x"); SerialM.print(readout, HEX); SerialM.print(")");
            writeOneByte(registerCurrent, readout ^ (1 << inputToogleBit));
            readFromRegister(registerCurrent, 1, &readout);
            SerialM.print(" is now: 0x"); SerialM.println(readout, HEX);
          }
          else {
            discardSerialRxData();
            inputToogleBit = registerCurrent = 0;
            SerialM.println("abort");
          }
        }
      }
    break;
    case '<':
    {
      if (segmentCurrent != 255 && registerCurrent != 255) {
        writeOneByte(0xF0, segmentCurrent);
        readFromRegister(registerCurrent, 1, &readout);
        writeOneByte(registerCurrent, readout - 1); // also allow wrapping
        Serial.print("S"); Serial.print(segmentCurrent);
        Serial.print("_"); Serial.print(registerCurrent, HEX);
        readFromRegister(registerCurrent, 1, &readout);
        Serial.print(" : "); Serial.println(readout, HEX);
      }
    }
    break;
    case '>':
    {
      if (segmentCurrent != 255 && registerCurrent != 255) {
        writeOneByte(0xF0, segmentCurrent);
        readFromRegister(registerCurrent, 1, &readout);
        writeOneByte(registerCurrent, readout + 1);
        Serial.print("S"); Serial.print(segmentCurrent);
        Serial.print("_"); Serial.print(registerCurrent, HEX);
        readFromRegister(registerCurrent, 1, &readout);
        Serial.print(" : "); Serial.println(readout, HEX);
      }
    }
    break;
    case '_':
    {
      uint32_t ticks = FrameSync::getPulseTicks();
      Serial.println(ticks);
    }
    break;
    case '~':
      goLowPowerWithInputDetection(); // test reset + input detect
    break;
    case 'w':
    {
      //Serial.flush();
      uint16_t value = 0;
      String what = Serial.readStringUntil(' ');

      if (what.length() > 5) {
        SerialM.println("abort");
        inputStage = 0;
        break;
      }
      value = Serial.parseInt();
      if (value < 4096) {
        SerialM.print("set "); SerialM.print(what); SerialM.print(" "); SerialM.println(value);
        if (what.equals("ht")) {
          //set_htotal(value);
          if (!rto->outModeHdBypass) {
            rto->forceRetime = 1;
            applyBestHTotal(value);
          }
          else {
            GBS::VDS_HSYNC_RST::write(value);
          }
        }
        else if (what.equals("vt")) {
          set_vtotal(value);
        }
        else if (what.equals("hsst")) {
          setHSyncStartPosition(value);
        }
        else if (what.equals("hssp")) {
          setHSyncStopPosition(value);
        }
        else if (what.equals("hbst")) {
          setMemoryHblankStartPosition(value);
        }
        else if (what.equals("hbsp")) {
          setMemoryHblankStopPosition(value);
        }
        else if (what.equals("hbstd")) {
          setDisplayHblankStartPosition(value);
        }
        else if (what.equals("hbspd")) {
          setDisplayHblankStopPosition(value);
        }
        else if (what.equals("vsst")) {
          setVSyncStartPosition(value);
        }
        else if (what.equals("vssp")) {
          setVSyncStopPosition(value);
        }
        else if (what.equals("vbst")) {
          setMemoryVblankStartPosition(value);
        }
        else if (what.equals("vbsp")) {
          setMemoryVblankStopPosition(value);
        }
        else if (what.equals("vbstd")) {
          setDisplayVblankStartPosition(value);
        }
        else if (what.equals("vbspd")) {
          setDisplayVblankStopPosition(value);
        }
        else if (what.equals("sog")) {
          setAndUpdateSogLevel(value);
        }
        else if (what.equals("ifini")) {
          GBS::IF_INI_ST::write(value);
          //Serial.println(GBS::IF_INI_ST::read());
        }
        else if (what.equals("ifvst")) {
          GBS::IF_VB_ST::write(value);
          //Serial.println(GBS::IF_VB_ST::read());
        }
        else if (what.equals("ifvsp")) {
          GBS::IF_VB_SP::write(value);
          //Serial.println(GBS::IF_VB_ST::read());
        }
      }
      else {
        SerialM.println("abort");
      }
    }
    break;
    case 'x':
    {
      uint16_t if_hblank_scale_stop = GBS::IF_HBIN_SP::read();
      GBS::IF_HBIN_SP::write(if_hblank_scale_stop + 1);
      SerialM.print("1_26: "); SerialM.println((if_hblank_scale_stop + 1), HEX);
    }
    break;
    case 'X':
    {
      uint16_t if_hblank_scale_stop = GBS::IF_HBIN_SP::read();
      GBS::IF_HBIN_SP::write(if_hblank_scale_stop - 1);
      SerialM.print("1_26: "); SerialM.println((if_hblank_scale_stop - 1), HEX);
    }
    break;
    case '(':
    {
      writeProgramArrayNew(ntsc_1920x1080, false);
      doPostPresetLoadSteps();
    }
    break;
    case ')':
    {
      writeProgramArrayNew(pal_1920x1080, false);
      doPostPresetLoadSteps();
    }
    break;
    case 'V':
    {
      SerialM.print("step response ");
      uopt->wantStepResponse = !uopt->wantStepResponse;
      if (uopt->wantStepResponse) {
        GBS::VDS_UV_STEP_BYPS::write(0);
        SerialM.println("on");
      }
      else {
        GBS::VDS_UV_STEP_BYPS::write(1);
        SerialM.println("off");
      }
      saveUserPrefs();
    }
    break;
    default:
      Serial.print("unknown command ");
      Serial.println(typeOneCommand, HEX);
      break;
    }

    delay(1); // give some time to read in eventual next chars

    // a web ui or terminal command has finished. good idea to reset sync lock timer
    // important if the command was to change presets, possibly others
    lastVsyncLock = millis();

    if (!Serial.available()) {
      // in case we handled a Serial or web server command and there's no more extra commands
      typeOneCommand = '@';
      handleWiFi(1);
    }
  }

  if (typeTwoCommand != '@') {
    handleType2Command(typeTwoCommand);
    typeTwoCommand = '@'; // in case we handled web server command
    lastVsyncLock = millis();
    handleWiFi(1);
  }

  // run FrameTimeLock if enabled
  if (uopt->enableFrameTimeLock && rto->sourceDisconnected == false && rto->autoBestHtotalEnabled && 
    rto->syncWatcherEnabled && FrameSync::ready() && millis() - lastVsyncLock > FrameSyncAttrs::lockInterval
    && rto->continousStableCounter > 20 && rto->noSyncCounter == 0) {
    
    uint8_t debug_backup = GBS::TEST_BUS_SEL::read();
    if (debug_backup != 0x0) {
      GBS::TEST_BUS_SEL::write(0x0);
    }
    if (!FrameSync::run(uopt->frameTimeLockMethod)) {
      if (rto->syncLockFailIgnore-- == 0) {
        FrameSync::reset(); // in case run() failed because we lost sync signal
      }
    }
    else if (rto->syncLockFailIgnore > 0) {
      rto->syncLockFailIgnore = 16;
    }
    if (debug_backup != 0x0) {
      GBS::TEST_BUS_SEL::write(debug_backup);
    }
    lastVsyncLock = millis();
  }

  if (rto->syncWatcherEnabled && rto->boardHasPower) {
    if ((millis() - lastTimeInterruptClear) > 3000) {
      GBS::INTERRUPT_CONTROL_00::write(0xfe); // reset except for SOGBAD
      GBS::INTERRUPT_CONTROL_00::write(0x00);
      lastTimeInterruptClear = millis();
    }
  }

  // information mode
  if (rto->printInfos == true) {
    printInfo();
  }

  //uint16_t testbus = GBS::TEST_BUS::read() & 0x0fff;
  //if (testbus >= 0x0FFD){
  //  Serial.println(testbus,HEX);
  //}
  //if (rto->videoIsFrozen && (rto->continousStableCounter >= 2)) {
  //    unfreezeVideo();
  //}

  // syncwatcher polls SP status. when necessary, initiates adjustments or preset changes
  if (rto->sourceDisconnected == false && rto->syncWatcherEnabled == true 
    && (millis() - lastTimeSyncWatcher) > 20) 
  {
    runSyncWatcher();
    lastTimeSyncWatcher = millis();
  }

  // frame sync + besthtotal init routine; run if it wasn't successful in postpresetloadsteps
  // continousStableCounter check was >= 5; raised to 10 so optimizePhaseSP runs before it
  if (rto->autoBestHtotalEnabled && !FrameSync::ready() && rto->syncWatcherEnabled) {
    if (rto->continousStableCounter >= 10 && rto->coastPositionIsSet) {
      if ((rto->continousStableCounter % 5) == 0) { // 5, 10, 15, .., 255
        runAutoBestHTotal();
      }
    }
  }
  
  // update clamp + coast positions after preset change // do it quickly
  if ((rto->videoStandardInput <= 14 && rto->videoStandardInput != 0) &&
    rto->syncWatcherEnabled && !rto->coastPositionIsSet)
  {
    if (rto->continousStableCounter >= 7) {
      // add stability check here? // yep; done
      if ((getStatus16SpHsStable() == 1) && (getVideoMode() == rto->videoStandardInput))
      {
        updateCoastPosition(0);
        if (rto->coastPositionIsSet) {
          if (GBS::GBS_OPTION_SCALING_RGBHV::read() == 0)
          {
            GBS::SP_DIS_SUB_COAST::write(0); // enable SUB_COAST
            GBS::SP_H_PROTECT::write(0);     // H_PROTECT stays off
          }
          else {
            GBS::SP_DIS_SUB_COAST::write(1); // keep disabled
            GBS::SP_H_PROTECT::write(1);     // but use this
          }
        }
      }
    }
  }

  // don't exclude modes 13 / 14 / 15 (rgbhv bypass)
  if ((rto->videoStandardInput != 0) && (rto->continousStableCounter >= 2) &&
    !rto->clampPositionIsSet && rto->syncWatcherEnabled)
  {
    updateClampPosition();
    if (rto->clampPositionIsSet) {
      if (GBS::SP_NO_CLAMP_REG::read() == 1) {
        GBS::SP_NO_CLAMP_REG::write(0);
      }
    }
  }
  
  // later stage post preset adjustments 
  if ((rto->applyPresetDoneStage == 1) &&
    ((rto->continousStableCounter > 35 && rto->continousStableCounter < 45) || // this
      !rto->syncWatcherEnabled))                                               // or that
  {
    if (rto->applyPresetDoneStage == 1) 
    {
      //Serial.println("2nd chance");
      GBS::DAC_RGBS_PWDNZ::write(1); // 2nd chance
      if (!uopt->wantOutputComponent) 
      {
        GBS::PAD_SYNC_OUT_ENZ::write(0); // enable sync out // 2nd chance
      }
      if (!rto->syncWatcherEnabled) 
      { 
        updateClampPosition();
        GBS::SP_NO_CLAMP_REG::write(0); // 5_57 0
      }
      rto->applyPresetDoneStage = 0;
      //printInfo();
    }
  }
  else if (rto->applyPresetDoneStage == 1 && (rto->continousStableCounter > 35))
  {
    //Serial.println("3rd chance");
    GBS::DAC_RGBS_PWDNZ::write(1); // enable DAC // 3rd chance
    rto->applyPresetDoneStage = 0; // timeout
    if (!uopt->wantOutputComponent) 
    {
      GBS::PAD_SYNC_OUT_ENZ::write(0); // enable sync out // 3rd chance
    }
  }
  
  if (rto->applyPresetDoneStage == 10)
  {
    rto->applyPresetDoneStage = 11; // set first, so we don't loop applying presets
    setOutModeHdBypass();
  }

  // run auto ADC gain feature (if enabled)
  if (rto->syncWatcherEnabled && uopt->enableAutoGain == 1 && !rto->sourceDisconnected
    && rto->videoStandardInput > 0 && rto->clampPositionIsSet
    && rto->noSyncCounter == 0 && rto->continousStableCounter > 40
    && ((millis() - lastTimeAutoGain) > 3) && rto->boardHasPower)
  {
    uint8_t debugRegBackup = 0, debugPinBackup = 0;
    debugPinBackup = GBS::PAD_BOUT_EN::read();
    debugRegBackup = GBS::TEST_BUS_SEL::read();
    GBS::PAD_BOUT_EN::write(0); // disable output to pin for test
    GBS::TEST_BUS_SEL::write(0xb); // decimation
    runAutoGain();
    GBS::TEST_BUS_SEL::write(debugRegBackup);
    GBS::PAD_BOUT_EN::write(debugPinBackup); // debug output pin back on
    lastTimeAutoGain = millis();
  }

  if (rto->syncWatcherEnabled == true && rto->sourceDisconnected == true && rto->boardHasPower)
  {
    if ((millis() - lastTimeSourceCheck) >= 500) 
    {
      if ( checkBoardPower() )
      {
        inputAndSyncDetect(); // source is off or just started; keep looking for new input
      }
      lastTimeSourceCheck = millis();
    }
  }

  // has the GBS board lost power? // check at 2 points, in case one doesn't register
  // values around 21 chosen to not do this check for small sync issues
  if ((rto->noSyncCounter == 21 || rto->noSyncCounter == 22) && rto->boardHasPower)
  {
    if ( !checkBoardPower() )
    {
      rto->noSyncCounter = 1; // some neutral value, meaning "no sync"
    }
    else
    {
      rto->noSyncCounter = 23; // avoid checking twice
    }
  }

  // power good now? // added syncWatcherEnabled check to enable passive modes
  if (!rto->boardHasPower && rto->syncWatcherEnabled) 
  { // then check if power has come on
    if (digitalRead(SCL) && digitalRead(SDA)) 
    {
      delay(50);
      if (digitalRead(SCL) && digitalRead(SDA)) 
      {
        Serial.println("power good");
        delay(350); // i've seen the MTV230 go on briefly on GBS power cycle
        startWire();
        rto->syncWatcherEnabled = true;
        rto->boardHasPower = true;
        delay(100);
        goLowPowerWithInputDetection();
      }
    }
  }

#ifdef HAVE_PINGER_LIBRARY
  // periodic pings for debugging WiFi issues
  if (WiFi.status() == WL_CONNECTED)
  {
    if (rto->enableDebugPings && millis() - pingLastTime > 1000) {
      // regular interval pings
      if (pinger.Ping(WiFi.gatewayIP(), 1, 750) == false)
      {
        Serial.println("Error during last ping command.");
      }
      pingLastTime = millis();
    }
  }
#endif
}

#if defined(ESP8266)
#include "webui_html.h"
// gzip -c9 webui.html > webui_html && xxd -i webui_html > webui_html.h && rm webui_html && sed -i -e 's/unsigned char webui_html\[]/const uint8_t webui_html[] PROGMEM/' webui_html.h && sed -i -e 's/unsigned int webui_html_len/const unsigned int webui_html_len/' webui_html.h

void handleType2Command(char argument) {
  switch (argument) {
  case '0':
    SerialM.print("pal force 60hz ");
    if (uopt->PalForce60 == 0) {
      uopt->PalForce60 = 1;
      Serial.println("on");
    }
    else {
      uopt->PalForce60 = 0;
      Serial.println("off");
    }
    saveUserPrefs();
    break;
  case '1':
    // reset to defaults button
    webSocket.close();
    loadDefaultUserOptions();
    saveUserPrefs();
    Serial.println(F("options set to defaults, restarting"));
    delay(60);
    ESP.reset(); // don't use restart(), messes up websocket reconnects
    //
    break;
  case '2':
    //
    break;
  case '3':  // load custom preset
  {
    //const uint8_t* preset = loadPresetFromSPIFFS(rto->videoStandardInput); // load for current video mode
    uopt->presetPreference = 2; // custom
    //writeProgramArrayNew(preset, false);
    //doPostPresetLoadSteps();
    //uopt->presetPreference = 2; // custom
    applyPresets(rto->videoStandardInput);
    saveUserPrefs();
  }
  break;
  case '4': // save custom preset
    savePresetToSPIFFS();
    uopt->presetPreference = 2; // custom
    saveUserPrefs();
    break;
  case '5':
    //Frame Time Lock toggle
    uopt->enableFrameTimeLock = !uopt->enableFrameTimeLock;
    saveUserPrefs();
    if (uopt->enableFrameTimeLock) { SerialM.println("FTL on"); }
    else { SerialM.println("FTL off"); }
    FrameSync::reset();
    //runAutoBestHTotal(); // includes needed checks
    break;
  case '6':
    //
    break;
  case '7':
    uopt->wantScanlines = !uopt->wantScanlines;
    SerialM.print("scanlines ");
    if (uopt->wantScanlines) {
      SerialM.println("on");
    }
    else {
      disableScanlines();
      SerialM.println("off");
    }
    saveUserPrefs();
    break;
  case '9':
    //
    break;
  case 'a':
    webSocket.close();
    Serial.println("restart");
    delay(60);
    ESP.reset(); // don't use restart(), messes up websocket reconnects
    break;
  case 'b':
    uopt->presetSlot = 1;
    uopt->presetPreference = 2; // custom
    saveUserPrefs();
    break;
  case 'c':
    uopt->presetSlot = 2;
    uopt->presetPreference = 2;
    saveUserPrefs();
    break;
  case 'd':
    uopt->presetSlot = 3;
    uopt->presetPreference = 2;
    saveUserPrefs();
    break;
  case 'j':
    uopt->presetSlot = 4;
    uopt->presetPreference = 2;
    saveUserPrefs();
    break;
  case 'k':
    uopt->presetSlot = 5;
    uopt->presetPreference = 2;
    saveUserPrefs();
    break;
  case 'e': // print files on spiffs
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      SerialM.print(dir.fileName()); SerialM.print(" "); SerialM.println(dir.fileSize());
      delay(1); // wifi stack
    }
    ////
    File f = SPIFFS.open("/preferencesv2.txt", "r");
    if (!f) {
      SerialM.println(F("failed opening preferences file"));
    }
    else {
      SerialM.print("preset preference = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("frame time lock = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("preset slot = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("frame lock method = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("auto gain = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("scanlines = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("component output = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("deinterlacer mode = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("line filter = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("peaking = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("preferScalingRgbhv = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("6-tap = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("pal force60 = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("matched = "); SerialM.println((uint8_t)(f.read() - '0'));
      SerialM.print("step response = "); SerialM.println((uint8_t)(f.read() - '0'));

      f.close();
    }
  }
  break;
  case 'f':
  case 'g':
  case 'h':
  case 'p':
  case 's':
  {
    // load preset via webui
    uint8_t videoMode = getVideoMode();
    if (videoMode == 0) videoMode = rto->videoStandardInput; // last known good as fallback
    //uint8_t backup = uopt->presetPreference;
    if (argument == 'f') uopt->presetPreference = 0; // 1280x960
    if (argument == 'g') uopt->presetPreference = 3; // 1280x720
    if (argument == 'h') uopt->presetPreference = 1; // 640x480
    if (argument == 'p') uopt->presetPreference = 4; // 1280x1024
    if (argument == 's') uopt->presetPreference = 5; // 1920x1080
    //rto->videoStandardInput = 0; // force hard reset  // update: why? it conflicts with early init
    applyPresets(videoMode);
    saveUserPrefs();
    //uopt->presetPreference = backup;
  }
  break;
  case 'i':
    // toggle active frametime lock method
    SerialM.print("FTL method: ");
    if (uopt->frameTimeLockMethod == 0) {
      uopt->frameTimeLockMethod = 1;
      SerialM.println("1");
    }
    else if (uopt->frameTimeLockMethod == 1) {
      uopt->frameTimeLockMethod = 0;
      SerialM.println("0");
    }
    saveUserPrefs();
    break;
  case 'l':
    // cycle through available SDRAM clocks
  {
    uint8_t PLL_MS = GBS::PLL_MS::read();
    uint8_t memClock = 0;
    PLL_MS++; PLL_MS &= 0x7;
    switch (PLL_MS) {
    case 0: memClock = 108; break;
    case 1: memClock = 81; break; // goes well with 4_2C = 0x14, 4_2D = 0x27
    case 2: memClock = 10; break; //feedback clock
    case 3: memClock = 162; break;
    case 4: memClock = 144; break;
    case 5: memClock = 185; break;
    case 6: memClock = 216; break;
    case 7: memClock = 129; break;
    default: break;
    }
    GBS::PLL_MS::write(PLL_MS);
    ResetSDRAM();
    if (memClock != 10) {
      SerialM.print("SDRAM clock: "); SerialM.print(memClock); SerialM.println("Mhz");
    }
    else {
      SerialM.print("SDRAM clock: "); SerialM.println("Feedback clock");
    }
  }
  break;
  case 'm':
    SerialM.print("line filter ");
    if (uopt->wantVdsLineFilter) {
      uopt->wantVdsLineFilter = 0;
      GBS::VDS_D_RAM_BYPS::write(1);
      SerialM.println("off");
    }
    else {
      uopt->wantVdsLineFilter = 1;
      GBS::VDS_D_RAM_BYPS::write(0);
      SerialM.println("on");
    }
    saveUserPrefs();
    break;
  case 'n':
    SerialM.print("ADC gain++ : ");
    uopt->enableAutoGain = 0;
    GBS::ADC_RGCTRL::write(GBS::ADC_RGCTRL::read() - 1);
    GBS::ADC_GGCTRL::write(GBS::ADC_GGCTRL::read() - 1);
    GBS::ADC_BGCTRL::write(GBS::ADC_BGCTRL::read() - 1);
    SerialM.println(GBS::ADC_RGCTRL::read(), HEX);
    break;
  case 'o':
    SerialM.print("ADC gain-- : ");
    uopt->enableAutoGain = 0;
    GBS::ADC_RGCTRL::write(GBS::ADC_RGCTRL::read() + 1);
    GBS::ADC_GGCTRL::write(GBS::ADC_GGCTRL::read() + 1);
    GBS::ADC_BGCTRL::write(GBS::ADC_BGCTRL::read() + 1);
    SerialM.println(GBS::ADC_RGCTRL::read(), HEX);
    break;
  case 'A':
  {
    uint16_t htotal = GBS::VDS_HSYNC_RST::read();
    uint16_t hbstd = GBS::VDS_DIS_HB_ST::read();
    uint16_t hbspd = GBS::VDS_DIS_HB_SP::read();
    if ((hbstd > 4) && (hbspd < (htotal - 4)))
    {
      GBS::VDS_DIS_HB_ST::write(GBS::VDS_DIS_HB_ST::read() - 4);
      GBS::VDS_DIS_HB_SP::write(GBS::VDS_DIS_HB_SP::read() + 4);
    }
    else
    {
      SerialM.println("limit");
    }
  }
  break;
  case 'B':
  {
    uint16_t htotal = GBS::VDS_HSYNC_RST::read();
    uint16_t hbstd = GBS::VDS_DIS_HB_ST::read();
    uint16_t hbspd = GBS::VDS_DIS_HB_SP::read();
    if ((hbstd < (htotal - 4)) && (hbspd > 4))
    {
      GBS::VDS_DIS_HB_ST::write(GBS::VDS_DIS_HB_ST::read() + 4);
      GBS::VDS_DIS_HB_SP::write(GBS::VDS_DIS_HB_SP::read() - 4);
    }
    else
    {
      SerialM.println("limit");
    }
  }
  break;
  case 'C':
  {
    uint16_t vtotal = GBS::VDS_VSYNC_RST::read();
    uint16_t vbstd = GBS::VDS_DIS_VB_ST::read();
    uint16_t vbspd = GBS::VDS_DIS_VB_SP::read();
    if ((vbstd > 6) && (vbspd < (vtotal - 4)))
    {
      GBS::VDS_DIS_VB_ST::write(vbstd - 2);
      GBS::VDS_DIS_VB_SP::write(vbspd + 2);
    }
    else
    {
      SerialM.println("limit");
    }
  }
  break;
  case 'D':
  {
    uint16_t vtotal = GBS::VDS_VSYNC_RST::read();
    uint16_t vbstd = GBS::VDS_DIS_VB_ST::read();
    uint16_t vbspd = GBS::VDS_DIS_VB_SP::read();
    if ((vbstd < (vtotal - 4)) && (vbspd > 6))
    {
      GBS::VDS_DIS_VB_ST::write(vbstd + 2);
      GBS::VDS_DIS_VB_SP::write(vbspd - 2);
    }
    else
    {
      SerialM.println("limit");
    }
  }
  break;
  case 'q':
    if (uopt->deintMode != 1)
    {
      uopt->deintMode = 1;
      disableMotionAdaptDeinterlace();
      saveUserPrefs();
    }
    SerialM.println("Deinterlacer: Bob");
  break;
  case 'r':
    if (uopt->deintMode != 0)
    {
      uopt->deintMode = 0;
      saveUserPrefs();
      // will enable next loop()
    }
    SerialM.println(F("Deinterlacer: Motion Adaptive"));
  break;
  case 't':
    SerialM.print("6-tap: ");
    if (uopt->wantTap6 == 0)
    {
      uopt->wantTap6 = 1;
      GBS::VDS_TAP6_BYPS::write(0);
      SerialM.println("on");
    }
    else {
      uopt->wantTap6 = 0;
      GBS::VDS_TAP6_BYPS::write(1);
      SerialM.println("off");
    }
    saveUserPrefs();
  break;
  case 'u':
    // restart to attempt wifi station mode connect
    delay(30);
    WiFi.mode(WIFI_STA);
    WiFi.hostname(device_hostname_partial); // _full
    delay(30);
    ESP.reset();
  break;
  case 'v':
    uopt->wantFullHeight = !uopt->wantFullHeight;
    saveUserPrefs();
  break;
  case 'w':
    uopt->enableCalibrationADC = !uopt->enableCalibrationADC;
    saveUserPrefs();
    break;
  case 'x':
    uopt->preferScalingRgbhv = !uopt->preferScalingRgbhv;
    SerialM.print("preferScalingRgbhv: ");
    if (uopt->preferScalingRgbhv) {
      SerialM.println("on");
    }
    else {
      SerialM.println("off");
    }
    saveUserPrefs();
    break;
  case 'z':
    // sog slicer level
    if (rto->currentLevelSOG > 0) {
      rto->currentLevelSOG -= 1;
    }
    else {
      rto->currentLevelSOG = 16;
    }
    setAndUpdateSogLevel(rto->currentLevelSOG);
    optimizePhaseSP();
    break;
  default:
    break;
  }
}

void webSocketEvent(uint8_t num, uint8_t type, uint8_t * payload, size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    //Serial.print("WS: #"); Serial.print(num); Serial.print(" disconnected,");
    //Serial.print(" remaining: "); Serial.println(webSocket.connectedClients());
  break;
  case WStype_CONNECTED:
    //Serial.print("WS: #"); Serial.print(num); Serial.print(" connected, ");
    //Serial.print(" total: "); Serial.println(webSocket.connectedClients());
    updateWebSocketData();
  break;
  case WStype_PONG:
    //Serial.print("p");
    updateWebSocketData();
  break;
  }
}

void startWebserver()
{
  persWM.setApCredentials(ap_ssid, ap_password);
  persWM.onConnect([]() {
    SerialM.print(F("(WiFi): STA mode connected; IP: "));
    SerialM.println(WiFi.localIP().toString());
    if (MDNS.begin(device_hostname_partial)) { // MDNS request for gbscontrol.local
      //Serial.println("MDNS started");
      MDNS.addService("http", "tcp", 80); // Add service to MDNS-SD
      MDNS.announce();
    }
    SerialM.println(FPSTR(st_info_string));
  });
  persWM.onAp([]() {
    SerialM.println(FPSTR(ap_info_string));
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    //Serial.println("sending web page");
    if (ESP.getFreeHeap() > 10000) {
      AsyncWebServerResponse* response = request->beginResponse_P(200, "text/html", webui_html, webui_html_len);
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
    }
  });

  server.on("/sc", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (ESP.getFreeHeap() > 10000) {
      int params = request->params();
      //Serial.print("got serial request params: ");
      //Serial.println(params);
      if (params > 0) {
        AsyncWebParameter* p = request->getParam(0);
        //Serial.println(p->name());
        typeOneCommand = p->name().charAt(0);

        // hack, problem with '+' command received via url param
        if (typeOneCommand == ' ') {
          typeOneCommand = '+';
        }
      }
      request->send(200); // reply
    }
  });

  server.on("/uc", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (ESP.getFreeHeap() > 10000) {
      int params = request->params();
      //Serial.print("got user request params: ");
      //Serial.println(params);
      if (params > 0) {
        AsyncWebParameter* p = request->getParam(0);
        //Serial.println(p->name());
        typeTwoCommand = p->name().charAt(0);
      }
      request->send(200); // reply
    }
  });

  server.on("/wifi/connect", HTTP_POST, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response =
      request->beginResponse(200, "text/plain", "connecting...");
    request->send(response);

    if (request->arg("n").length()) { // n holds ssid
      if (request->arg("p").length()) { // p holds password
        // false = only save credentials, don't connect
        WiFi.begin(request->arg("n").c_str(), request->arg("p").c_str(), 0, 0, false);
      }
      else {
        WiFi.begin(request->arg("n").c_str(), emptyString, 0, 0, false);
      }
    }
    else {
      WiFi.begin();
    }

    typeTwoCommand = 'u'; // next loop, set wifi station mode and restart device
  });

  webSocket.onEvent(webSocketEvent);

  persWM.setConnectNonBlock(true);
  if (WiFi.SSID().length() == 0) {
    // no stored network to connect to > start AP mode right away
    persWM.setupWiFiHandlers();
    persWM.startApMode();
  }
  else {
    persWM.begin(); // first try connecting to stored network, go AP mode after timeout
  }

  server.begin(); // Webserver for the site
  webSocket.begin();  // Websocket for interaction
  yield();

#ifdef HAVE_PINGER_LIBRARY
  // pinger library
  pinger.OnReceive([](const PingerResponse& response)
  {
    if (response.ReceivedResponse)
    {
      Serial.printf(
        "Reply from %s: time=%lums\n",
        response.DestIPAddress.toString().c_str(),
        response.ResponseTime
        );

      pingLastTime = millis() - 900; // produce a fast stream of pings if connection is good
    }
    else
    {
      Serial.printf("Request timed out.\n");
    }

    // Return true to continue the ping sequence.
    // If current event returns false, the ping sequence is interrupted.
    return true;
  });

  pinger.OnEnd([](const PingerResponse& response)
  {
    // detailed info not necessary
    return true;
  });
#endif
}

void initUpdateOTA() {
  ArduinoOTA.setHostname("GBS OTA");

  // ArduinoOTA.setPassword("admin");
  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  //ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  // update: no password is as (in)secure as this publicly stated hash..
  // rely on the user having to enable the OTA feature on the web ui

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    SPIFFS.end();
    SerialM.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    SerialM.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    SerialM.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    SerialM.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) SerialM.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) SerialM.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) SerialM.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) SerialM.println("Receive Failed");
    else if (error == OTA_END_ERROR) SerialM.println("End Failed");
  });
  ArduinoOTA.begin();
  yield();
}

// sets every element of str to 0 (clears array)
void StrClear(char *str, uint16_t length)
{
  for (int i = 0; i < length; i++) {
    str[i] = 0;
  }
}

const uint8_t* loadPresetFromSPIFFS(byte forVideoMode) {
  static uint8_t preset[432];
  String s = "";
  char slot = '0';
  File f;

  f = SPIFFS.open("/preferencesv2.txt", "r");
  if (f) {
    SerialM.println("preferencesv2.txt opened");
    char result[3];
    result[0] = f.read(); // todo: move file cursor manually
    result[1] = f.read();
    result[2] = f.read();

    f.close();
    if ((uint8_t)(result[2] - '0') < 10) {
      slot = result[2]; // otherwise not stored on spiffs
    }
  }
  else {
    // file not found, we don't know what preset to load
    SerialM.println(F("please select a preset slot first!")); // say "slot" here to make people save usersettings
    if (forVideoMode == 2 || forVideoMode == 4) return pal_240p;
    else return ntsc_240p;
  }

  SerialM.print("loading from preset slot "); SerialM.print(String(slot));
  SerialM.print(": ");

  if (forVideoMode == 1) {
    f = SPIFFS.open("/preset_ntsc." + String(slot), "r");
  }
  else if (forVideoMode == 2) {
    f = SPIFFS.open("/preset_pal." + String(slot), "r");
  }
  else if (forVideoMode == 3) {
    f = SPIFFS.open("/preset_ntsc_480p." + String(slot), "r");
  }
  else if (forVideoMode == 4) {
    f = SPIFFS.open("/preset_pal_576p." + String(slot), "r");
  }
  else if (forVideoMode == 5) {
    f = SPIFFS.open("/preset_ntsc_720p." + String(slot), "r");
  }
  else if (forVideoMode == 6) {
    f = SPIFFS.open("/preset_ntsc_1080p." + String(slot), "r");
  }
  else if (forVideoMode == 8) {
    f = SPIFFS.open("/preset_medium_res." + String(slot), "r");
  }
  else if (forVideoMode == 0) {
    f = SPIFFS.open("/preset_unknown." + String(slot), "r");
  }

  if (!f) {
    SerialM.println(F("no preset file for this slot and source"));
    if (forVideoMode == 2 || forVideoMode == 4) return pal_240p;
    else return ntsc_240p;
  }
  else {
    SerialM.println(f.name());
    s = f.readStringUntil('}');
    f.close();
  }

  char *tmp;
  uint16_t i = 0;
  tmp = strtok(&s[0], ",");
  while (tmp) {
    preset[i++] = (uint8_t)atoi(tmp);
    tmp = strtok(NULL, ",");
    yield(); // wifi stack
  }

  return preset;
}

void savePresetToSPIFFS() {
  uint8_t readout = 0;
  File f;
  char slot = '1';

  // first figure out if the user has set a preferenced slot
  f = SPIFFS.open("/preferencesv2.txt", "r");
  if (f) {
    char result[3];
    result[0] = f.read(); // todo: move file cursor manually
    result[1] = f.read();
    result[2] = f.read();

    f.close();
    slot = result[2];     // got the slot to save to now
  }
  else {
    // file not found, we don't know where to save this preset
    SerialM.println(F("please select a preset slot first!"));
    return;
  }

  SerialM.print(F("saving to preset slot ")); SerialM.println(String(slot));

  if (rto->videoStandardInput == 1) {
    f = SPIFFS.open("/preset_ntsc." + String(slot), "w");
  }
  else if (rto->videoStandardInput == 2) {
    f = SPIFFS.open("/preset_pal." + String(slot), "w");
  }
  else if (rto->videoStandardInput == 3) {
    f = SPIFFS.open("/preset_ntsc_480p." + String(slot), "w");
  }
  else if (rto->videoStandardInput == 4) {
    f = SPIFFS.open("/preset_pal_576p." + String(slot), "w");
  }
  else if (rto->videoStandardInput == 5) {
    f = SPIFFS.open("/preset_ntsc_720p." + String(slot), "w");
  }
  else if (rto->videoStandardInput == 6) {
    f = SPIFFS.open("/preset_ntsc_1080p." + String(slot), "w");
  }
  else if (rto->videoStandardInput == 8) {
    f = SPIFFS.open("/preset_medium_res." + String(slot), "w");
  }
  else if (rto->videoStandardInput == 0) {
    f = SPIFFS.open("/preset_unknown." + String(slot), "w");
  }

  if (!f) {
    SerialM.println(F("open save file failed!"));
  }
  else {
    SerialM.println(F("open save file ok"));

    GBS::GBS_PRESET_CUSTOM::write(1); // use one reserved bit to mark this as a custom preset
    // don't store scanlines
    if (GBS::GBS_OPTION_SCANLINES_ENABLED::read() == 1) {
      disableScanlines();
    }
    // next: check for vertical adjust and undo if necessary
    if (GBS::IF_AUTO_OFST_RESERVED_2::read() == 1)
    {
      //GBS::VDS_VB_ST::write(GBS::VDS_VB_ST::read() - rto->presetVlineShift);
      //GBS::VDS_VB_SP::write(GBS::VDS_VB_SP::read() - rto->presetVlineShift);
      for (uint8_t a = 0; a <= 5; a++) {
        shiftVerticalUpIF();
      }
      GBS::IF_AUTO_OFST_RESERVED_2::write(0);
    }

    for (int i = 0; i <= 5; i++) {
      writeOneByte(0xF0, i);
      switch (i) {
      case 0:
        for (int x = 0x40; x <= 0x5F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        for (int x = 0x90; x <= 0x9F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 1:
        for (int x = 0x0; x <= 0x2F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 2:
        // not needed anymore
        break;
      case 3:
        for (int x = 0x0; x <= 0x7F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 4:
        for (int x = 0x0; x <= 0x5F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 5:
        for (int x = 0x0; x <= 0x6F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      }
    }
    f.println("};");
    SerialM.print(F("preset saved as: "));
    SerialM.println(f.name());
    f.close();
  }
}

void saveUserPrefs() {
  File f = SPIFFS.open("/preferencesv2.txt", "w");
  if (!f) {
    SerialM.println(F("saveUserPrefs: open file failed"));
    return;
  }
  f.write(uopt->presetPreference + '0');  // #1
  f.write(uopt->enableFrameTimeLock + '0');
  f.write(uopt->presetSlot + '0');
  f.write(uopt->frameTimeLockMethod + '0');
  f.write(uopt->enableAutoGain + '0');
  f.write(uopt->wantScanlines + '0');
  f.write(uopt->wantOutputComponent + '0');
  f.write(uopt->deintMode + '0');
  f.write(uopt->wantVdsLineFilter + '0');
  f.write(uopt->wantPeaking + '0');
  f.write(uopt->preferScalingRgbhv + '0');
  f.write(uopt->wantTap6 + '0');
  f.write(uopt->PalForce60 + '0');
  f.write(uopt->matchPresetSource + '0'); // #14
  f.write(uopt->wantStepResponse + '0');  // #15
  f.write(uopt->wantFullHeight + '0');    // #16
  f.write(uopt->enableCalibrationADC + '0');    // #17

  f.close();
}

#endif
