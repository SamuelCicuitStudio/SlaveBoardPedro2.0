#include <ShockHandler.hpp>
#include <ConfigNvs.hpp>
#include <Transport.hpp>

// Shock module opCodes
static constexpr uint8_t SHOCK_ENABLE  = 0x01;
static constexpr uint8_t SHOCK_DISABLE = 0x02;

void ShockHandler::onMessage(const transport::TransportMessage& msg) {
  if (!nvs_) { sendStatus_(msg, transport::StatusCode::DENIED); return; }

  switch (msg.header.opCode) {
    case SHOCK_ENABLE:
      nvs_->PutBool(MOTION_TRIG_ALARM, true);
      sendStatus_(msg, transport::StatusCode::OK);
      break;
    case SHOCK_DISABLE:
      nvs_->PutBool(MOTION_TRIG_ALARM, false);
      sendStatus_(msg, transport::StatusCode::OK);
      break;
    default:
      sendStatus_(msg, transport::StatusCode::UNSUPPORTED);
      break;
  }
}

void ShockHandler::sendStatus_(const transport::TransportMessage& req,
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
