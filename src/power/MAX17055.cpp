#include <MAX17055.hpp>

namespace { inline uint32_t ms() { return millis(); } }

// -----------------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------------
MAX17055::MAX17055() {
  // Initial cached values
  lastVoltage_V_ = NAN;
  lastSOC_pct_   = NAN;
  lastUpdateMs_  = 0;
  last_fresh_    = false;
}

// -----------------------------------------------------------------------------
// Public: begin / tick
// -----------------------------------------------------------------------------
bool MAX17055::begin(int sdaPin, int sclPin, const Config& cfg, float SenseRes) {
  return beginOnBus(sdaPin, sclPin, cfg, SenseRes, true);
}

bool MAX17055::beginOnBus(int sdaPin,
                          int sclPin,
                          const Config& cfg,
                          float SenseRes,
                          bool initBus) {
  // Configure instance
  _Sense_Resistor = SenseRes / 1000.0f; // user passes mIc - convert to Ic
  cfg_            = cfg;
  last_err_       = OK;
  sdaPin_         = sdaPin;
  sclPin_         = sclPin;

  // Init I2C bus if requested
  if (initBus) {
    Wire.begin(sdaPin_, sclPin_, cfg_.i2cHz);
  } else {
    Wire.setClock(cfg_.i2cHz);
  }
  Wire.setTimeOut(50);

  // Basic configuration (each I2C op is self-contained)
  (void)Set_Config(1, 0x0000);
  (void)Set_Config(2, 0x0218);
  (void)Set_HibCFG(0x0000);
  (void)Set_Design_Capacity(cfg_.designCap_mAh);
  (void)Set_ModelCfg(2, false);
  (void)Set_Empty_Recovery_Voltage(3.0f, 4.1f);
  (void)Set_Max_Min_Voltage(3.0f, 4.2f);
  (void)Set_Charge_Termination_Current();

  delay(50);

  // First probe + post-online init if applicable
  (void)probe(true);

  return isOnline();
}

void MAX17055::tick() {
  // Optional heartbeat to keep online/offline state fresh
  (void)probe(false);
}

// -----------------------------------------------------------------------------
// Unified device info snapshot (no additional I2C here)
// -----------------------------------------------------------------------------
bool MAX17055::getBattInfo(BattInfo& info_out) const {
  info_out.online       = online_state_;
  info_out.lastError    = last_err_;
  info_out.initialized  = initialized_;
  info_out.dataFresh    = last_fresh_;
  info_out.allowStale   = allow_stale_reads_;
  info_out.lastUpdateMs = lastUpdateMs_;
  info_out.voltage_V    = lastVoltage_V_;
  info_out.soc_pct      = lastSOC_pct_;

  // Extra fields not maintained in this minimal no-RTOS variant (leave as-is)
  // Caller (PowerManager) can compute/store if desired.

  return (!isnan(info_out.voltage_V) || !isnan(info_out.soc_pct));
}

// -----------------------------------------------------------------------------
// Public: measurement API
// -----------------------------------------------------------------------------
bool MAX17055::readVoltage(float& volts_out) {
  uint16_t raw;
  if (!readVCellRaw(raw)) {
    if (!isOnline() && allow_stale_reads_ && !isnan(lastVoltage_V_)) {
      volts_out   = lastVoltage_V_;
      last_fresh_ = false;
      return true;
    }
    return false;
  }

  // From datasheet: 78.125 µV/LSB
  const float v  = raw * 0.000078125f;
  const float vr = floorf(v * 10.0f) / 10.0f;  // 0.1 V resolution

  volts_out      = vr;
  lastVoltage_V_ = vr;
  lastUpdateMs_  = ms();
  last_fresh_    = true;
  last_err_      = OK;
  return true;
}

bool MAX17055::readVoltageFiltered(float& volts_out) {
  // For now just alias to readVoltage; could use Average_Voltage()
  return readVoltage(volts_out);
}

bool MAX17055::readSOC(float& soc_percent) {
  uint16_t raw = 0;

  if (!i2cRead16(REG_MixSOC, raw)) {
    if (!isOnline() && allow_stale_reads_ && !isnan(lastSOC_pct_)) {
      soc_percent = lastSOC_pct_;
      last_fresh_ = false;
      return true;
    }
    return false;
  }

  // Convert raw to percent: upper 8 bits integer, lower 8 bits fractional
  float soc = float((raw >> 8) & 0xFF) + float(raw & 0xFF) / 256.0f;

  soc_percent  = soc;
  lastSOC_pct_ = soc;
  lastUpdateMs_= ms();
  last_fresh_  = true;
  last_err_    = OK;
  return true;
}

// -----------------------------------------------------------------------------
// Extra measurement helpers (kept for compatibility)
// -----------------------------------------------------------------------------
float MAX17055::Instant_Voltage() {
  uint16_t raw = 0;
  if (!i2cRead16(REG_VCell, raw)) {
    if (!isOnline() && allow_stale_reads_ && !isnan(lastVoltage_V_)) {
      return lastVoltage_V_;
    }
    return NAN;
  }
  float voltage = raw * 0.000078125f;
  lastVoltage_V_ = voltage;
  lastUpdateMs_  = ms();
  last_fresh_    = true;
  return voltage;
}

float MAX17055::Average_Voltage() {
  const uint8_t READ_COUNT = 5;
  float sum = 0.0f;
  for (uint8_t i = 0; i < READ_COUNT; i++) {
    uint16_t raw = 0;
    if (!i2cRead16(REG_AVeragVolt, raw)) {
      return (!isOnline() && allow_stale_reads_) ? lastVoltage_V_ : NAN;
    }
    sum += (raw * 0.000078125f);
    delay(2);
  }
  float avg = sum / READ_COUNT;
  return avg;
}

float MAX17055::Empty_Voltage() {
  const uint8_t READ_COUNT = 5;
  float sum = 0.0f;
  for (uint8_t i = 0; i < READ_COUNT; i++) {
    uint16_t raw = 0;
    if (!i2cRead16(REG_VEmpty, raw)) return NAN;
    uint16_t value = (raw & 0xFF80) >> 7;   // 10 mV / LSB
    float voltage  = (float)value * 0.010f;
    sum += voltage;
    delay(2);
  }
  return sum / READ_COUNT;
}

float MAX17055::Recovery_Voltage() {
  const uint8_t READ_COUNT = 5;
  float sum = 0.0f;
  for (uint8_t i = 0; i < READ_COUNT; i++) {
    uint16_t raw = 0;
    if (!i2cRead16(REG_VEmpty, raw)) return NAN;
    uint16_t value = raw & 0x007F; // 40 mV / LSB
    float voltage  = (float)value * 0.040f;
    sum += voltage;
    delay(2);
  }
  return sum / READ_COUNT;
}

float MAX17055::Instant_Current() {
  const uint8_t READ_COUNT = 5;
  float samples[READ_COUNT];

  for (uint8_t i = 0; i < READ_COUNT; i++) {
    uint16_t raw = 0;
    if (!i2cRead16(REG_InstantCur, raw)) {
      return NAN;
    }

    bool negative = ((raw >> 12) == 0xF);
    if (negative) raw = 0xFFFF - raw;

    // 1.5625 µV / LSB across sense resistor -> mA
    float current = (raw * 1.5625f) / _Sense_Resistor / 1000.0f;
    if (negative) current = -current;

    samples[i] = current;
  }

  float avg = 0.0f;
  for (uint8_t i = 0; i < READ_COUNT; i++) avg += samples[i];
  avg /= READ_COUNT;

  return avg;
}

float MAX17055::Average_Current() {
  const uint8_t READ_COUNT = 5;
  float samples[READ_COUNT];

  for (uint8_t i = 0; i < READ_COUNT; i++) {
    uint16_t raw = 0;
    if (!i2cRead16(REG_AvrgCur, raw)) {
      return NAN;
    }

    bool negative = ((raw >> 12) == 0xF);
    if (negative) raw = 0xFFFF - raw;

    float current = (raw * 1.5625f) / _Sense_Resistor / 1000.0f;
    if (negative) current = -current;

    samples[i] = current;
  }

  float avg = 0.0f;
  for (uint8_t i = 0; i < READ_COUNT; i++) avg += samples[i];
  avg /= READ_COUNT;

  return avg;
}

float MAX17055::AverageSOC() {
  const uint8_t READ_COUNT = 5;
  float samples[READ_COUNT];

  for (uint8_t i = 0; i < READ_COUNT; i++) {
    uint16_t raw = 0;
    if (!i2cRead16(REG_MixSOC, raw)) {
      return (!isOnline() && allow_stale_reads_) ? lastSOC_pct_ : NAN;
    }
    float soc = float((raw >> 8) & 0xFF) + float(raw & 0xFF) / 256.0f;
    samples[i] = soc;
  }

  float avg = 0.0f;
  for (uint8_t i = 0; i < READ_COUNT; i++) avg += samples[i];
  avg /= READ_COUNT;

  lastSOC_pct_  = avg;
  lastUpdateMs_ = ms();
  last_fresh_   = true;

  return avg;
}

uint16_t MAX17055::Instant_Capacity() {
  uint16_t raw = 0;
  if (!i2cRead16(REG_INstantCap, raw)) {
    return 0;
  }
  uint16_t capacity = (uint16_t)((raw * 5) / 1000.0f / _Sense_Resistor);
  return capacity;
}

uint16_t MAX17055::Design_Capacity() {
  uint16_t raw = 0;
  if (!i2cRead16(REG_DesignCap, raw)) {
    return cfg_.designCap_mAh;
  }
  uint16_t capacity = (uint16_t)((raw * 5) / 1000.0f / _Sense_Resistor);
  return capacity;
}

uint16_t MAX17055::Full_Capacity() {
  uint16_t raw = 0;
  if (!i2cRead16(REG_FullCap, raw)) {
    return 0;
  }
  uint16_t capacity = (uint16_t)((raw * 5) / 1000.0f / _Sense_Resistor);
  return capacity;
}

uint16_t MAX17055::Time_To_Empty() {
  const uint8_t READ_COUNT = 5;
  float samples[READ_COUNT];

  for (uint8_t i = 0; i < READ_COUNT; i++) {
    uint16_t raw = 0;
    if (!i2cRead16(REG_TimeToEmpty, raw)) {
      return 0;
    }
    float hours = ((float)raw * 5.625f) / 3600.0f;
    samples[i] = hours;
  }

  float avg = 0.0f;
  for (uint8_t i = 0; i < READ_COUNT; i++) avg += samples[i];
  avg /= READ_COUNT;

  return (uint16_t)avg;
}

uint16_t MAX17055::Battery_Age() {
  uint16_t raw = 0;
  if (!i2cRead16(REG_RepSOC + 1, raw)) {
    return 0;
  }
  return raw;
}

uint16_t MAX17055::Charge_Cycle() {
  const uint8_t READ_COUNT = 5;
  float samples[READ_COUNT];

  for (uint8_t i = 0; i < READ_COUNT; i++) {
    uint16_t raw = 0;
    if (!i2cRead16(0x17, raw)) {
      return 0;
    }
    samples[i] = raw;
  }
  float avg = 0.0f;
  for (uint8_t i = 0; i < READ_COUNT; i++) avg += samples[i];
  avg /= READ_COUNT;

  return (uint16_t)avg;
}

// -----------------------------------------------------------------------------
// Config helpers
// -----------------------------------------------------------------------------
bool MAX17055::Set_Design_Capacity(uint16_t capacity) {
  if (!i2cWrite16(REG_DesignCap, capacity)) return false;
  return true;
}

bool MAX17055::Set_Config(const uint8_t channel, const uint16_t config) {
  if (channel == 1)      return i2cWrite16(REG_Channel_1, config);
  else if (channel == 2) return i2cWrite16(REG_Channel_2, config);
  return false;
}

bool MAX17055::Set_HibCFG(const uint16_t config) {
  return i2cWrite16(REG_HibCFG, config);
}

bool MAX17055::Set_ModelCfg(const uint8_t modelID, const bool vChg) {
  uint16_t data = 0x0000;
  if (vChg)  bitSet(data, 10); else bitClear(data, 10);

  if      (modelID == 0) { bitClear(data,4); bitClear(data,5); bitClear(data,6); bitClear(data,7); }
  else if (modelID == 2) { bitClear(data,4); bitSet(data,5);   bitClear(data,6); bitClear(data,7); }
  else if (modelID == 6) { bitClear(data,4); bitSet(data,5);   bitSet(data,6);   bitClear(data,7); }

  return i2cWrite16(REG_ModelCFG, data);
}

bool MAX17055::Set_Empty_Recovery_Voltage(const float emptyVoltage,
                                          const float recoveryVoltage) {
  uint32_t rawEmpty    = (uint32_t(emptyVoltage * 1000.0f) / 10);
  rawEmpty             = (rawEmpty << 7) & 0xFF80;

  uint32_t rawRecovery = (uint32_t(recoveryVoltage * 1000.0f) / 40) & 0x7F;

  uint16_t raw = (uint16_t)(rawEmpty | rawRecovery);
  return i2cWrite16(REG_VEmpty, raw);
}

bool MAX17055::Set_Charge_Termination_Current() {
  uint16_t rawTerm = 0x0280;
  return i2cWrite16(REG_IChgTerm, rawTerm);
}

bool MAX17055::Set_Max_Min_Voltage(float maxVoltage, float minVoltage) {
  uint16_t rawMax = (uint16_t)((maxVoltage * 1000.0f) / 20.0f);
  rawMax = (rawMax << 7) & 0xFF80;

  uint16_t rawMin = (uint16_t)((minVoltage * 1000.0f) / 20.0f) & 0x007F;

  uint16_t rawValue = rawMax | rawMin;
  return i2cWrite16(REG_VEmpty, rawValue);
}

bool MAX17055::Set_dQAcc(uint16_t capacity) {
  uint16_t raw = capacity / 32;
  return i2cWrite16(REG_dQAcc, raw);
}

bool MAX17055::Set_dPAcc(uint16_t capacity) {
  if (capacity == 0) return setError(BAD_VALUE);

  uint16_t dQRaw = 0;
  if (!i2cRead16(REG_dQAcc, dQRaw)) return false;

  uint16_t dPRaw = (uint16_t)((uint32_t)dQRaw * 51200u / capacity);
  return i2cWrite16(REG_dPAcc, dPRaw);
}

// -----------------------------------------------------------------------------
// Serial ID
// -----------------------------------------------------------------------------
String MAX17055::Serial_ID() {
  const uint8_t words[8] = {
    REG_Serial_WORD0, REG_Serial_WORD1, REG_Serial_WORD2, REG_Serial_WORD3,
    REG_Serial_WORD4, REG_Serial_WORD5, REG_Serial_WORD6, REG_Serial_WORD7
  };

  String serial = "";
  for (int i = 7; i >= 0; --i) {
    uint16_t val = 0;
    if (!i2cRead16(words[i], val)) {
      return serial; // empty if failed
    }
    if (val < 0x1000) serial += "0";
    if (val < 0x0100) serial += "0";
    if (val < 0x0010) serial += "0";
    serial += String(val, HEX);
  }
  serial.toUpperCase();
  return serial;
}

// -----------------------------------------------------------------------------
// Private: I2C helpers
// -----------------------------------------------------------------------------
bool MAX17055::i2cWrite16(uint8_t reg, uint16_t val) {
  Wire.beginTransmission(I2C_ADDR);
  Wire.write(reg);
  Wire.write(uint8_t(val & 0xFF));
  Wire.write(uint8_t((val >> 8) & 0xFF));
  int rc = Wire.endTransmission();
  if (rc != 0) {
    setOnline(false);
    return setError(I2C_ERROR);
  }
  return true;
}

bool MAX17055::i2cRead16(uint8_t reg, uint16_t& val) {
  Wire.beginTransmission(I2C_ADDR);
  Wire.write(reg);
  int rc = Wire.endTransmission(false); // repeated start
  if (rc == 0) {
    int n = Wire.requestFrom((int)I2C_ADDR, 2);
    if (n == 2) {
      uint8_t lo = Wire.read();
      uint8_t hi = Wire.read();
      val = (uint16_t(hi) << 8) | lo;
      return true;
    }
  }
  setOnline(false);
  return setError(I2C_ERROR);
}

bool MAX17055::i2cReadBit(uint8_t reg, uint8_t mask, bool clearAfterRead) {
  Wire.beginTransmission(I2C_ADDR);
  Wire.write(reg);
  int rc = Wire.endTransmission(false);
  if (rc != 0) {
    setOnline(false);
    return setError(I2C_ERROR);
  }

  int n = Wire.requestFrom((int)I2C_ADDR, 1);
  if (n != 1) {
    setOnline(false);
    return setError(I2C_ERROR);
  }

  uint8_t value = Wire.read();
  bool result   = (value & mask) != 0;

  if (clearAfterRead) {
    value &= ~mask;
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
  }
  return result;
}

bool MAX17055::writeVerify16(uint8_t reg, uint16_t val) {
  for (int i = 0; i < 3; ++i) {
    if (!i2cWrite16(reg, val)) { delay(1); continue; }
    uint16_t rb = 0;
    if (!i2cRead16(reg, rb))   { delay(1); continue; }
    if (rb == val) return true;
  }
  return setError(I2C_ERROR);
}

// -----------------------------------------------------------------------------
// Private: core helpers
// -----------------------------------------------------------------------------
bool MAX17055::waitDNRclear(uint32_t timeout_ms) {
  uint32_t t0 = ms();
  while (ms() - t0 < timeout_ms) {
    uint16_t fstat = 0;
    if (i2cRead16(REG_FSTAT, fstat)) {
      if ((fstat & 0x0001) == 0) return true; // DNR == 0
    } else {
      return setError(TIMEOUT);
    }
    delay(10);
  }
  return setError(TIMEOUT);
}

bool MAX17055::waitModelRefreshClear(uint32_t timeout_ms) {
  uint32_t t0 = ms();
  while (ms() - t0 < timeout_ms) {
    uint16_t modelcfg = 0;
    if (i2cRead16(REG_ModelCFG, modelcfg)) {
      if ((modelcfg & 0x8000) == 0) return true;
    } else {
      return setError(TIMEOUT);
    }
    delay(10);
  }
  return setError(TIMEOUT);
}

bool MAX17055::runEZConfig() {
  if (!waitDNRclear()) return false;

  uint16_t dqacc = cfg_.designCap_mAh / 32;
  uint16_t dpacc = (uint16_t)((float)dqacc * 44138.0f / (float)cfg_.designCap_mAh);

  if (!writeVerify16(REG_DesignCap, cfg_.designCap_mAh)) return false;
  if (!writeVerify16(REG_dQAcc,     dqacc))               return false;
  if (!writeVerify16(REG_IChgTerm,  cfg_.iChgTerm_mA))    return false;
  if (!writeVerify16(REG_VEmpty,    cfg_.vEmpty_mV))      return false;

  uint16_t hibcfg = 0;
  if (!i2cRead16(REG_HibCFG, hibcfg)) return false;

  // Exit hibernate, reconfigure, issue ModelCfg, wait refresh
  if (!i2cWrite16(REG_Command, 0x0090)) return false; // exit Hibernate
  if (!i2cWrite16(REG_HibCFG,  0x0000)) return false;
  if (!i2cWrite16(REG_Command, 0x0000)) return false;

  uint16_t dQRaw = 0;
  if (!i2cRead16(REG_dQAcc, dQRaw)) return false;
  uint16_t dPRaw = (uint16_t)((uint32_t)dQRaw * 51200u / cfg_.designCap_mAh);
  if (!i2cWrite16(REG_dPAcc,  dPRaw)) return false;

  if (!writeVerify16(REG_ModelCFG, 0x8000)) return false;
  if (!waitModelRefreshClear())            return false;

  if (!writeVerify16(REG_HibCFG, hibcfg))  return false;

  return clearPOR();
}

bool MAX17055::clearPOR() {
  uint16_t status = 0;
  if (!i2cRead16(REG_Status, status)) return false;
  status &= ~0x0002;
  return writeVerify16(REG_Status, status);
}

bool MAX17055::readVCellRaw(uint16_t& raw) {
  return i2cRead16(REG_VCell, raw);
}

// -----------------------------------------------------------------------------
// Robustness / probe plumbing (now called via tick())
// -----------------------------------------------------------------------------
bool MAX17055::probe(bool force) {
  const uint32_t now = ms();
  if (!force) {
    if (now - last_probe_ms_ < probe_interval_ms_) {
      return isOnline();
    }
  }
  last_probe_ms_ = now;

  // Simple address probe (no register)
  Wire.beginTransmission(I2C_ADDR);
  int rc = Wire.endTransmission();

  if (rc == 0) {
    setOnline(true);
    (void)postOnlineInit();
    fail_count_        = 0;
    probe_interval_ms_ = PROBE_MIN_MS;
    return true;
  }

  setOnline(false);
  fail_count_ = std::min<uint8_t>(fail_count_ + 1, 10);
  uint32_t next = PROBE_MIN_MS << std::min<uint8_t>(fail_count_, 5);
  probe_interval_ms_ = (next > PROBE_MAX_MS) ? PROBE_MAX_MS : next;
  return false;
}

void MAX17055::setOnline(bool on) {
  OnlineState prev = online_state_;
  OnlineState ns   = on ? ONLINE : OFFLINE;

  if (ns != prev) {
    online_state_ = ns;
    if (on) {
      initialized_ = true;
      last_err_    = OK;
      last_fresh_  = false;
      if (onConnect_) onConnect_();
    } else {
      initialized_ = false;
      last_err_    = I2C_ERROR;
      last_fresh_  = false;
      if (onDisconnect_) onDisconnect_();
    }
  }
}

bool MAX17055::postOnlineInit() {
  uint16_t status = 0;
  if (!i2cRead16(REG_Status, status)) return setError(I2C_ERROR);

  // If POR bit set, run EZ Config
  if (status & 0x0002) {
    if (!runEZConfig()) return setError(INIT_FAILED);
  }
  initialized_ = true;
  return true;
}
