/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class RTCManager;
extern "C" bool psramFound();

// ---------------- Tunables (override via -D at build) ---------------
#ifndef LOGGER_MAX_LINE_BYTES
#define LOGGER_MAX_LINE_BYTES  192
#endif
#ifndef LOGGER_QUEUE_DEPTH
#define LOGGER_QUEUE_DEPTH     64
#endif
#ifndef LOGGER_ROTATE_BYTES
#define LOGGER_ROTATE_BYTES    (5u * 1024u * 1024u)
#endif
#ifndef LOGGER_TASK_STACK
#define LOGGER_TASK_STACK      4096
#endif
#ifndef LOGGER_TASK_PRIO
#define LOGGER_TASK_PRIO       1
#endif
#ifndef LOGGER_TASK_CORE
#define LOGGER_TASK_CORE       tskNO_AFFINITY
#endif
#ifndef LOGGER_TICK_MS
#define LOGGER_TICK_MS         500
#endif
#ifndef LOGGER_MIN_FREE_HEAP
#define LOGGER_MIN_FREE_HEAP   (20*1024)
#endif
#ifndef LOGGER_FS_FREE_MARGIN
#define LOGGER_FS_FREE_MARGIN  (128u * 1024u)
#endif
#ifndef LOGGER_REQUIRE_PSRAM
#define LOGGER_REQUIRE_PSRAM   1
#endif

// Recovery behavior
#ifndef LOGGER_RECOVERY_BASE_MS
#define LOGGER_RECOVERY_BASE_MS   1000
#endif
#ifndef LOGGER_RECOVERY_MAX_MS
#define LOGGER_RECOVERY_MAX_MS    30000
#endif
#ifndef LOGGER_RECOVERY_FMT_EVERY
#define LOGGER_RECOVERY_FMT_EVERY 5
#endif

class Logger {
public:
    // -------- Singleton access (pointer-style) --------
    // Call once at boot (optional, Get() auto-creates too):
    //   Logger::Init(&rtc);
    //   LOGG->Begin();
    static void    Init(RTCManager* rtc = nullptr);
    static Logger* Get();     // ALWAYS returns a valid pointer (auto-constructs)
    static Logger* TryGet();  // May return nullptr if not created yet

    // -------- Lifecycle --------
    bool Begin();             // mount FS, start tasks, create log file if missing
    ~Logger() = default;

    // Optional: attach/replace RTC after construction
    void SetRTC(RTCManager* rtc) { Rtc = rtc; }

    // -------- Public API --------
    bool   addLogEntry(const JsonObject& newEntry);
    String readLogFile();
    bool   clearLogFile();
    bool   deleteLogFile();
    bool   createLogFile();
    bool   closeLogFile();

    // Convenience (heap-free)
    void   logLockAction(const String& action);
    void   logBatteryLow(const String& message);
    void   logMessageReceived(const String& message);
    void   logAckSent(const String& message);

private:
    // Make constructor private → singleton only
    Logger() = default;

    // No copy/assign
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Static singleton storage
    static Logger* s_instance;

    // Internal state
    bool        initialized   = false;
    RTCManager* Rtc           = nullptr;

    // RTOS primitives
    TaskHandle_t      maintTask_   = nullptr;  // rotation + flushing
    TaskHandle_t      recoverTask_ = nullptr;  // FS recovery/backoff
    SemaphoreHandle_t mutex_       = nullptr;  // protects FS & queue

    struct Item { char line[LOGGER_MAX_LINE_BYTES]; };

    // PSRAM queue (strict — no DRAM fallback)
    Item*    queue_         = nullptr;
    uint16_t qCap_          = 0;
    uint16_t qHead_         = 0;
    uint16_t qTail_         = 0;
    uint16_t qCount_        = 0;

    // FS health & recovery
    bool     fsHealthy_     = false;
    bool     notifiedDrop_  = false;
    bool     warnedNoPSRAM_ = false;

    enum FSState : uint8_t {
        FS_UNMOUNTED = 0,
        FS_MOUNTING,
        FS_MOUNTED,
        FS_NEEDS_FORMAT,
        FS_FORMATTING,
        FS_ERROR
    };
    volatile FSState fsState_ = FS_UNMOUNTED;
    uint32_t backoffMs_       = LOGGER_RECOVERY_BASE_MS;
    uint32_t attempts_        = 0;

    // --- RTOS tasks ---
    static void MaintTaskTrampoline(void* arg);
    void        MaintTaskLoop();
    static void RecoverTaskTrampoline(void* arg);
    void        RecoverTaskLoop();

    // --- FS helpers ---
    bool   ensureFS(bool allowFormat);
    void   safeFormat();
    bool   tryAppendLine(const char* line);
    void   rotateIfNeeded();
    void   ensureFsBudget(size_t bytesNeeded);
    size_t fsFreeBytes() const;

    // --- PSRAM queue ops ---
    bool   allocateQueue();
    void   freeQueue();
    void   enqueueLine(const char* line);
    bool   dequeueLine(Item& out);
    void   flushQueue();

    // --- line formatter ---
    void   formatLine(char* out, size_t outSz,
                      const char* eventType,
                      const char* message,
                      bool status,
                      const char* macOpt = nullptr);
    size_t jsonEscape(char* dst, size_t dstSz, const char* src);
};

// Pointer-style convenience macro:
//   LOGG->Begin(); LOGG->logLockAction(".."); LOGG->addLogEntry(obj);
#define LOGG Logger::Get()

#endif // LOGGER_H






