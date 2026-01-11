#pragma once

#include <Arduino.h>
#include <Wire.h>

class I2CBusManager {
public:
    using ReinitFn = bool (*)(void*);

    static I2CBusManager& Get();

    void registerClient(const char* name, ReinitFn cb, void* ctx);
    bool ensureStarted(int sda, int scl, uint32_t hz);
    bool resetBus();

    TwoWire& wire() { return Wire; }
    int sda() const { return sda_; }
    int scl() const { return scl_; }
    uint32_t hz() const { return hz_; }
    bool started() const { return started_; }

private:
    I2CBusManager() = default;

    struct Client {
        char name[16] = {0};
        ReinitFn cb = nullptr;
        void* ctx = nullptr;
    };

    static constexpr size_t kMaxClients = 4;
    Client clients_[kMaxClients];
    size_t clientCount_ = 0;

    bool started_ = false;
    int sda_ = -1;
    int scl_ = -1;
    uint32_t hz_ = 0;

    bool beginBus_(int sda, int scl, uint32_t hz);
    void notifyReinit_();
};
