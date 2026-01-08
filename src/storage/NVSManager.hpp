/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H
/**
 * @file NVSManager.h
 * @brief Thread-safe global config / Preferences manager for ESP32.
 *
 * - Singleton (NVS::Init(), then NVS::Get()).
 * - Owns Preferences internally.
 * - Auto-opens RO/RW lazily.
 * - All calls are mutex-protected.
 *
 * After these changes:
 *   NVS::Get()->begin();
 *   CONF->PutBool(...);
 */

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class NVS {
public:
    // -----------------------------------------------------------------
    // Singleton access
    // -----------------------------------------------------------------

    // Call once at boot (optional now, but still nice for clarity):
    //   NVS::Init();
    //   NVS::Get()->begin();
    //
    // Init() guarantees the singleton exists.
    static void Init();

    // Global accessor. ALWAYS returns a valid pointer.
    // If the singleton was never created yet, it will create it.
    static NVS* Get();

    // -----------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------
    void begin();   // open prefs, run reset/first-boot logic if needed
    void end();     // close prefs

    ~NVS();

    // -----------------------------------------------------------------
    // Writes (auto-open RW)
    // -----------------------------------------------------------------
    void PutBool     (const char* key, bool value);
    void PutInt      (const char* key, int value);
    void PutFloat    (const char* key, float value);
    void PutString   (const char* key, const String& value);
    void PutUInt     (const char* key, int value);
    void PutULong64  (const char* key, int value);

    // -----------------------------------------------------------------
    // Reads (auto-open RO)
    // -----------------------------------------------------------------
    bool     GetBool    (const char* key, bool defaultValue);
    int      GetInt     (const char* key, int defaultValue);
    uint64_t GetULong64 (const char* key, int defaultValue);
    float    GetFloat   (const char* key, float defaultValue);
    String   GetString  (const char* key, const String& defaultValue);

    // -----------------------------------------------------------------
    // Keys / maintenance
    // -----------------------------------------------------------------
    void RemoveKey(const char* key);
    void ClearKey();

    // -----------------------------------------------------------------
    // System helpers (reboot, countdown, powerdown)
    // -----------------------------------------------------------------
    void RestartSysDelay(unsigned long delayTime);
    void RestartSysDelayDown(unsigned long delayTime);
    void simulatePowerDown();
    void CountdownDelay(unsigned long delayTime);

    // Optional debug helpers to force-open prefs
    void startPreferencesReadWrite();  // force RW open
    void startPreferencesRead();       // force RO open

private:
    // -----------------------------------------------------------------
    // Singleton internals
    // -----------------------------------------------------------------
    NVS();                                // private ctor
    NVS(const NVS&) = delete;
    NVS& operator=(const NVS&) = delete;

    static NVS* s_instance;               // the one global instance

    // -----------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------
    void initializeDefaults();   // calls initializeVariables()
    void initializeVariables();  // writes all default keys
    bool getResetFlag();

    // locking helpers
    inline void lock_();
    inline void unlock_();

    // prefs open helpers
    void ensureOpenRO_();  // open prefs RO if needed
    void ensureOpenRW_();  // open prefs RW if needed

    static inline void sleepMs_(uint32_t ms);

    // -----------------------------------------------------------------
    // NVS state
    // -----------------------------------------------------------------
    Preferences  preferences;        // fully owned instance
    const char*  namespaceName;      // CONFIG_PARTITION

    bool open_rw_   = false;         // true if prefs currently RW
    bool is_open_   = false;         // true if prefs.begin() called

    // Recursive mutex so nested Put*/RemoveKey() etc. are safe
    SemaphoreHandle_t mutex_ = nullptr;
};

// -----------------------------------------------------------------
// Convenience macro, now pointer-style.
// You can do:
//   CONF->GetBool(...);
//   CONF->PutBool(...);
//   CONF->begin();    // after Init()
// -----------------------------------------------------------------
#define CONF NVS::Get()

#endif // NVS_MANAGER_H






