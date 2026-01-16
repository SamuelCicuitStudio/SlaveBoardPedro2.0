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
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

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

bool parseU16Le_(const uint8_t* payload, size_t len, uint16_t& out) {
  if (!payload || len < 2) return false;
  out = static_cast<uint16_t>(payload[0]) |
        (static_cast<uint16_t>(payload[1]) << 8);
  return true;
}

// Capability bits mapping: bit0=Open, bit1=Shock, bit2=Reed, bit3=FP.
uint8_t capBitsFromCmd(uint16_t opcode, uint8_t currentBits) {
  uint8_t bits = currentBits;
  auto setBit = [&bits](uint8_t b, bool on) {
    if (on) bits |= (1u << b);
    else    bits &= ~(1u << b);
  };
  if (opcode == CMD_CAP_OPEN_ON)  setBit(0, true);
  if (opcode == CMD_CAP_OPEN_OFF) setBit(0, false);
  if (opcode == CMD_CAP_SHOCK_ON) setBit(1, true);
  if (opcode == CMD_CAP_SHOCK_OFF)setBit(1, false);
  if (opcode == CMD_CAP_REED_ON)  setBit(2, true);
  if (opcode == CMD_CAP_REED_OFF) setBit(2, false);
  if (opcode == CMD_CAP_FP_ON)    setBit(3, true);
  if (opcode == CMD_CAP_FP_OFF)   setBit(3, false);
  return bits;
}
} // namespace

// =============================================================
//  Commands (bridged to transport)
// =============================================================
void EspNowManager::ProcessComand(uint16_t opcode, const uint8_t* payload, size_t payloadLen) {
  if (Slp) Slp->reset();

  if (!isConfigured_()) {
    if (opcode == CMD_CONFIG_STATUS) {
     // DBG_PRINTLN("[ESPNOW][CMD] Unconfigured -> CMD_CONFIG_STATUS");
      SendAck(ACK_NOT_CONFIGURED, false);
      return;
    }
    //DBG_PRINTLN("[ESPNOW][CMD] Unconfigured -> command ignored");
    return;
  }

  auto dispatchTransport = [this](Module module,
                                  uint8_t op,
                                  const std::vector<uint8_t>& payloadVec,
                                  const char* tag) -> bool {
    if (!transport) {
      //DBG_PRINTLN(String("[ESPNOW][CMD] ") + (tag ? tag : "TRANSPORT") + " -> transport missing");
      SendAck(ACK_UNINTENDED, false);
      return false;
    }
    if (!injectTransportRx(this, module, op, payloadVec, false)) {
      //DBG_PRINTLN(String("[ESPNOW][CMD] ") + (tag ? tag : "TRANSPORT") + " -> inject failed");
      SendAck(ACK_UNINTENDED, false);
      return false;
    }
    return true;
  };

  // Always honor reboot/reset per CommandAPI.
  if (opcode == CMD_REBOOT) {
    //DBG_PRINTLN("[ESPNOW][CMD] CMD_REBOOT -> Device Reboot");
    SendAck(ACK_REBOOT, true);
    ResetManager::RequestReboot("ESP-NOW CMD_REBOOT");
    return;
  }
  if (opcode == CMD_SET_CHANNEL) {
    if (!payload || payloadLen < 1) {
     // DBG_PRINTLN("[ESPNOW][CMD] CMD_SET_CHANNEL -> missing payload");
      SendAck(ACK_UNINTENDED, false);
      return;
    }
    const uint8_t channel = payload[0];
    if (channel < 1 || channel > 13) {
      //DBG_PRINTF("[ESPNOW][CMD] CMD_SET_CHANNEL -> invalid=%u\n",static_cast<unsigned>(channel));
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    if (!CONF) {
     // DBG_PRINTLN("[ESPNOW][CMD] CMD_SET_CHANNEL -> conf missing");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
   // DBG_PRINTF("[ESPNOW][CMD] CMD_SET_CHANNEL -> channel=%u (reboot)\n",static_cast<unsigned>(channel));

    TxAckEvent ack{};
    size_t frameLen = 0;
    if (buildResponse_(ACK_SET_CHANNEL, nullptr, 0, ack.data, &frameLen)) {
      ack.len = static_cast<uint16_t>(frameLen);
      ack.status = true;
      ack.attempts = 0;
      if (!sendAckNow_(ack)) {
        SendAck(ACK_SET_CHANNEL, true);
      }
    } else {
      SendAck(ACK_SET_CHANNEL, true);
    }

    const uint32_t start = millis();
    while ((millis() - start) < 800) {
      bool inflight = false;
      bool pending = false;
      taskENTER_CRITICAL(&sendMux_);
      inflight = hasInFlight_;
      taskEXIT_CRITICAL(&sendMux_);
      if (sendQ && uxQueueMessagesWaiting(sendQ) > 0) pending = true;
      if (txQ && uxQueueMessagesWaiting(txQ) > 0) pending = true;
      if (!inflight && !pending) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      esp_task_wdt_reset();
    }
    CONF->PutIntImmediate(MASTER_CHANNEL_KEY, static_cast<int>(channel));
    ResetManager::RequestReboot("ESP-NOW CMD_SET_CHANNEL");
    return;
  }
  if (opcode == CMD_REMOVE_SLAVE) {
    //DBG_PRINTLN("[ESPNOW][CMD] CMD_REMOVE_SLAVE -> Device Reset procedure (factory)");

    TxAckEvent ack{};
    size_t frameLen = 0;
    if (buildResponse_(ACK_REMOVED, nullptr, 0, ack.data, &frameLen)) {
      ack.len = static_cast<uint16_t>(frameLen);
      ack.status = true;
      ack.attempts = 0;
      if (!sendAckNow_(ack)) {
        SendAck(ACK_REMOVED, true);
      }
    } else {
      SendAck(ACK_REMOVED, true);
    }

    const uint32_t start = millis();
    while ((millis() - start) < 800) {
      bool inflight = false;
      bool pending = false;
      taskENTER_CRITICAL(&sendMux_);
      inflight = hasInFlight_;
      taskEXIT_CRITICAL(&sendMux_);
      if (sendQ && uxQueueMessagesWaiting(sendQ) > 0) pending = true;
      if (txQ && uxQueueMessagesWaiting(txQ) > 0) pending = true;
      if (!inflight && !pending) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      esp_task_wdt_reset();
    }

    if (CONF) {
      CONF->PutString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
      CONF->PutString(MASTER_LMK_KEY, MASTER_LMK_DEFAULT);
      CONF->PutBool(DEVICE_CONFIGURED, false);
      CONF->PutBool(ARMED_STATE, false);
      CONF->PutBool(MOTION_TRIG_ALARM, false);
      CONF->PutBool(HAS_OPEN_SWITCH_KEY,  false);
      CONF->PutBool(HAS_SHOCK_SENSOR_KEY, false);
      CONF->PutBool(HAS_REED_SWITCH_KEY,  false);
     // DBG_PRINTLN("[ESPNOW][CMD] Reset -> cleared pairing");
    }
    capBitsShadowValid_ = false;
    capBitsShadow_ = 0;
    ResetManager::RequestFactoryReset("ESP-NOW CMD_REMOVE_SLAVE");
    return;
  }
  if (opcode == CMD_FACTORY_RESET) {
    //DBG_PRINTLN("[ESPNOW][CMD] CMD_FACTORY_RESET -> Device Reset procedure (factory)");
    if (CONF) {
      CONF->PutString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
      CONF->PutString(MASTER_LMK_KEY, MASTER_LMK_DEFAULT);
      CONF->PutBool(DEVICE_CONFIGURED, false);
      CONF->PutBool(ARMED_STATE, false);
      CONF->PutBool(MOTION_TRIG_ALARM, false);
      CONF->PutBool(HAS_OPEN_SWITCH_KEY,  false);
      CONF->PutBool(HAS_SHOCK_SENSOR_KEY, false);
      CONF->PutBool(HAS_REED_SWITCH_KEY,  false);
     // DBG_PRINTLN("[ESPNOW][CMD] Reset -> cleared pairing");
    }
    capBitsShadowValid_ = false;
    capBitsShadow_ = 0;
    SendAck(ACK_FACTORY_RESET, true);
    ResetManager::RequestFactoryReset("ESP-NOW CMD_FACTORY_RESET");
    return;
  }

 // DBG_PRINTF("[ESPNOW][CMD] opcode=0x%04X\n", (unsigned)opcode);

  // Fast, read-only replies (stay on ESP-NOW edge)
  if (opcode == CMD_STATE_QUERY) {
   // DBG_PRINTLN("[ESPNOW][CMD] CMD_STATE_QUERY -> sendState()");
    sendState("CMD_STATE_QUERY");
    return;
  }
  if (opcode == CMD_HEARTBEAT_REQ) {
  //  DBG_PRINTLN("[ESPNOW][CMD] CMD_HEARTBEAT_REQ -> sendHeartbeat(force=true)");
    sendHeartbeat(true);
    return;
  }
  if (opcode == CMD_CONFIG_STATUS) {
  //  DBG_PRINTLN("[ESPNOW][CMD] CMD_CONFIG_STATUS");
    const bool configured = (CONF && CONF->GetBool(DEVICE_CONFIGURED, false));
    SendAck(configured ? ACK_CONFIGURED : ACK_NOT_CONFIGURED, configured);
    return;
  }
  if (opcode == CMD_FP_QUERY_DB) {
    if (IS_SLAVE_ALARM) {
    //  DBG_PRINTLN("[ESPNOW][CMD] FP_QUERY_DB -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    //DBG_PRINTLN("[ESPNOW][CMD] CMD_FP_QUERY_DB -> transport QueryDb");
    dispatchTransport(Module::Fingerprint, /*op*/0x06, {}, "FP_QUERY_DB");
    return;
  }
  if (opcode == CMD_FP_NEXT_ID) {
    if (IS_SLAVE_ALARM) {
    //  DBG_PRINTLN("[ESPNOW][CMD] FP_NEXT_ID -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
   // DBG_PRINTLN("[ESPNOW][CMD] CMD_FP_NEXT_ID -> transport NextId");
    dispatchTransport(Module::Fingerprint, /*op*/0x07, {}, "FP_NEXT_ID");
    return;
  }
  if (opcode == CMD_BATTERY_LEVEL) {
    //DBG_PRINTLN("[ESPNOW][CMD] CMD_BATTERY_LEVEL");
    uint8_t pct = static_cast<uint8_t>(Power ? Power->batteryPercentage : 0);
    SendAck(EVT_BATTERY_PREFIX, &pct, 1, true);
    return;
  }
  if (opcode == CMD_CLEAR_ALARM) {
   // DBG_PRINTLN("[ESPNOW][CMD] CMD_CLEAR_ALARM -> clear alarm/buzzer");
    breach = false;
    SendAck(ACK_ALARM_CLEARED, true);
    return;
  }

  // ---------- State-changing: bridge to transport then ACK ----------
  if (opcode == CMD_ARM_SYSTEM) {
    //DBG_PRINTLN("[ESPNOW][CMD] ARM_SYSTEM -> Device Arm (op=0x04)");
    dispatchTransport(Module::Device, /*op*/0x04, {}, "ARM");
    return;
  }
  if (opcode == CMD_DISARM_SYSTEM) {
   // DBG_PRINTLN("[ESPNOW][CMD] DISARM_SYSTEM -> Device Disarm (op=0x05)");
    dispatchTransport(Module::Device, /*op*/0x05, {}, "DISARM");
    return;
  }
  if (opcode == CMD_ENABLE_MOTION) {
   // DBG_PRINTLN("[ESPNOW][CMD] ENABLE_MOTION -> Shock Enable (op=0x01)");
    dispatchTransport(Module::Shock, /*op*/0x01, {}, "MOTION_ENABLE");
    return;
  }
  if (opcode == CMD_DISABLE_MOTION) {
   // DBG_PRINTLN("[ESPNOW][CMD] DISABLE_MOTION -> Shock Disable (op=0x02)");
    dispatchTransport(Module::Shock, /*op*/0x02, {}, "MOTION_DISABLE");
    return;
  }
  if (opcode == CMD_SET_SHOCK_SENSOR_TYPE) {
    if (!payload || payloadLen < 1) {
    //  DBG_PRINTLN("[ESPNOW][CMD] SET_SHOCK_SENSOR_TYPE -> missing payload");
      SendAck(ACK_UNINTENDED, false);
      return;
    }
    uint8_t type = payload[0];
   // DBG_PRINTF("[ESPNOW][CMD] SET_SHOCK_SENSOR_TYPE -> type=%u\n",static_cast<unsigned>(type));
    dispatchTransport(Module::Shock, /*op*/0x10, {type}, "SHOCK_TYPE");
    return;
  }
  if (opcode == CMD_SET_SHOCK_SENS_THRESHOLD) {
    if (!payload || payloadLen < 1) {
      //DBG_PRINTLN("[ESPNOW][CMD] SET_SHOCK_SENS_THRESHOLD -> missing payload");
      SendAck(ACK_UNINTENDED, false);
      return;
    }
    uint8_t ths = payload[0];
   // DBG_PRINTF("[ESPNOW][CMD] SET_SHOCK_SENS_THRESHOLD -> ths=%u\n",static_cast<unsigned>(ths));
    dispatchTransport(Module::Shock, /*op*/0x11, {ths}, "SHOCK_THS");
    return;
  }
  if (opcode == CMD_SET_SHOCK_L2D_CFG) {
    if (!payload || payloadLen < 11) {
    //  DBG_PRINTLN("[ESPNOW][CMD] SET_SHOCK_L2D_CFG -> missing payload");
      SendAck(ACK_UNINTENDED, false);
      return;
    }
    std::vector<uint8_t> cfg(payload, payload + 11);
   // DBG_PRINTLN("[ESPNOW][CMD] SET_SHOCK_L2D_CFG -> apply");
    dispatchTransport(Module::Shock, /*op*/0x12, cfg, "SHOCK_L2D_CFG");
    return;
  }
  if (opcode == CMD_ENTER_TEST_MODE) {
    //DBG_PRINTLN("[ESPNOW][CMD] ENTER_TEST_MODE -> Device SetConfigMode (op=0x01)");
    setConfigMode(true);
    dispatchTransport(Module::Device, /*op*/0x01, {}, "TEST_MODE");
    return;
  }
  if (opcode == CMD_CAPS_QUERY) {
   // DBG_PRINTLN("[ESPNOW][CMD] CAPS_QUERY -> ACK_CAPS");
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
    SendAck(ACK_CAPS, &bits, 1, true);
    return;
  }
  if (opcode == CMD_SET_ROLE) {
   // DBG_PRINTLN("[ESPNOW][CMD] SET_ROLE -> Device SetRole (op=0x16)");
    uint8_t role = (payloadLen >= 1) ? payload[0] : 0;
    dispatchTransport(Module::Device, /*op*/0x16, {role}, "SET_ROLE");
    return;
  }
  if (opcode == CMD_CANCEL_TIMERS) {
   // DBG_PRINTLN("[ESPNOW][CMD] CANCEL_TIMERS -> Device CancelTimers (op=0x15)");
    dispatchTransport(Module::Device, /*op*/0x15, {}, "CANCEL_TIMERS");
    return;
  }
  if (opcode == CMD_SYNC_REQ) {
   // DBG_PRINTLN("[ESPNOW][CMD] SYNC_REQ -> flushJournalToMaster + ACK_SYNCED");
    size_t flushed = flushJournalToMaster_();
    //DBG_PRINTF("[ESPNOW][CMD] Journal flushed lines=%u\n", (unsigned)flushed);
    SendAck(ACK_SYNCED, true);
    return;
  }

  // Motor commands
  if (opcode == CMD_LOCK_SCREW) {
    if (IS_SLAVE_ALARM) {
    //  DBG_PRINTLN("[ESPNOW][CMD] LOCK_SCREW -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
   // DBG_PRINTLN("[ESPNOW][CMD] LOCK_SCREW -> Motor Lock (op=0x01)");
    pendingForceAck_ = 0;
    dispatchTransport(Module::Motor, /*op*/0x01, {}, "LOCK_SCREW");
    // ACK will be emitted by Device on completion.
    return;
  }
  if (opcode == CMD_UNLOCK_SCREW) {
    if (IS_SLAVE_ALARM) {
     // DBG_PRINTLN("[ESPNOW][CMD] UNLOCK_SCREW -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
   // DBG_PRINTLN("[ESPNOW][CMD] UNLOCK_SCREW -> Motor Unlock (op=0x02)");
    pendingForceAck_ = 0;
    dispatchTransport(Module::Motor, /*op*/0x02, {}, "UNLOCK_SCREW");
    return;
  }
  if (opcode == CMD_FORCE_LOCK) {
    if (IS_SLAVE_ALARM) {
     // DBG_PRINTLN("[ESPNOW][CMD] FORCE_LOCK -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
    //DBG_PRINTLN("[ESPNOW][CMD] FORCE_LOCK -> Motor Lock (op=0x01)");
    pendingForceAck_ = 1;
    dispatchTransport(Module::Motor, /*op*/0x01, {}, "FORCE_LOCK");
    return;
  }
  if (opcode == CMD_FORCE_UNLOCK) {
    if (IS_SLAVE_ALARM) {
    //  DBG_PRINTLN("[ESPNOW][CMD] FORCE_UNLOCK -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
   // DBG_PRINTLN("[ESPNOW][CMD] FORCE_UNLOCK -> Motor Unlock (op=0x02)");
    pendingForceAck_ = 2;
    dispatchTransport(Module::Motor, /*op*/0x02, {}, "FORCE_UNLOCK");
    return;
  }

  // Capability control -> CapsSet
  if (opcode == CMD_CAP_OPEN_ON || opcode == CMD_CAP_OPEN_OFF ||
      opcode == CMD_CAP_SHOCK_ON|| opcode == CMD_CAP_SHOCK_OFF||
      opcode == CMD_CAP_REED_ON || opcode == CMD_CAP_REED_OFF ||
      opcode == CMD_CAP_FP_ON   || opcode == CMD_CAP_FP_OFF) {
    //DBG_PRINTLN("[ESPNOW][CMD] CAP* -> Device CapsSet (op=0x07)");
    uint8_t current = getCapBits_();
    uint8_t bits = capBitsFromCmd(opcode, current);
    setCapBitsShadow_(bits);
    dispatchTransport(Module::Device, /*op*/0x07, {bits}, "CAPS_SET");
    return;
  }

  // Lock driver mode (screw vs electromagnet) -> Device NvsWrite(LOCK_EMAG_KEY)
  if (opcode == CMD_LOCK_EMAG_ON) {
    if (IS_SLAVE_ALARM) {
     // DBG_PRINTLN("[ESPNOW][CMD] LOCK_EMAG_ON -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
   // DBG_PRINTLN("[ESPNOW][CMD] LOCK_EMAG_ON -> Device NvsWrite keyId=7 (LOCK_EMAG_KEY=true)");
    std::vector<uint8_t> payloadVec = {7u, 1u};
    pendingLockEmag_ = 1;
    dispatchTransport(Module::Device, /*op*/0x0C, payloadVec, "LOCK_EMAG_ON");
    return;
  }
  if (opcode == CMD_LOCK_EMAG_OFF) {
    if (IS_SLAVE_ALARM) {
    //  DBG_PRINTLN("[ESPNOW][CMD] LOCK_EMAG_OFF -> ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
   // DBG_PRINTLN("[ESPNOW][CMD] LOCK_EMAG_OFF -> Device NvsWrite keyId=7 (LOCK_EMAG_KEY=false)");
    std::vector<uint8_t> payloadVec = {7u, 0u};
    pendingLockEmag_ = 0;
    dispatchTransport(Module::Device, /*op*/0x0C, payloadVec, "LOCK_EMAG_OFF");
    return;
  }

  // Fingerprint domain
  if (IS_SLAVE_ALARM) {
    const bool isFpCmd =
        opcode == CMD_FP_VERIFY_ON ||
        opcode == CMD_FP_VERIFY_OFF ||
        opcode == CMD_ENROLL_FINGERPRINT ||
        opcode == CMD_FP_DELETE_ID ||
        opcode == CMD_FP_CLEAR_DB ||
        opcode == CMD_FP_QUERY_DB ||
        opcode == CMD_FP_NEXT_ID ||
        opcode == CMD_FP_ADOPT_SENSOR ||
        opcode == CMD_FP_RELEASE_SENSOR;
    if (isFpCmd) {
   //   DBG_PRINTLN("[ESPNOW][CMD] FP command ignored (alarm role)");
      SendAck(ACK_ERR_POLICY, false);
      return;
    }
  }
  if (opcode == CMD_FP_VERIFY_ON) {
  //  DBG_PRINTLN("[ESPNOW][CMD] FP_VERIFY_ON -> VerifyOn (op=0x01)");
    dispatchTransport(Module::Fingerprint, /*op*/0x01, {}, "FP_VERIFY_ON");
    return;
  }
  if (opcode == CMD_FP_VERIFY_OFF) {
  //  DBG_PRINTLN("[ESPNOW][CMD] FP_VERIFY_OFF -> VerifyOff (op=0x02)");
    dispatchTransport(Module::Fingerprint, /*op*/0x02, {}, "FP_VERIFY_OFF");
    return;
  }
  if (opcode == CMD_ENROLL_FINGERPRINT) {
    uint16_t slotId = 0;
    if (!parseU16Le_(payload, payloadLen, slotId)) {
      SendAck(ACK_UNINTENDED, false);
      return;
    }
  //  DBG_PRINTF("[ESPNOW][CMD] ENROLL slot=%u -> Enroll (op=0x03)\n", (unsigned)slotId);
    std::vector<uint8_t> payloadVec{static_cast<uint8_t>(slotId & 0xFF),
                                    static_cast<uint8_t>((slotId >> 8) & 0xFF)};
    dispatchTransport(Module::Fingerprint, /*op*/0x03, payloadVec, "FP_ENROLL");
    return;
  }
  if (opcode == CMD_FP_DELETE_ID) {
    uint16_t slotId = 0;
    if (!parseU16Le_(payload, payloadLen, slotId)) {
      SendAck(ACK_UNINTENDED, false);
      return;
    }
  //  DBG_PRINTF("[ESPNOW][CMD] FP_DELETE slot=%u -> DeleteId (op=0x04)\n", (unsigned)slotId);
    std::vector<uint8_t> payloadVec{static_cast<uint8_t>(slotId & 0xFF),
                                    static_cast<uint8_t>((slotId >> 8) & 0xFF)};
    dispatchTransport(Module::Fingerprint, /*op*/0x04, payloadVec, "FP_DELETE");
    return;
  }
  if (opcode == CMD_FP_CLEAR_DB) {
  //  DBG_PRINTLN("[ESPNOW][CMD] FP_CLEAR_DB -> ClearDb (op=0x05)");
    dispatchTransport(Module::Fingerprint, /*op*/0x05, {}, "FP_CLEAR_DB");
    return;
  }
  if (opcode == CMD_FP_ADOPT_SENSOR) {
  //  DBG_PRINTLN("[ESPNOW][CMD] FP_ADOPT -> AdoptSensor (op=0x08)");
    dispatchTransport(Module::Fingerprint, /*op*/0x08, {}, "FP_ADOPT");
    return;
  }
  if (opcode == CMD_FP_RELEASE_SENSOR) {
  //  DBG_PRINTLN("[ESPNOW][CMD] FP_RELEASE -> ReleaseSensor (op=0x09)");
    dispatchTransport(Module::Fingerprint, /*op*/0x09, {}, "FP_RELEASE");
    return;
  }

  // Unknown
//  DBG_PRINTF("[ESPNOW][CMD] Unhandled opcode=0x%04X\n", (unsigned)opcode);
  SendAck(ACK_UNINTENDED, false);
}
