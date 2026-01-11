#include <ShockHandler.hpp>
#include <ConfigNvs.hpp>
#include <ShockSensor.hpp>
#include <Transport.hpp>

// Shock module opCodes
static constexpr uint8_t SHOCK_ENABLE  = 0x01;
static constexpr uint8_t SHOCK_DISABLE = 0x02;
static constexpr uint8_t SHOCK_SET_TYPE = 0x10;
static constexpr uint8_t SHOCK_SET_THS  = 0x11;
static constexpr uint8_t SHOCK_SET_L2D  = 0x12;
static constexpr uint8_t SHOCK_REASON_INT_MISSING = 0x01;

void ShockHandler::onMessage(const transport::TransportMessage& msg) {
  if (!nvs_) { sendStatus_(msg, transport::StatusCode::DENIED); return; }

  auto applyCfg = [this]() -> bool {
    if (!sensor_) return true;
    const bool hasShock =
        nvs_ ? nvs_->GetBool(HAS_SHOCK_SENSOR_KEY, HAS_SHOCK_SENSOR_DEFAULT)
             : HAS_SHOCK_SENSOR_DEFAULT;
    if (!hasShock) {
      sensor_->disable();
      return true;
    }
    ShockConfig cfg = ShockSensor::loadConfig(nvs_);
    return sensor_->applyConfig(cfg);
  };

  switch (msg.header.opCode) {
    case SHOCK_ENABLE:
      nvs_->PutBool(MOTION_TRIG_ALARM, true);
      sendStatus_(msg, transport::StatusCode::OK);
      break;
    case SHOCK_DISABLE:
      nvs_->PutBool(MOTION_TRIG_ALARM, false);
      sendStatus_(msg, transport::StatusCode::OK);
      break;
    case SHOCK_SET_TYPE: {
      if (msg.payload.size() < 1) {
        sendStatus_(msg, transport::StatusCode::INVALID_PARAM);
        break;
      }
      uint8_t type = msg.payload[0];
      if (type != SHOCK_SENSOR_TYPE_EXTERNAL && type != SHOCK_SENSOR_TYPE_INTERNAL) {
        sendStatus_(msg, transport::StatusCode::INVALID_PARAM);
        break;
      }
      if (type == SHOCK_SENSOR_TYPE_INTERNAL) {
        if (!sensor_) {
          nvs_->PutInt(SHOCK_SENSOR_TYPE_KEY, SHOCK_SENSOR_TYPE_EXTERNAL);
          sendStatus_(msg,
                      transport::StatusCode::APPLY_FAIL,
                      {SHOCK_REASON_INT_MISSING});
          break;
        }

        ShockConfig prev = ShockSensor::loadConfig(nvs_);
        ShockConfig cfg = prev;
        cfg.type = SHOCK_SENSOR_TYPE_INTERNAL;

        const bool ok = sensor_->applyConfig(cfg);
        if (ok) {
          nvs_->PutInt(SHOCK_SENSOR_TYPE_KEY, type);
          nvs_->PutBool(HAS_SHOCK_SENSOR_KEY, true);
          sendStatus_(msg, transport::StatusCode::OK);
        } else {
          (void)sensor_->applyConfig(prev);
          nvs_->PutInt(SHOCK_SENSOR_TYPE_KEY, SHOCK_SENSOR_TYPE_EXTERNAL);
          sendStatus_(msg,
                      transport::StatusCode::APPLY_FAIL,
                      {SHOCK_REASON_INT_MISSING});
        }
        break;
      }

      nvs_->PutInt(SHOCK_SENSOR_TYPE_KEY, type);
      sendStatus_(msg, applyCfg() ? transport::StatusCode::OK
                                  : transport::StatusCode::APPLY_FAIL);
      break;
    }
    case SHOCK_SET_THS: {
      if (msg.payload.size() < 1) {
        sendStatus_(msg, transport::StatusCode::INVALID_PARAM);
        break;
      }
      uint8_t ths = msg.payload[0] & 0x7F;
      nvs_->PutInt(SHOCK_SENS_THRESHOLD_KEY, ths);
      sendStatus_(msg, applyCfg() ? transport::StatusCode::OK
                                  : transport::StatusCode::APPLY_FAIL);
      break;
    }
    case SHOCK_SET_L2D: {
      if (msg.payload.size() < 11) {
        sendStatus_(msg, transport::StatusCode::INVALID_PARAM);
        break;
      }
      ShockConfig cfg = ShockSensor::loadConfig(nvs_);
      cfg.odr      = msg.payload[0];
      cfg.scale    = msg.payload[1];
      cfg.res      = msg.payload[2];
      cfg.evtMode  = msg.payload[3];
      cfg.dur      = msg.payload[4];
      cfg.axisMask = msg.payload[5];
      cfg.hpfMode  = msg.payload[6];
      cfg.hpfCut   = msg.payload[7];
      cfg.hpfEn    = (msg.payload[8] != 0);
      cfg.latch    = (msg.payload[9] != 0);
      cfg.intLevel = msg.payload[10];
      cfg = ShockSensor::sanitizeConfig(cfg);

      nvs_->PutInt (SHOCK_L2D_ODR_KEY,      cfg.odr);
      nvs_->PutInt (SHOCK_L2D_SCALE_KEY,    cfg.scale);
      nvs_->PutInt (SHOCK_L2D_RES_KEY,      cfg.res);
      nvs_->PutInt (SHOCK_L2D_EVT_MODE_KEY, cfg.evtMode);
      nvs_->PutInt (SHOCK_L2D_DUR_KEY,      cfg.dur);
      nvs_->PutInt (SHOCK_L2D_AXIS_KEY,     cfg.axisMask);
      nvs_->PutInt (SHOCK_L2D_HPF_MODE_KEY, cfg.hpfMode);
      nvs_->PutInt (SHOCK_L2D_HPF_CUT_KEY,  cfg.hpfCut);
      nvs_->PutBool(SHOCK_L2D_HPF_EN_KEY,   cfg.hpfEn);
      nvs_->PutBool(SHOCK_L2D_LATCH_KEY,    cfg.latch);
      nvs_->PutInt (SHOCK_L2D_INT_LVL_KEY,  cfg.intLevel);

      sendStatus_(msg, applyCfg() ? transport::StatusCode::OK
                                  : transport::StatusCode::APPLY_FAIL);
      break;
    }
    default:
      sendStatus_(msg, transport::StatusCode::UNSUPPORTED);
      break;
  }
}

void ShockHandler::sendStatus_(const transport::TransportMessage& req,
                               transport::StatusCode status) {
  sendStatus_(req, status, {});
}

void ShockHandler::sendStatus_(const transport::TransportMessage& req,
                               transport::StatusCode status,
                               const std::vector<uint8_t>& extra) {
  transport::TransportMessage resp;
  resp.header = req.header;
  resp.header.srcId  = req.header.destId;
  resp.header.destId = req.header.srcId;
  resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
  resp.header.flags  = 0x02;
  resp.payload.clear();
  resp.payload.reserve(1 + extra.size());
  resp.payload.push_back(static_cast<uint8_t>(status));
  resp.payload.insert(resp.payload.end(), extra.begin(), extra.end());
  resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());
  if (port_) port_->send(resp, true);
}
