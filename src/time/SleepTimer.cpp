#include <SleepTimer.hpp>
#include <Config.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <RTCManager.hpp>
#include <PowerManager.hpp>
#include <Utils.hpp>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

// Singleton backing pointer
SleepTimer* SleepTimer::s_instance = nullptr;

void SleepTimer::Init(RTCManager* rtc, PowerManager* pow) {
    if (!s_instance) {
        s_instance = new SleepTimer(rtc, pow);
    } else {
        s_instance->attachDeps(rtc, pow);
    }
}

SleepTimer* SleepTimer::Get() {
    if (!s_instance) {
        s_instance = new SleepTimer(nullptr, nullptr);
    }
    return s_instance;
}

SleepTimer* SleepTimer::TryGet() {
    return s_instance;
}

void SleepTimer::attachDeps(RTCManager* rtc, PowerManager* pow) {
    RTC = rtc;
    Pow = pow;
}

// Small helper
static inline uint32_t nowMs() { return millis(); }

// ---------- ctor ----------
SleepTimer::SleepTimer(RTCManager* rtc, PowerManager* pow)
: RTC(rtc), Pow(pow) {
    DBGSTR();
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#           Starting SleepTimer (RTOS-free polling)       #");
    DBG_PRINTLN("###########################################################");
    DBGSTP();
    lastActivityTime = nowMs();
}

// ---------- thread-safe kicks ----------
void SleepTimer::reset() {
    portENTER_CRITICAL(&mux);
    lastActivityTime = nowMs();
    portEXIT_CRITICAL(&mux);
    DBG_PRINTLN("[SLEEP] Timer reset (activity).");
}

void SleepTimer::resetQuiet() {
    portENTER_CRITICAL(&mux);
    lastActivityTime = nowMs();
    portEXIT_CRITICAL(&mux);
}

void SleepTimer::notifyActivityFromISR() {
    portENTER_CRITICAL_ISR(&mux);
    lastActivityTime = nowMs();
    portEXIT_CRITICAL_ISR(&mux);
}

// ---------- approx remaining time ----------
uint32_t SleepTimer::msUntilSleep() const {
    const uint32_t t = nowMs();
    uint32_t last;
    portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&mux));
    last = lastActivityTime;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&mux));

    const uint32_t elapsed = t - last;        // overflow-safe on uint32_t
    return (elapsed >= SLEEP_TIMER) ? 0u : (SLEEP_TIMER - elapsed);
}

// ---------- poll from main loop ----------
void SleepTimer::service() {
#if SLEEPTIMER_MIN_CHECK_MS > 0
    const uint32_t t = nowMs();
    if ((t - lastCheckPrintMs_) < SLEEPTIMER_MIN_CHECK_MS) return;
    lastCheckPrintMs_ = t;
#endif
    checkInactivity();
}

// ---------- inactivity check (no tasks) ----------
void SleepTimer::checkInactivity() {
    // Fast read of lastActivityTime (guarded)
    const uint32_t t = nowMs();
    uint32_t last;
    portENTER_CRITICAL(&mux);
    last = lastActivityTime;
    portEXIT_CRITICAL(&mux);

    if ((uint32_t)(t - last) < (uint32_t)SLEEP_TIMER) return;

    // Timeout reached → try to sleep
    DBGSTR();
    DBG_PRINTLN("[SLEEP] Inactivity timeout reached → entering deep sleep 🛌");
    DBGSTP();
    goToSleep();   // does not return
}

// ---------- compatibility shim ----------
void SleepTimer::timerLoop() {
    DBG_PRINTLN("[SLEEP] timerLoop() ignored: SleepTimer is RTOS-free now. "
                  "Call sleepTimer.service() regularly from your main loop.");
}

// ---------- enter deep sleep ----------
void SleepTimer::goToSleep() {
    // Re-entry guard
    portENTER_CRITICAL(&mux);
    if (isSleepMode) { portEXIT_CRITICAL(&mux); return; }
    isSleepMode = true;
    portEXIT_CRITICAL(&mux);

    const bool deviceConfigured =
        (CONF && CONF->GetBool(DEVICE_CONFIGURED, false));

    const bool hasReed =
        IS_SLAVE_ALARM ? true
                       : (CONF && CONF->GetBool(HAS_REED_SWITCH_KEY,
                                                HAS_REED_SWITCH_DEFAULT));

    const bool hasOpenBtn =
        (!IS_SLAVE_ALARM &&
         CONF && CONF->GetBool(HAS_OPEN_SWITCH_KEY, HAS_OPEN_SWITCH_DEFAULT));

    const bool hasShock =
        IS_SLAVE_ALARM ? true
                       : (CONF && CONF->GetBool(HAS_SHOCK_SENSOR_KEY,
                                                HAS_SHOCK_SENSOR_DEFAULT));

    // If reed exists and door is OPEN → don't sleep; reset timer
    if (hasReed) {
        bool doorOpenNow = (digitalRead(WAKE_UP_GPIO_REED_SWITCH) == HIGH);
        if (doorOpenNow) {
            DBGSTR();
            DBG_PRINTLN("[SLEEP] Sleep blocked: door is OPEN (reed HIGH). Resetting timer.");
            DBGSTP();
            reset();
            portENTER_CRITICAL(&mux);
            isSleepMode = false;
            portEXIT_CRITICAL(&mux);
            return;
        }
    }

    DBGSTR();
    DBG_PRINTLN("[SLEEP] Preparing deep sleep… 💤");
    DBG_PRINT  ("        configured="); DBG_PRINT(deviceConfigured ? "yes" : "no");
    DBG_PRINT  ("        reed=");       DBG_PRINT(hasReed    ? "yes" : "no");
    DBG_PRINT  ("        openBtn=");    DBG_PRINT(hasOpenBtn ? "yes" : "no");
    DBG_PRINT  ("        shock=");      DBG_PRINTLN(hasShock   ? "yes" : "no");
    DBGSTP();

    // Persist time if possible; keep RTC PERIPH domain ON for EXT wake
    DBGSTR();
    if (RTC && CONF) {
        uint64_t now = RTC->getUnixTime();
        CONF->PutULong64(LAST_TIME_SAVED,    now);
        CONF->PutULong64(CURRENT_TIME_SAVED, now);
        DBG_PRINTLN("[SLEEP] Saved current time into NVS");
    } else {
        DBG_PRINTLN("[SLEEP] Time save skipped (RTC/CONF missing)");
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    DBG_PRINTLN("[SLEEP] RTC PERIPH domain set to ON during deep sleep");
    DBGSTP();

    // RTC bias helpers
    auto prep_rtc_input_pullup = [](gpio_num_t g){
        if (rtc_gpio_is_valid_gpio(g)) {
            rtc_gpio_pullup_en(g);
            rtc_gpio_pulldown_dis(g);
        } else {
            DBG_PRINTF("[SLEEP] GPIO %d not RTC-capable (no wake) \n", (int)g);
        }
    };
    auto prep_rtc_input_pulldown = [](gpio_num_t g){
        if (rtc_gpio_is_valid_gpio(g)) {
            rtc_gpio_pulldown_en(g);
            rtc_gpio_pullup_dis(g);
        } else {
            DBG_PRINTF("[SLEEP] GPIO %d not RTC-capable (no wake) \n", (int)g);
        }
    };

    // Wake source #1 (EXT0): REED HIGH
    DBGSTR();
    if (hasReed) {
        esp_sleep_enable_ext0_wakeup(
            (gpio_num_t)WAKE_UP_GPIO_REED_SWITCH,
            1 /* HIGH */
        );
        prep_rtc_input_pulldown((gpio_num_t)WAKE_UP_GPIO_REED_SWITCH);
        DBG_PRINTLN("[SLEEP] [Wakeup] EXT0 on REED (level HIGH) armed");
    } else {
        DBG_PRINTLN("[SLEEP] [Wakeup] EXT0 not armed (no reed sensor)");
    }
    DBGSTP();

    // Wake source #2/#3 (EXT1): OPEN button + SHOCK
    uint64_t ext1Mask = 0ULL;
    bool useAnyHigh = false;
    bool shockActiveLow = true;
    if (deviceConfigured && hasShock && CONF) {
        const int type =
            CONF->GetInt(SHOCK_SENSOR_TYPE_KEY, SHOCK_SENSOR_TYPE_DEFAULT);
        if (type == SHOCK_SENSOR_TYPE_INTERNAL) {
            const int lvl =
                CONF->GetInt(SHOCK_L2D_INT_LVL_KEY, SHOCK_L2D_INT_LVL_DEFAULT);
            shockActiveLow = (lvl != 0);
        } else {
            shockActiveLow = true; // external is active-low
        }
    }

    if (deviceConfigured && hasShock) {
        ext1Mask |= BUTTON_PIN_BITMASK(WAKE_UP_GPIO_SHOCK_SENSOR1);
        if (shockActiveLow) {
            prep_rtc_input_pullup((gpio_num_t)WAKE_UP_GPIO_SHOCK_SENSOR1);
        } else {
            useAnyHigh = true;
            prep_rtc_input_pulldown((gpio_num_t)WAKE_UP_GPIO_SHOCK_SENSOR1);
        }
    }

    if (hasOpenBtn) {
        if (useAnyHigh) {
            DBG_PRINTLN("[SLEEP] [Wakeup] OPEN button wake skipped (EXT1 ANY_HIGH)");
        } else {
            ext1Mask |= BUTTON_PIN_BITMASK(WAKE_UP_GPIO_OPEN_SWITCH);
            prep_rtc_input_pullup((gpio_num_t)WAKE_UP_GPIO_OPEN_SWITCH);
        }
    }

    DBGSTR();
    if (ext1Mask != 0ULL) {
        esp_sleep_enable_ext1_wakeup(
            ext1Mask,
            useAnyHigh ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ALL_LOW);
        if (useAnyHigh) {
            DBG_PRINTLN("[SLEEP] [Wakeup] EXT1 ANY_HIGH on (SHOCK) armed");
        } else if (hasOpenBtn && (deviceConfigured && hasShock)) {
            DBG_PRINTLN("[SLEEP] [Wakeup] EXT1 ALL_LOW on (OPEN button + SHOCK) armed");
        } else if (hasOpenBtn) {
            DBG_PRINTLN("[SLEEP] [Wakeup] EXT1 ALL_LOW on (OPEN button) armed");
        } else {
            DBG_PRINTLN("[SLEEP] [Wakeup] EXT1 ALL_LOW on (SHOCK) armed");
        }
    } else {
        DBG_PRINTLN("[SLEEP] [Wakeup] EXT1 not armed (no eligible pins)");
    }
    DBGSTP();

    // Hold RTC-capable pins
    DBGSTR();
    if (hasReed && rtc_gpio_is_valid_gpio((gpio_num_t)WAKE_UP_GPIO_REED_SWITCH)) {
        rtc_gpio_hold_en((gpio_num_t)WAKE_UP_GPIO_REED_SWITCH);
        DBG_PRINTLN("[SLEEP] Hold REED pin in RTC domain");
    }
    if (hasOpenBtn && rtc_gpio_is_valid_gpio((gpio_num_t)WAKE_UP_GPIO_OPEN_SWITCH)) {
        rtc_gpio_hold_en((gpio_num_t)WAKE_UP_GPIO_OPEN_SWITCH);
        DBG_PRINTLN("[SLEEP] Hold OPEN button pin in RTC domain");
    }
    if ((deviceConfigured && hasShock) &&
        rtc_gpio_is_valid_gpio((gpio_num_t)WAKE_UP_GPIO_SHOCK_SENSOR1)) {
        rtc_gpio_hold_en((gpio_num_t)WAKE_UP_GPIO_SHOCK_SENSOR1);
        DBG_PRINTLN("[SLEEP] Hold SHOCK pin in RTC domain");
    }
    DBGSTP();

    DBGSTR();
    DBG_PRINTLN("[SLEEP] Entering deep sleep now…");
    DBGSTP();

    Serial.flush();
    esp_deep_sleep_start();
    // Should never return
    while (true) { /* no-op */ }
}
