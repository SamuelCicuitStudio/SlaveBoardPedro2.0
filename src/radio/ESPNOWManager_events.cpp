#include <ESPNOWManager.hpp>
#include <CommandAPI.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <Transport.hpp>
#include <Utils.hpp>

// =============================================================
//  Transport -> CommandAPI bridge (Responses/Events to ACK_*)
// =============================================================
namespace {
bool isStatusOk_(const transport::TransportMessage& msg, size_t minPayload = 1) {
  if (msg.payload.size() < minPayload) return false;
  return msg.payload[0] == static_cast<uint8_t>(transport::StatusCode::OK);
}
} // namespace

bool EspNowManager::handleTransportTx(const transport::TransportMessage& msg) {
  // Only translate messages destined to master (destId=1).
  if (msg.header.destId != 1) return false;

  auto sendResp = [this](uint16_t opcode, const uint8_t* payload, size_t len, bool ok = true) {
    SendAck(opcode, payload, len, ok);
  };
  auto sendRespNoPayload = [this](uint16_t opcode, bool ok = true) {
    SendAck(opcode, ok);
  };

  const uint8_t mod = msg.header.module;
  const uint8_t op  = msg.header.opCode;
  const auto& pl    = msg.payload;
  const auto statusOk = isStatusOk_(msg);

  // ---------- Device module ----------
  if (mod == static_cast<uint8_t>(transport::Module::Device)) {
    switch (op) {
      case 0x02: // StateQuery Response
      case 0x09: // StateReport Event
        sendState("TRSPRT");
        return true;
      case 0x01: // ConfigMode Response
        sendRespNoPayload(ACK_TEST_MODE, statusOk);
        return true;
      case 0x03: { // ConfigStatus Response
        if (pl.size() >= 2) {
          const bool configured = pl[1] != 0;
          sendRespNoPayload(configured ? ACK_CONFIGURED : ACK_NOT_CONFIGURED, configured);
          return true;
        }
        break;
      }
      case 0x04: // Arm Response
        sendRespNoPayload(ACK_ARMED, statusOk);
        return true;
      case 0x05: // Disarm Response
        sendRespNoPayload(ACK_DISARMED, statusOk);
        return true;
      case 0x07: // CapsSet Response
        sendRespNoPayload(ACK_CAP_SET, statusOk);
        return true;
      case 0x08: { // CapsQuery Response
        if (pl.size() >= 2) {
          uint8_t bits = pl[1];
          setCapBitsShadow_(bits);
          sendResp(ACK_CAPS, &bits, 1, statusOk);
          return true;
        }
        break;
      }
      case 0x0C: // NvsWrite Response
        if (pendingLockEmag_ >= 0) {
          sendRespNoPayload(pendingLockEmag_ ? ACK_LOCK_EMAG_ON : ACK_LOCK_EMAG_OFF, statusOk);
          pendingLockEmag_ = -1;
          return true;
        }
        sendRespNoPayload(ACK_CAP_SET, statusOk);
        return true;
      case 0x0B: { // PairingStatus Response
        if (pl.size() >= 2) {
          const bool configured = pl[1] != 0;
          sendRespNoPayload(configured ? ACK_CONFIGURED : ACK_NOT_CONFIGURED, configured);
          return true;
        }
        break;
      }
      case 0x0D: // Heartbeat Response
      case 0x17: { // Ping Response
        if (pl.size() >= 7) {
          uint32_t up = (uint32_t)pl[1] | ((uint32_t)pl[2] << 8) |
                        ((uint32_t)pl[3] << 16) | ((uint32_t)pl[4] << 24);
          uint16_t seq16 = (uint16_t)pl[5] | ((uint16_t)pl[6] << 8);
          struct HeartbeatPayload {
            uint32_t seq_le;
            uint32_t up_ms_le;
          } payload{};
          payload.seq_le = seq16;
          payload.up_ms_le = up;
          sendResp(ACK_HEARTBEAT,
                   reinterpret_cast<const uint8_t*>(&payload),
                   sizeof(payload),
                   statusOk);
          return true;
        }
        break;
      }
      case 0x0E: // UnlockRequest Event
        sendRespNoPayload(EVT_GENERIC, false);
        return true;
      case 0x0F: { // AlarmRequest Event
        uint8_t reason = (pl.size() >= 1) ? pl[0] : 0;
        if (reason == 0) sendRespNoPayload(EVT_BREACH, true);
        else             sendRespNoPayload(EVT_MTRTTRG, false);
        return true;
      }
      case 0x10: // DriverFar
        sendRespNoPayload(ACK_DRIVER_FAR, true);
        return true;
      case 0x15: // CancelTimers Response
        sendRespNoPayload(ACK_TMR_CANCELLED, statusOk);
        return true;
      case 0x16: // SetRole Response
        sendRespNoPayload(ACK_ROLE, statusOk);
        return true;
      case 0x11: // LockCanceled
        sendRespNoPayload(ACK_LOCK_CANCELED, pl.size() ? (pl[0] == 0) : true);
        return true;
      case 0x12: // AlarmOnlyMode
        sendRespNoPayload(ACK_ALARM_ONLY_MODE, pl.size() ? (pl[0] == 0) : true);
        return true;
      case 0x14: // CriticalPower
        sendRespNoPayload(EVT_CRITICAL, false);
        return true;
      default:
        break;
    }
  }

  // ---------- Motor module ----------
  if (mod == static_cast<uint8_t>(transport::Module::Motor)) {
    if (op == 0x01 || op == 0x02) { // Lock/Unlock response
      if (!statusOk) {
        pendingForceAck_ = 0;
        sendRespNoPayload(ACK_LOCK_CANCELED, false);
        return true;
      }
      return true; // success: wait for MotorDone event
    }
    if (op == 0x05) { // MotorDone event
      if (pl.size() >= 2) {
        bool locked = pl[1] != 0;
        if (pendingForceAck_ == 1) {
          sendRespNoPayload(ACK_FORCE_LOCKED, statusOk);
          pendingForceAck_ = 0;
          return true;
        }
        if (pendingForceAck_ == 2) {
          sendRespNoPayload(ACK_FORCE_UNLOCKED, statusOk);
          pendingForceAck_ = 0;
          return true;
        }
        sendRespNoPayload(locked ? ACK_LOCKED : ACK_UNLOCKED, statusOk);
        return true;
      }
    }
  }

  // ---------- Shock module ----------
  if (mod == static_cast<uint8_t>(transport::Module::Shock)) {
    if (op == 0x03) {
      const bool motionEnabled = (CONF && CONF->GetBool(MOTION_TRIG_ALARM, false));
      DBG_PRINT("[ESPNOW][TX] shock sensor triggered (motion ");
      DBG_PRINT(motionEnabled ? "enabled" : "disabled");
      DBG_PRINTLN(")");
      sendRespNoPayload(EVT_MTRTTRG, false);
      return true;
    }
    if (op == 0x10) {
      if (!statusOk && pl.size() >= 2 && pl[1] == 0x01) {
        sendRespNoPayload(ACK_SHOCK_INT_MISSING, false);
        return true;
      }
      sendRespNoPayload(ACK_SHOCK_SENSOR_TYPE_SET, statusOk);
      return true;
    }
    if (op == 0x11) {
      sendRespNoPayload(ACK_SHOCK_SENS_THRESHOLD_SET, statusOk);
      return true;
    }
    if (op == 0x12) {
      sendRespNoPayload(ACK_SHOCK_L2D_CFG_SET, statusOk);
      return true;
    }
  }

  // ---------- Switch/Reed module ----------
  if (mod == static_cast<uint8_t>(transport::Module::SwitchReed)) {
    if (op == 0x01 && pl.size() >= 1) { // DoorEdge
      uint8_t open = pl[0] ? 1 : 0;
      sendResp(EVT_REED, &open, 1, false);
      return true;
    }
    if (op == 0x02) { // OpenRequest
      sendRespNoPayload(EVT_GENERIC, false);
      return true;
    }
  }

  // ---------- Power module ----------
  if (mod == static_cast<uint8_t>(transport::Module::Power)) {
    if (op == 0x02) { // LowBatt
      if (!pl.empty()) {
        sendResp(EVT_LWBT, &pl[0], 1, false);
      } else {
        sendRespNoPayload(EVT_LWBT, false);
      }
      return true;
    }
    if (op == 0x03) { // CriticalBatt
      if (!pl.empty()) {
        sendResp(EVT_CRITICAL, &pl[0], 1, false);
      } else {
        sendRespNoPayload(EVT_CRITICAL, false);
      }
      return true;
    }
  }

  // ---------- Fingerprint module ----------
  if (mod == static_cast<uint8_t>(transport::Module::Fingerprint)) {
    switch (op) {
      case 0x01: // VerifyOn response
        sendRespNoPayload(ACK_FP_VERIFY_ON, statusOk);
        return true;
      case 0x02: // VerifyOff response
        sendRespNoPayload(ACK_FP_VERIFY_OFF, statusOk);
        return true;
      case 0x0A: { // MatchEvent
        if (pl.size() >= 3) {
          uint8_t out[3] = {pl[0], pl[1], pl[2]};
          sendResp(EVT_FP_MATCH, out, sizeof(out), false);
          return true;
        }
        break;
      }
      case 0x0B: { // Fail/busy/no-sensor/tamper
        if (pl.empty()) break;
        uint8_t reason = pl[0];
        if (pl.size() >= 2 && reason > 3) {
          reason = pl[1];
        }
        switch (reason) {
          case 0: sendRespNoPayload(EVT_FP_FAIL, false);          return true;
          case 1: sendRespNoPayload(ACK_FP_NO_SENSOR,     false); return true;
          case 2: sendRespNoPayload(ACK_FP_BUSY,          false); return true;
          case 3: sendRespNoPayload(ACK_ERR_TOKEN,        false); return true;
          default: break;
        }
        break;
      }
      case 0x0C: { // EnrollProgress
        if (pl.size() < 4) break;
        uint8_t stage = pl[0];
        uint8_t status = pl[3];
        uint16_t ack = 0;
        switch (stage) {
          case 1: ack = ACK_FP_ENROLL_START;   break;
          case 2: ack = ACK_FP_ENROLL_CAP1;    break;
          case 3: ack = ACK_FP_ENROLL_LIFT;    break;
          case 4: ack = ACK_FP_ENROLL_CAP2;    break;
          case 5: ack = ACK_FP_ENROLL_STORING; break;
          case 6: ack = ACK_FP_ENROLL_OK;      break;
          case 7: ack = ACK_FP_ENROLL_FAIL;    break;
          case 8: ack = ACK_FP_ENROLL_TIMEOUT; break;
          default: break;
        }
        if (ack != 0) {
          sendRespNoPayload(ack, status == 0);
          return true;
        }
        break;
      }
      case 0x06: { // QueryDb response
        if (pl.size() >= 5) {
          uint8_t out[4] = {pl[1], pl[2], pl[3], pl[4]};
          sendResp(ACK_FP_DB_INFO, out, sizeof(out), statusOk);
          return true;
        }
        break;
      }
      case 0x07: { // NextId response
        if (pl.size() >= 3) {
          uint8_t out[2] = {pl[1], pl[2]};
          sendResp(ACK_FP_NEXT_ID, out, sizeof(out), statusOk);
          return true;
        }
        break;
      }
      case 0x04: { // DeleteId response
        if (pl.size() >= 3) {
          uint8_t out[2] = {pl[1], pl[2]};
          sendResp(ACK_FP_ID_DELETED, out, sizeof(out), statusOk);
          return true;
        }
        break;
      }
      case 0x05: // ClearDb response
        sendRespNoPayload(ACK_FP_DB_CLEARED, statusOk);
        return true;
      case 0x08: // AdoptSensor response
        sendRespNoPayload(statusOk ? ACK_FP_ADOPT_OK : ACK_FP_ADOPT_FAIL, statusOk);
        return true;
      case 0x09: // ReleaseSensor response
        sendRespNoPayload(statusOk ? ACK_FP_RELEASE_OK : ACK_FP_RELEASE_FAIL, statusOk);
        return true;
      default:
        break;
    }
  }

  // Not handled: swallow to prevent raw transport frames reaching master.
  DBG_PRINTF("[ESPNOW][TRSPRT] Unhandled response mod=0x%02X op=0x%02X len=%u\n",
               (unsigned)mod, (unsigned)op, (unsigned)pl.size());
  return true;
}
