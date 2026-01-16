/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef SHOCK_SENSOR_H
#define SHOCK_SENSOR_H

#include <Arduino.h>
#include <Config.hpp>
#include <esp_timer.h>
#include "l2d.hpp"

/**
 * @file ShockSensor.h
 * @brief One-shot, cooldown-gated shock detector for the lock device.
 *
 * @details
 * The ShockSensor latches a trigger **once per physical shock** and then
 * enforces a **cooldown** window so repeated vibrations don’t flood the system.
 * It uses an **active-LOW** input (LOW = shock) with an internal pull-up.
 *
 * How the device reacts in your stack:
 * - **While awake:** Your app calls `isTriggered()` periodically (e.g., in a
 *   monitor loop). On the **first** LOW edge after being armed, it returns
 *   `true`, logs the event, and internally **disarms** for
 *   `SHOCK_COOLDOWN_MS`. During cooldown, further shocks are ignored.
 *   Use the returned `true` to raise overlays/alarms as you already do.
 * - **During deep sleep:** The **SleepTimer** (not this class) arms **EXT1**
 *   wake on the shock pin (LOW level). A shock will wake the device; after
 *   reboot/wake, your app may call `isTriggered()` to decide whether to show a
 *   notification or start an alarm scenario.
 * - **After cooldown:** The sensor **re-arms automatically**:
 *   - On ESP32 via a **one-shot `esp_timer`**.
 *   - On non-ESP32 via a **millis() deadline** checked opportunistically.
 *
 * Reliability & memory:
 * - No dynamic allocation in public paths; only tiny scalars are used.
 * - ISR-free: this class does not attach interrupts; polling keeps behavior
 *   deterministic and stack-safe.
 * - Strategic **grouped debug prints** keep logs atomic and readable.
 *
 * Typical usage:
 * @code
 * ShockSensor shock;
 * shock.begin();                 // configure pins + create rearm timer (ESP32)
 *
 * // Poll in a loop/task:
 * if (shock.isTriggered()) {
 *   // -> raise overlay / alarm / journal event
 * }
 *
 * // To clear a visible latched state in your UI (does not rearm):
 * shock.reset();
 * @endcode
 */

// Cooldown duration after a trigger (ms)
#ifndef SHOCK_COOLDOWN_MS
#define SHOCK_COOLDOWN_MS 1000
#endif

class NVS;

struct ShockConfig {
    uint8_t type;       // 0=external GPIO, 1=internal LIS2DHTR
    uint8_t threshold;  // LIS2DHTR INT1_THS (0..127)
    uint8_t odr;        // l2d_odr_t
    uint8_t scale;      // l2d_scale_t
    uint8_t res;        // l2d_res_t
    uint8_t evtMode;    // l2d_evt_mode_t
    uint8_t dur;        // INT1_DUR (0..127)
    uint8_t axisMask;   // bit0=XL,1=XH,2=YL,3=YH,4=ZL,5=ZH
    uint8_t hpfMode;    // l2d_hpf_t
    uint8_t hpfCut;     // HPF cutoff (0..3)
    bool    hpfEn;      // enable HPF on INT1
    bool    latch;      // latch INT1 until INT1_SRC read
    uint8_t intLevel;   // 0=active high, 1=active low
};

class ShockSensor {
public:
    explicit ShockSensor();
    bool begin(const ShockConfig& cfg);
    bool applyConfig(const ShockConfig& cfg);
    void disable();
    bool reinitI2C();
    bool isTriggered();   // true exactly once per distinct shock (then cooldown)
    void reset();         // clears "triggered" latch; does NOT force rearm
    bool isInternal() const { return internal_; }

    static ShockConfig loadConfig(NVS* nvs);
    static ShockConfig sanitizeConfig(ShockConfig cfg);

private:
    // Singleton-style pointer used by static ISR thunk
    static ShockSensor* s_instance_;

    // Set by hardware ISR when a shock edge is detected
    volatile bool edgeFlag_   = false;

    volatile bool triggered = false;    // latched edge (reported once)
    volatile bool armed     = true;     // blocks retriggering during cooldown

    // ESP32: one-shot rearm using esp_timer
    esp_timer_handle_t rearmTimer = nullptr;
    static void RearmTimerCb(void* arg);
    inline void scheduleRearmOnce();

    // ISR helpers for hardware edge capture
    static void IRAM_ATTR isrThunk_();
    void IRAM_ATTR onShockEdge_();

    void detachInterrupt_();
    void configureExternal_();
    bool configureInternal_(const ShockConfig& cfg);
    int interruptMode_(const ShockConfig& cfg) const;

    ShockConfig cfg_{};
    bool internal_ = false;
    bool l2dReady_ = false;
    L2D  l2d_;
};

#endif // SHOCK_SENSOR_H






