#include <NVSManager.hpp>
#include <Config.hpp>
#include <ConfigNvs.hpp>
#include <Utils.hpp>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>

// ======================================================
// Static singleton pointer
// ======================================================
NVS* NVS::s_instance = nullptr;


// ======================================================
// Singleton Init() and Get()
// ======================================================
void NVS::Init() {
    // Just force construction so caller doesn't have to think about it.
    (void)NVS::Get();
}

NVS* NVS::Get() {
    if (!s_instance) {
        s_instance = new NVS();
    }
    return s_instance;
}


// ======================================================
// ctor / dtor
// ======================================================
NVS::NVS()
: namespaceName(CONFIG_PARTITION) {
    mutex_ = xSemaphoreCreateRecursiveMutex();
}

NVS::~NVS() {
    end();
    if (mutex_) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}


// ======================================================
// small RTOS-friendly sleep helper
// ======================================================
inline void NVS::sleepMs_(uint32_t ms) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        delay(ms);
    }
}


// ======================================================
// locking helpers
// ======================================================
inline void NVS::lock_()   { if (mutex_) xSemaphoreTakeRecursive(mutex_, portMAX_DELAY); }
inline void NVS::unlock_() { if (mutex_) xSemaphoreGiveRecursive(mutex_); }


// ======================================================
// Preferences open state helpers
// - Lazy open RO or RW
// - If we’re RO and need RW, we reopen RW
// ======================================================
void NVS::ensureOpenRO_() {
    if (!is_open_) {
        preferences.begin(namespaceName, /*readOnly=*/true);
        is_open_ = true;
        open_rw_ = false;
    } else if (open_rw_) {
        // already RW -> fine
    }
}

void NVS::ensureOpenRW_() {
    if (!is_open_) {
        preferences.begin(namespaceName, /*readOnly=*/false);
        is_open_ = true;
        open_rw_ = true;
    } else if (!open_rw_) {
        // currently RO, need to reopen RW
        preferences.end();
        preferences.begin(namespaceName, /*readOnly=*/false);
        is_open_ = true;
        open_rw_ = true;
    }
}

void NVS::startPreferencesReadWrite() {
    lock_();
    ensureOpenRW_();
    DBG_PRINTLN("Preferences opened RW");
    unlock_();
}

void NVS::startPreferencesRead() {
    ensureOpenRO_();
    DBG_PRINTLN("Preferences opened RO");
}


// ======================================================
// end() - close preferences
// ======================================================
void NVS::end() {
    lock_();
    if (is_open_) {
        preferences.end();
        is_open_ = false;
        open_rw_ = false;
    }
    unlock_();
}


// ======================================================
// begin()
// - decides first boot vs existing config
// - on first boot we write defaults and reboot
// (same logic you already had in ConfigManager, now under NVS) :contentReference[oaicite:2]{index=2} :contentReference[oaicite:3]{index=3}
/*
   Usage at startup:
       NVS::Init();
       NVS::Get()->begin();
*/
void NVS::begin() {
    DBGSTR() ;
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#                 Starting NVS Manager ⚙️                 #");
    DBG_PRINTLN("###########################################################");
    DBGSTP();
    ensureOpenRO_();
    bool resetFlag = preferences.getBool(RESET_FLAG, RESET_FLAG_DEFAULT);

    if (resetFlag) {
        DBG_PRINTLN("[NVS] Initializing the device... 🔄");
        initializeDefaults();
        RestartSysDelay(10000);
    } else {
        DBG_PRINTLN("[NVS] Using existing configuration... ✅");
    }
}


// ======================================================
// Core utils
// ======================================================
bool NVS::getResetFlag() {
    esp_task_wdt_reset();
    ensureOpenRO_();
    bool v = preferences.getBool(RESET_FLAG, RESET_FLAG_DEFAULT);
    return v;
}

void NVS::initializeDefaults() {
    initializeVariables();
}

// All default keys at first boot.
// - identity / pairing
// - runtime state
// - lock config (electromagnet vs screw, timeout)
// - hardware presence map
void NVS::initializeVariables() {
    const bool prevFpCap = GetBool(HAS_FINGERPRINT_KEY, HAS_FINGERPRINT_DEFAULT);
    //
    // -------------------------------------------------
    // Factory / reset flags
    // -------------------------------------------------
    PutBool(RESET_FLAG, false);

    //
    // -------------------------------------------------
    // Device identity / pairing
    // -------------------------------------------------
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String suffix = mac.substring(6);  // last bytes of MAC, no ':'
    String devId = String(DEVICE_ID_DEFAULT) + suffix;

    PutString(DEVICE_NAME,          DEVICE_NAME_DEFAULT);
    PutString(DEVICE_ID,            devId);
    PutString(MASTER_ESPNOW_ID,     MASTER_ESPNOW_ID_DEFAULT);
    PutString(MASTER_LMK_KEY,       MASTER_LMK_DEFAULT);
    PutBool  (DEVICE_CONFIGURED,    DEVICE_CONFIGURED_DEFAULT);

    //
    // -------------------------------------------------
    // Runtime state
    // -------------------------------------------------
    PutBool (LOCK_STATE,        LOCK_STATE_DEFAULT);        // locked/unlocked
    PutBool (DIR_STATE,         DIR_STATE_DEFAULT);         // motor direction
    PutBool (ARMED_STATE,       ARMED_STATE_DEFAULT);       // armed/disarmed
    PutBool (MOTION_TRIG_ALARM, MOTION_TRIG_ALARM_DEFAULT); // motion/shock trigger enabled
    PutBool (FINGERPRINT_ENABLED, FINGERPRINT_ENABLED_DEFAULT); // FP auth allowed
    PutULong64(CURRENT_TIME_SAVED, DEFAULT_CURRENT_TIME_SAVED);
    PutULong64(LAST_TIME_SAVED,    DEFAULT_LAST_TIME_SAVED);

    //
    // -------------------------------------------------
    // Lock driver configuration
    // -------------------------------------------------
    // false = screw motor lock (endstop switches)
    // true  = electromagnet (pulse only, no endstop)
    PutBool(LOCK_EMAG_KEY, LOCK_EMAG_DEFAULT);

    // ms runtime / pulse width
    PutULong64 (LOCK_TIMEOUT_KEY, LOCK_TIMEOUT_DEFAULT);

    // -------------------------------------------------
    // Hardware presence map
    // -------------------------------------------------
    // Unpaired/default state: disable all capabilities until master sets them.
    PutBool(HAS_OPEN_SWITCH_KEY,   false);   // open/request button
    PutBool(HAS_SHOCK_SENSOR_KEY,  false);   // shock sensor
    PutBool(HAS_REED_SWITCH_KEY,   false);   // door reed switch
    PutBool(HAS_FINGERPRINT_KEY,   prevFpCap); // fingerprint capability persists

    // -------------------------------------------------
    // Pairing channel + fingerprint provisioning flags
    // -------------------------------------------------
    PutInt (MASTER_CHANNEL_KEY, MASTER_CHANNEL_DEFAULT);
    PutBool(FP_DEVICE_CONFIGURED_KEY, FP_DEVICE_CONFIGURED_DEFAULT);
}

// ======================================================
// Reads (auto-open RO)
// ======================================================
bool NVS::GetBool(const char* key, bool defaultValue) {
    esp_task_wdt_reset();
    ensureOpenRO_();
    bool v = preferences.getBool(key, defaultValue);
    return v;
}

int NVS::GetInt(const char* key, int defaultValue) {
    esp_task_wdt_reset();
    ensureOpenRO_();
    int v = preferences.getInt(key, defaultValue);
    return v;
}

uint64_t NVS::GetULong64(const char* key, int defaultValue) {
    esp_task_wdt_reset();
    ensureOpenRO_();
    uint64_t v = preferences.getULong64(key, defaultValue);
    return v;
}

float NVS::GetFloat(const char* key, float defaultValue) {
    esp_task_wdt_reset();
    ensureOpenRO_();
    float v = preferences.getFloat(key, defaultValue);
    return v;
}

String NVS::GetString(const char* key, const String& defaultValue) {
    esp_task_wdt_reset();
    ensureOpenRO_();
    String v = preferences.getString(key, defaultValue);
    return v;
}


// ======================================================
// Writes (auto-open RW)
// (We remove existing key first to guarantee type)
// ======================================================
void NVS::PutBool(const char* key, bool value) {
    esp_task_wdt_reset();
    lock_();
    ensureOpenRW_();
    if (preferences.isKey(key)) preferences.remove(key);
    preferences.putBool(key, value);
    unlock_();
}

void NVS::PutUInt(const char* key, int value) {
    esp_task_wdt_reset();
    lock_();
    ensureOpenRW_();
    if (preferences.isKey(key)) preferences.remove(key);
    preferences.putUInt(key, value);
    unlock_();
}

void NVS::PutULong64(const char* key, int value) {
    esp_task_wdt_reset();
    lock_();
    ensureOpenRW_();
    if (preferences.isKey(key)) preferences.remove(key);
    preferences.putULong64(key, value);
    unlock_();
}

void NVS::PutInt(const char* key, int value) {
    esp_task_wdt_reset();
    lock_();
    ensureOpenRW_();
    if (preferences.isKey(key)) preferences.remove(key);
    preferences.putInt(key, value);
    unlock_();
}

void NVS::PutIntImmediate(const char* key, int value) {
    esp_task_wdt_reset();
    lock_();
    ensureOpenRW_();
    if (preferences.isKey(key)) preferences.remove(key);
    preferences.putInt(key, value);
    unlock_();
}

void NVS::PutFloat(const char* key, float value) {
    esp_task_wdt_reset();
    lock_();
    ensureOpenRW_();
    if (preferences.isKey(key)) preferences.remove(key);
    preferences.putFloat(key, value);
    unlock_();
}

void NVS::PutString(const char* key, const String& value) {
    esp_task_wdt_reset();
    lock_();
    ensureOpenRW_();
    if (preferences.isKey(key)) preferences.remove(key);
    preferences.putString(key, value);
    unlock_();
}


// ======================================================
// Key management
// ======================================================
void NVS::ClearKey() {
    lock_();
    ensureOpenRW_();
    preferences.clear();
    unlock_();
}

void NVS::RemoveKey(const char* key) {
    esp_task_wdt_reset();
    lock_();
    ensureOpenRW_();
    if (preferences.isKey(key)) {
        preferences.remove(key);
    } else {
        DBG_PRINT("[NVS] Key not found, skipping: ");
        DBG_PRINTLN(key);
    }
    unlock_();
}


// ======================================================
// System helpers / reboot paths
// ======================================================
void NVS::RestartSysDelayDown(unsigned long delayTime) {
    unsigned long interval = delayTime / 30;
    DBGSTR();
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#           Restarting the Device in: " + String(delayTime / 1000)+ " Sec              #");
    DBG_PRINTLN("###########################################################");
    
    for (int i = 0; i < 30; i++) {
        DBG_PRINT("🔵");
        sleepMs_(interval);
        esp_task_wdt_reset();
    }
    DBG_PRINTLN();
    DBG_PRINTLN("[NVS] Restarting now...");
    DBGSTP() ;
    simulatePowerDown();
}

void NVS::RestartSysDelay(unsigned long delayTime) {
    unsigned long interval = delayTime / 30;
    DBGSTR() ;
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#           Restarting the Device in: " + String(delayTime / 1000)+ " Sec              #");
    DBG_PRINTLN("###########################################################");
    for (int i = 0; i < 30; i++) {
        DBG_PRINT("🔵");
        sleepMs_(interval);
        esp_task_wdt_reset();
    }
    DBG_PRINTLN();
    DBG_PRINTLN("[NVS] Restarting now...");
    DBGSTP() ;
    simulatePowerDown();
}

void NVS::CountdownDelay(unsigned long delayTime) {
    unsigned long interval = delayTime / 32;
    DBGSTR() ;
    DBG_PRINTLN("###########################################################");
    DBG_PRINT("[NVS] Waiting User Action: ");
    DBG_PRINT(delayTime / 1000);
    DBG_PRINTLN(" Sec");
    for (int i = 0; i < 32; i++) {
        DBG_PRINT("#");
        sleepMs_(interval);
        esp_task_wdt_reset();
    }
    DBG_PRINTLN();
    DBGSTP() ;
}

void NVS::simulatePowerDown() {
    esp_sleep_enable_timer_wakeup(1000000); // 1s
    esp_deep_sleep_start();
}
