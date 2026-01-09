#include <ESPNOWManager.hpp>
#include <ConfigNvs.hpp>
#include <Logger.hpp>
#include <NVSManager.hpp>
#include <Utils.hpp>
#include <string.h>

// =============================================================
//  Callbacks
// =============================================================
void EspNowManager::onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  if (!instance) return;

  DBG_PRINTF("[ESPNOW][TX][onDataSent] status=%d\n", (int)status);

  if (instance->pendingPairInitAckInFlight_ && instance->pendingPairInit_) {
    if (mac_addr && memcmp(mac_addr, instance->pendingPairInitMac_, 6) == 0) {
      instance->pendingPairInitAckInFlight_ = false;
      instance->pendingPairInitAckDone_ = true;
      instance->pendingPairInitAckOk_ = (status == ESP_NOW_SEND_SUCCESS);
      instance->pendingPairInitAckDoneMs_ = millis();
      DBG_PRINTF("[ESPNOW][pair] ACK_PAIR_INIT delivered=%u\n",
                 (unsigned)(instance->pendingPairInitAckOk_ ? 1 : 0));
    }
  }

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
void EspNowManager::SendAck(uint16_t opcode, bool Status) {
  SendAck(opcode, nullptr, 0, Status);
}

void EspNowManager::SendAck(uint16_t opcode, const uint8_t* payload, size_t payloadLen, bool Status) {
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
  size_t frameLen = 0;
  if (!buildResponse_(opcode, payload, payloadLen, e.data, &frameLen)) {
    DBG_PRINTLN("[ESPNOW][SendAck] buildResponse failed");
    return;
  }
  e.len = static_cast<uint16_t>(frameLen);

  DBG_PRINTF("[ESPNOW][ACK][enqueue] op=0x%04X len=%u status=%u\n",
             (unsigned)opcode, (unsigned)e.len, (unsigned)(Status ? 1 : 0));
  if (xQueueSend(txQ, &e, 0) != pdPASS) {
    DBG_PRINTLN("[ESPNOW][ACK] txQ full (drop)");
  }
}

void EspNowManager::doSendAck(const TxAckEvent& e) {
  uint16_t opcode = 0;
  if (e.len >= 3) {
    opcode = static_cast<uint16_t>(e.data[1]) |
             (static_cast<uint16_t>(e.data[2]) << 8);
  }
  DBG_PRINTF("[ESPNOW][ACK][doSendAck] op=0x%04X len=%u attempt=%u status=%u\n",
             (unsigned)opcode, (unsigned)e.len, (unsigned)e.attempts,
             (unsigned)(e.status ? 1 : 0));
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

  if (e.len == 0 || e.len > ESPNOW_MAX_DATA_LEN) {
    DBG_PRINTLN("[ESPNOW][sendAckNow_] invalid length");
    return false;
  }
  uint16_t opcode = 0;
  if (e.len >= 3) {
    opcode = static_cast<uint16_t>(e.data[1]) |
             (static_cast<uint16_t>(e.data[2]) << 8);
  }

  DBG_PRINTF("[ESPNOW][ACK][send] op=0x%04X len=%u -> master\n",
             (unsigned)opcode, (unsigned)e.len);

  esp_err_t r = sendData(peerMac, e.data, e.len);
  if (r == ESP_OK) {
    taskENTER_CRITICAL(&sendMux_);
    inFlight_    = e;
    hasInFlight_ = true;
    taskEXIT_CRITICAL(&sendMux_);
    if (LOGG) {
      String logLine = String("op=0x") + String(opcode, HEX) +
                       " len=" + String(e.len);
      LOGG->logAckSent(logLine);
    }
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
    uint16_t opcode = 0;
    if (next.len >= 3) {
      opcode = static_cast<uint16_t>(next.data[1]) |
               (static_cast<uint16_t>(next.data[2]) << 8);
    }
    DBG_PRINTF("[ESPNOW][TX] Dequeued for send: op=0x%04X attempt=%u\n",
               (unsigned)opcode, (unsigned)next.attempts);
    (void)sendAckNow_(next);
  }
}

// =============================================================
//  Response builder
// =============================================================
bool EspNowManager::buildResponse_(uint16_t opcode,
                                   const uint8_t* payload,
                                   size_t payloadLen,
                                   uint8_t* out,
                                   size_t* outLen) {
  if (!out || !outLen) {
    return false;
  }
  if (payloadLen > 0xFF) {
    return false;
  }
  if (payloadLen && !payload) {
    return false;
  }
  const size_t total = 4 + payloadLen;
  if (total > ESPNOW_MAX_DATA_LEN) {
    return false;
  }
  out[0] = NOW_FRAME_RESP;
  out[1] = static_cast<uint8_t>(opcode & 0xFF);
  out[2] = static_cast<uint8_t>((opcode >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>(payloadLen);
  if (payloadLen && payload) {
    memcpy(out + 4, payload, payloadLen);
  }
  *outLen = total;
  return true;
}
