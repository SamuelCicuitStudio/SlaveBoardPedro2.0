/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

#include <Arduino.h>
#include <Config.hpp>
#include <ConfigNvs.hpp>
#include <Transport.hpp>
#include <esp_err.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>

class PowerManager;
class MotorDriver;
class RTCManager;
class SleepTimer;
class SwitchManager;
class Fingerprint;
class TransportManager;
// ============================================================================
//                                DEFINITIONS
// ============================================================================

// ---------- ESPNOW Config ----------
#define ESPNOW_MAX_DATA_LEN          250

// ---------- Queues & Worker ----------
#define ESPNOW_RX_QUEUE_SIZE         32
#define ESPNOW_TX_QUEUE_SIZE         32
#undef  ESPNOW_WORKER_STACK
#define ESPNOW_WORKER_STACK          6144      // ↑ more headroom for String ops
#define ESPNOW_WORKER_PRIO           3
#define ESPNOW_WORKER_CORE           APP_CPU_NUM

// ---------- Timing ----------
#define HB_INTERVAL_MS               15000UL   // Heartbeat interval
#define STATE_MIN_INTERVAL_MS        120000UL  // Min interval between state reports
#define PING_INTERVAL_MS  30000UL   // one ping every 30s (no retry bursts)
// ---------- Motor ACK Task ----------
#undef  LOCK_ACK_TASK_STACK_SIZE
#define LOCK_ACK_TASK_STACK_SIZE     4096      // ↑ extra stack for safety
#ifndef LOCK_TASK_PRIORITY
#define LOCK_TASK_PRIORITY           3
#endif
#ifndef LOCK_TASK_CORE
#define LOCK_TASK_CORE               APP_CPU_NUM
#endif

// ---------- ESPNOW Peer Defaults ----------
#ifndef PREER_CHANNEL
#define PREER_CHANNEL                0
#endif
#ifndef PREER_ENCRYPT
#define PREER_ENCRYPT                0
#endif

// ---------- Transmission Retries ----------
#define ESPNOW_TX_MAX_RETRY          4          // 1 initial + 4 retries

// ============================================================================
//                                 DATA STRUCTS
// ============================================================================

// ---------- App-Level Messages ----------
struct InitMessage {
    char token[33];
};

struct CommandMessage {
    String   command;   // App-level only (not on wire)
    char     token[33];
    uint64_t UnixTime;
};

struct Acknowledgment {
    bool     success;
    String   message;   // App-level only (not on wire)
    String   SlaveMac;
};

// ============================================================================
//                                 CLASS DEF
// ============================================================================
class Fingerprint;
class EspNowManager {
public:
    EspNowManager(RTCManager* RTC,
                  PowerManager* Power,
                  MotorDriver* motor,
                  SleepTimer* Slp,
                  Fingerprint* fng);
    ~EspNowManager();

    // ---------- Lifecycle ----------
    esp_err_t init();
    esp_err_t deinit();

    // ---------- Peer Management ----------
    esp_err_t registerPeer(const uint8_t* peer_addr, bool encrypt);
    esp_err_t unregisterPeer(const uint8_t* peer_addr);

    // ---------- Transmission ----------
    esp_err_t sendData(const uint8_t* peer_addr, const uint8_t* data, size_t len);

    // ---------- Public API ----------
    void setInitMode(bool mode);
    // Enable/disable temporary config/unlocked mode (not persisted).
    void setConfigMode(bool enabled);
    void storeMacAddress(const uint8_t* mac_addr);
    esp_err_t getMacAddress(uint8_t* mac_addr);
    bool compareMacAddress(const uint8_t* mac_addr);
    void ProcessComand(String Msg);

    // ---------- TX Helpers ----------
    void SendAck(const String& Msg, bool Status);
    void RequestOff();
    void RequestUnlock();
    void SendMotionTrigg();
    void RequesAlarm();

    // Bridge transport Responses/Events to CommandAPI ACK strings.
    bool handleTransportTx(const transport::TransportMessage& msg);

    // ---------- Utilities ----------
    bool parseMacToBytes(const String& macAddress, uint8_t out[6]);

    // ---------- ESPNOW Callbacks ----------
    static void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status);
    static void onDataReceived(const uint8_t* mac_addr, const uint8_t* data, int len);

    // ---------- Collaborators ----------
    PowerManager*   Power;
    MotorDriver*    motor;
    RTCManager*     RTC;
    SleepTimer*     Slp;
    SwitchManager*  sw;
    Fingerprint*    fng;
    TransportManager* transport = nullptr;
    // ---------- Global Instance ----------
    static EspNowManager* instance;

    // ---------- Security / Alarm ----------
    volatile bool breach;
    bool         isConfigMode() const { return configMode_; }

    // ---------- Master Requests ----------
    void sendHeartbeat(bool force = false);
    void sendState(const char* reason);
    void attachTransport(TransportManager* mgr) { transport = mgr; }
    inline bool isMasterOnline() const { return online_; }

private:
    // ========================================================================
    //                         PRESENCE / WATCHDOG
    // ========================================================================
    bool     online_ = true;
    uint32_t nextPingDueMs_ = 0;
    uint32_t pingBackoffMs_ = 10000;   // 10s -> doubled up to cap

    // ========================================================================
    //                             JOURNAL SYSTEM
    // ========================================================================
    String   journalBuf_;
    uint16_t journalCount_ = 0;
    uint32_t lastJournalSaveMs_ = 0;
    bool     needsFlush_ = false;
    bool     journalDegraded_ = false;

    // NVS key names (must be short; keep ≤ 6 chars)
    const char* nvsKeyBuf_ = "jb";   // journal buffer (NDJSON)
    const char* nvsKeyCnt_ = "jc";   // journal line count (stringified int)
    const char* nvsKeySeq_ = "js";   // last seq (stringified int)

    // Coalesce thresholds
    static constexpr uint16_t JOURNAL_COALESCE_MAX = 8;
    static constexpr uint32_t JOURNAL_COALESCE_MS  = 3000;

    // ========================================================================
    //                             QUEUES & TASKS
    // ========================================================================
    struct RxEvent {
        uint8_t mac[6];
        int     len;
        uint8_t buf[ESPNOW_MAX_DATA_LEN];
    };

    struct TxAckEvent {
        bool     status;
        char     msg[128];
        uint8_t  attempts;
    };

    QueueHandle_t rxQ    = nullptr;
    QueueHandle_t txQ    = nullptr;
    QueueHandle_t sendQ  = nullptr;
    TaskHandle_t  workerH = nullptr;

    // ========================================================================
    //                              STATE TRACKING
    // ========================================================================
    uint32_t seq_           = 0;
    uint32_t lastHbMs_      = 0;
    uint32_t lastStateMs_   = 0;
    bool     hasInFlight_   = false;
    TxAckEvent inFlight_;
    uint8_t  capBitsShadow_ = 0;
    bool     capBitsShadowValid_ = false;
    int8_t   pendingLockEmag_ = -1;
    uint8_t  pendingForceAck_ = 0;

    portMUX_TYPE sendMux_ = portMUX_INITIALIZER_UNLOCKED;

    // ========================================================================
    //                              INTERNAL HELPERS
    // ========================================================================
    static void workerTask(void* self);
    void        processRx(const RxEvent& e);
    void        doSendAck(const TxAckEvent& e);

    void        trySendNext_();
    bool        sendAckNow_(const TxAckEvent& e);

    static uint8_t getDefaultChannel_();
    bool        isConfigured_() const;
    void        sendConfiguredBundle_(const char* reason);
    String      buildStateLine_(const char* reason);
    void        heartbeatTick_();
    uint8_t     getCapBits_();
    void        setCapBitsShadow_(uint8_t bits);

    // Motor completion -> ACK only (no STATE)
    static void MotorAckTask(void* parameter);

    // Safe string builder
    static String makeSafeString_(const char* src, size_t n);
    static String extractCmdCode_(const String& msg);

    // Presence & Journal Helpers
    bool        pingMaster(uint8_t tries);
    inline bool isOnline() const { return online_; }
    inline void setOffline(bool v) { online_ = v; }

    bool        spoolImportant_(const char* type, const String& json);
    void        nvLoadJournal_();
    bool        nvSaveJournal_(const char* reason);
    void        nvClearJournal_();
    size_t      flushJournalToMaster_();

    // Config mode (master-requested; cleared on reboot)
    bool        configMode_ = false;
    bool        secure_ = false;
    uint8_t     channel_ = MASTER_CHANNEL_DEFAULT;
    bool        pendingPairInit_ = false;
    uint32_t    pendingPairInitMs_ = 0;
    uint8_t     pendingPairInitMac_[6] = {0};
    uint8_t     pendingPairInitChannel_ = MASTER_CHANNEL_DEFAULT;

    bool        setChannel_(uint8_t channel);
    bool        setupSecurePeer_(const uint8_t masterMac[6], uint8_t channel);
    bool        handlePairInit_(const uint8_t masterMac[6], const String& msg);
    void        pollPairing_();
};

#endif // ESPNOW_MANAGER_H






