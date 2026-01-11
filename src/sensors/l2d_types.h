/*
 * LIS2DHTR driver types and enums.
 */
#ifndef L2D_TYPES_H
#define L2D_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define L2D_OK                   0   // no error
#define L2D_NOK                 -1   // generic failure

#define L2D_INT_ERR_MASK         0x000f  // low bits: bus errors
#define L2D_DRV_ERR_MASK         0xfff0  // high bits: driver errors

// I2C error codes (OR into err_)
#define L2D_ERR_I2C_RD           1   // read failed
#define L2D_ERR_I2C_WR           2   // write failed
#define L2D_ERR_I2C_BUSY         3   // bus busy

// Driver error codes (OR into err_)
#define L2D_ERR_WRONG_ID            ( 1 << 8)  // bad WHO_AM_I
#define L2D_ERR_WRONG_BW            ( 2 << 8)  // ODR not supported
#define L2D_ERR_RAW_RD              ( 3 << 8)  // raw read failed
#define L2D_ERR_RAW_FIFO_RD         ( 4 << 8)  // FIFO read failed
#define L2D_ERR_WRONG_INT           ( 5 << 8)  // bad int type
#define L2D_ERR_INT_CFG             ( 6 << 8)  // int config failed
#define L2D_ERR_INT_EN              ( 7 << 8)  // int enable failed
#define L2D_ERR_INT_SRC             ( 8 << 8)  // int source failed
#define L2D_ERR_HPF_CFG             ( 9 << 8)  // HPF config failed
#define L2D_ERR_HPF_EN              (10 << 8)  // HPF enable failed
#define L2D_ERR_CLICK_CFG           (11 << 8)  // click config failed
#define L2D_ERR_CLICK_SRC           (12 << 8)  // click source failed
#define L2D_ERR_ADC_RD              (13 << 8)  // ADC read failed
#define L2D_ERR_BYPASS              (14 << 8)  // not in bypass
#define L2D_ERR_FIFO                (15 << 8)  // not in FIFO
#define L2D_ERR_ODR_HIGH            (16 << 8)  // FIFO overrun

#define L2D_FIFO_MAX              32  // FIFO depth

typedef enum {
    L2D_ODR_PD = 0,  // power-down
    L2D_ODR_1,       // 1 Hz
    L2D_ODR_10,      // 10 Hz
    L2D_ODR_25,      // 25 Hz
    L2D_ODR_50,      // 50 Hz
    L2D_ODR_100,     // 100 Hz
    L2D_ODR_200,     // 200 Hz
    L2D_ODR_400,     // 400 Hz
    L2D_ODR_1600,    // 1.6 kHz (LP)
    L2D_ODR_5000     // 1.25 kHz (N) / 5 kHz (LP)
} l2d_odr_t;

typedef enum {
    L2D_RES_LP = 0,  // low power (8-bit)
    L2D_RES_N,       // normal (10-bit)
    L2D_RES_H        // high-res (12-bit)
} l2d_res_t;

typedef enum {
    L2D_SCALE_2G = 0,  // +/-2g
    L2D_SCALE_4G,      // +/-4g
    L2D_SCALE_8G,      // +/-8g
    L2D_SCALE_16G      // +/-16g
} l2d_scale_t;

typedef enum {
    L2D_FM_BYPASS = 0,  // bypass (no FIFO)
    L2D_FM_FIFO,        // FIFO mode
    L2D_FM_STREAM,      // stream mode
    L2D_FM_TRIG         // trigger mode
} l2d_fifo_t;

typedef enum {
    L2D_INT1 = 0,  // INT1 pin
    L2D_INT2       // INT2 pin
} l2d_int_sig_t;

typedef enum {
    L2D_EVT1 = 0,  // event generator 1
    L2D_EVT2       // event generator 2
} l2d_evt_gen_t;

typedef enum {
    L2D_INT_DRDY = 0,  // data ready
    L2D_INT_WTM,       // FIFO watermark
    L2D_INT_OVR,       // FIFO overrun
    L2D_INT_EVT1,      // event 1
    L2D_INT_EVT2,      // event 2
    L2D_INT_CLICK      // click/tap
} l2d_int_t;

typedef struct {
    bool drdy;  // data ready active
    bool wtm;   // watermark active
    bool ovr;   // overrun active
} l2d_int_src_t;

typedef enum {
    L2D_EVT_WAKE = 0,  // wake-up
    L2D_EVT_FF,        // free-fall
    L2D_EVT_6D_MOV,    // 6D movement
    L2D_EVT_6D_POS,    // 6D position
    L2D_EVT_4D_MOV,    // 4D movement
    L2D_EVT_4D_POS     // 4D position
} l2d_evt_mode_t;

typedef struct {
    l2d_evt_mode_t mode;  // event mode
    uint8_t ths;          // threshold
    bool xl;              // X low enable
    bool xh;              // X high enable
    bool yl;              // Y low enable
    bool yh;              // Y high enable
    bool zl;              // Z low enable
    bool zh;              // Z high enable
    bool latch;           // latch interrupt
    uint8_t dur;          // duration
} l2d_evt_cfg_t;

typedef struct {
    bool act;  // any event active
    bool xl;   // X low
    bool xh;   // X high
    bool yl;   // Y low
    bool yh;   // Y high
    bool zl;   // Z low
    bool zh;   // Z high
} l2d_evt_src_t;

typedef struct {
    bool xs;       // X single
    bool xd;       // X double
    bool ys;       // Y single
    bool yd;       // Y double
    bool zs;       // Z single
    bool zd;       // Z double
    uint8_t ths;   // click threshold
    bool latch;    // latch click
    uint8_t tl;    // time limit
    uint8_t lat;   // time latency
    uint8_t win;   // time window
} l2d_click_cfg_t;

typedef struct {
    bool x;     // X click
    bool y;     // Y click
    bool z;     // Z click
    bool sign;  // click sign
    bool sc;    // single click
    bool dc;    // double click
    bool act;   // click active
} l2d_click_src_t;

typedef enum {
    L2D_INT_HIGH = 0,  // active high
    L2D_INT_LOW         // active low
} l2d_int_lvl_t;

typedef struct {
    int16_t x;  // raw X
    int16_t y;  // raw Y
    int16_t z;  // raw Z
} l2d_raw_t;

typedef l2d_raw_t l2d_raw_fifo_t[L2D_FIFO_MAX];

typedef struct {
    float x;  // g on X
    float y;  // g on Y
    float z;  // g on Z
} l2d_flt_t;

typedef l2d_flt_t l2d_flt_fifo_t[L2D_FIFO_MAX];

typedef enum {
    L2D_HPF_NORM = 0,  // normal (reset by ref read)
    L2D_HPF_REF,       // reference mode
    L2D_HPF_NORM2,     // normal (alt)
    L2D_HPF_AUTO       // autoreset on interrupt
} l2d_hpf_t;

#endif // L2D_TYPES_H
