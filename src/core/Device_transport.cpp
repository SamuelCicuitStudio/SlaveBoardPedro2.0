#include <Device.hpp>
#include <ESPNOWManager.hpp>
#include <PowerManager.hpp>
#include <TransportManager.hpp>

// =========================
// Master comms (gated)
// =========================
void Device::sendAck_(uint16_t opcode, bool ok) {
  if (!isConfigured_() || !Now) return;
  Now->SendAck(opcode, ok);
}

void Device::sendAck_(uint16_t opcode, const uint8_t* payload, size_t payloadLen, bool ok) {
  if (!isConfigured_() || !Now) return;
  Now->SendAck(opcode, payload, payloadLen, ok);
}
void Device::sendMotionTrig_() {
  if (!isConfigured_() || !Now) return;
  Now->SendMotionTrigg();
}
void Device::requestUnlock_() {
  if (!isConfigured_() || !Now) return;
  Now->RequestUnlock();
}
void Device::requestAlarm_() {
  if (!isConfigured_() || !Now) return;
  Now->RequesAlarm();
}

void Device::sendTransportEvent_(transport::Module mod, uint8_t op,
                                 const std::vector<uint8_t>& payload,
                                 transport::MessageType type) {
  if (!Transport) return;
  if (!isConfigured_()) return;  // No transport traffic while unpaired
  transport::TransportMessage msg;
  msg.header.destId = 1; // master
  msg.header.module = static_cast<uint8_t>(mod);
  msg.header.type   = static_cast<uint8_t>(type);
  msg.header.opCode = op;
  msg.header.flags  = 0;
  msg.payload       = payload;
  msg.header.payloadLen = static_cast<uint8_t>(payload.size());
  Transport->port().send(msg, true);
}

std::vector<uint8_t> Device::buildStatePayload_() const {
  std::vector<uint8_t> pl;
  pl.reserve(17);
  const bool armed     = isArmed_();
  const bool motion    = isMotionEnabled_();
  const bool locked    = isAlarmRole_ ? false : isLocked_();
  const bool doorOpen  = isDoorOpen_();
  const bool breach    = (Now ? Now->breach : false);
  const bool motorMove = isMotorMoving_();
  const uint8_t batt   = PowerMgr ? (uint8_t)PowerMgr->getBatteryPercentage() : 0;
  const uint8_t pmode  = PowerMgr ? (uint8_t)PowerMgr->getPowerMode() : 0;
  const uint8_t band   = effectiveBand_;
  const bool cfgMode   = configModeActive_;
  const bool configured= isConfigured_();
  const bool sleepPend = sleepPending_;
  const uint32_t up    = millis();
  const uint8_t role   = isAlarmRole_ ? 1 : 0;

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
  return pl;
}
