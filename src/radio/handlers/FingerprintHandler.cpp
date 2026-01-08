#include <FingerprintHandler.hpp>
#include <Transport.hpp>

// Fingerprint module opCodes (from spec)
static constexpr uint8_t FP_VERIFY_ON     = 0x01;
static constexpr uint8_t FP_VERIFY_OFF    = 0x02;
static constexpr uint8_t FP_ENROLL        = 0x03;
static constexpr uint8_t FP_DELETE_ID     = 0x04;
static constexpr uint8_t FP_CLEAR_DB      = 0x05;
static constexpr uint8_t FP_QUERY_DB      = 0x06;
static constexpr uint8_t FP_NEXT_ID       = 0x07;
static constexpr uint8_t FP_ADOPT_SENSOR  = 0x08;
static constexpr uint8_t FP_RELEASE       = 0x09;

void FingerprintHandler::onMessage(const transport::TransportMessage& msg) {
  if (!fp_ || !fp_->isSensorPresent()) {
    sendStatus_(msg, transport::StatusCode::DENIED);
    return;
  }

  switch (msg.header.opCode) {
    case FP_VERIFY_ON:
      fp_->startVerifyMode();
      sendStatus_(msg, transport::StatusCode::OK);
      break;
    case FP_VERIFY_OFF:
      fp_->stopVerifyMode();
      sendStatus_(msg, transport::StatusCode::OK);
      break;
    case FP_ENROLL: {
      if (msg.payload.size() < 2) { sendStatus_(msg, transport::StatusCode::INVALID_PARAM); break; }
      uint16_t slot = msg.payload[0] | (uint16_t(msg.payload[1]) << 8);
      auto st = fp_->requestEnrollment(slot);
      sendStatus_(msg, st);
      break;
    }
    case FP_DELETE_ID: {
      if (msg.payload.size() < 2) { sendStatus_(msg, transport::StatusCode::INVALID_PARAM); break; }
      uint16_t slot = msg.payload[0] | (uint16_t(msg.payload[1]) << 8);
      auto st = fp_->deleteFingerprint(slot);
      std::vector<uint8_t> extra{
        uint8_t(slot & 0xFF), uint8_t((slot >> 8) & 0xFF)
      };
      sendStatus_(msg, st, extra);
      break;
    }
    case FP_CLEAR_DB:
      sendStatus_(msg, fp_->deleteAllFingerprints());
      break;
    case FP_QUERY_DB:
      {
        uint16_t count = 0, cap = 0;
        auto ok = fp_->getDbInfo(count, cap);
        std::vector<uint8_t> extra{
          uint8_t(count & 0xFF), uint8_t((count >> 8) & 0xFF),
          uint8_t(cap & 0xFF),   uint8_t((cap >> 8) & 0xFF)
        };
        sendStatus_(msg, ok ? transport::StatusCode::OK : transport::StatusCode::DENIED, extra);
      }
      break;
    case FP_NEXT_ID:
      {
        uint16_t id = 0;
        bool ok = fp_->getNextFreeId(id);
        std::vector<uint8_t> extra;
        if (ok) {
          extra.push_back(uint8_t(id & 0xFF));
          extra.push_back(uint8_t((id >> 8) & 0xFF));
        }
        sendStatus_(msg, ok ? transport::StatusCode::OK : transport::StatusCode::APPLY_FAIL, extra);
      }
      break;
    case FP_ADOPT_SENSOR:
      sendStatus_(msg, fp_->adoptNewSensor());
      break;
    case FP_RELEASE:
      sendStatus_(msg, fp_->releaseSensorToDefault());
      break;
    default:
      sendStatus_(msg, transport::StatusCode::UNSUPPORTED);
      break;
  }
}

void FingerprintHandler::sendStatus_(const transport::TransportMessage& req,
                                     transport::StatusCode status,
                                     const std::vector<uint8_t>& extra) {
  transport::TransportMessage resp;
  resp.header = req.header;
  resp.header.srcId  = req.header.destId;
  resp.header.destId = req.header.srcId;
  resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
  resp.header.flags  = 0x02;

  resp.payload.clear();
  resp.payload.push_back(static_cast<uint8_t>(status));
  resp.payload.insert(resp.payload.end(), extra.begin(), extra.end());
  resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());

  if (port_) port_->send(resp, true);
}
