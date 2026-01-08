/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#pragma once
/**
 * @file DeviceHandler.h
 * @brief Transport handler for Device module opCodes.
 *
 * Responsibilities:
 *  - Handle Device module requests (config mode, state query, config status, arm/disarm, reboot, caps set/query).
 *  - Build responses per transport spec (status + payload).
 *  - Does not touch radio directly; uses TransportPort send().
 */

#include <Transport.hpp>

class Device;

class DeviceHandler : public transport::TransportHandler {
public:
  DeviceHandler(Device* dev, transport::TransportPort* port)
      : dev_(dev), port_(port) {}

  void onMessage(const transport::TransportMessage& msg) override;

private:
  void handleConfigMode_(const transport::TransportMessage& msg);
  void handleStateQuery_(const transport::TransportMessage& msg);
  void handleConfigStatus_(const transport::TransportMessage& msg);
  void handleArm_(const transport::TransportMessage& msg, bool arm);
  void handleReboot_(const transport::TransportMessage& msg);
  void handleCapsSet_(const transport::TransportMessage& msg);
  void handleCapsQuery_(const transport::TransportMessage& msg);
  void handlePairInit_(const transport::TransportMessage& msg);
  void handlePairStatus_(const transport::TransportMessage& msg);
  void handleNvsWrite_(const transport::TransportMessage& msg);
  void handleHeartbeat_(const transport::TransportMessage& msg);
  void handleCancelTimers_(const transport::TransportMessage& msg);
  void handleSetRole_(const transport::TransportMessage& msg);
  void sendStatusOnly_(const transport::TransportMessage& req, transport::StatusCode status);

  Device* dev_;
  transport::TransportPort* port_;
};






