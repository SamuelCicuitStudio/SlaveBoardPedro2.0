#include <DeviceHandler.hpp>
#include <Device.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <ESPNOWManager.hpp>
#include <PowerManager.hpp>
#include <RGBLed.hpp>
#include <Transport.hpp>
#include <Utils.hpp>
#include <stdio.h>

// Device module opCodes (align with readme/transport.md)
static constexpr uint8_t OPC_CONFIG_MODE    = 0x01;
static constexpr uint8_t OPC_STATE_QUERY    = 0x02;
static constexpr uint8_t OPC_CONFIG_STATUS  = 0x03;
static constexpr uint8_t OPC_ARM            = 0x04;
static constexpr uint8_t OPC_DISARM         = 0x05;
static constexpr uint8_t OPC_REBOOT         = 0x06;
static constexpr uint8_t OPC_CAPS_SET       = 0x07;
static constexpr uint8_t OPC_CAPS_QUERY     = 0x08;
static constexpr uint8_t OPC_PAIR_INIT      = 0x0A;
static constexpr uint8_t OPC_PAIR_STATUS    = 0x0B;
static constexpr uint8_t OPC_NVS_WRITE      = 0x0C;
static constexpr uint8_t OPC_HEARTBEAT      = 0x0D;
static constexpr uint8_t OPC_CANCEL_TIMERS  = 0x15;
static constexpr uint8_t OPC_SET_ROLE       = 0x16;
static constexpr uint8_t OPC_PING           = 0x17;

void DeviceHandler::onMessage(const transport::TransportMessage& msg) {
  const uint8_t op = msg.header.opCode;
  switch (op) {
    case OPC_CONFIG_MODE:   handleConfigMode_(msg);   break;
    case OPC_STATE_QUERY:   handleStateQuery_(msg);   break;
    case OPC_CONFIG_STATUS: handleConfigStatus_(msg); break;
    case OPC_ARM:           handleArm_(msg, true);    break;
    case OPC_DISARM:        handleArm_(msg, false);   break;
    case OPC_REBOOT:        handleReboot_(msg);       break;
    case OPC_CAPS_SET:      handleCapsSet_(msg);      break;
    case OPC_CAPS_QUERY:    handleCapsQuery_(msg);    break;
    case OPC_PAIR_INIT:     handlePairInit_(msg);     break;
    case OPC_PAIR_STATUS:   handlePairStatus_(msg);   break;
    case OPC_NVS_WRITE:     handleNvsWrite_(msg);     break;
    case OPC_HEARTBEAT:
    case OPC_PING:          handleHeartbeat_(msg);    break;
    case OPC_CANCEL_TIMERS: handleCancelTimers_(msg); break;
    case OPC_SET_ROLE:      handleSetRole_(msg);      break;
    default:
      sendStatusOnly_(msg, transport::StatusCode::UNSUPPORTED);
      break;
  }
}

void DeviceHandler::handleConfigMode_(const transport::TransportMessage& msg) {
  // Trigger config mode on Device via ESPNOW command path
  if (!dev_ || !dev_->Now) {
    sendStatusOnly_(msg, transport::StatusCode::DENIED);
    return;
  }
  dev_->Now->setConfigMode(true);
  sendStatusOnly_(msg, transport::StatusCode::OK);
}

void DeviceHandler::handleStateQuery_(const transport::TransportMessage& msg) {
  if (!dev_) { sendStatusOnly_(msg, transport::StatusCode::DENIED); return; }

  // Build state struct payload
  std::vector<uint8_t> pl;
  pl.reserve(17);
  const bool armed     = dev_->isArmed_();
  const bool motion    = dev_->isMotionEnabled_();
  const bool locked    = dev_->isLocked_();
  const bool doorOpen  = dev_->isDoorOpen_();
  const bool breach    = (dev_->Now ? dev_->Now->breach : false);
  const bool motorMove = dev_->isMotorMoving_();
  const uint8_t batt   = dev_->PowerMgr ? (uint8_t)dev_->PowerMgr->getBatteryPercentage() : 0;
  const uint8_t pmode  = dev_->PowerMgr ? (uint8_t)dev_->PowerMgr->getPowerMode() : 0;
  const uint8_t band   = dev_->effectiveBand_;
  const bool cfgMode   = dev_->configModeActive_;
  const bool configured= dev_->isConfigured_();
  const bool sleepPend = dev_->sleepPending_;
  const uint32_t up    = millis();
  const uint8_t role   = dev_->isAlarmRole_ ? 1 : 0;

  pl.push_back(armed);
  pl.push_back(locked);
  pl.push_back(doorOpen);
  pl.push_back(breach);
  pl.push_back(motorMove);
  pl.push_back(batt);
  pl.push_back(pmode);
  pl.push_back(band);
  pl.push_back(cfgMode);
  pl.push_back(configured);
  pl.push_back(sleepPend);
  pl.push_back(uint8_t(up & 0xFF));
  pl.push_back(uint8_t((up >> 8) & 0xFF));
  pl.push_back(uint8_t((up >> 16) & 0xFF));
  pl.push_back(uint8_t((up >> 24) & 0xFF));
  pl.push_back(role);
  pl.push_back(motion);

  transport::TransportMessage resp;
  resp.header = msg.header;
  resp.header.srcId  = msg.header.destId;
  resp.header.destId = msg.header.srcId;
  resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
  resp.header.flags  = 0x02; // isResponse
  resp.header.payloadLen = static_cast<uint8_t>(pl.size() + 1);
  resp.payload.clear();
  resp.payload.push_back(static_cast<uint8_t>(transport::StatusCode::OK));
  resp.payload.insert(resp.payload.end(), pl.begin(), pl.end());

  if (port_) port_->send(resp, true);
}

void DeviceHandler::handleConfigStatus_(const transport::TransportMessage& msg) {
  const bool configured = dev_ ? dev_->isConfigured_() : false;
  transport::TransportMessage resp;
  resp.header = msg.header;
  resp.header.srcId  = msg.header.destId;
  resp.header.destId = msg.header.srcId;
  resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
  resp.header.flags  = 0x02;
  resp.payload = {
    static_cast<uint8_t>(transport::StatusCode::OK),
    static_cast<uint8_t>(configured)
  };
  resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());
  if (port_) port_->send(resp, true);
}

void DeviceHandler::handleArm_(const transport::TransportMessage& msg, bool arm) {
  if (!dev_ || !CONF) { sendStatusOnly_(msg, transport::StatusCode::DENIED); return; }
  CONF->PutBool(ARMED_STATE, arm);
  sendStatusOnly_(msg, transport::StatusCode::OK);

  // Visual overlay: show armed/disarmed transition (Lock and Alarm roles)
  if (RGBLed::TryGet()) {
  }
}

void DeviceHandler::handleReboot_(const transport::TransportMessage& msg) {
  if (!dev_) { sendStatusOnly_(msg, transport::StatusCode::DENIED); return; }
  // Optional payload[0]: 0 = plain reboot, 1 = factory reset.
  bool factoryReset = false;
  if (!msg.payload.empty()) {
    factoryReset = (msg.payload[0] != 0);
  } else {
    // Backward-compatible default: treat no-payload as factory reset.
    factoryReset = true;
  }
  dev_->requestReset(factoryReset,
                     factoryReset ? "Transport OPC_REBOOT (factory)" : "Transport OPC_REBOOT (reboot)");
  sendStatusOnly_(msg, transport::StatusCode::OK);
}

void DeviceHandler::handleCapsSet_(const transport::TransportMessage& msg) {
  if (!dev_ || !CONF || msg.payload.size() < 1) {
    sendStatusOnly_(msg, transport::StatusCode::INVALID_PARAM);
    return;
  }
  uint8_t prevBits = 0;
  prevBits |= CONF->GetBool(HAS_OPEN_SWITCH_KEY,   HAS_OPEN_SWITCH_DEFAULT)   ? 0x01 : 0;
  prevBits |= CONF->GetBool(HAS_SHOCK_SENSOR_KEY,  HAS_SHOCK_SENSOR_DEFAULT)  ? 0x02 : 0;
  prevBits |= CONF->GetBool(HAS_REED_SWITCH_KEY,   HAS_REED_SWITCH_DEFAULT)   ? 0x04 : 0;
  prevBits |= CONF->GetBool(HAS_FINGERPRINT_KEY,   HAS_FINGERPRINT_DEFAULT)   ? 0x08 : 0;

  uint8_t bits = msg.payload[0];
  if (dev_ && dev_->isAlarmRole_) {
    // Alarm role forces reed + shock only.
    bits = 0x06;
  }
  DBG_PRINTF("[Caps] Set by master: prev=0x%02X new=0x%02X (O%d S%d R%d F%d)\n",
               (unsigned)prevBits,
               (unsigned)bits,
               (bits & 0x01) ? 1 : 0,
               (bits & 0x02) ? 1 : 0,
               (bits & 0x04) ? 1 : 0,
               (bits & 0x08) ? 1 : 0);
  CONF->PutBool(HAS_OPEN_SWITCH_KEY,   bits & 0x01);
  CONF->PutBool(HAS_SHOCK_SENSOR_KEY,  bits & 0x02);
  CONF->PutBool(HAS_REED_SWITCH_KEY,   bits & 0x04);
  CONF->PutBool(HAS_FINGERPRINT_KEY,   bits & 0x08);
  dev_->refreshCapabilities_();
  uint8_t nowBits = 0;
  nowBits |= CONF->GetBool(HAS_OPEN_SWITCH_KEY,   HAS_OPEN_SWITCH_DEFAULT)   ? 0x01 : 0;
  nowBits |= CONF->GetBool(HAS_SHOCK_SENSOR_KEY,  HAS_SHOCK_SENSOR_DEFAULT)  ? 0x02 : 0;
  nowBits |= CONF->GetBool(HAS_REED_SWITCH_KEY,   HAS_REED_SWITCH_DEFAULT)   ? 0x04 : 0;
  nowBits |= CONF->GetBool(HAS_FINGERPRINT_KEY,   HAS_FINGERPRINT_DEFAULT)   ? 0x08 : 0;
  DBG_PRINTF("[Caps] NVS updated: bits=0x%02X (O%d S%d R%d F%d)\n",
               (unsigned)nowBits,
               (nowBits & 0x01) ? 1 : 0,
               (nowBits & 0x02) ? 1 : 0,
               (nowBits & 0x04) ? 1 : 0,
               (nowBits & 0x08) ? 1 : 0);
  sendStatusOnly_(msg, transport::StatusCode::OK);
}

void DeviceHandler::handleCapsQuery_(const transport::TransportMessage& msg) {
  if (!dev_ || !CONF) { sendStatusOnly_(msg, transport::StatusCode::DENIED); return; }
  uint8_t bits = 0;
  bits |= CONF->GetBool(HAS_OPEN_SWITCH_KEY,   HAS_OPEN_SWITCH_DEFAULT)   ? 0x01 : 0;
  bits |= CONF->GetBool(HAS_SHOCK_SENSOR_KEY,  HAS_SHOCK_SENSOR_DEFAULT)  ? 0x02 : 0;
  bits |= CONF->GetBool(HAS_REED_SWITCH_KEY,   HAS_REED_SWITCH_DEFAULT)   ? 0x04 : 0;
  bits |= CONF->GetBool(HAS_FINGERPRINT_KEY,   HAS_FINGERPRINT_DEFAULT)   ? 0x08 : 0;
  if (dev_ && dev_->isAlarmRole_) {
    bits = 0x06;
  }

  transport::TransportMessage resp;
  resp.header = msg.header;
  resp.header.srcId  = msg.header.destId;
  resp.header.destId = msg.header.srcId;
  resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
  resp.header.flags  = 0x02;
  resp.payload = {
    static_cast<uint8_t>(transport::StatusCode::OK),
    bits
  };
  resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());
  if (port_) port_->send(resp, true);
}

void DeviceHandler::handleNvsWrite_(const transport::TransportMessage& msg) {
  // keyId:uint8 + value bytes (bool expected)
  if (!CONF || msg.payload.size() < 2) {
    sendStatusOnly_(msg, transport::StatusCode::INVALID_PARAM);
    return;
  }
  uint8_t keyId = msg.payload[0];
  bool val = msg.payload[1] != 0;
  bool capChanged = false;
  switch (keyId) {
    case 1: CONF->PutBool(ARMED_STATE, val); break;
    case 2: CONF->PutBool(LOCK_STATE, val); break;
    case 3: CONF->PutBool(HAS_OPEN_SWITCH_KEY, val); capChanged = true; break;
    case 4: CONF->PutBool(HAS_SHOCK_SENSOR_KEY, val); capChanged = true; break;
    case 5: CONF->PutBool(HAS_REED_SWITCH_KEY, val); capChanged = true; break;
    case 6: CONF->PutBool(HAS_FINGERPRINT_KEY, val); capChanged = true; break;
    case 7: CONF->PutBool(LOCK_EMAG_KEY, val); break;
    default:
      sendStatusOnly_(msg, transport::StatusCode::UNSUPPORTED);
      return;
  }
  if (capChanged && dev_) dev_->refreshCapabilities_();
  sendStatusOnly_(msg, transport::StatusCode::OK);
}

void DeviceHandler::sendStatusOnly_(const transport::TransportMessage& req,
                                    transport::StatusCode status) {
  transport::TransportMessage resp;
  resp.header = req.header;
  resp.header.srcId  = req.header.destId;
  resp.header.destId = req.header.srcId;
  resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
  resp.header.flags  = 0x02;
  resp.payload = { static_cast<uint8_t>(status) };
  resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());
  if (port_) port_->send(resp, true);
}

void DeviceHandler::handlePairInit_(const transport::TransportMessage& msg) {
  // Payload: master MAC (6 bytes) + optional token (ignored here)
  if (!CONF || msg.payload.size() < 6) {
    sendStatusOnly_(msg, transport::StatusCode::INVALID_PARAM);
    return;
  }
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           msg.payload[0], msg.payload[1], msg.payload[2],
           msg.payload[3], msg.payload[4], msg.payload[5]);
  CONF->PutString(MASTER_ESPNOW_ID, macStr);
  CONF->PutBool(DEVICE_CONFIGURED, true);
  sendStatusOnly_(msg, transport::StatusCode::OK);
}

void DeviceHandler::handlePairStatus_(const transport::TransportMessage& msg) {
  transport::TransportMessage resp;
  resp.header = msg.header;
  resp.header.srcId  = msg.header.destId;
  resp.header.destId = msg.header.srcId;
  resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
  resp.header.flags  = 0x02;
  bool configured = CONF ? CONF->GetBool(DEVICE_CONFIGURED, false) : false;
  resp.payload.push_back(static_cast<uint8_t>(transport::StatusCode::OK));
  resp.payload.push_back(static_cast<uint8_t>(configured));
  if (CONF) {
    String mac = CONF->GetString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
    uint8_t buf[6] = {0};
    sscanf(mac.c_str(), "%hhX:%hhX:%hhX:%hhX:%hhX:%hhX",
           &buf[0], &buf[1], &buf[2], &buf[3], &buf[4], &buf[5]);
    resp.payload.insert(resp.payload.end(), buf, buf + 6);
  }
  resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());
  if (port_) port_->send(resp, true);
}

void DeviceHandler::handleHeartbeat_(const transport::TransportMessage& msg) {
  static uint16_t seq = 0;
  transport::TransportMessage resp;
  resp.header = msg.header;
  resp.header.srcId  = msg.header.destId;
  resp.header.destId = msg.header.srcId;
  resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
  resp.header.flags  = 0x02;
  uint32_t up = millis();
  resp.payload = {
    static_cast<uint8_t>(transport::StatusCode::OK),
    uint8_t(up & 0xFF), uint8_t((up >> 8) & 0xFF),
    uint8_t((up >> 16) & 0xFF), uint8_t((up >> 24) & 0xFF),
    uint8_t(seq & 0xFF), uint8_t((seq >> 8) & 0xFF)
  };
  seq++;
  resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());
  if (port_) port_->send(resp, true);
}

void DeviceHandler::handleCancelTimers_(const transport::TransportMessage& msg) {
  sendStatusOnly_(msg, transport::StatusCode::OK);
}

void DeviceHandler::handleSetRole_(const transport::TransportMessage& msg) {
  // Role not persisted here; accept and respond OK.
  sendStatusOnly_(msg, transport::StatusCode::OK);
}
