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
 * @file MotorHandler.h
 * @brief Transport handler for Motor module opCodes.
 */

#include <Transport.hpp>
#include <MotorDriver.hpp>
#include <NVSManager.hpp>

class MotorHandler : public transport::TransportHandler {
public:
  MotorHandler(MotorDriver* motor, NVS* nvs, transport::TransportPort* port)
      : motor_(motor), nvs_(nvs), port_(port) {}

  void onMessage(const transport::TransportMessage& msg) override;

private:
  void sendStatus_(const transport::TransportMessage& req,
                   transport::StatusCode status);

  MotorDriver* motor_;
  NVS* nvs_;
  transport::TransportPort* port_;
};






