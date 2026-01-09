#include <ESPNOWManager.hpp>
#include <CommandAPI.hpp>
#include <SleepTimer.hpp>
#include <TransportManager.hpp>
#include <Utils.hpp>
#include <string.h>

namespace {
bool parseCommandFrame_(const uint8_t* data, size_t len, uint16_t& opcodeOut,
                        const uint8_t*& payloadOut, uint8_t& payloadLenOut) {
  if (!data || len < 4) {
    return false;
  }
  if (data[0] != NOW_FRAME_CMD) {
    return false;
  }
  const uint16_t opcode = static_cast<uint16_t>(data[1]) |
                          (static_cast<uint16_t>(data[2]) << 8);
  const uint8_t payloadLen = data[3];
  if (len < static_cast<size_t>(4 + payloadLen)) {
    return false;
  }
  opcodeOut = opcode;
  payloadLenOut = payloadLen;
  payloadOut = data + 4;
  return true;
}
} // namespace

// =============================================================
//  Callbacks
// =============================================================
void EspNowManager::onDataReceived(const uint8_t* mac_addr, const uint8_t* data, int len) {
  if (!instance || !mac_addr || !data || len <= 0) return;

  DBG_PRINTF("[ESPNOW][RX][onDataReceived] from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
               mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5], len);

  RxEvent e{};
  memcpy(e.mac, mac_addr, 6);
  e.len = (len > ESPNOW_MAX_DATA_LEN) ? ESPNOW_MAX_DATA_LEN : len;
  memcpy(e.buf, data, e.len);
  if (xQueueSend(instance->rxQ, &e, 0) == pdPASS) {
    DBG_PRINTF("[ESPNOW][RX] Queued RxEvent: len=%d\n", e.len);
  } else {
    DBG_PRINTLN("[ESPNOW][RX] RxEvent queue full (dropped)");
  }
  // After queueing the RxEvent:
  if (instance->isConfigured_()) {
    instance->lastHbMs_ = millis();
    if (!instance->isOnline()) {
      instance->setOffline(false);
    }
  }
  if (instance->Slp) instance->Slp->reset();
  // Note: raw frame is forwarded to TransportManager from the ESPNOW worker task
  // via processRx(), so callback stays lightweight.
}

// =============================================================
//  Worker Helpers
// =============================================================
void EspNowManager::processRx(const RxEvent& e) {
  DBG_PRINTF("[ESPNOW][processRx] len=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
               e.len, e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
  debugDumpPacket_("RX", e.buf, static_cast<size_t>(e.len));

  // Keep the device awake on traffic
  if (Slp) Slp->reset();

  if (!isConfigured_()) {
    DBG_PRINTLN("[ESPNOW][processRx] Unconfigured -> pairing mode");
    if (handlePairInit_(e.mac, e.buf, static_cast<size_t>(e.len))) {
      return;
    }
    uint16_t opcode = 0;
    const uint8_t* payload = nullptr;
    uint8_t payloadLen = 0;
    if (parseCommandFrame_(e.buf, static_cast<size_t>(e.len), opcode, payload, payloadLen) &&
        opcode == CMD_CONFIG_STATUS) {
      SendAck(ACK_NOT_CONFIGURED, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][processRx] Unconfigured -> non-pairing cmd ignored");
    return;
  }

  if (!compareMacAddress(e.mac)) {
    DBG_PRINTLN("[ESPNOW][processRx] Sender MAC mismatch -> ignore");
    return;
  }

  if (e.len < 1) {
    return;
  }

  const uint8_t frameType = e.buf[0];
  if (frameType != NOW_FRAME_CMD) {
    DBG_PRINTF("[ESPNOW][processRx] Non-command frame type=0x%02X ignored\n",
               (unsigned)frameType);
    return;
  }

  uint16_t opcode = 0;
  const uint8_t* payload = nullptr;
  uint8_t payloadLen = 0;
  if (!parseCommandFrame_(e.buf, static_cast<size_t>(e.len), opcode, payload, payloadLen)) {
    DBG_PRINTLN("[ESPNOW][processRx] Invalid command frame");
    return;
  }

  DBG_PRINTF("[ESPNOW][processRx] CMD opcode=0x%04X payloadLen=%u\n",
             (unsigned)opcode, (unsigned)payloadLen);

  // Presence seen on any valid packet
  lastHbMs_ = millis();

  // Hand off to command handler (CommandAPI -> transport bridge)
  ProcessComand(opcode, payload, payloadLen);
}
