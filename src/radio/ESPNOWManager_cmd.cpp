#include <ESPNOWManager.hpp>
#include <CommandAPI.hpp>
#include <ConfigNvs.hpp>
#include <FingerprintScanner.hpp>
#include <NVSManager.hpp>
#include <PowerManager.hpp>
#include <ResetManager.hpp>
#include <SleepTimer.hpp>
#include <TransportManager.hpp>
#include <Transport.hpp>
#include <Utils.hpp>

using transport::MessageType;
using transport::Module;
using transport::Serializer;

namespace {
constexpr uint8_t kMasterId = 1;
constexpr uint8_t kSelfId   = 2; // slave logical ID
static uint16_t s_injectMsgId = 1;

// Build a transport Request as if it came from master->slave and inject on RX path.
bool injectTransportRx(EspNowManager* mgr,
                       Module module,
                       uint8_t op,
                       const std::vector<uint8_t>& payload,
                       bool ackRequired = false) {
  if (!mgr || !mgr->transport) return false;

  transport::TransportMessage msg{};
  msg.header.version    = 1;
  msg.header.msgId      = s_injectMsgId++;
  if (s_injectMsgId == 0) s_injectMsgId = 1;
  msg.header.srcId      = kMasterId;
  msg.header.destId     = kSelfId;
  msg.header.module     = static_cast<uint8_t>(module);
  msg.header.type       = static_cast<uint8_t>(MessageType::Request);
  msg.header.opCode     = op;
  msg.header.flags      = ackRequired ? 0x01 : 0x00;
  msg.payload           = payload;
  msg.header.payloadLen = static_cast<uint8_t>(msg.payload.size());

  std::vector<uint8_t> buf;
  if (!Serializer::encode(msg, buf)) return false;
  mgr->transport->onRadioReceive(buf.data(), buf.size());
  return true;
}

// Parse payload after "CMD_xx:" to uint16 slot (fingerprint).
uint16_t parseSlot(const String& msg) {
  int idx = msg.indexOf('_');
  if (idx < 0) idx = msg.indexOf(':');
  if (idx < 0) return 0;
  return static_cast<uint16_t>(msg.substring(idx + 1).toInt());
}

// Capability bits mapping: bit0=Open, bit1=Shock, bit2=Reed, bit3=FP.
uint8_t capBitsFromCmd(const String& cmd, uint8_t currentBits) {
  uint8_t bits = currentBits;
  auto setBit = [&bits](uint8_t b, bool on) {
    if (on) bits |= (1u << b);
    else    bits &= ~(1u << b);
  };
  if (cmd == CMD_CAP_OPEN_ON)  setBit(0, true);
  if (cmd == CMD_CAP_OPEN_OFF) setBit(0, false);
  if (cmd == CMD_CAP_SHOCK_ON) setBit(1, true);
  if (cmd == CMD_CAP_SHOCK_OFF)setBit(1, false);
  if (cmd == CMD_CAP_REED_ON)  setBit(2, true);
  if (cmd == CMD_CAP_REED_OFF) setBit(2, false);
  if (cmd == CMD_CAP_FP_ON)    setBit(3, true);
  if (cmd == CMD_CAP_FP_OFF)   setBit(3, false);
  return bits;
}
} // namespace

// =============================================================
//  Commands (bridged to transport)
// =============================================================
void EspNowManager::ProcessComand(String Msg) {
  if (Slp) Slp->reset();

  if (!Msg.startsWith("0x")) {
    DBG_PRINTLN("[ESPNOW][CMD] Non-CommandAPI token ignored");
    return;
  }

  if (!isConfigured_()) {
    if (Msg.equals(CMD_CONFIG_STATUS)) {
      DBG_PRINTLN("[ESPNOW][CMD] Unconfigured -> CMD_CONFIG_STATUS");
      SendAck(ACK_NOT_CONFIGURED, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] Unconfigured -> command ignored");
    return;
  }

  auto dispatchTransport = [this](Module module,
                                  uint8_t op,
                                  const std::vector<uint8_t>& payload,
                                  const char* tag) -> bool {
    if (!transport) {
      DBG_PRINTLN(String("[ESPNOW][CMD] ") + (tag ? tag : "TRANSPORT") + " -> transport missing");
      SendAck(ACK_UNINTENDED, false);
      return false;
    }
    if (!injectTransportRx(this, module, op, payload, false)) {
      DBG_PRINTLN(String("[ESPNOW][CMD] ") + (tag ? tag : "TRANSPORT") + " -> inject failed");
      SendAck(ACK_UNINTENDED, false);
      return false;
    }
    return true;
  };

  // Always honor reboot/reset per CommandAPI.
  if (Msg.equals(CMD_REBOOT)) {
    DBG_PRINTLN("[ESPNOW][CMD] CMD_REBOOT -> Device Reboot");
    SendAck(ACK_REBOOT, true);
    ResetManager::RequestReboot("ESP-NOW CMD_REBOOT");
    return;
  }
  if (Msg.equals(CMD_FACTORY_RESET) || Msg.equals(CMD_REMOVE_SLAVE)) {
    const bool isRemove = Msg.equals(CMD_REMOVE_SLAVE);
    DBG_PRINTLN(isRemove
                    ? "[ESPNOW][CMD] CMD_REMOVE_SLAVE -> Device Reset procedure (factory)"
                    : "[ESPNOW][CMD] CMD_FACTORY_RESET -> Device Reset procedure (factory)");
    if (CONF) {
      CONF->PutString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
      CONF->PutBool(DEVICE_CONFIGURED, false);
      CONF->PutBool(ARMED_STATE, false);
      CONF->PutBool(MOTION_TRIG_ALARM, false);
      CONF->PutBool(HAS_OPEN_SWITCH_KEY,  false);
      CONF->PutBool(HAS_SHOCK_SENSOR_KEY, false);
      CONF->PutBool(HAS_REED_SWITCH_KEY,  false);
      DBG_PRINTLN("[ESPNOW][CMD] Reset -> cleared pairing");
    }
    capBitsShadowValid_ = false;
    capBitsShadow_ = 0;
    SendAck(isRemove ? ACK_REMOVED : ACK_FACTORY_RESET, true);
    ResetManager::RequestFactoryReset(isRemove
                                          ? "ESP-NOW CMD_REMOVE_SLAVE"
                                          : "ESP-NOW CMD_FACTORY_RESET");
    return;
  }

  DBG_PRINTLN(String("[ESPNOW][CMD] Received: '") + Msg + "'");

  // Fast, read-only replies (stay on ESP-NOW edge)
  if (Msg.equals(CMD_STATE_QUERY)) {
    DBG_PRINTLN("[ESPNOW][CMD] CMD_STATE_QUERY -> sendState()");
    sendState(CMD_STATE_QUERY);
    return;
  }
  if (Msg.equals(CMD_HEARTBEAT_REQ)) {
    DBG_PRINTLN("[ESPNOW][CMD] CMD_HEARTBEAT_REQ -> sendHeartbeat(force=true)");
    sendHeartbeat(true);
    return;
  }
  if (Msg.equals(CMD_CONFIG_STATUS)) {
    DBG_PRINTLN("[ESPNOW][CMD] CMD_CONFIG_STATUS");
    const bool configured = (CONF && CONF->GetBool(DEVICE_CONFIGURED, false));
    SendAck(configured ? ACK_CONFIGURED : ACK_NOT_CONFIGURED, configured);
    return;
  }
  if (Msg.equals(CMD_FP_QUERY_DB)) {
    if (IS_SLAVE_ALARM) {
      DBG_PRINTLN("[ESPNOW][CMD] FP_QUERY_DB -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] CMD_FP_QUERY_DB -> transport QueryDb");
    dispatchTransport(Module::Fingerprint, /*op*/0x06, {}, "FP_QUERY_DB");
    return;
  }
  if (Msg.equals(CMD_FP_NEXT_ID)) {
    if (IS_SLAVE_ALARM) {
      DBG_PRINTLN("[ESPNOW][CMD] FP_NEXT_ID -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] CMD_FP_NEXT_ID -> transport NextId");
    dispatchTransport(Module::Fingerprint, /*op*/0x07, {}, "FP_NEXT_ID");
    return;
  }
  if (Msg.equals(CMD_BATTERY_LEVEL)) {
    DBG_PRINTLN("[ESPNOW][CMD] CMD_BATTERY_LEVEL");
    if (!isConfigured_()) { DBG_PRINTLN("[ESPNOW][CMD] Not configured -> ignore BATTERY_LEVEL"); return; }
    uint8_t pct = (uint8_t)(Power ? Power->batteryPercentage : 0);
    String batteryStatus = String(EVT_BATTERY_PREFIX) + ":" + String(pct);
    DBG_PRINTF("[ESPNOW][CMD] Battery=%u%% -> ACK '%s'\n", (unsigned)pct, batteryStatus.c_str());
    SendAck(batteryStatus, true);
    return;
  }
  if (Msg.equals(CMD_CLEAR_ALARM)) {
    DBG_PRINTLN("[ESPNOW][CMD] CMD_CLEAR_ALARM -> clear alarm/buzzer");
    breach = false;
    SendAck(ACK_ALARM_CLEARED, true);
    return;
  }

  // ---------- State-changing: bridge to transport then ACK ----------
  if (Msg.equals(CMD_ARM_SYSTEM)) {
    DBG_PRINTLN("[ESPNOW][CMD] ARM_SYSTEM -> Device Arm (op=0x04)");
    dispatchTransport(Module::Device, /*op*/0x04, {}, "ARM");
    return;
  }
  if (Msg.equals(CMD_DISARM_SYSTEM)) {
    DBG_PRINTLN("[ESPNOW][CMD] DISARM_SYSTEM -> Device Disarm (op=0x05)");
    dispatchTransport(Module::Device, /*op*/0x05, {}, "DISARM");
    return;
  }
  if (Msg.equals(CMD_ENABLE_MOTION)) {
    DBG_PRINTLN("[ESPNOW][CMD] ENABLE_MOTION -> Shock Enable (op=0x01)");
    dispatchTransport(Module::Shock, /*op*/0x01, {}, "MOTION_ENABLE");
    return;
  }
  if (Msg.equals(CMD_DISABLE_MOTION)) {
    DBG_PRINTLN("[ESPNOW][CMD] DISABLE_MOTION -> Shock Disable (op=0x02)");
    dispatchTransport(Module::Shock, /*op*/0x02, {}, "MOTION_DISABLE");
    return;
  }
  if (Msg.equals(CMD_ENTER_TEST_MODE)) {
    DBG_PRINTLN("[ESPNOW][CMD] ENTER_TEST_MODE -> Device SetConfigMode (op=0x01)");
    setConfigMode(true);
    dispatchTransport(Module::Device, /*op*/0x01, {}, "TEST_MODE");
    return;
  }
  if (Msg.startsWith(CMD_CAPS_QUERY)) {
    DBG_PRINTLN("[ESPNOW][CMD] CAPS_QUERY -> ACK_CAPS");
    uint8_t bits = 0;
    if (CONF) {
      bits |= CONF->GetBool(HAS_OPEN_SWITCH_KEY,   HAS_OPEN_SWITCH_DEFAULT)   ? 0x01 : 0;
      bits |= CONF->GetBool(HAS_SHOCK_SENSOR_KEY,  HAS_SHOCK_SENSOR_DEFAULT)  ? 0x02 : 0;
      bits |= CONF->GetBool(HAS_REED_SWITCH_KEY,   HAS_REED_SWITCH_DEFAULT)   ? 0x04 : 0;
      bits |= CONF->GetBool(HAS_FINGERPRINT_KEY,   HAS_FINGERPRINT_DEFAULT)   ? 0x08 : 0;
    }
    if (IS_SLAVE_ALARM) {
      bits = 0x06; // Shock + Reed only
    }
    String caps = String(ACK_CAPS) + ":O" + String((bits & 0x01) ? 1 : 0) +
                  "S" + String((bits & 0x02) ? 1 : 0) +
                  "R" + String((bits & 0x04) ? 1 : 0) +
                  "F" + String((bits & 0x08) ? 1 : 0);
    DBG_PRINTLN(String("[ESPNOW][CMD] CAPS_QUERY -> ") + caps);
    SendAck(caps, true);
    return;
  }
  if (Msg.startsWith(CMD_SET_ROLE)) {
    DBG_PRINTLN("[ESPNOW][CMD] SET_ROLE -> Device SetRole (op=0x16)");
    uint8_t role = 0;
    int sep = Msg.indexOf(':');
    if (sep >= 0) role = (uint8_t)Msg.substring(sep + 1).toInt();
    dispatchTransport(Module::Device, /*op*/0x16, {role}, "SET_ROLE");
    return;
  }
  if (Msg.equals(CMD_CANCEL_TIMERS)) {
    DBG_PRINTLN("[ESPNOW][CMD] CANCEL_TIMERS -> Device CancelTimers (op=0x15)");
    dispatchTransport(Module::Device, /*op*/0x15, {}, "CANCEL_TIMERS");
    return;
  }
  if (Msg.equals(CMD_SYNC_REQ)) {
    DBG_PRINTLN("[ESPNOW][CMD] SYNC_REQ -> flushJournalToMaster + ACK_SYNCED");
    size_t flushed = flushJournalToMaster_();
    DBG_PRINTF("[ESPNOW][CMD] Journal flushed lines=%u\n", (unsigned)flushed);
    SendAck(ACK_SYNCED, true);
    return;
  }

  // Motor commands
  if (Msg.equals(CMD_LOCK_SCREW)) {
    if (IS_SLAVE_ALARM) {
      DBG_PRINTLN("[ESPNOW][CMD] LOCK_SCREW -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] LOCK_SCREW -> Motor Lock (op=0x01)");
    pendingForceAck_ = 0;
    dispatchTransport(Module::Motor, /*op*/0x01, {}, "LOCK_SCREW");
    // ACK will be emitted by Device on completion.
    return;
  }
  if (Msg.equals(CMD_UNLOCK_SCREW)) {
    if (IS_SLAVE_ALARM) {
      DBG_PRINTLN("[ESPNOW][CMD] UNLOCK_SCREW -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] UNLOCK_SCREW -> Motor Unlock (op=0x02)");
    pendingForceAck_ = 0;
    dispatchTransport(Module::Motor, /*op*/0x02, {}, "UNLOCK_SCREW");
    return;
  }
  if (Msg.equals(CMD_FORCE_LOCK)) {
    if (IS_SLAVE_ALARM) {
      DBG_PRINTLN("[ESPNOW][CMD] FORCE_LOCK -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] FORCE_LOCK -> Motor Lock (op=0x01)");
    pendingForceAck_ = 1;
    dispatchTransport(Module::Motor, /*op*/0x01, {}, "FORCE_LOCK");
    return;
  }
  if (Msg.equals(CMD_FORCE_UNLOCK)) {
    if (IS_SLAVE_ALARM) {
      DBG_PRINTLN("[ESPNOW][CMD] FORCE_UNLOCK -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] FORCE_UNLOCK -> Motor Unlock (op=0x02)");
    pendingForceAck_ = 2;
    dispatchTransport(Module::Motor, /*op*/0x02, {}, "FORCE_UNLOCK");
    return;
  }

  // Capability control -> CapsSet
  if (Msg.startsWith(CMD_CAP_OPEN_ON) || Msg.startsWith(CMD_CAP_OPEN_OFF) ||
      Msg.startsWith(CMD_CAP_SHOCK_ON)|| Msg.startsWith(CMD_CAP_SHOCK_OFF)||
      Msg.startsWith(CMD_CAP_REED_ON) || Msg.startsWith(CMD_CAP_REED_OFF) ||
      Msg.startsWith(CMD_CAP_FP_ON)   || Msg.startsWith(CMD_CAP_FP_OFF)) {
    DBG_PRINTLN("[ESPNOW][CMD] CAP* -> Device CapsSet (op=0x07)");
    uint8_t current = getCapBits_();
    uint8_t bits = capBitsFromCmd(Msg, current);
    setCapBitsShadow_(bits);
    dispatchTransport(Module::Device, /*op*/0x07, {bits}, "CAPS_SET");
    return;
  }

  // Lock driver mode (screw vs electromagnet) -> Device NvsWrite(LOCK_EMAG_KEY)
  if (Msg.equals(CMD_LOCK_EMAG_ON)) {
    if (IS_SLAVE_ALARM) {
      DBG_PRINTLN("[ESPNOW][CMD] LOCK_EMAG_ON -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] LOCK_EMAG_ON -> Device NvsWrite keyId=7 (LOCK_EMAG_KEY=true)");
    std::vector<uint8_t> payload = {7u, 1u};
    pendingLockEmag_ = 1;
    dispatchTransport(Module::Device, /*op*/0x0C, payload, "LOCK_EMAG_ON");
    return;
  }
  if (Msg.equals(CMD_LOCK_EMAG_OFF)) {
    if (IS_SLAVE_ALARM) {
      DBG_PRINTLN("[ESPNOW][CMD] LOCK_EMAG_OFF -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    DBG_PRINTLN("[ESPNOW][CMD] LOCK_EMAG_OFF -> Device NvsWrite keyId=7 (LOCK_EMAG_KEY=false)");
    std::vector<uint8_t> payload = {7u, 0u};
    pendingLockEmag_ = 0;
    dispatchTransport(Module::Device, /*op*/0x0C, payload, "LOCK_EMAG_OFF");
    return;
  }

  // Shock enable/disable handled above

  // Fingerprint domain
  if (IS_SLAVE_ALARM) {
    const bool isFpCmd =
        Msg.equals(CMD_FP_VERIFY_ON) ||
        Msg.equals(CMD_FP_VERIFY_OFF) ||
        Msg.startsWith(CMD_ENROLL_FINGERPRINT) ||
        Msg.startsWith(CMD_FP_DELETE_ID) ||
        Msg.equals(CMD_FP_CLEAR_DB) ||
        Msg.equals(CMD_FP_QUERY_DB) ||
        Msg.equals(CMD_FP_NEXT_ID) ||
        Msg.equals(CMD_FP_ADOPT_SENSOR) ||
        Msg.equals(CMD_FP_RELEASE_SENSOR);
    if (isFpCmd) {
      DBG_PRINTLN("[ESPNOW][CMD] FP command ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
  }
  if (Msg.equals(CMD_FP_VERIFY_ON)) {
    DBG_PRINTLN("[ESPNOW][CMD] FP_VERIFY_ON -> VerifyOn (op=0x01)");
    dispatchTransport(Module::Fingerprint, /*op*/0x01, {}, "FP_VERIFY_ON");
    return;
  }
  if (Msg.equals(CMD_FP_VERIFY_OFF)) {
    DBG_PRINTLN("[ESPNOW][CMD] FP_VERIFY_OFF -> VerifyOff (op=0x02)");
    dispatchTransport(Module::Fingerprint, /*op*/0x02, {}, "FP_VERIFY_OFF");
    return;
  }
  if (Msg.startsWith(CMD_ENROLL_FINGERPRINT)) {
    uint16_t slotId = parseSlot(Msg);
    DBG_PRINTF("[ESPNOW][CMD] ENROLL slot=%u -> Enroll (op=0x03)\n", (unsigned)slotId);
    std::vector<uint8_t> payload{static_cast<uint8_t>(slotId & 0xFF),
                                 static_cast<uint8_t>((slotId >> 8) & 0xFF)};
    dispatchTransport(Module::Fingerprint, /*op*/0x03, payload, "FP_ENROLL");
    return;
  }
  if (Msg.startsWith(CMD_FP_DELETE_ID)) {
    uint16_t slotId = parseSlot(Msg);
    DBG_PRINTF("[ESPNOW][CMD] FP_DELETE slot=%u -> DeleteId (op=0x04)\n", (unsigned)slotId);
    std::vector<uint8_t> payload{static_cast<uint8_t>(slotId & 0xFF),
                                 static_cast<uint8_t>((slotId >> 8) & 0xFF)};
    dispatchTransport(Module::Fingerprint, /*op*/0x04, payload, "FP_DELETE");
    return;
  }
  if (Msg.equals(CMD_FP_CLEAR_DB)) {
    DBG_PRINTLN("[ESPNOW][CMD] FP_CLEAR_DB -> ClearDb (op=0x05)");
    dispatchTransport(Module::Fingerprint, /*op*/0x05, {}, "FP_CLEAR_DB");
    return;
  }
  if (Msg.equals(CMD_FP_ADOPT_SENSOR)) {
    DBG_PRINTLN("[ESPNOW][CMD] FP_ADOPT -> AdoptSensor (op=0x08)");
    dispatchTransport(Module::Fingerprint, /*op*/0x08, {}, "FP_ADOPT");
    return;
  }
  if (Msg.equals(CMD_FP_RELEASE_SENSOR)) {
    DBG_PRINTLN("[ESPNOW][CMD] FP_RELEASE -> ReleaseSensor (op=0x09)");
    dispatchTransport(Module::Fingerprint, /*op*/0x09, {}, "FP_RELEASE");
    return;
  }

  // Unknown
  DBG_PRINTLN(String("[ESPNOW][CMD] Unhandled: '") + Msg + "'");
  SendAck(ACK_UNINTENDED, false);
}
