#include <ShockSensor.hpp>
#include <Logger.hpp>
#include <Utils.hpp>

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

void ShockSensor::begin() {
    // Active-LOW inputs with pull-ups
    pinMode(SHOCK_SENSOR1_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SHOCK_SENSOR1_PIN),
                    ShockSensor::isrThunk_,
                    FALLING);  // LOW pulse = shock

    // Create the one-shot timer once; reuse it
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
