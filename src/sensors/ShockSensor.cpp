#include <ShockSensor.hpp>
#include <ConfigNvs.hpp>
#include <I2CBusManager.hpp>
#include <Logger.hpp>
#include <NVSManager.hpp>
#include <Utils.hpp>

namespace {
    bool reinitL2dCb_(void* ctx) {
        auto* self = static_cast<ShockSensor*>(ctx);
        return self ? self->reinitI2C() : false;
    }
}

// Static instance for ISR thunk
ShockSensor* ShockSensor::s_instance_ = nullptr;

// Constructor
ShockSensor::ShockSensor() {
    DBGSTR();DBG_PRINTLN();
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#                 Starting Shock Manager                  #");
    DBG_PRINTLN("###########################################################");
    DBGSTP();

    triggered = false;
    armed     = true;

    s_instance_ = this;
}

ShockConfig ShockSensor::loadConfig(NVS* nvs) {
    ShockConfig cfg{};
    cfg.type      = nvs ? nvs->GetInt(SHOCK_SENSOR_TYPE_KEY, SHOCK_SENSOR_TYPE_DEFAULT)
                        : SHOCK_SENSOR_TYPE_DEFAULT;
    cfg.threshold = nvs ? nvs->GetInt(SHOCK_SENS_THRESHOLD_KEY, SHOCK_SENS_THRESHOLD_DEFAULT)
                        : SHOCK_SENS_THRESHOLD_DEFAULT;
    cfg.odr       = nvs ? nvs->GetInt(SHOCK_L2D_ODR_KEY, SHOCK_L2D_ODR_DEFAULT)
                        : SHOCK_L2D_ODR_DEFAULT;
    cfg.scale     = nvs ? nvs->GetInt(SHOCK_L2D_SCALE_KEY, SHOCK_L2D_SCALE_DEFAULT)
                        : SHOCK_L2D_SCALE_DEFAULT;
    cfg.res       = nvs ? nvs->GetInt(SHOCK_L2D_RES_KEY, SHOCK_L2D_RES_DEFAULT)
                        : SHOCK_L2D_RES_DEFAULT;
    cfg.evtMode   = nvs ? nvs->GetInt(SHOCK_L2D_EVT_MODE_KEY, SHOCK_L2D_EVT_MODE_DEFAULT)
                        : SHOCK_L2D_EVT_MODE_DEFAULT;
    cfg.dur       = nvs ? nvs->GetInt(SHOCK_L2D_DUR_KEY, SHOCK_L2D_DUR_DEFAULT)
                        : SHOCK_L2D_DUR_DEFAULT;
    cfg.axisMask  = nvs ? nvs->GetInt(SHOCK_L2D_AXIS_KEY, SHOCK_L2D_AXIS_DEFAULT)
                        : SHOCK_L2D_AXIS_DEFAULT;
    cfg.hpfMode   = nvs ? nvs->GetInt(SHOCK_L2D_HPF_MODE_KEY, SHOCK_L2D_HPF_MODE_DEFAULT)
                        : SHOCK_L2D_HPF_MODE_DEFAULT;
    cfg.hpfCut    = nvs ? nvs->GetInt(SHOCK_L2D_HPF_CUT_KEY, SHOCK_L2D_HPF_CUT_DEFAULT)
                        : SHOCK_L2D_HPF_CUT_DEFAULT;
    cfg.hpfEn     = nvs ? nvs->GetBool(SHOCK_L2D_HPF_EN_KEY, SHOCK_L2D_HPF_EN_DEFAULT)
                        : SHOCK_L2D_HPF_EN_DEFAULT;
    cfg.latch     = nvs ? nvs->GetBool(SHOCK_L2D_LATCH_KEY, SHOCK_L2D_LATCH_DEFAULT)
                        : SHOCK_L2D_LATCH_DEFAULT;
    cfg.intLevel  = nvs ? nvs->GetInt(SHOCK_L2D_INT_LVL_KEY, SHOCK_L2D_INT_LVL_DEFAULT)
                        : SHOCK_L2D_INT_LVL_DEFAULT;
    return sanitizeConfig(cfg);
}

ShockConfig ShockSensor::sanitizeConfig(ShockConfig cfg) {
    cfg.type = (cfg.type == SHOCK_SENSOR_TYPE_INTERNAL) ? SHOCK_SENSOR_TYPE_INTERNAL
                                                        : SHOCK_SENSOR_TYPE_EXTERNAL;
    cfg.threshold &= 0x7F;
    if (cfg.odr > L2D_ODR_5000)    cfg.odr = SHOCK_L2D_ODR_DEFAULT;
    if (cfg.scale > L2D_SCALE_16G)cfg.scale = SHOCK_L2D_SCALE_DEFAULT;
    if (cfg.res > L2D_RES_H)      cfg.res = SHOCK_L2D_RES_DEFAULT;
    if (cfg.evtMode > L2D_EVT_4D_POS) cfg.evtMode = SHOCK_L2D_EVT_MODE_DEFAULT;
    if (cfg.hpfMode > L2D_HPF_AUTO)   cfg.hpfMode = SHOCK_L2D_HPF_MODE_DEFAULT;
    if (cfg.hpfCut > 3)               cfg.hpfCut = SHOCK_L2D_HPF_CUT_DEFAULT;
    cfg.axisMask &= 0x3F;
    if (cfg.axisMask == 0)            cfg.axisMask = SHOCK_L2D_AXIS_DEFAULT;
    cfg.dur &= 0x7F;
    cfg.intLevel = cfg.intLevel ? 1 : 0;
    return cfg;
}

bool ShockSensor::begin(const ShockConfig& cfg) {
    return applyConfig(cfg);
}

bool ShockSensor::applyConfig(const ShockConfig& cfg) {
    cfg_ = sanitizeConfig(cfg);
    internal_ = (cfg_.type == SHOCK_SENSOR_TYPE_INTERNAL);
    l2dReady_ = false;
    edgeFlag_ = false;
    triggered = false;
    armed     = true;

    if (rearmTimer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &ShockSensor::RearmTimerCb;
        args.arg      = this;
        args.name     = "shock_rearm";
        esp_err_t ok  = esp_timer_create(&args, &rearmTimer);

        DBGSTR();
        if (ok == ESP_OK) {
            DBG_PRINTLN("[Shock] Rearm timer created");
        } else {
            DBG_PRINTLN("[Shock] Failed to create rearm timer!");
        }
        DBGSTP();
    }

    detachInterrupt_();
    if (internal_) {
        return configureInternal_(cfg_);
    }
    configureExternal_();
    return true;
}

void ShockSensor::disable() {
    detachInterrupt_();
    edgeFlag_ = false;
    triggered = false;
    armed     = true;

    if (internal_ && l2dReady_) {
        (void)l2d_.mode(L2D_ODR_PD, L2D_RES_LP, false, false, false);
    }
    l2dReady_ = false;
}

bool ShockSensor::reinitI2C() {
    if (!internal_) {
        return true;
    }
    return applyConfig(cfg_);
}

void ShockSensor::configureExternal_() {
    pinMode(SHOCK_SENSOR1_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SHOCK_SENSOR1_PIN),
                    ShockSensor::isrThunk_,
                    FALLING);  // active-low
}

bool ShockSensor::configureInternal_(const ShockConfig& cfg) {
    l2dReady_ = false;

    I2CBusManager& bus = I2CBusManager::Get();
    bus.registerClient("LIS2DHTR", reinitL2dCb_, this);
    if (!bus.ensureStarted(LIS2DHTR_SDA_PIN, LIS2DHTR_SCL_PIN, 100000)) {
        return false;
    }

    bool ok = l2d_.beginOnBus(bus.wire(), L2D_ADDR0, bus.hz());
    if (!ok) {
        DBG_PRINTLN("[Shock] LIS2DHTR begin failed");
    }

    const uint8_t am = cfg.axisMask;
    const bool xEn = (am & 0x03) != 0;
    const bool yEn = (am & 0x0C) != 0;
    const bool zEn = (am & 0x30) != 0;

    ok &= l2d_.mode(static_cast<l2d_odr_t>(cfg.odr),
                    static_cast<l2d_res_t>(cfg.res),
                    xEn, yEn, zEn);
    ok &= l2d_.scale(static_cast<l2d_scale_t>(cfg.scale));
    ok &= l2d_.hpfCfg(static_cast<l2d_hpf_t>(cfg.hpfMode),
                      cfg.hpfCut,
                      false,
                      false,
                      cfg.hpfEn,
                      false);
    l2d_evt_cfg_t evt{};
    evt.mode  = static_cast<l2d_evt_mode_t>(cfg.evtMode);
    evt.ths   = cfg.threshold & 0x7F;
    evt.dur   = cfg.dur & 0x7F;
    evt.latch = cfg.latch;
    evt.xl    = (am & 0x01) != 0;
    evt.xh    = (am & 0x02) != 0;
    evt.yl    = (am & 0x04) != 0;
    evt.yh    = (am & 0x08) != 0;
    evt.zl    = (am & 0x10) != 0;
    evt.zh    = (am & 0x20) != 0;
    ok &= l2d_.evtSet(&evt, L2D_EVT1);
    ok &= l2d_.intEn(L2D_INT_EVT1, L2D_INT1, true);
    ok &= l2d_.intLevel(cfg.intLevel ? L2D_INT_LOW : L2D_INT_HIGH);

    if (!ok) {
        DBG_PRINTLN("[Shock] LIS2DHTR config failed");
    }

    pinMode(LIS2DHTR_INT_PIN, cfg.intLevel ? INPUT_PULLUP : INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(LIS2DHTR_INT_PIN),
                    ShockSensor::isrThunk_,
                    interruptMode_(cfg));

    l2dReady_ = ok;
    return ok;
}

void ShockSensor::detachInterrupt_() {
    detachInterrupt(digitalPinToInterrupt(SHOCK_SENSOR1_PIN));
}

int ShockSensor::interruptMode_(const ShockConfig& cfg) const {
    return cfg.intLevel ? FALLING : RISING;
}

// Returns true once per physical event, then ignores for SHOCK_COOLDOWN_MS
bool ShockSensor::isTriggered() {
    if (!armed) {
        return false; // still in cooldown
    }

    // Check and consume any latched edge from the ISR
    bool edge = false;
    if (edgeFlag_) {
        edge = true;
        edgeFlag_ = false;
    }

    if (!edge) {
        return false;
    }

    if (internal_) {
        if (!l2dReady_) {
            return false;
        }
        l2d_evt_src_t src{};
        if (!l2d_.evtSrc(&src, L2D_EVT1) || !src.act) {
            return false;
        }
    }

    if (edge && armed) {
        // Latch and start cooldown
        triggered = true;
        armed     = false;

        if (LOGG) LOGG->logLockAction("Shock Sensor Triggered!");

        DBGSTR();
        DBG_PRINTLN("[Shock] Triggered -> cooling down");
        DBG_PRINT  ("        cooldown(ms)="); DBG_PRINTLN((int)SHOCK_COOLDOWN_MS);
        DBGSTP();

        // Schedule one-shot rearm (no RTOS)
        scheduleRearmOnce();
        return true;
    }

    // No new event or still in cooldown
    return false;
}

void ShockSensor::reset() {
    // Preserve "armed" as-is; callers typically just clear the latched flag.
    triggered = false;
    edgeFlag_ = false;

    if (internal_ && l2dReady_) {
        l2d_evt_src_t src{};
        (void)l2d_.evtSrc(&src, L2D_EVT1);
    }

    DBGSTR();
    DBG_PRINTLN("[Shock] Latch reset (armed state unchanged)");
    DBGSTP();
}

// ----------- One-shot rearm implementations (no RTOS) -----------

void ShockSensor::RearmTimerCb(void* arg) {
    auto* self = static_cast<ShockSensor*>(arg);
    self->triggered = false;
    self->armed     = true;

    DBGSTR();
    DBG_PRINTLN("[Shock] Rearmed (esp_timer)");
    DBGSTP();
}

inline void ShockSensor::scheduleRearmOnce() {
    if (rearmTimer) {
        // Stop if already running, then start once
        esp_timer_stop(rearmTimer); // ignore error if not running
        esp_timer_start_once(rearmTimer, (uint64_t)SHOCK_COOLDOWN_MS * 1000ULL);

        DBGSTR();
        DBG_PRINTLN("[Shock] esp_timer one-shot (rearm scheduled)");
        DBGSTP();
    }
}

// ----------- ISR helpers -----------
void IRAM_ATTR ShockSensor::isrThunk_() {
    if (s_instance_) {
        s_instance_->onShockEdge_();
    }
}

void IRAM_ATTR ShockSensor::onShockEdge_() {
    // Just latch the event; all expensive work happens in isTriggered()
    edgeFlag_ = true;
}
