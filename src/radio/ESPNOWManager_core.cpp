#include <ESPNOWManager.hpp>
#include <CommandAPI.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <Transport.hpp>
#include <TransportManager.hpp>
#include <Utils.hpp>
#include <esp_task_wdt.h>
#include <stdio.h>
#include <string.h>


// =============================================================
//  Static Storage
// =============================================================
EspNowManager* EspNowManager::instance = nullptr;
namespace {
// Journal allow-list (no timer entries)
inline bool isAllowedJournalType(const char* t) {
  if (!t) return false;
  return !strcmp(t,"LOW_BATT")   || !strcmp(t,"CRITICAL") ||
         !strcmp(t,"LOCKED")     || !strcmp(t,"UNLOCKED") ||
         !strcmp(t,"BREACH")     || !strcmp(t,"FP_MATCH") ||
         !strcmp(t,"FP_FAIL")    || !strcmp(t,"STATE")    ||
         !strcmp(t,"MOTOR_FAIL") || !strcmp(t,"RESET");
}

// Keep NDJSON compact so "EVT:<line>" fits inside 120-byte wire payload
static constexpr size_t MAX_NDJSON_LINE = 100;
}

// =============================================================
//  Ctor / Dtor
// =============================================================
EspNowManager::EspNowManager(RTCManager* RTC,
                             PowerManager* Power,
                             MotorDriver* motor,
                             SleepTimer* Slp,
                             Fingerprint* fng)
    : RTC(RTC), Power(Power), motor(motor), Slp(Slp), sw(nullptr),fng(fng) {
  instance = this;
  breach = (CONF ? CONF->GetBool(BREACH_STATE, BREACH_STATE_DEFAULT) : false);

  //DBG_PRINTLN("[ESPNOW][Ctor] Constructing EspNowManager…");

  // Queues
  rxQ   = xQueueCreate(ESPNOW_RX_QUEUE_SIZE, sizeof(RxEvent));
  txQ   = xQueueCreate(ESPNOW_TX_QUEUE_SIZE, sizeof(TxAckEvent));
  sendQ = xQueueCreate(ESPNOW_TX_QUEUE_SIZE, sizeof(TxAckEvent));

  DBG_PRINTF("[ESPNOW][Ctor] Queues rxQ=%p txQ=%p sendQ=%p (sizes: rx=%u tx=%u)\n",
               (void*)rxQ, (void*)txQ, (void*)sendQ, (unsigned)ESPNOW_RX_QUEUE_SIZE, (unsigned)ESPNOW_TX_QUEUE_SIZE);

  // Worker (persistent)
  xTaskCreate(
      workerTask,
      "esn_worker",
      ESPNOW_WORKER_STACK,
      this,
      ESPNOW_WORKER_PRIO,
      &workerH);

  DBG_PRINTF("[ESPNOW][Ctor] Worker created: handle=%p core=%d stack=%u prio=%u\n",
               (void*)workerH, (int)ESPNOW_WORKER_CORE, (unsigned)ESPNOW_WORKER_STACK, (unsigned)ESPNOW_WORKER_PRIO);
}

EspNowManager::~EspNowManager() {
  DBG_PRINTLN("[ESPNOW][Dtor] ~EspNowManager() deinit…");
  deinit();
  DBG_PRINTLN("[ESPNOW][Dtor] Done.");
}

// =============================================================
//  Init / Deinit
// =============================================================
esp_err_t EspNowManager::init() {
  DBG_PRINTLN("[ESPNOW][init] Begin init()");
  DBG_PRINTLN("[ESPNOW][init] Starting ESP-NOW stack...");

  uint8_t desiredChannel = getDefaultChannel_();
  if (CONF && CONF->GetBool(DEVICE_CONFIGURED, false)) {
    desiredChannel = static_cast<uint8_t>(
        CONF->GetInt(MASTER_CHANNEL_KEY, MASTER_CHANNEL_DEFAULT));
    DBG_PRINTF("[ESPNOW][init] Configured: use stored channel=%u\n",
               (unsigned)desiredChannel);
  } else {
    DBG_PRINTF("[ESPNOW][init] Unconfigured: use default channel=%u\n",
               (unsigned)desiredChannel);
  }
  setChannel_(desiredChannel);

  if (esp_now_init() != ESP_OK) {
    DBG_PRINTLN("[ESPNOW]Failed to initialize ESPNOW ");
    return ESP_FAIL;
  }
  if (!setPmk_()) {
    DBG_PRINTLN("[ESPNOW][init] Failed to set PMK");
    return ESP_FAIL;
  }
  DBG_PRINTLN("[ESPNOW][init] esp_now_init() OK, registering callbacks");
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataReceived);


  // If already configured, ensure peer is registered and send startup bundle
  if (isConfigured_()) {
    DBG_PRINTLN("[ESPNOW][init] Device is configured; ensure master peer and send BOOT bundle");
    secure_ = true;
    uint8_t mac_addr[6];
    if (getMacAddress(mac_addr) == ESP_OK) {
      bool existed = esp_now_is_peer_exist(mac_addr);
      DBG_PRINTF("[ESPNOW][init] Stored master: %02X:%02X:%02X:%02X:%02X:%02X exist=%d\n",
                   mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5], existed);
      if (!setupSecurePeer_(mac_addr, channel_)) {
        DBG_PRINTLN("[ESPNOW][init] secure peer setup failed");
      }
    } else {
      DBG_PRINTLN("[ESPNOW][init] No stored master MAC yet.");
    }

    // Startup bundle — ONLY configured + battery (NO STATE here, NO HB)
    sendConfiguredBundle_("BOOT");
    // Load any existing journal from NVS to RAM
    nvLoadJournal_();
  } else {
    secure_ = false;
    DBG_PRINTLN("[ESPNOW][init] Device not configured; waiting for INIT (pairing).");
  }

  // Start the presence timer fresh to avoid an immediate watchdog ping
  lastHbMs_ = millis();
  nextPingDueMs_ = lastHbMs_ + PING_INTERVAL_MS;

  DBG_PRINTLN("[ESPNOW]ESPNOW Initialized Successfully ");
  return ESP_OK;
}

esp_err_t EspNowManager::deinit() {
  DBG_PRINTLN("[ESPNOW][deinit] Unregister and deinit ESPNOW…");
  esp_now_unregister_send_cb();
  esp_now_unregister_recv_cb();
  esp_now_deinit();

  if (workerH) { vTaskDelete(workerH); workerH = nullptr; DBG_PRINTLN("[ESPNOW][deinit] Worker task deleted."); }
  if (rxQ)     { vQueueDelete(rxQ);    rxQ = nullptr;     DBG_PRINTLN("[ESPNOW][deinit] rxQ deleted.");        }
  if (txQ)     { vQueueDelete(txQ);    txQ = nullptr;     DBG_PRINTLN("[ESPNOW][deinit] txQ deleted.");        }
  if (sendQ)   { vQueueDelete(sendQ);  sendQ = nullptr;   DBG_PRINTLN("[ESPNOW][deinit] sendQ deleted.");      }
  DBG_PRINTLN("[ESPNOW][deinit] Done.");
  return ESP_OK;
}

// =============================================================
//  Peer Management / Send
// =============================================================
esp_err_t EspNowManager::sendData(const uint8_t* peer_addr, const uint8_t* data, size_t len) {
  if (!peer_addr || !data) {
    DBG_PRINTLN("[ESPNOW][sendData] Invalid args: peer/data is null");
    return ESP_ERR_INVALID_ARG;
  }
  if (len > ESPNOW_MAX_DATA_LEN) {
    DBG_PRINTF("[ESPNOW][sendData] Invalid size: %u > max %u\n",
                 (unsigned)len, (unsigned)ESPNOW_MAX_DATA_LEN);
    return ESP_ERR_INVALID_SIZE;
  }
  DBG_PRINTF("[ESPNOW][sendData] -> %02X:%02X:%02X:%02X:%02X:%02X len=%u\n",
               peer_addr[0], peer_addr[1], peer_addr[2], peer_addr[3], peer_addr[4], peer_addr[5], (unsigned)len);
  debugDumpPacket_("TX", data, len);
  return esp_now_send(peer_addr, data, len);
}

// =============================================================
//  Public Helpers
// =============================================================
void EspNowManager::setInitMode(bool) { /* no-op */ }

void EspNowManager::setConfigMode(bool enabled) {
  configMode_ = enabled;
  DBG_PRINTLN(String("[ESPNOW] Config mode ") + (enabled ? "ENABLED" : "DISABLED"));
}

void EspNowManager::storeMacAddress(const uint8_t* mac_addr) {
  if (!CONF || !mac_addr) { DBG_PRINTLN("[ESPNOW][storeMacAddress] Missing Conf or mac"); return; }
  char mac_str[18];
  snprintf(mac_str,
           sizeof(mac_str),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  DBG_PRINTF("[ESPNOW][storeMacAddress] %s\n", mac_str);
  CONF->PutString(MASTER_ESPNOW_ID, mac_str);
}

esp_err_t EspNowManager::getMacAddress(uint8_t* mac_addr) {
  if (!mac_addr || !CONF) {
    DBG_PRINTLN("[ESPNOW][getMacAddress] Invalid arg or Conf=null");
    return ESP_ERR_INVALID_ARG;
  }
  String mac_str = CONF->GetString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
  DBG_PRINTLN(String("[ESPNOW][getMacAddress] String=") + mac_str);
  if (mac_str.length() == 17 && parseMacToBytes(mac_str, mac_addr)) return ESP_OK;
  return ESP_ERR_INVALID_ARG;
}

bool EspNowManager::compareMacAddress(const uint8_t* mac_addr) {
  if (!mac_addr || !CONF) { DBG_PRINTLN("[ESPNOW][compareMacAddress] Missing mac or Conf"); return false; }
  String stored_mac_str = CONF->GetString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
  uint8_t stored_mac[6];
  if (!parseMacToBytes(stored_mac_str, stored_mac)) {
    DBG_PRINTLN("[ESPNOW][compareMacAddress] Stored master parse failed");
    return false;
  }
  bool eq = true;
  for (int i = 0; i < 6; ++i) if (stored_mac[i] != mac_addr[i]) { eq = false; break; }
  DBG_PRINTF("[ESPNOW][compareMacAddress] equal=%d\n", (int)eq);
  return eq;
}

// =============================================================
//  Worker
// =============================================================
void EspNowManager::workerTask(void* selfPtr) {
  auto* self = static_cast<EspNowManager*>(selfPtr);
  if (!self) vTaskDelete(nullptr);

  DBG_PRINTLN("[ESPNOW][worker] Started.");

  uint32_t ctr = 0;
  for (;;) {
    // Keep task watchdog happy while doing radio/queue work.
    esp_task_wdt_reset();

    const uint32_t now = millis();

    self->pollPairing_();

    // 1) RX
    RxEvent rx{};
    if (xQueueReceive(self->rxQ, &rx, pdMS_TO_TICKS(5)) == pdPASS) {
      DBG_PRINTF("[ESPNOW][worker][RX] pop len=%d from %02X:%02X:%02X:%02X:%02X:%02X\n",
                   rx.len, rx.mac[0], rx.mac[1], rx.mac[2], rx.mac[3], rx.mac[4], rx.mac[5]);
      self->processRx(rx);
    }

    // 2) Convert queued responses to paced send entries (txQ -> sendQ)
    TxAckEvent tx{};
    if (xQueueReceive(self->txQ, &tx, 0) == pdPASS) {
      uint16_t opcode = 0;
      if (tx.len >= 3) {
        opcode = static_cast<uint16_t>(tx.data[1]) |
                 (static_cast<uint16_t>(tx.data[2]) << 8);
      }
      DBG_PRINTF("[ESPNOW][worker][ACK] move to sendQ op=0x%04X attempt=%u status=%u\n",
                 (unsigned)opcode, (unsigned)tx.attempts, (unsigned)(tx.status ? 1 : 0));
      (void)xQueueSend(self->sendQ, &tx, 0);
    }

    // 3) If nothing is in flight, send next
    self->trySendNext_();

    // 4) No periodic heartbeat — master requests HB via HBRQ
    self->heartbeatTick_();  // no-op

    // ---- Everything below is DISABLED until device is configured ----
    const bool configured = self->isConfigured_();
    if (configured) {
      if (now >= self->nextPingDueMs_) {
        // Schedule next tick first (fixed 30s cadence)
        self->nextPingDueMs_ = now + PING_INTERVAL_MS;

        const bool ok = self->pingMaster(1);     // one single attempt
        if (ok) {
          if (!self->online_) {
            self->setOffline(false);
            self->lastHbMs_ = now;
            // Optional: flush any offline journal
            (void)self->flushJournalToMaster_();
          }
        } else {
          if (self->online_) {
            self->setOffline(true);
          }
        }
      }
    } else {
      // Unconfigured: never ping; force offline
      if (self->online_) self->setOffline(true);
    }

    // Optional: monitor stack headroom
    if ((++ctr % 2000) == 0) {
      UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);
      //DBG_PRINTF("[ESPNOW][worker] free stack (words): %u\n", (unsigned)hw);
    }

    vTaskDelay(pdMS_TO_TICKS(1));
    taskYIELD();
  }
}

// =============================================================
//  Utility
// =============================================================
bool EspNowManager::parseMacToBytes(const String& macAddress, uint8_t out[6]) {
  if (!out) {
    DBG_PRINTLN("[ESPNOW][parseMacToBytes] out=null");
    return false;
  }
  unsigned int tmp[6];
  if (sscanf(macAddress.c_str(), "%x:%x:%x:%x:%x:%x",
             &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]) != 6) {
    DBG_PRINTLN("[ESPNOW][parseMacToBytes] sscanf failed");
    return false;
  }
  for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(tmp[i]);
  DBG_PRINTLN(String("[ESPNOW][parseMacToBytes] OK: ") + macAddress);
  return true;
}

void EspNowManager::debugDumpPacket_(const char* tag, const uint8_t* data, size_t len) {
  if (!tag || !data || len == 0) return;
  char hex[(ESPNOW_MAX_DATA_LEN * 3) + 1];
  size_t out = 0;
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len && (out + 3) < sizeof(hex); ++i) {
    hex[out++] = kHex[(data[i] >> 4) & 0x0F];
    hex[out++] = kHex[data[i] & 0x0F];
    if (i + 1 < len && (out + 1) < sizeof(hex)) {
      hex[out++] = ' ';
    }
  }
  hex[out] = '\0';
  DBG_PRINTF("[ESPNOW][%s] len=%u data=", tag, (unsigned)len);
  DBG_PRINT(hex);
  DBG_PRINTLN("");
}

// =============================================================
//  Journal: RAM ring (String) + NVS coalesce
// =============================================================
bool EspNowManager::spoolImportant_(const char* type, const String& json) {
  // Skip completely if device not configured yet
  if (!isConfigured_()) {
    DBG_PRINTLN("[ESPNOW][journal] skip: not configured");
    return false;
  }

  // Enforce journal policy: allow-list only (no timer entries)
  if (!isAllowedJournalType(type)) {
    DBG_PRINTLN(String("[ESPNOW][journal] drop type=") + (type ? type : "NULL"));
    return false;
  }

  // Build compact NDJSON: {"seq":N,"type":"X","d":{...}}\n
  String inner; inner.reserve(json.length());
  if (json.length()) {
    if (json.charAt(0) == '{') {
      // Strip surrounding braces so we control final size precisely
      int L = json.length();
      inner = (L >= 2) ? json.substring(1, L - 1) : "";
    } else {
      inner = json; // already "k:v" style
    }
  }

  // Fixed pieces we’ll render: prefix + inner + suffix
  // prefix: {"seq":<N>,"type":"<T>","d":{
  String seqStr = String(++seq_);
  const String prefix = String("{\"seq\":") + seqStr + ",\"type\":\"" +
                        (type ? type : "UNK") + "\",\"d\":{";
  const String suffix = "}}\n";

  // Budget for inner so total line <= MAX_NDJSON_LINE
  size_t budget = (MAX_NDJSON_LINE > (prefix.length() + suffix.length()))
                  ? (MAX_NDJSON_LINE - prefix.length() - suffix.length())
                  : 0;

  if (inner.length() > budget) {
    // If too large, prefer empty payload (keeps event but fits the wire)
    DBG_PRINTF("[ESPNOW][journal] truncate inner %u->%u\n",
                 (unsigned)inner.length(), (unsigned)budget);
    inner = ""; // minimal `{}` data — policy: keep the event, trim details
  }

  String line;
  line.reserve(prefix.length() + inner.length() + suffix.length());
  line += prefix;
  line += inner;
  line += suffix;

  // Final safety: hard cap (shouldn’t trigger with above budgeting)
  if (line.length() > MAX_NDJSON_LINE) {
    DBG_PRINTF("[ESPNOW][journal] hard-cap %u->%u\n",
                 (unsigned)line.length(), (unsigned)MAX_NDJSON_LINE);
    // Keep valid JSON: drop inner entirely
    line = String("{\"seq\":") + seqStr + ",\"type\":\"" +
           (type ? type : "UNK") + "\",\"d\":{}}\n";
  }

  // Append to RAM buffer and mark dirty
  journalBuf_ += line;
  journalCount_++;
  needsFlush_ = true;

  DBG_PRINTF("[ESPNOW][journal] spool seq=%lu type=%s len=%u count=%u\n",
               (unsigned long)seq_, type ? type : "UNK",
               (unsigned)line.length(), (unsigned)journalCount_);
  return true;
}

void EspNowManager::nvLoadJournal_() {
  if (!CONF) { DBG_PRINTLN("[ESPNOW][journal] nvLoadJournal_: Conf=null"); return; }
  journalBuf_   = CONF->GetString(nvsKeyBuf_, "");
  journalCount_ = (uint16_t)CONF->GetString(nvsKeyCnt_, "0").toInt();
  lastJournalSaveMs_ = millis();
  DBG_PRINTF("[ESPNOW][journal] nvLoad bufLen=%u count=%u\n",
               (unsigned)journalBuf_.length(), (unsigned)journalCount_);
}

bool EspNowManager::nvSaveJournal_(const char* reason) {
  if (!CONF) { DBG_PRINTLN("[ESPNOW][journal] nvSaveJournal_: Conf=null"); return false; }
  if (!isConfigured_()) { DBG_PRINTLN("[ESPNOW][journal] skip save (unconfigured)"); return true; }
  if (!needsFlush_) { return true; }

  CONF->PutString(nvsKeyBuf_, journalBuf_);
  CONF->PutString(nvsKeyCnt_, String((int)journalCount_));
  CONF->PutString(nvsKeySeq_, String((unsigned long)seq_));
  needsFlush_ = false;
  lastJournalSaveMs_ = millis();
  DBG_PRINTLN(String("[ESPNOW][journal] nvSave OK (reason=") + (reason ? reason : "N/A") + ")");
  return true;
}


void EspNowManager::nvClearJournal_() {
  if (!CONF) { DBG_PRINTLN("[ESPNOW][journal] nvClearJournal_: Conf=null"); return; }
  CONF->PutString(nvsKeyBuf_, "");
  CONF->PutString(nvsKeyCnt_, "0");
  // Keep seq_ growing; do not reset nvsKeySeq_
  journalBuf_ = "";
  journalCount_ = 0;
  needsFlush_ = false;
  DBG_PRINTLN("[ESPNOW][journal] Cleared NVS + RAM buffers");
}

size_t EspNowManager::flushJournalToMaster_() {
  if (!isConfigured_()) { DBG_PRINTLN("[ESPNOW][journal] flush: not configured"); return 0; }

  // Ensure latest RAM -> NVS sync before we start
  (void)nvSaveJournal_("preflush");

  // Combine any NVS content (safety) with RAM (already done in nvLoad/nvSave path)
  String buf = journalBuf_;
  size_t sent = 0;
  int start = 0;
  for (;;) {
    int nl = buf.indexOf('\n', start);
    if (nl < 0) break;
    String line = buf.substring(start, nl);
    start = nl + 1;
    if (line.length() == 0) continue;

    // Feed watchdog between journal lines; flush can span many events.
    esp_task_wdt_reset();

    // Replay as EVT_GENERIC with NDJSON payload bytes
    SendAck(EVT_GENERIC,
            reinterpret_cast<const uint8_t*>(line.c_str()),
            static_cast<size_t>(line.length()),
            true);
    ++sent;

    // Pace a little to let sendQ drain
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  // Clear after enqueueing
  nvClearJournal_();
  DBG_PRINTF("[ESPNOW][journal] Flushed %u lines to master\n", (unsigned)sent);
  return sent;
}
