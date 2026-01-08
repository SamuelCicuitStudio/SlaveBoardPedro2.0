#include <MotorHandler.hpp>
#include <ConfigNvs.hpp>
#include <Transport.hpp>

// Motor module opCodes
static constexpr uint8_t MTR_LOCK     = 0x01;
static constexpr uint8_t MTR_UNLOCK   = 0x02;
static constexpr uint8_t MTR_PULSE_CCW= 0x03;
static constexpr uint8_t MTR_PULSE_CW = 0x04;

void MotorHandler::onMessage(const transport::TransportMessage& msg) {
  if (!motor_) {
    sendStatus_(msg, transport::StatusCode::UNSUPPORTED);
    return;
  }

  switch (msg.header.opCode) {
    case MTR_LOCK: {
      bool ok = motor_->startLockTask();
      sendStatus_(msg, ok ? transport::StatusCode::OK : transport::StatusCode::APPLY_FAIL);
      if (ok && nvs_) nvs_->PutBool(LOCK_STATE, true);
      break;
    }
    case MTR_UNLOCK: {
      bool ok = motor_->startUnlockTask();
      sendStatus_(msg, ok ? transport::StatusCode::OK : transport::StatusCode::APPLY_FAIL);
      if (ok && nvs_) nvs_->PutBool(LOCK_STATE, false);
      break;
    }
    case MTR_PULSE_CCW: {
      motor_->setDirection(true);
      if (nvs_) {
        if (nvs_->GetBool(LOCK_STATE, LOCK_STATE_DEFAULT)) motor_->lockScrew();
        else                                             motor_->unlockScrew();
      }
      motor_->stop();
      sendStatus_(msg, transport::StatusCode::OK);
      break;
    }
    case MTR_PULSE_CW: {
      motor_->setDirection(false);
      if (nvs_) {
        if (nvs_->GetBool(LOCK_STATE, LOCK_STATE_DEFAULT)) motor_->lockScrew();
        else                                             motor_->unlockScrew();
      }
      motor_->stop();
      sendStatus_(msg, transport::StatusCode::OK);
      break;
    }
    default:
      sendStatus_(msg, transport::StatusCode::UNSUPPORTED);
      break;
  }
}

void MotorHandler::sendStatus_(const transport::TransportMessage& req,
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
