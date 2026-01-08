/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#pragma once

#include <Arduino.h>
#include <algorithm>
#include <Wire.h>
#include <math.h>

/**
 * MAX17055 — NO-RTOS variant
 * ---------------------------------------------
 * - No FreeRTOS includes, no semaphores, no background tasks.
 * - All concurrency must be handled by the caller (e.g., PowerManager).
 * - Public API unchanged vs previous RTOS version.
 */

class MAX17055 {
public:
  // ---------------------------------------------------------------------------
  // Public types
  // ---------------------------------------------------------------------------
  enum Error : int8_t {
    OK          = 0,
    I2C_ERROR   = -1,
    TIMEOUT     = -2,
    INIT_FAILED = -3,
    BAD_VALUE   = -4
  };

  enum OnlineState : uint8_t {
    UNKNOWN = 0,
    ONLINE  = 1,
    OFFLINE = 2
  };

  struct Config {
    uint16_t designCap_mAh = 3000;
    uint16_t iChgTerm_mA   = 100;
    uint16_t vEmpty_mV     = 3200;
    uint32_t i2cHz         = 100000;
  };

  // Unified device information snapshot (NO I2C, lock-free here).
  struct BattInfo {
    OnlineState online       = UNKNOWN;
    Error       lastError    = OK;
    bool        initialized  = false;
    bool        dataFresh    = false;
    bool        allowStale   = true;
    uint32_t    lastUpdateMs = 0;

    float    voltage_V      = NAN;
    float    soc_pct        = NAN;
    float    instCurrent_mA = NAN;
    float    avgCurrent_mA  = NAN;
    float    avgVoltage_V   = NAN;

    uint16_t instantCapacity_mAh = 0;
    uint16_t fullCapacity_mAh    = 0;
    uint16_t designCapacity_mAh  = 0;
    uint16_t timeToEmpty_hr      = 0;
    uint16_t chargeCycles        = 0;
    uint16_t batteryAge_raw      = 0;

    String   serial;
  };

  // ---------------------------------------------------------------------------
  // Construction / basic control
  // ---------------------------------------------------------------------------
  MAX17055();
  bool begin(int sdaPin, int sclPin, const Config& cfg, float SenseRes);

  // Call this regularly (e.g., every 250 ms) from caller’s task/loop
  void tick();

  // Online / init state
  bool        isOnline()    const { return online_state_ == ONLINE; }
  OnlineState onlineState() const { return online_state_; }
  bool        initialized() const { return initialized_; }

  // Error handling
  Error lastError() const { return last_err_; }

  // Last read cache
  bool     lastDataFresh() const { return last_fresh_; }
  float    getLastVoltage() const { return lastVoltage_V_; }
  float    getLastSOC()     const { return lastSOC_pct_; }
  uint32_t lastUpdateMs()   const { return lastUpdateMs_; }

  // Stale-read policy
  void setStaleReadPolicy(bool allow) { allow_stale_reads_ = allow; }
  bool staleReadPolicy() const        { return allow_stale_reads_; }

  // Access to configuration and sense resistor
  Config getConfig() const { return cfg_; }
  float  getSenseResistor() const { return _Sense_Resistor; }

  // ---------------------------------------------------------------------------
  // Unified device info snapshot API (NO I2C)
  // ---------------------------------------------------------------------------
  bool getBattInfo(BattInfo& info_out) const;

  // ---------------------------------------------------------------------------
  // Measurement API (perform I2C, update caches)
  // ---------------------------------------------------------------------------
  bool  readVoltage(float& volts_out);
  bool  readSOC(float& soc_percent);
  bool  readVoltageFiltered(float& volts_out);

  float Instant_Voltage();
  float Average_Voltage();
  float Empty_Voltage();
  float Recovery_Voltage();

  float    Instant_Current();
  float    Average_Current();
  float    AverageSOC();
  uint16_t Instant_Capacity();
  uint16_t Design_Capacity();
  uint16_t Full_Capacity();
  uint16_t Time_To_Empty();
  uint16_t Charge_Cycle();
  uint16_t Battery_Age();

  String Serial_ID();

  // ---------------------------------------------------------------------------
  // Configuration helpers
  // ---------------------------------------------------------------------------
  bool Set_Design_Capacity(uint16_t _Capacity);
  bool Set_Config(const uint8_t _Channel, const uint16_t _Config);
  bool Set_HibCFG(const uint16_t _Config);
  bool Set_ModelCfg(const uint8_t _Model_ID, const bool _VChg);
  bool Set_Empty_Recovery_Voltage(const float _Empty_Voltage,
                                  const float _Recovery_Voltage);
  bool Set_Charge_Termination_Current();
  bool Set_Max_Min_Voltage(const float _Max_Voltage,
                           const float _Min_Voltage);
  bool Set_dQAcc(uint16_t capacity);
  bool Set_dPAcc(uint16_t capacity);

  // ---------------------------------------------------------------------------
  // Status bit helpers (read, optionally clear bits)
  // ---------------------------------------------------------------------------
  bool is_Power_on_Reset()    { return i2cReadBit(REG_Status, 0x01, true); }
  bool is_Min_Current()       { return i2cReadBit(REG_Status, 0x02, false); }
  bool is_Max_Current()       { return i2cReadBit(REG_Status, 0x06, false); }
  bool is_Min_Voltage()       { return i2cReadBit(REG_Status, 0x08, false); }
  bool is_Max_Voltage()       { return i2cReadBit(REG_Status, 0x12, false); }
  bool is_Min_Temperature()   { return i2cReadBit(REG_Status, 0x09, false); }
  bool is_Max_Temperature()   { return i2cReadBit(REG_Status, 0x13, false); }
  bool is_Min_SOC()           { return i2cReadBit(REG_Status, 0x10, false); }
  bool is_Max_SOC()           { return i2cReadBit(REG_Status, 0x14, false); }
  bool is_Battery_Present()   { return i2cReadBit(REG_Status, 0x03, false); }
  bool is_SOC_Change()        { return i2cReadBit(REG_Status, 0x07, false); }
  bool is_Battery_Insertion() { return i2cReadBit(REG_Status, 0x11, false); }
  bool is_Battery_Removal()   { return i2cReadBit(REG_Status, 0x15, false); }

  // ---------------------------------------------------------------------------
  // Connection-state callbacks (invoked on edges)
  // ---------------------------------------------------------------------------
  typedef void (*OnConnectFn)();
  typedef void (*OnDisconnectFn)();

  void setOnConnectCallback(OnConnectFn fn)      { onConnect_ = fn; }
  void setOnDisconnectCallback(OnDisconnectFn fn){ onDisconnect_ = fn; }

private:
  // I2C helpers (single-threaded now; caller must serialize)
  bool i2cWrite16(uint8_t reg, uint16_t val);
  bool i2cRead16(uint8_t reg, uint16_t& val);
  bool writeVerify16(uint8_t reg, uint16_t val);
  bool i2cReadBit(uint8_t reg, uint8_t mask, bool clearAfterRead);

  // MAX17055 helpers
  bool waitDNRclear(uint32_t timeout_ms = 1000);
  bool waitModelRefreshClear(uint32_t timeout_ms = 1000);
  bool runEZConfig();
  bool clearPOR();
  bool readVCellRaw(uint16_t& raw);

  bool setError(Error e) { last_err_ = e; return false; }

  // Robustness / probe plumbing
  bool probe(bool force = false);
  void setOnline(bool on);
  bool postOnlineInit();

  // ---------------------------------------------------------------------------
  // Internal state (single-threaded; caller provides external locking)
  // ---------------------------------------------------------------------------
  Config  cfg_{};

  volatile bool  initialized_ = false;
  volatile Error last_err_    = OK;

  volatile OnlineState online_state_ = UNKNOWN;
  uint8_t     fail_count_        = 0;
  uint32_t    last_probe_ms_     = 0;
  uint32_t    probe_interval_ms_ = 250;  // min interval for probe()

  static constexpr uint32_t PROBE_MIN_MS = 250;
  static constexpr uint32_t PROBE_MAX_MS = 8000;

  float     lastVoltage_V_ = NAN;
  float     lastSOC_pct_   = NAN;
  uint32_t  lastUpdateMs_  = 0;
  volatile bool last_fresh_ = false;

  bool allow_stale_reads_ = true;

  int   sdaPin_ = -1;
  int   sclPin_ = -1;
  float _Sense_Resistor = 0.01f; // Ω

  // Connection-state callbacks
  OnConnectFn    onConnect_    = nullptr;
  OnDisconnectFn onDisconnect_ = nullptr;

  // Registers
  static constexpr uint8_t I2C_ADDR = 0x36;

  enum : uint8_t {
    REG_Status      = 0x00,
    REG_RepSOC      = 0x06,
    REG_VCell       = 0x09,
    REG_InstantCur  = 0x0A,
    REG_AvrgCur     = 0x0B,
    REG_MixSOC      = 0x0E,
    REG_TimeToEmpty = 0x11,
    REG_INstantCap  = 0x05,
    REG_DesignCap   = 0x18,
    REG_AVeragVolt  = 0x19,
    REG_IChgTerm    = 0x1E,
    REG_Channel_1   = 0x1D,
    REG_Channel_2   = 0xBB,
    REG_FullCap     = 0x35,
    REG_VEmpty      = 0x3A,
    REG_dQAcc       = 0x45,
    REG_dPAcc       = 0x46,
    REG_FSTAT       = 0x3D,
    REG_HibCFG      = 0xBA,
    REG_ModelCFG    = 0xDB,
    REG_Command     = 0x60
  };

  enum : uint8_t {
    REG_Serial_WORD0 = 0xD4,
    REG_Serial_WORD1 = 0xD5,
    REG_Serial_WORD2 = 0xD9,
    REG_Serial_WORD3 = 0xDA,
    REG_Serial_WORD4 = 0xDC,
    REG_Serial_WORD5 = 0xDD,
    REG_Serial_WORD6 = 0xDE,
    REG_Serial_WORD7 = 0xDF
  };
};






