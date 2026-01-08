#include <ESPNOWManager.hpp>
#include <CommandAPI.hpp>
#include <SleepTimer.hpp>
#include <TransportManager.hpp>
#include <Utils.hpp>
#include <string.h>

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

  // Feed raw frame into transport layer (binary protocol) from worker context.
  if (transport && isConfigured_()) {
    transport->onRadioReceive(e.buf, static_cast<size_t>(e.len));
  }

  // Keep the device awake on traffic
  if (Slp) Slp->reset();

  String msg = makeSafeString_(reinterpret_cast<const char*>(e.buf),
                               static_cast<size_t>(e.len));
  msg.trim();

  if (!isConfigured_()) {
    DBG_PRINTLN("[ESPNOW][processRx] Unconfigured -> pairing mode");
    if (handlePairInit_(e.mac, msg)) {
      return;
    }
    if (msg.equals(CMD_CONFIG_STATUS)) {
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

  if (msg.isEmpty()) {
    return;
  }

  DBG_PRINTLN(String("[ESPNOW][processRx] CMD='") + msg + "'");
  DBG_PRINTLN(String("[ESPNOW][RX] ") + extractCmdCode_(msg));

  // Presence seen on any valid packet
  lastHbMs_ = millis();

  // Hand off to command handler (CommandAPI -> transport bridge)
  ProcessComand(msg);
}

// =============================================================
//  Safe String Helper
// =============================================================
String EspNowManager::makeSafeString_(const char* src, size_t n) {
  // copy n bytes, force-terminate, then build String
  char buf[128];
  size_t m = (n < sizeof(buf) - 1) ? n : sizeof(buf) - 1;
  memcpy(buf, src, m);
  buf[m] = '\0';
  // Debug small hint when truncation might occur
  if (n >= sizeof(buf)) {
    DBG_PRINTF("[ESPNOW][makeSafeString_] trunc %u->%u\n", (unsigned)n, (unsigned)m);
  }
  return String(buf);
}
