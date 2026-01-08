#include <ESPNOWManager.hpp>
#include <ConfigNvs.hpp>
#include <Logger.hpp>
#include <NVSManager.hpp>
#include <Utils.hpp>
#include <string.h>

// =============================================================
//  Callbacks
// =============================================================
void EspNowManager::onDataSent(const uint8_t* /*mac_addr*/, esp_now_send_status_t status) {
  if (!instance) return;

  DBG_PRINTF("[ESPNOW][TX][onDataSent] status=%d\n", (int)status);

  if (status == ESP_NOW_SEND_SUCCESS) {
    taskENTER_CRITICAL(&instance->sendMux_);
    instance->hasInFlight_ = false;
    taskEXIT_CRITICAL(&instance->sendMux_);
    DBG_PRINTLN("[ESPNOW][TX] Success; clearing in-flight and trying next.");
    instance->trySendNext_();
    return;
  }

  // Failure: retry up to ESPNOW_TX_MAX_RETRY times
  TxAckEvent retry{};
  bool haveRetry = false;

  taskENTER_CRITICAL(&instance->sendMux_);
  if (instance->inFlight_.attempts < ESPNOW_TX_MAX_RETRY) {
    retry = instance->inFlight_;
    retry.attempts++;
    haveRetry = true;
  }
  instance->hasInFlight_ = false;
  taskEXIT_CRITICAL(&instance->sendMux_);

  if (haveRetry) {
    DBG_PRINTF("[ESPNOW][TX] Failure; requeue attempt %u/%u\n",
                 (unsigned)retry.attempts, (unsigned)ESPNOW_TX_MAX_RETRY);
    (void)xQueueSend(instance->sendQ, &retry, 0);
  } else {
    DBG_PRINTLN("[ESPNOW]ESP-NOW callback failure; drop after max retries.");
  }

  instance->trySendNext_();
}

// =============================================================
//  Public API (TX)
// =============================================================
void EspNowManager::SendAck(const String& Msg, bool Status) {
  if (!isConfigured_()) { DBG_PRINTLN("[ESPNOW][SendAck] Ignored: not configured"); return; }
  if (!txQ)             { DBG_PRINTLN("[ESPNOW][SendAck] txQ=null"); return; }
  if (CONF) {
    String master = CONF->GetString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
    if (master.isEmpty() || master == MASTER_ESPNOW_ID_DEFAULT) {
      DBG_PRINTLN("[ESPNOW][SendAck] Ignored: master MAC missing");
      return;
    }
  }
  TxAckEvent e{};
  e.status   = Status;
  e.attempts = 0;  // first try
  size_t maxC = sizeof(e.msg) - 1;
  Msg.substring(0, maxC).toCharArray(e.msg, sizeof(e.msg));

  DBG_PRINTLN(String("[ESPNOW][ACK][enqueue] '") + e.msg + "' status=" + (Status ? "1" : "0"));
  if (xQueueSend(txQ, &e, 0) != pdPASS) {
    DBG_PRINTLN("[ESPNOW][ACK] txQ full (drop)");
  }
}

void EspNowManager::doSendAck(const TxAckEvent& e) {
  DBG_PRINTLN(String("[ESPNOW][ACK][doSendAck] '") + e.msg +
                "' attempt=" + String(e.attempts) + " status=" + (e.status ? "1" : "0"));
  TxAckEvent copy = e;
  if (xQueueSend(sendQ, &copy, 0) != pdPASS) {
    DBG_PRINTLN("[ESPNOW][ACK] sendQ full (drop)");
  }
}

bool EspNowManager::sendAckNow_(const TxAckEvent& e) {
  if (!LOGG) {
    DBG_PRINTLN("[ESPNOW][sendAckNow_] Log=null; abort");
    return false;
  }

  uint8_t peerMac[6]{};
  if (getMacAddress(peerMac) != ESP_OK) {
    DBG_PRINTLN("[ESPNOW][sendAckNow_] getMacAddress failed");
    return false;
  }

  const size_t maxCopy = sizeof(e.msg) - 1;
  const size_t n = strnlen(e.msg, maxCopy);
  const size_t len = n + 1; // include NUL to match master expectations

  DBG_PRINTLN(String("[ESPNOW][ACK][send] '") + e.msg + "' -> master");
  DBG_PRINTLN(String("[ESPNOW][TX] ") + extractCmdCode_(String(e.msg)));

  esp_err_t r = sendData(peerMac, reinterpret_cast<const uint8_t*>(e.msg), len);
  if (r == ESP_OK) {
    taskENTER_CRITICAL(&sendMux_);
    inFlight_    = e;
    hasInFlight_ = true;
    taskEXIT_CRITICAL(&sendMux_);
    if (LOGG) LOGG->logAckSent(String(e.msg));
    DBG_PRINTLN("[ESPNOW][ACK] In-flight set.");
    return true;
  }

  DBG_PRINTF("[ESPNOW][ACK] sendData failed -> %d\n", (int)r);
  if (e.attempts < ESPNOW_TX_MAX_RETRY) {
    TxAckEvent retry = e;
    retry.attempts++;
    DBG_PRINTF("[ESPNOW][ACK] Immediate fail; requeue attempt %u/%u\n",
                 (unsigned)retry.attempts, (unsigned)ESPNOW_TX_MAX_RETRY);
    (void)xQueueSend(sendQ, &retry, 0);
  } else {
    DBG_PRINTLN("[ESPNOW]ESP-NOW immediate send failed; drop after max retries.");
  }
  return false;
}

void EspNowManager::trySendNext_() {
  // Check in-flight under lock
  bool busy = false;
  taskENTER_CRITICAL(&sendMux_);
  busy = hasInFlight_;
  taskEXIT_CRITICAL(&sendMux_);
  if (busy || !sendQ) {
    if (busy) DBG_PRINTLN("[ESPNOW][TX] In-flight present; not sending next.");
    return;
  }

  TxAckEvent next{};
  if (xQueueReceive(sendQ, &next, 0) == pdPASS) {
    DBG_PRINTLN(String("[ESPNOW][TX] Dequeued for send: '") + next.msg +
                  "' attempt=" + String(next.attempts));
    (void)sendAckNow_(next);
  }
}
