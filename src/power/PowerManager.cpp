#include <PowerManager.hpp>
#include <Config.hpp>
#include <Logger.hpp>
#include <RGBLed.hpp>
#include <Utils.hpp>
#include <math.h>

// -------- Singleton backing pointer --------
PowerManager* PowerManager::s_instance = nullptr;

void PowerManager::Init(TwoWire* wirePort) {
    if (!s_instance) {
        s_instance = new PowerManager(wirePort);
    } else if (wirePort) {
        s_instance->attachWire(wirePort);
    }
}

PowerManager* PowerManager::Get() {
    if (!s_instance) {
        s_instance = new PowerManager(nullptr);
    }
    return s_instance;
}

PowerManager* PowerManager::TryGet() {
    return s_instance;
}

void PowerManager::attachWire(TwoWire* wirePort) {
    if (wirePort) {
        this->wirePort = wirePort;
    }
}

#ifndef MAX17055_SDA_PIN
#define MAX17055_SDA_PIN 4
#endif
#ifndef MAX17055_SCL_PIN
#define MAX17055_SCL_PIN 5
#endif
// Sense resistor in mΩ (override from build flags or Config.h if needed)
#ifndef MAX17055_SENSE_RES_MILLIOHM
#define MAX17055_SENSE_RES_MILLIOHM 10
#endif

namespace {
    inline uint32_t ms() { return millis(); }
}

PowerManager::PowerManager(TwoWire* wirePort)
: wirePort(wirePort) {
    mtx_ = xSemaphoreCreateRecursiveMutex();

    DBGSTR();
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#                   Starting Power Manager                #");
    DBG_PRINTLN("###########################################################");
    DBGSTP();

    currentMode         = LOW_POWER;
    batteryPercentage   = 0.0f;
    batteryVoltage      = 0.0f;
    isCharging          = false;
    gaugeOnline         = false;
    gaugeDataFresh      = false;
    battInfoValid_      = false;

    lastFastTickMs_ = lastEvalMs_ = ms();
}

void PowerManager::begin() {
    pinMode(CHARGE_STATUS_PIN, INPUT_PULLUP);
    pinMode(USER_BUTTON_PIN,   INPUT_PULLUP);
    pinMode(BOOT_BUTTON_PIN,   INPUT_PULLUP);

#if POWER_CLAMP_SOC_PERCENT > 0
    // Test mode: skip I2C bring-up and use fixed SOC/voltage.
    lock_();
    batteryPercentage = (float)POWER_CLAMP_SOC_PERCENT;
    batteryVoltage    = POWER_CLAMP_VOLTAGE_V;
    gaugeOnline       = false;
    gaugeDataFresh    = false;
    battInfoValid_    = false;
    unlock_();

    lastFastTickMs_ = lastEvalMs_ = ms();
    return;
#endif

    MAX17055::Config cfg;
    cfg.designCap_mAh = 3000;
    cfg.iChgTerm_mA   = 100;
    cfg.vEmpty_mV     = 3200;
    cfg.i2cHz         = 100000;

    // Allow stale cached reads when gauge goes offline
    gauge_.setStaleReadPolicy(true);

    bool ok = gauge_.begin(
        MAX17055_SDA_PIN,
        MAX17055_SCL_PIN,
        cfg,
        MAX17055_SENSE_RES_MILLIOHM
    );

    if (!ok) {
        DBG_PRINTLN("[POWER] MAX17055 begin() FAILED – will use cached values when available.");
    } else {
        DBG_PRINTLN("[POWER] MAX17055 online and initialized.");
    }

    // Prime from gauge snapshot (NO extra I2C here)
    MAX17055::BattInfo info;
    if (gauge_.getBattInfo(info)) {
        lock_();
        lastBattInfo_   = info;
        battInfoValid_  = true;
        gaugeOnline     = (info.online == MAX17055::ONLINE);
        gaugeDataFresh  = info.dataFresh;
        batteryVoltage  = info.voltage_V;                 // FYI only
        if (!isnan(info.soc_pct)) batteryPercentage = info.soc_pct;  // SOC only
        unlock_();
    } else {
        // Try one live SOC read to initialize, if possible
        float soc = NAN;
        if (gauge_.readSOC(soc)) {
            lock_();
            batteryPercentage = soc;
            gaugeOnline       = gauge_.isOnline();
            gaugeDataFresh    = gauge_.lastDataFresh();
            unlock_();
        }
    }

    updateGaugeOnlineState();

    // Initialize schedulers
    lastFastTickMs_ = lastEvalMs_ = ms();
}


// ---- New polling API ----
void PowerManager::service() {
    const uint32_t now = ms();

    // Fast gauge heartbeat
    if ((now - lastFastTickMs_) >= POWER_FAST_TICK_MS) {
        fastTick();
        lastFastTickMs_ = now;
    }

    // Slow power-mode evaluation
    if ((now - lastEvalMs_) >= POWER_MODE_UPDATE) {
        updatePowerMode();
        lastEvalMs_ = now;
    }
}

void PowerManager::fastTick() {
#if POWER_CLAMP_SOC_PERCENT > 0
    // Test mode: no I2C probing; just keep charge status updated.
    updateChargeStatus();
    return;
#endif
    gauge_.tick();              // quick online/offline probe + cache maintenance
    updateGaugeOnlineState();   // reflect ONLINE/OFFLINE edges and print once
    // Optional: status pin sample
    updateChargeStatus();
}

bool PowerManager::evalIfDue() {
    const uint32_t now = ms();
    if ((now - lastEvalMs_) >= POWER_MODE_UPDATE) {
        updatePowerMode();
        lastEvalMs_ = now;
        return true;
    }
    return false;
}

void PowerManager::forceEvaluate() {
    updatePowerMode();
    lastEvalMs_ = ms();
}

// ---- Data access / logic ----
float PowerManager::getBatteryPercentage() {
#if POWER_CLAMP_SOC_PERCENT > 0
    lock_();
    batteryPercentage = (float)POWER_CLAMP_SOC_PERCENT;
    batteryVoltage    = POWER_CLAMP_VOLTAGE_V;
    gaugeOnline       = false;
    gaugeDataFresh    = false;
    float outClamp = batteryPercentage;
    unlock_();
    return outClamp;
#endif
    // Strictly use SOC from MAX17055; fall back to cached value if needed.
    float soc = NAN;
    bool ok   = gauge_.readSOC(soc);     // live if online; respects stale policy internally

    MAX17055::BattInfo info;
    bool haveInfo = gauge_.getBattInfo(info);  // NO I2C

    lock_();
    if (ok) {
        batteryPercentage = soc;
        if (haveInfo) {
            lastBattInfo_   = info;
            battInfoValid_  = true;
            gaugeOnline     = (info.online == MAX17055::ONLINE);
            gaugeDataFresh  = info.dataFresh;
            batteryVoltage  = info.voltage_V;  // optional display
        } else {
            gaugeOnline     = gauge_.isOnline();
            gaugeDataFresh  = gauge_.lastDataFresh();
        }
    } else {
        // Keep last known SOC; just refresh online/fresh flags if we can
        if (haveInfo) {
            lastBattInfo_   = info;
            battInfoValid_  = true;
            gaugeOnline     = (info.online == MAX17055::ONLINE);
            gaugeDataFresh  = info.dataFresh;
            if (!isnan(info.soc_pct)) batteryPercentage = info.soc_pct;
            batteryVoltage  = info.voltage_V;
        } else {
            gaugeOnline     = gauge_.isOnline();
            gaugeDataFresh  = gauge_.lastDataFresh();
        }
    }
    float out = batteryPercentage;
    unlock_();

    /*DBG_PRINT(F("[POWER] Battery SOC : "));
    DBG_PRINT(out, 1);
    DBG_PRINTLN(F(" %🔋"));*/

    return out;
}

PowerMode PowerManager::getPowerMode() {
    lock_(); PowerMode m = currentMode; unlock_(); return m;
}

void PowerManager::setPowerMode(PowerMode mode) {
    lock_(); currentMode = mode; unlock_();
}

void PowerManager::updatePowerMode() {
#if POWER_CLAMP_SOC_PERCENT <= 0
    updateGaugeOnlineState();
#endif
    float pct = getBatteryPercentage();   // SOC %
    lock_(); PowerMode cur = currentMode; unlock_();

    // ========================================================
    //        Critical Power (≤ 3%)
    // ========================================================
    if (pct <= 3 && cur != CRITICAL_POWER_MODE) {
        setPowerMode(CRITICAL_POWER_MODE);
        DBG_PRINTLN("[POWER] ⚠️ CRITICAL: Battery ≤3%! System protection mode engaged.");
        if (RGB) RGB->postOverlay(OverlayEvent::CRITICAL_BATT);
        if (LOGG) LOGG->logBatteryLow("CRITICAL: Battery ≤3%! ⚠️");
        return;
    }

    // ========================================================
    //        Emergency Power (>3% and ≤6%)
    // ========================================================
    if (pct > 3 && pct <= 6 && cur != EMERGENCY_POWER_MODE) {
        setPowerMode(EMERGENCY_POWER_MODE);
        DBG_PRINTLN("[POWER] 🚨 EMERGENCY: Battery 3–6%! Restricting operations.");
        if (RGB) RGB->postOverlay(OverlayEvent::LOW_BATT);
        if (LOGG) LOGG->logBatteryLow("EMERGENCY: Battery 3–6%! ⚠️");
        return;
    }

    // ========================================================
    //        Low Power (>6% and ≤10%)
    // ========================================================
    if (pct > 6 && pct <= 10 && cur != LOW_POWER) {
        setPowerMode(LOW_POWER);
        DBG_PRINTLN("[POWER] LowPower battery! 🔋⚠️");
        if (RGB) RGB->postOverlay(OverlayEvent::LOW_BATT);
        if (LOGG) LOGG->logBatteryLow("[POWER] LowPower battery! 🔋⚠️");
        DBG_PRINTLN("[POWER] Power mode set to 10%. 💡🔋");
        return;
    }

    // ========================================================
    //        Normal Power Ranges (purely SOC-based)
    // ========================================================
    else if (pct > 10 && pct <= 20 && cur != POWER_20) {
        setPowerMode(POWER_20);  DBG_PRINTLN("[POWER] Power mode set to 20%. 💡⚡");
    } else if (pct > 20 && pct <= 30 && cur != POWER_30) {
        setPowerMode(POWER_30);  DBG_PRINTLN("[POWER] Power mode set to 30%. 💡⚡");
    } else if (pct > 30 && pct <= 40 && cur != POWER_40) {
        setPowerMode(POWER_40);  DBG_PRINTLN("[POWER] Power mode set to 40%. 💡⚡");
    } else if (pct > 40 && pct <= 50 && cur != POWER_50) {
        setPowerMode(POWER_50);  DBG_PRINTLN("[POWER] Power mode set to 50%. 💡🔋");
    } else if (pct > 50 && pct <= 60 && cur != POWER_60) {
        setPowerMode(POWER_60);  DBG_PRINTLN("[POWER] Power mode set to 60%. 💡🔋");
    } else if (pct > 60 && pct <= 70 && cur != POWER_70) {
        setPowerMode(POWER_70);  DBG_PRINTLN("[POWER] Power mode set to 70%. 💡🔋");
    } else if (pct > 70 && pct <= 80 && cur != POWER_80) {
        setPowerMode(POWER_80);  DBG_PRINTLN("[POWER] Power mode set to 80%. 💡🔋");
    } else if (pct > 80 && pct <= 90 && cur != POWER_90) {
        setPowerMode(POWER_90);  DBG_PRINTLN("[POWER] Power mode set to 90%. 💡🔋");
    } else if (pct > 90 && pct <= 100 && cur != FULL_POWER) {
        setPowerMode(FULL_POWER);DBG_PRINTLN("[POWER] Power mode set to full power. ⚡💡");
    }
}

void PowerManager::updateChargeStatus() {
    bool ch = (digitalRead(CHARGE_STATUS_PIN) == HIGH);
    lock_(); isCharging = ch; unlock_();
}

bool PowerManager::isGaugeOnline() const {
    lock_(); bool v = gaugeOnline; unlock_(); return v;
}
bool PowerManager::isBatteryDataFresh() const {
    lock_(); bool v = gaugeDataFresh; unlock_(); return v;
}

bool PowerManager::getBatteryInfo(MAX17055::BattInfo& infoOut) const {
#if POWER_CLAMP_SOC_PERCENT > 0
    MAX17055::BattInfo info{};
    info.online    = MAX17055::OFFLINE;
    info.dataFresh = false;
    info.soc_pct   = POWER_CLAMP_SOC_PERCENT;
    info.voltage_V = POWER_CLAMP_VOLTAGE_V;
    infoOut = info;
    return true;
#endif
    // First try our own cached snapshot
    lock_();
    bool valid = battInfoValid_;
    if (valid) { infoOut = lastBattInfo_; unlock_(); return true; }
    unlock_();

    // No snapshot yet – ask gauge for its cached state (NO I2C)
    MAX17055::BattInfo tmp;
    bool have = gauge_.getBattInfo(tmp);
    if (have) {
        lock_();
        lastBattInfo_  = tmp;   // allowed because lastBattInfo_ is mutable
        battInfoValid_ = true;  // mutable
        infoOut        = tmp;
        unlock_();
        return true;
    }
    return false;
}

void PowerManager::updateGaugeOnlineState() {
#if POWER_CLAMP_SOC_PERCENT > 0
    return;
#endif
    MAX17055::OnlineState cur = gauge_.onlineState();

    lock_();
    if (cur == lastOnlineState_) { unlock_(); return; }
    lastOnlineState_ = cur;
    gaugeOnline      = (cur == MAX17055::ONLINE);
    unlock_();

    if (cur == MAX17055::ONLINE) {
        DBG_PRINTLN("[POWER] Battery gauge ONLINE ✅.");
    } else if (cur == MAX17055::OFFLINE) {
        DBG_PRINTLN("[POWER] Battery gauge OFFLINE ⚠️ (serving cached values).");
        if (LOGG) LOGG->logBatteryLow("Battery gauge offline; serving cached values.");
    } else {
        DBG_PRINTLN("[POWER] Battery gauge state UNKNOWN.");
    }
}
