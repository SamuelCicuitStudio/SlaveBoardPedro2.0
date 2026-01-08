/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef SLEEPTIMER_H
#define SLEEPTIMER_H

/**
 * @file SleepTimer.h (RTOS-free)
 * Poll with service() from your main loop (e.g., every ~300 ms).
 * Any user/device activity should call notifyActivity() (or ...FromISR()).
 */

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

class RTCManager;
class PowerManager;

#define BUTTON_PIN_BITMASK(GPIO) (1ULL << (GPIO))

// How often you call service() is up to you. No internal timing required.
// If you still want a soft rate-limit on debug prints, you can define this:
#ifndef SLEEPTIMER_MIN_CHECK_MS
#define SLEEPTIMER_MIN_CHECK_MS 0   // 0 = check on every service() call
#endif

class SleepTimer {
public:
    // ---------------- Singleton access ----------------
    // Call once at boot to wire dependencies (optional).
    static void       Init(RTCManager* RTC, PowerManager* Pow);
    // Always returns a valid pointer (auto-creates with null deps if no Init yet).
    static SleepTimer* Get();
    // Returns nullptr if never created.
    static SleepTimer* TryGet();

    void attachDeps(RTCManager* RTC, PowerManager* Pow);

    // Activity kicks
    void reset();
    void resetQuiet();
    inline void notifyActivity() { reset(); }
    void notifyActivityFromISR();

    // Poll this from your main loop (e.g., every ~300 ms)
    void service();

    // Remaining time until sleep (ms)
    uint32_t msUntilSleep() const;

    // Enter deep sleep now (idempotent, guarded)
    void goToSleep();

    // Compatibility shim: previously started an RTOS task; now a no-op + warning
    void timerLoop();

    // State helpers
    inline bool isSleeping() const { return isSleepMode; }

private:
    SleepTimer(RTCManager* RTC, PowerManager* Pow);
    SleepTimer() = delete;
    SleepTimer(const SleepTimer&) = delete;
    SleepTimer& operator=(const SleepTimer&) = delete;

    static SleepTimer* s_instance;

    RTCManager*    RTC;
    PowerManager*  Pow;

    // Shared state
    volatile uint32_t lastActivityTime = 0; // millis()
    volatile bool     isSleepMode      = false;

    // Soft rate-limit for prints (optional)
    uint32_t lastCheckPrintMs_ = 0;

    // Lightweight spinlock so ISR path can safely bump the timestamp
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

    // Internal
    void checkInactivity();
};

#endif






