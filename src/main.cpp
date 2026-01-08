#include <Device.hpp>
#include <Logger.hpp>
#include <NVSManager.hpp>
#include <RTCManager.hpp>
#include <RGBLed.hpp>
#include <Utils.hpp>
#include <FreeRTOS.h>
#include <task.h>

Device device;

void setup() {
    Debug::begin(SERIAL_BAUD_RATE);
    NVS::Init();         // guarantees singleton exists
    CONF->begin();       // safe: Get() always returns a valid pointer

    // RTC + Logger come up after config, before RGB
    static struct tm timeInfo{};
    RTCManager::Init(&timeInfo);
    Logger::Init(RTCM);
    LOGG->Begin();

    RGBLed::Init(LOWBAT_LED_PIN, DATA_FLAG_LED_PIN, BLE_FLAG_LED_PIN, false);
    RGB->begin();
    RGB->setDeviceState(DeviceState::BOOT);

    device.begin();
}

void loop() {
    device.loop();
      vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_DELAY_MS));
}
