#include <Logger.hpp>
#include <Config.hpp>
#include <RTCManager.hpp>
#include <Utils.hpp>
#include <FS.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <ctype.h>
#include <stdio.h>

// simple member-aware lock macros
#define LOCK()   if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY)
#define UNLOCK() if (mutex_) xSemaphoreGive(mutex_)

// ---------------- Singleton storage ----------------
Logger* Logger::s_instance = nullptr;

// ---------------- Singleton API ----------------
void Logger::Init(RTCManager* rtc) {
    if (!s_instance) s_instance = new Logger();
    if (rtc) s_instance->Rtc = rtc;
}

Logger* Logger::Get() {
    if (!s_instance) s_instance = new Logger();
    return s_instance;
}

Logger* Logger::TryGet() {
    return s_instance; // may be nullptr if Init/Get not called yet
}

// ---------------- Begin ----------------
bool Logger::Begin() {
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();

    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#                   Starting Log Manager                  #");
    DBG_PRINTLN("###########################################################");

    // First mount attempt (non-blocking, but do once here)
    fsHealthy_ = ensureFS(/*allowFormat=*/true);
    fsState_   = fsHealthy_ ? FS_MOUNTED : FS_UNMOUNTED;

    if (fsHealthy_ && !SPIFFS.exists(LOGFILE_PATH)) {
        createLogFile();
    }

    // Allocate PSRAM queue (strict; no DRAM fallback)
    (void)allocateQueue();

    if (!maintTask_) {
        xTaskCreate(
            MaintTaskTrampoline,
            "LoggerMaint",
            LOGGER_TASK_STACK,
            this,
            LOGGER_TASK_PRIO,
            &maintTask_
        );
    }
    if (!recoverTask_) {
        xTaskCreate(
            RecoverTaskTrampoline,
            "LoggerRecover",
            LOGGER_TASK_STACK,
            this,
            LOGGER_TASK_PRIO,
            &recoverTask_
        );
    }

    initialized = true;
    return fsHealthy_;
}

// ---------------- Public API ----------------
bool Logger::addLogEntry(const JsonObject& entry) {
    if (!initialized) return false;

    const char* et  = entry.containsKey("event_type") ? entry["event_type"].as<const char*>() : "event";
    const char* msg = entry.containsKey("message")    ? entry["message"].as<const char*>()    : "";
    bool st         = entry.containsKey("status")     ? entry["status"].as<bool>()            : false;
    const char* mac = entry.containsKey("mac_address")? entry["mac_address"].as<const char*>(): nullptr;

    char line[LOGGER_MAX_LINE_BYTES];
    formatLine(line, sizeof(line), et, msg, st, mac);

    if (!tryAppendLine(line)) {
        // FS down → enqueue for later flushing
        enqueueLine(line);
        return false;
    }
    return true;
}

String Logger::readLogFile() {
    if (!initialized) return String();
    if (!SPIFFS.exists(LOGFILE_PATH)) return String();

    LOCK();
    File f = SPIFFS.open(LOGFILE_PATH, FILE_READ);
    if (!f) { UNLOCK(); return String(); }

    String s;
    s.reserve(f.size() + 16);
    while (f.available()) s += (char)f.read();
    f.close();
    UNLOCK();
    return s;
}

bool Logger::clearLogFile() {
    if (!initialized) return false;
    LOCK();
    SPIFFS.remove(LOGFILE_PATH);
    bool ok = createLogFile();
    UNLOCK();
    return ok;
}

bool Logger::deleteLogFile() {
    if (!initialized) return false;
    LOCK();
    bool ok = SPIFFS.remove(LOGFILE_PATH);
    UNLOCK();
    return ok;
}

bool Logger::createLogFile() {
    LOCK();
    File f = SPIFFS.open(LOGFILE_PATH, FILE_WRITE);
    if (!f) { UNLOCK(); return false; }
    f.close();
    UNLOCK();
    return true;
}

bool Logger::closeLogFile() { return true; }

// ---------------- Convenience (heap-free) ----------------
void Logger::logLockAction(const String& action) {
    if (!initialized) return;
    char line[LOGGER_MAX_LINE_BYTES];
    formatLine(line, sizeof(line), "lock", action.c_str(), true, nullptr);
    if (!tryAppendLine(line)) enqueueLine(line);
}
void Logger::logBatteryLow(const String& message) {
    if (!initialized) return;
    char line[LOGGER_MAX_LINE_BYTES];
    formatLine(line, sizeof(line), "battery", message.c_str(), false, nullptr);
    if (!tryAppendLine(line)) enqueueLine(line);
}
void Logger::logMessageReceived(const String& message) {
    if (!initialized) return;
    char line[LOGGER_MAX_LINE_BYTES];
    formatLine(line, sizeof(line), "message", message.c_str(), true, nullptr);
    if (!tryAppendLine(line)) enqueueLine(line);
}
void Logger::logAckSent(const String& message) {
    if (!initialized) return;
    char line[LOGGER_MAX_LINE_BYTES];
    formatLine(line, sizeof(line), "ack_sent", message.c_str(), true, "12:34:56:78:9A:BC");
    if (!tryAppendLine(line)) enqueueLine(line);
}

// ---------------- FS helpers ----------------
bool Logger::ensureFS(bool allowFormat) {
    if (SPIFFS.begin(false)) return true;
    if (SPIFFS.begin(true))  return true;

    if (allowFormat) {
        safeFormat();
        if (SPIFFS.begin(false)) return true;
    }
    return false;
}

void Logger::safeFormat() {
    DBG_PRINTLN("[Logger] SPIFFS: formatting (requested by recovery)...");
    SPIFFS.format();
}

size_t Logger::fsFreeBytes() const {
    return SPIFFS.totalBytes() - SPIFFS.usedBytes();
}

void Logger::ensureFsBudget(size_t bytesNeeded) {
    size_t freeNow = fsFreeBytes();
    if (freeNow > bytesNeeded + LOGGER_FS_FREE_MARGIN) return;

    String bak = String(LOGFILE_PATH) + ".1";
    if (SPIFFS.exists(bak)) {
        SPIFFS.remove(bak);
        (void)fsFreeBytes();
    }
}

bool Logger::tryAppendLine(const char* line) {
    // take a snapshot of health under lock
    bool healthy;
    LOCK(); healthy = fsHealthy_; UNLOCK();
    if (!healthy) return false;

    rotateIfNeeded();
    ensureFsBudget(strlen(line) + 2);

    LOCK();
    File f = SPIFFS.open(LOGFILE_PATH, FILE_APPEND);
    if (!f) {
        // flip health while we own the lock
        fsHealthy_ = false;
        UNLOCK();
        return false;
    }

    size_t w1 = f.print(line);
    size_t w2 = f.print("\n");
    f.close();
    if (w1 == 0 || w2 == 0) {
        fsHealthy_ = false;              // mark unhealthy while locked
        UNLOCK();
        return false;
    }
    UNLOCK();
    return true;
}

void Logger::rotateIfNeeded() {
    LOCK();
    File f = SPIFFS.open(LOGFILE_PATH, FILE_READ);
    if (!f) { UNLOCK(); return; }
    size_t sz = f.size();
    f.close();
    if (sz < LOGGER_ROTATE_BYTES) { UNLOCK(); return; }

    String bak = String(LOGFILE_PATH) + ".1";
    if (SPIFFS.exists(bak)) SPIFFS.remove(bak);
    SPIFFS.rename(LOGFILE_PATH, bak);
    (void)createLogFile();
    UNLOCK();
}

// ---------------- PSRAM queue (strict) ----------------
bool Logger::allocateQueue() {
    if (!psramFound()) {
#if LOGGER_REQUIRE_PSRAM
        if (!warnedNoPSRAM_) {
            DBG_PRINTLN("[Logger] PSRAM not found → buffering disabled.");
            warnedNoPSRAM_ = true;
        }
        return false;
#else
        return false;
#endif
    }

    uint16_t target = LOGGER_QUEUE_DEPTH;
    while (target >= 8) {
        size_t need = sizeof(Item) * target;
        Item* mem = (Item*) heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (mem) {
            queue_ = mem;
            qCap_  = target;
            DBG_PRINT("[Logger] PSRAM queue depth="); DBG_PRINTLN(qCap_);
            return true;
        }
        target /= 2;
    }
    return false;
}

void Logger::freeQueue() {
    if (queue_) heap_caps_free(queue_);
    queue_ = nullptr;
    qCap_ = qHead_ = qTail_ = qCount_ = 0;
}

void Logger::enqueueLine(const char* line) {
    if (!queue_ || qCap_ == 0) {
        if (!warnedNoPSRAM_) {
            DBG_PRINTLN("[Logger] PSRAM queue unavailable → dropping buffered logs.");
            warnedNoPSRAM_ = true;
        }
        return;
    }

    LOCK();

    // shed if DRAM critically low (stability first)
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < LOGGER_MIN_FREE_HEAP && qCount_ > 0) {
        qHead_ = (qHead_ + 1) % qCap_;
        qCount_--;
        if (!notifiedDrop_) { DBG_PRINTLN("[Logger] low heap → dropped oldest queued entry."); notifiedDrop_ = true; }
    }

    if (qCount_ == qCap_) {
        qHead_ = (qHead_ + 1) % qCap_;
        qCount_--;
        if (!notifiedDrop_) { DBG_PRINTLN("[Logger] PSRAM queue full → dropping oldest."); notifiedDrop_ = true; }
    }

    strncpy(queue_[qTail_].line, line, LOGGER_MAX_LINE_BYTES - 1);
    queue_[qTail_].line[LOGGER_MAX_LINE_BYTES - 1] = '\0';
    qTail_ = (qTail_ + 1) % qCap_;
    qCount_++;

    UNLOCK();
}

bool Logger::dequeueLine(Item& out) {
    if (!queue_ || qCount_ == 0) return false;
    LOCK();
    out = queue_[qHead_];
    qHead_ = (qHead_ + 1) % qCap_;
    qCount_--;
    UNLOCK();
    return true;
}

void Logger::flushQueue() {
    // snapshot of health under lock
    bool healthy;
    LOCK(); healthy = fsHealthy_; UNLOCK();
    if (!healthy || !queue_) return;

    Item it;
    while (dequeueLine(it)) {
        if (!tryAppendLine(it.line)) {
            // push back to the head to retry later
            LOCK();
            qHead_ = (qHead_ + qCap_ - 1) % qCap_;
            queue_[qHead_] = it;
            qCount_++;
            UNLOCK();
            break;
        }
    }
    notifiedDrop_ = false;
}

// ---------------- RTOS tasks ----------------
void Logger::MaintTaskTrampoline(void* arg) {
    static_cast<Logger*>(arg)->MaintTaskLoop();
}
void Logger::MaintTaskLoop() {
    for (;;) {
        bool healthy;
        LOCK(); healthy = fsHealthy_; UNLOCK();
        if (healthy) {
            rotateIfNeeded();
            flushQueue();
        }
        vTaskDelay(pdMS_TO_TICKS(LOGGER_TICK_MS));
    }
}

void Logger::RecoverTaskTrampoline(void* arg) {
    static_cast<Logger*>(arg)->RecoverTaskLoop();
}
void Logger::RecoverTaskLoop() {
    for (;;) {
        bool healthy;
        LOCK(); healthy = fsHealthy_; UNLOCK();

        if (!healthy) {
            // Enter recovery state machine
            FSState stateSnapshot;
            LOCK(); stateSnapshot = fsState_; UNLOCK();

            if (stateSnapshot == FS_UNMOUNTED || stateSnapshot == FS_ERROR) {
                LOCK(); fsState_ = FS_MOUNTING; UNLOCK();

                DBG_PRINTLN("[Logger] Recovery: mounting SPIFFS...");
                bool ok = ensureFS(/*allowFormat=*/false);
                if (!ok) {
                    attempts_++;
                    if (attempts_ % LOGGER_RECOVERY_FMT_EVERY == 0) {
                        LOCK(); fsState_ = FS_FORMATTING; UNLOCK();
                        DBG_PRINTLN("[Logger] Recovery: formatting SPIFFS (escalation)...");
                        safeFormat();
                        ok = ensureFS(/*allowFormat=*/false);
                    }
                }

                if (ok) {
                    DBG_PRINTLN("[Logger] Recovery: SPIFFS mounted ✅");
                    LOCK();
                    fsState_  = FS_MOUNTED;
                    fsHealthy_ = true;
                    UNLOCK();

                    attempts_ = 0;
                    backoffMs_ = LOGGER_RECOVERY_BASE_MS;

                    if (!SPIFFS.exists(LOGFILE_PATH)) {
                        (void)createLogFile();
                    }
                    flushQueue();
                } else {
                    LOCK(); fsState_ = FS_ERROR; UNLOCK();
                    backoffMs_ = (backoffMs_ << 1);
                    if (backoffMs_ > LOGGER_RECOVERY_MAX_MS) backoffMs_ = LOGGER_RECOVERY_MAX_MS;
                    DBG_PRINT("[Logger] Recovery: mount failed. Backing off ");
                    DBG_PRINT(backoffMs_); DBG_PRINTLN(" ms");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(healthy ? 2000 : backoffMs_));
    }
}

// ---------------- Compact JSON lines ----------------
void Logger::formatLine(char* out, size_t outSz,
                        const char* eventType,
                        const char* message,
                        bool status,
                        const char* macOpt) {
    // {"t":epoch,"e":"x","m":"...","k":1,"a":"mac"}
    char msgEsc[LOGGER_MAX_LINE_BYTES/2];
    jsonEscape(msgEsc, sizeof(msgEsc), message ? message : "");

    unsigned long epoch = Rtc ? (unsigned long)Rtc->getUnixTime() : 0UL;
    char et = 'e';
    if (eventType && eventType[0]) et = (char)tolower((unsigned char)eventType[0]);

    if (macOpt && macOpt[0]) {
        snprintf(out, outSz,
            "{\"t\":%lu,\"e\":\"%c\",\"m\":\"%s\",\"k\":%d,\"a\":\"%s\"}",
            epoch, et, msgEsc, status ? 1 : 0, macOpt);
    } else {
        snprintf(out, outSz,
            "{\"t\":%lu,\"e\":\"%c\",\"m\":\"%s\",\"k\":%d}",
            epoch, et, msgEsc, status ? 1 : 0);
    }
}

size_t Logger::jsonEscape(char* dst, size_t dstSz, const char* src) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 2 < dstSz; ++i) {
        const char c = src[i];
        switch (c) {
            case '\"': if (di+2<dstSz){ dst[di++]='\\'; dst[di++]='\"'; } break;
            case '\\': if (di+2<dstSz){ dst[di++]='\\'; dst[di++]='\\'; } break;
            case '\b': if (di+2<dstSz){ dst[di++]='\\'; dst[di++]='b'; }  break;
            case '\f': if (di+2<dstSz){ dst[di++]='\\'; dst[di++]='f'; }  break;
            case '\n': if (di+2<dstSz){ dst[di++]='\\'; dst[di++]='n'; }  break;
            case '\r': if (di+2<dstSz){ dst[di++]='\\'; dst[di++]='r'; }  break;
            case '\t': if (di+2<dstSz){ dst[di++]='\\'; dst[di++]='t'; }  break;
            default:
                if ((uint8_t)c < 0x20) {
                    if (di+6 < dstSz) di += snprintf(dst+di, dstSz-di, "\\u%04x", (unsigned char)c);
                } else {
                    dst[di++] = c;
                }
        }
    }
    dst[di] = '\0';
    return di;
}
