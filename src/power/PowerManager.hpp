/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <MAX17055.hpp>

// ---- Cadences (used by service()) ----
#ifndef POWER_FAST_TICK_MS
#define POWER_FAST_TICK_MS 250      // fast gauge heartbeat (ms)
#endif
#ifndef POWER_MODE_UPDATE
#define POWER_MODE_UPDATE  10000    // power-mode evaluation period (ms)
#endif
// Testing: force reported SOC (%) and disable gauge I2C. Set to 0 to disable.
#ifndef POWER_CLAMP_SOC_PERCENT
#define POWER_CLAMP_SOC_PERCENT 75
#endif
// Testing: fixed battery voltage (V) used when POWER_CLAMP_SOC_PERCENT > 0.
#ifndef POWER_CLAMP_VOLTAGE_V
#define POWER_CLAMP_VOLTAGE_V 3.9f
#endif

enum PowerMode {
    CRITICAL_POWER_MODE  = 3,   // ≤3% — critical, imminent shutdown
    EMERGENCY_POWER_MODE = 6,   // >3% and ≤6% — emergency low battery
    LOW_POWER            = 10,  // >6% and ≤10% — low power mode
    POWER_20             = 20,
    POWER_30             = 30,
    POWER_40             = 40,
    POWER_50             = 50,
    POWER_60             = 60,
    POWER_70             = 70,
    POWER_80             = 80,
    POWER_90             = 90,
    FULL_POWER           = 100
};

class PowerManager {
public:
    // ---------------- Singleton access ----------------
    // Call once at boot to set the I2C wire port (optional).
    static void        Init(TwoWire* wirePort = nullptr);
    // Always returns a valid pointer (auto-creates with null wire if no Init yet).
    static PowerManager* Get();
    // Returns nullptr if never created.
    static PowerManager* TryGet();

    void attachWire(TwoWire* wirePort);

    // One-time init (I2C + gauge config)
    void begin();

    // ---- New polling API (no RTOS) ----
    // Call this frequently (e.g., in your loop/task):
    // - ticks the gauge at POWER_FAST_TICK_MS
    // - evaluates power mode at POWER_MODE_UPDATE
    void service();

    // If you need finer control:
    void fastTick();           // just drives gauge_.tick() + online state
    bool evalIfDue();          // run updatePowerMode() if due; returns true if ran
    void forceEvaluate();      // always run updatePowerMode() now

    // Data access
    float     getBatteryPercentage();   // SOC (%) from MAX17055; uses stale if allowed
    PowerMode getPowerMode();
    void      setPowerMode(PowerMode mode);
    void      updatePowerMode();        // actual evaluation logic (SOC-based)
    void      updateChargeStatus();

    bool isGaugeOnline() const;
    bool isBatteryDataFresh() const;

    // NO I2C: returns latest cached gauge snapshot, also caches it locally.
    bool getBatteryInfo(MAX17055::BattInfo& infoOut) const;

    // State (public for quick access/telemetry)
    PowerMode   currentMode;
    float       batteryVoltage;     // FYI only (display), not used for % mode logic
    float       batteryPercentage;  // SOC from MAX17055
    bool        isCharging;

private:
    PowerManager(TwoWire* wirePort);
    PowerManager() = delete;
    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    static PowerManager* s_instance;

    // Optional external hook, currently unused
    MAX17055* fuelGauge = nullptr;

    TwoWire*  wirePort;
    MAX17055  gauge_;            // Owned gauge instance (no RTOS inside)

    // Cached BattInfo snapshot (used by other classes).
    // Marked mutable so we can lazily populate/update it in a const getter.
    mutable MAX17055::BattInfo lastBattInfo_{};
    mutable bool               battInfoValid_ = false;

    bool      gaugeOnline     = false;
    bool      gaugeDataFresh  = false;
    MAX17055::OnlineState lastOnlineState_ = MAX17055::UNKNOWN;

    void updateGaugeOnlineState();

    // ---- Concurrency: small recursive mutex for our fields (external callers may be multi-task) ----
    mutable SemaphoreHandle_t mtx_ = nullptr;
    inline void lock_()   const { if (mtx_) xSemaphoreTakeRecursive(mtx_, portMAX_DELAY); }
    inline void unlock_() const { if (mtx_) xSemaphoreGiveRecursive(mtx_); }

    // ---- Polling scheduler timestamps (ms) ----
    uint32_t lastFastTickMs_ = 0;
    uint32_t lastEvalMs_     = 0;
};

// Pointer-style convenience macro (like CONF/LOG):
//   POWERMGR->begin();
#define POWERMGR PowerManager::Get()

#endif // POWER_MANAGER_H

//64:e8:33:54:43:58





