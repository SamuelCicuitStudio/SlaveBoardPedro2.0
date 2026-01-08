#include <RTCManager.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <Utils.hpp>
#include <stdio.h>
#include <sys/time.h>

namespace {
    // Clamp utility to keep fields sane before mktime()
    template<typename T>
    inline T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // Fallback storage if caller passed a null tm*
    static tm g_fallbackTm{};

    // Persist epoch to NVS if Config exists (reduces cold-boot drift)
    inline void persistEpoch(uint64_t epoch) {
        if (!CONF) return;
        // Only write if changed to reduce flash wear
        uint64_t cur = CONF->GetULong64(CURRENT_TIME_SAVED, (uint64_t)DEFAULT_CURRENT_TIME_SAVED);
        if (cur != epoch) CONF->PutULong64(CURRENT_TIME_SAVED, epoch);
    }

    // Try to fill a tm using getLocalTime(); if it fails, fall back to time()+localtime_r
    inline bool safeGetLocalTime(tm* out) {
        if (!out) return false;
        if (getLocalTime(out)) return true;
        time_t now = time(nullptr);
        if (now <= 0) return false;
        localtime_r(&now, out);
        return true;
    }
}

// ----------- singleton backing ----------
RTCManager* RTCManager::s_instance = nullptr;

void RTCManager::Init(struct tm* timeinfo) {
    if (!s_instance) {
        s_instance = new RTCManager(timeinfo);
    } else {
        s_instance->attachTimeinfo(timeinfo);
    }
}

RTCManager* RTCManager::Get() {
    if (!s_instance) {
        s_instance = new RTCManager(nullptr);
    }
    return s_instance;
}

RTCManager* RTCManager::TryGet() {
    return s_instance;
}

void RTCManager::attachTimeinfo(struct tm* timeinfo) {
    if (!timeinfo) return;
    lock_();
    this->timeinfo = timeinfo;
    unlock_();
}

// ---------- ctor ----------
RTCManager::RTCManager(struct tm* timeinfo)
: timeinfo(timeinfo ? timeinfo : &g_fallbackTm), formattedTime(), formattedDate()
{
    // create recursive mutex once
    mtx_ = xSemaphoreCreateRecursiveMutex();
    DBGSTR();
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#                   Starting RTC Manager                  #");
    DBG_PRINTLN("###########################################################");
    DBGSTP();
    // Load persisted epoch if available; otherwise use default
    uint64_t saved = CONF->GetULong64(CURRENT_TIME_SAVED, (uint64_t)DEFAULT_CURRENT_TIME_SAVED);
    setUnixTime((unsigned long)saved); // keep API the same
    update();
}

// Set the system time from a Unix timestamp (seconds since Jan 1, 1970)
void RTCManager::setUnixTime(unsigned long timestamp) {
    lock_();
    DBGSTR();DBG_PRINT("[RTC] Setting system time from Unix timestamp: "); DBG_PRINT(timestamp); DBG_PRINTLN(" 🕒"); DBGSTP();
    struct timeval tv;
    tv.tv_sec  = (time_t)timestamp;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    // Persist new time (optional but helpful)
    persistEpoch((uint64_t)timestamp);

    DBGSTR();DBG_PRINT("[RTC] System time set to: "); DBG_PRINT(timestamp); DBG_PRINTLN(" ✅");
    unlock_();
}

// Get the current Unix timestamp (no need to mutate shared tm buffer)
unsigned long RTCManager::getUnixTime() {
    tm tmp{};
    if (safeGetLocalTime(&tmp)) {
        time_t now = mktime(&tmp);
        return (unsigned long)now;
    }
    // Fallback: time()
    time_t now = time(nullptr);
    if (now > 0) {
        DBGSTR();DBG_PRINT("[RTC] Current Unix time (fallback): "); DBG_PRINT((unsigned long)now); DBG_PRINTLN(" ⏱️");DBGSTP();
        return (unsigned long)now;
    }
    DBG_PRINTLN("[RTC] Failed to get current Unix time. ❌");
    return 0;
}

// Get the current time as a formatted string
String RTCManager::getTime() {
    lock_();
    String s = formattedTime;   // copy under lock
    unlock_();
    return s;
}

// Get the current date as a formatted string
String RTCManager::getDate() {
    lock_();
    String s = formattedDate;   // copy under lock
    unlock_();
    return s;
}

// Update the formatted time and date values
void RTCManager::update() {
    tm tmp{};
    if (!safeGetLocalTime(&tmp)) {
        DBG_PRINTLN("[RTC] Failed to get local time. ❌");
        return;
    }

    char timeString[6];   // "HH:MM"
    char dateString[11];  // "YYYY-MM-DD"
    snprintf(timeString, sizeof(timeString), "%02d:%02d", tmp.tm_hour, tmp.tm_min);
    snprintf(dateString, sizeof(dateString), "%04d-%02d-%02d", tmp.tm_year + 1900, tmp.tm_mon + 1, tmp.tm_mday);

    lock_();
    // keep working tm in sync for callers that read it elsewhere
    *timeinfo = tmp;

    if (formattedTime != timeString) {
        formattedTime = String(timeString);
        DBGSTR();DBG_PRINT("[RTC] Updated time: "); DBG_PRINT(formattedTime); DBG_PRINTLN(" ✅");DBGSTP();
    }
    if (formattedDate != dateString) {
        formattedDate = String(dateString);
        DBGSTR();DBG_PRINT("[RTC] Updated date: "); DBG_PRINT(formattedDate); DBG_PRINTLN(" 📅");DBGSTP();
    }
    unlock_();
}

// Set the time of the RTC directly
void RTCManager::setRTCTime(int year, int month, int day, int hour, int minute, int second) {
    DBGSTR();
    DBG_PRINT("[RTC] Setting RTC time to: ");
    DBG_PRINT("Year: ");   DBG_PRINT(year);
    DBG_PRINT(", Month: "); DBG_PRINT(month);
    DBG_PRINT(", Day: ");   DBG_PRINT(day);
    DBG_PRINT(", Hour: ");  DBG_PRINT(hour);
    DBG_PRINT(", Minute: ");DBG_PRINT(minute);
    DBG_PRINT(", Second: ");DBG_PRINT(second);
    DBG_PRINTLN(" 📝");DBGSTP();

    // Basic clamping to keep inputs sane before mktime() normalization
    year   = clamp(year,   1970, 2099);
    month  = clamp(month,     1,   12);
    day    = clamp(day,       1,   31);
    hour   = clamp(hour,      0,   23);
    minute = clamp(minute,    0,   59);
    second = clamp(second,    0,   59);

    lock_();
    timeinfo->tm_year = year - 1900;
    timeinfo->tm_mon  = month - 1;
    timeinfo->tm_mday = day;
    timeinfo->tm_hour = hour;
    timeinfo->tm_min  = minute;
    timeinfo->tm_sec  = second;

    // mktime() converts local tm → epoch (seconds since 1970-01-01 localtime)
    time_t epoch = mktime(timeinfo);

    struct timeval tv;
    tv.tv_sec  = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    // Persist new time to NVS
    persistEpoch((uint64_t)epoch);
    unlock_();

    update();
}
