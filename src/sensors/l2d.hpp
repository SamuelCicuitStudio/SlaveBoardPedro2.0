/*
 * LIS2DHTR driver (ESP32 + Wire), compact API.
 */
#ifndef L2D_H
#define L2D_H

#include <Arduino.h>
#include <Wire.h>

#include "l2d_regs.h"
#include "l2d_types.h"

// Compact LIS2DHTR driver wrapper for Wire.
class L2D {
public:
    L2D() = default;

    // Bus init + basic chip check + default config.
    bool begin(TwoWire& w = Wire, uint8_t addr = L2D_ADDR0,
               int sda = -1, int scl = -1, uint32_t hz = 400000); // init bus + basic config
    bool beginOnBus(TwoWire& w = Wire, uint8_t addr = L2D_ADDR0,
                    uint32_t hz = 0); // use already-started bus

    // Basic identity/control helpers.
    uint8_t who(); // read WHO_AM_I
    bool reset(); // reset device registers
    bool cfg(l2d_odr_t odr, l2d_scale_t sc, bool hr = true); // quick config helper

    // Output mode and data rate control.
    bool mode(l2d_odr_t odr, l2d_res_t res, bool x, bool y, bool z); // set ODR/res/axes
    bool scale(l2d_scale_t sc); // set full-scale
    bool fifo(l2d_fifo_t mode, uint8_t ths, l2d_int_sig_t trig); // configure FIFO
    bool ready(); // data-ready check

    // Floating point samples (g).
    bool getF(l2d_flt_t* out); // read float sample
    uint8_t getFFifo(l2d_flt_fifo_t out); // read float FIFO

    // Raw samples (LSB).
    bool getR(l2d_raw_t* out); // read raw sample
    uint8_t getRFifo(l2d_raw_fifo_t out); // read raw FIFO

    // Interrupts, events, and click logic.
    bool intEn(l2d_int_t type, l2d_int_sig_t sig, bool on); // enable/disable interrupt
    bool intSrc(l2d_int_src_t* src); // read interrupt sources
    bool evtSet(l2d_evt_cfg_t* cfg, l2d_evt_gen_t gen); // set event config
    bool evtGet(l2d_evt_cfg_t* cfg, l2d_evt_gen_t gen); // get event config
    bool evtSrc(l2d_evt_src_t* src, l2d_evt_gen_t gen); // read event source
    bool clickSet(l2d_click_cfg_t* cfg); // set click config
    bool clickGet(l2d_click_cfg_t* cfg); // get click config
    bool clickSrc(l2d_click_src_t* src); // read click source

    // Signal polarity + filter control.
    bool intLevel(l2d_int_lvl_t lvl); // set interrupt polarity
    bool hpfCfg(l2d_hpf_t mode, uint8_t cut, bool data, bool click, bool int1, bool int2); // HPF config
    bool hpfSet(int8_t ref); // set HPF reference
    int8_t hpfGet(); // read HPF reference

    // ADC / temperature.
    bool adcEn(bool adc, bool temp); // enable ADC/temp
    bool adcGet(uint16_t* a1, uint16_t* a2, uint16_t* a3); // read ADC/temp

    // Quick axis read helper.
    bool axes(int16_t& x, int16_t& y, int16_t& z); // read XYZ axes

    // Low-level register I/O.
    bool rd(uint8_t reg, uint8_t& val); // read reg
    bool wr(uint8_t reg, uint8_t val); // write reg
    bool rds(uint8_t reg, uint8_t* buf, size_t len); // read regs
    bool wrs(uint8_t reg, const uint8_t* buf, size_t len); // write regs

    // Last error (ORed flags).
    int err() const { return err_; } // last error

private:
    // Bus state.
    TwoWire* w_ = nullptr;
    uint8_t addr_ = L2D_ADDR0;
    int err_ = L2D_OK;

    // Cached configuration.
    l2d_scale_t sc_ = L2D_SCALE_2G;
    l2d_res_t res_ = L2D_RES_H;
    l2d_fifo_t fifo_ = L2D_FM_BYPASS;
    bool fifo_first_ = true;

    // Internal helpers.
    bool ok_(); // check WHO_AM_I
    bool upd_(uint8_t reg, uint8_t mask, uint8_t val); // masked update
    bool initDevice_(); // common init sequence
};

#endif // L2D_H
