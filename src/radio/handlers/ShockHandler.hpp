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
 * @file ShockHandler.h
 * @brief Transport handler for Shock module opCodes (enable/disable/config).
 */

#include <Transport.hpp>
#include <NVSManager.hpp>

class ShockSensor;

class ShockHandler : public transport::TransportHandler {
public:
  ShockHandler(NVS* nvs, transport::TransportPort* port, ShockSensor* sensor)
      : nvs_(nvs), port_(port), sensor_(sensor) {}

  void onMessage(const transport::TransportMessage& msg) override;

private:
  void sendStatus_(const transport::TransportMessage& req,
                   transport::StatusCode status);

  NVS* nvs_;
  transport::TransportPort* port_;
  ShockSensor* sensor_;
};






