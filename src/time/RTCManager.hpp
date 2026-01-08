/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef RTCMANAGER_H
#define RTCMANAGER_H

#include <Arduino.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class RTCManager {
public:
    // ---------------- Singleton access ----------------
    // Call once at boot to set the tm buffer (optional).
    static void        Init(struct tm* timeinfo);
    // Always returns a valid pointer (auto-creates with fallback tm if no Init yet).
    static RTCManager* Get();
    // Returns nullptr if never created.
    static RTCManager* TryGet();

    void attachTimeinfo(struct tm* timeinfo);

    void         setUnixTime(unsigned long timestamp);  // Set RTC time using Unix timestamp
    unsigned long getUnixTime();                        // Get current Unix timestamp
    String       getTime();                             // "HH:MM"
    String       getDate();                             // "YYYY-MM-DD"
    void         update();                              // Refresh formatted time/date
    void         setRTCTime(int year, int month, int day, int hour, int minute, int second);

private:
    RTCManager(struct tm* timeinfo);  // Constructor
    RTCManager() = delete;
    RTCManager(const RTCManager&) = delete;
    RTCManager& operator=(const RTCManager&) = delete;

    static RTCManager* s_instance;

    // Shared state
    struct tm* timeinfo;     // working tm buffer
    String     formattedTime;
    String     formattedDate;

    // Concurrency
    SemaphoreHandle_t mtx_ = nullptr;   // recursive mutex
    inline void lock_()   { if (mtx_) xSemaphoreTakeRecursive(mtx_, portMAX_DELAY); }
    inline void unlock_() { if (mtx_) xSemaphoreGiveRecursive(mtx_); }
};

// Pointer-style convenience macro (like CONF/LOG):
//   RTCM->getUnixTime();
#define RTCM RTCManager::Get()

#endif  // RTCMANAGER_H






