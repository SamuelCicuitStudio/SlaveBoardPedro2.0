/*
 * LIS2DHTR driver (ESP32 + Wire), compact implementation.
 */
#include "l2d.hpp"

static const float k_sc[] = { 0.001f, 0.002f, 0.004f, 0.012f };

static inline int16_t comb(uint8_t lo, uint8_t hi) {
    return (int16_t)((uint16_t)hi << 8 | lo);
}

static uint8_t sc_to_fs(l2d_scale_t sc) {
    switch (sc) {
        case L2D_SCALE_2G:  return 0x00;
        case L2D_SCALE_4G:  return 0x10;
        case L2D_SCALE_8G:  return 0x20;
        case L2D_SCALE_16G: return 0x30;
        default: return 0x00;
    }
}

bool L2D::begin(TwoWire& w, uint8_t addr, int sda, int scl, uint32_t hz) {
    w_ = &w;
    addr_ = addr;
    err_ = L2D_OK;

    if (sda >= 0 && scl >= 0) {
        w_->begin(sda, scl);
    } else {
        w_->begin();
    }

    if (hz > 0) {
        w_->setClock(hz);
    }

    return initDevice_();
}

bool L2D::beginOnBus(TwoWire& w, uint8_t addr, uint32_t hz) {
    w_ = &w;
    addr_ = addr;
    err_ = L2D_OK;

    if (hz > 0) {
        w_->setClock(hz);
    }

    return initDevice_();
}

bool L2D::initDevice_() {
    if (!ok_()) {
        return false;
    }

    if (!reset()) {
        return false;
    }

    sc_ = L2D_SCALE_2G;
    res_ = L2D_RES_H;
    fifo_ = L2D_FM_BYPASS;
    fifo_first_ = true;

    if (!scale(sc_)) {
        return false;
    }

    if (!mode(L2D_ODR_100, res_, true, true, true)) {
        return false;
    }

    if (!upd_(L2D_REG_CTRL4, L2D_CTRL4_BDU, L2D_CTRL4_BDU)) {
        return false;
    }

    return true;
}

uint8_t L2D::who() {
    uint8_t v = 0;
    if (!rd(L2D_REG_WHOAMI, v)) {
        return 0;
    }
    return v;
}

bool L2D::reset() {
    err_ = L2D_OK;

    uint8_t z[8] = {0};
    if (!wrs(L2D_REG_TEMP_CFG, z, 8)) return false;
    if (!wr(L2D_REG_FIFO_CTRL, 0)) return false;
    if (!wr(L2D_REG_INT1_CFG, 0)) return false;
    if (!wrs(L2D_REG_INT1_THS, z, 2)) return false;
    if (!wr(L2D_REG_INT2_CFG, 0)) return false;
    if (!wrs(L2D_REG_INT2_THS, z, 2)) return false;
    if (!wr(L2D_REG_CLICK_CFG, 0)) return false;
    if (!wrs(L2D_REG_CLICK_THS, z, 4)) return false;

    return true;
}

bool L2D::cfg(l2d_odr_t odr, l2d_scale_t sc, bool hr) {
    l2d_res_t res = hr ? L2D_RES_H : L2D_RES_N;
    if (!mode(odr, res, true, true, true)) {
        return false;
    }
    return scale(sc);
}

bool L2D::mode(l2d_odr_t odr, l2d_res_t res, bool x, bool y, bool z) {
    if (!w_) return false;

    err_ = L2D_OK;
    res_ = res;

    uint8_t c1 = 0;
    if (!rd(L2D_REG_CTRL1, c1)) return false;

    uint8_t old_odr = (c1 & L2D_CTRL1_ODR_MASK) >> 4;

    c1 &= ~(L2D_CTRL1_ODR_MASK | L2D_CTRL1_XEN | L2D_CTRL1_YEN | L2D_CTRL1_ZEN | L2D_CTRL1_LPEN);
    c1 |= ((uint8_t)odr << 4) & L2D_CTRL1_ODR_MASK;
    if (x) c1 |= L2D_CTRL1_XEN;
    if (y) c1 |= L2D_CTRL1_YEN;
    if (z) c1 |= L2D_CTRL1_ZEN;
    if (res == L2D_RES_LP) c1 |= L2D_CTRL1_LPEN;

    if (!wr(L2D_REG_CTRL1, c1)) return false;

    uint8_t hr = (res == L2D_RES_H) ? L2D_CTRL4_HR : 0;
    if (!upd_(L2D_REG_CTRL4, L2D_CTRL4_HR, hr)) return false;

    if (old_odr == L2D_ODR_PD && odr != L2D_ODR_PD) {
        delay(15);
    }

    return true;
}

bool L2D::scale(l2d_scale_t sc) {
    if (!w_) return false;

    err_ = L2D_OK;
    sc_ = sc;

    uint8_t fs = sc_to_fs(sc);
    return upd_(L2D_REG_CTRL4, L2D_CTRL4_FS_MASK, fs);
}

bool L2D::fifo(l2d_fifo_t mode, uint8_t ths, l2d_int_sig_t trig) {
    if (!w_) return false;

    err_ = L2D_OK;
    fifo_ = mode;
    fifo_first_ = true;

    uint8_t en = (mode != L2D_FM_BYPASS) ? L2D_CTRL5_FIFO_EN : 0;
    if (!upd_(L2D_REG_CTRL5, L2D_CTRL5_FIFO_EN, en)) return false;

    uint8_t fc = 0;
    fc |= (ths & L2D_FIFO_WTM_MASK);
    if (trig == L2D_INT2) {
        fc |= L2D_FIFO_TRIG_INT2;
    }
    fc |= ((uint8_t)mode << 6) & L2D_FIFO_MODE_MASK;

    return wr(L2D_REG_FIFO_CTRL, fc);
}

bool L2D::ready() {
    if (!w_) return false;

    err_ = L2D_OK;

    if (fifo_ == L2D_FM_BYPASS) {
        uint8_t st = 0;
        if (!rd(L2D_REG_STATUS, st)) return false;
        return (st & L2D_STATUS_ZYXDA) != 0;
    }

    uint8_t fs = 0;
    if (!rd(L2D_REG_FIFO_SRC, fs)) return false;
    return (fs & L2D_FIFO_SRC_EMPTY) == 0;
}

bool L2D::getF(l2d_flt_t* out) {
    if (!out) return false;

    l2d_raw_t raw;
    if (!getR(&raw)) return false;

    out->x = k_sc[sc_] * (raw.x >> 4);
    out->y = k_sc[sc_] * (raw.y >> 4);
    out->z = k_sc[sc_] * (raw.z >> 4);
    return true;
}

uint8_t L2D::getFFifo(l2d_flt_fifo_t out) {
    if (!out) return 0;

    l2d_raw_fifo_t raw;
    uint8_t n = getRFifo(raw);

    for (uint8_t i = 0; i < n; ++i) {
        out[i].x = k_sc[sc_] * (raw[i].x >> 4);
        out[i].y = k_sc[sc_] * (raw[i].y >> 4);
        out[i].z = k_sc[sc_] * (raw[i].z >> 4);
    }

    return n;
}

bool L2D::getR(l2d_raw_t* out) {
    if (!out) return false;

    err_ = L2D_OK;
    if (fifo_ != L2D_FM_BYPASS) {
        err_ = L2D_ERR_BYPASS;
        return false;
    }

    uint8_t buf[6] = {0};
    if (!rds(L2D_REG_OUT_X_L, buf, 6)) {
        err_ |= L2D_ERR_RAW_RD;
        return false;
    }

    out->x = comb(buf[0], buf[1]);
    out->y = comb(buf[2], buf[3]);
    out->z = comb(buf[4], buf[5]);
    return true;
}

uint8_t L2D::getRFifo(l2d_raw_fifo_t out) {
    err_ = L2D_OK;

    if (fifo_ == L2D_FM_BYPASS) {
        return getR(out) ? 1 : 0;
    }

    uint8_t fs = 0;
    if (!rd(L2D_REG_FIFO_SRC, fs)) return 0;
    if (fs & L2D_FIFO_SRC_EMPTY) return 0;

    uint8_t n = fs & L2D_FIFO_SRC_FSS_MASK;
    if (fs & L2D_FIFO_SRC_OVRN) n += 1;

    for (uint8_t i = 0; i < n; ++i) {
        uint8_t buf[6] = {0};
        if (!rds(L2D_REG_OUT_X_L, buf, 6)) {
            err_ |= L2D_ERR_RAW_FIFO_RD;
            return i;
        }
        out[i].x = comb(buf[0], buf[1]);
        out[i].y = comb(buf[2], buf[3]);
        out[i].z = comb(buf[4], buf[5]);
    }

    if (!rd(L2D_REG_FIFO_SRC, fs)) return n;

    if (fs & L2D_FIFO_SRC_FSS_MASK) {
        err_ = L2D_ERR_ODR_HIGH;
        return 0;
    }

    if (fifo_ == L2D_FM_FIFO && n == L2D_FIFO_MAX) {
        upd_(L2D_REG_FIFO_CTRL, L2D_FIFO_MODE_MASK, L2D_FIFO_BYPASS);
        upd_(L2D_REG_FIFO_CTRL, L2D_FIFO_MODE_MASK, L2D_FIFO_FIFO);
    }

    return n;
}

bool L2D::intEn(l2d_int_t type, l2d_int_sig_t sig, bool on) {
    err_ = L2D_OK;

    uint8_t r = 0;
    uint8_t a = 0;

    if (type == L2D_INT_DRDY || type == L2D_INT_WTM || type == L2D_INT_OVR) {
        a = L2D_REG_CTRL3;
        if (!rd(a, r)) { err_ |= L2D_ERR_INT_EN; return false; }
    } else if (sig == L2D_INT1) {
        a = L2D_REG_CTRL3;
        if (!rd(a, r)) { err_ |= L2D_ERR_INT_EN; return false; }
    } else {
        a = L2D_REG_CTRL6;
        if (!rd(a, r)) { err_ |= L2D_ERR_INT_EN; return false; }
    }

    switch (type) {
        case L2D_INT_DRDY:
            r = on ? (r | L2D_CTRL3_I1_DRDY1) : (r & ~L2D_CTRL3_I1_DRDY1);
            break;
        case L2D_INT_WTM:
            r = on ? (r | L2D_CTRL3_I1_WTM) : (r & ~L2D_CTRL3_I1_WTM);
            break;
        case L2D_INT_OVR:
            r = on ? (r | L2D_CTRL3_I1_OVERRUN) : (r & ~L2D_CTRL3_I1_OVERRUN);
            break;
        case L2D_INT_EVT1:
            if (sig == L2D_INT1) r = on ? (r | L2D_CTRL3_I1_AOI1) : (r & ~L2D_CTRL3_I1_AOI1);
            else r = on ? (r | L2D_CTRL6_I2_AOI1) : (r & ~L2D_CTRL6_I2_AOI1);
            break;
        case L2D_INT_EVT2:
            if (sig == L2D_INT1) r = on ? (r | L2D_CTRL3_I1_AOI2) : (r & ~L2D_CTRL3_I1_AOI2);
            else r = on ? (r | L2D_CTRL6_I2_AOI2) : (r & ~L2D_CTRL6_I2_AOI2);
            break;
        case L2D_INT_CLICK:
            if (sig == L2D_INT1) r = on ? (r | L2D_CTRL3_I1_CLICK) : (r & ~L2D_CTRL3_I1_CLICK);
            else r = on ? (r | L2D_CTRL6_I2_CLICK) : (r & ~L2D_CTRL6_I2_CLICK);
            break;
        default:
            err_ = L2D_ERR_WRONG_INT;
            return false;
    }

    if (!wr(a, r)) {
        err_ |= L2D_ERR_INT_EN;
        return false;
    }

    return true;
}

bool L2D::intSrc(l2d_int_src_t* src) {
    if (!src) return false;
    err_ = L2D_OK;

    uint8_t c3 = 0;
    uint8_t st = 0;
    uint8_t fs = 0;

    if (!rd(L2D_REG_CTRL3, c3) ||
        !rd(L2D_REG_STATUS, st) ||
        !rd(L2D_REG_FIFO_SRC, fs)) {
        err_ |= L2D_ERR_INT_SRC;
        return false;
    }

    src->drdy = (st & L2D_STATUS_ZYXDA) && (c3 & L2D_CTRL3_I1_DRDY1);
    src->wtm  = (fs & L2D_FIFO_SRC_WTM) && (c3 & L2D_CTRL3_I1_WTM);
    src->ovr  = (fs & L2D_FIFO_SRC_OVRN) && (c3 & L2D_CTRL3_I1_OVERRUN);
    return true;
}

bool L2D::evtSet(l2d_evt_cfg_t* cfg, l2d_evt_gen_t gen) {
    if (!cfg) return false;
    err_ = L2D_OK;

    uint8_t ic = 0;
    if (cfg->xl) ic |= L2D_INT_CFG_XL;
    if (cfg->xh) ic |= L2D_INT_CFG_XH;
    if (cfg->yl) ic |= L2D_INT_CFG_YL;
    if (cfg->yh) ic |= L2D_INT_CFG_YH;
    if (cfg->zl) ic |= L2D_INT_CFG_ZL;
    if (cfg->zh) ic |= L2D_INT_CFG_ZH;

    bool d4d = false;
    switch (cfg->mode) {
        case L2D_EVT_WAKE:    ic &= ~L2D_INT_CFG_AOI; ic &= ~L2D_INT_CFG_6D; break;
        case L2D_EVT_FF:      ic |=  L2D_INT_CFG_AOI; ic &= ~L2D_INT_CFG_6D; break;
        case L2D_EVT_4D_MOV:  d4d = true;
        case L2D_EVT_6D_MOV:  ic &= ~L2D_INT_CFG_AOI; ic |=  L2D_INT_CFG_6D; break;
        case L2D_EVT_4D_POS:  d4d = true;
        case L2D_EVT_6D_POS:  ic |=  L2D_INT_CFG_AOI; ic |=  L2D_INT_CFG_6D; break;
        default: break;
    }

    uint8_t cfg_a = (gen == L2D_EVT1) ? L2D_REG_INT1_CFG : L2D_REG_INT2_CFG;
    uint8_t ths_a = (gen == L2D_EVT1) ? L2D_REG_INT1_THS : L2D_REG_INT2_THS;
    uint8_t dur_a = (gen == L2D_EVT1) ? L2D_REG_INT1_DUR : L2D_REG_INT2_DUR;

    if (!wr(ths_a, cfg->ths) || !wr(dur_a, cfg->dur) || !wr(cfg_a, ic)) {
        err_ |= L2D_ERR_INT_CFG;
        return false;
    }

    if (gen == L2D_EVT1) {
        upd_(L2D_REG_CTRL5, L2D_CTRL5_LIR_INT1, cfg->latch ? L2D_CTRL5_LIR_INT1 : 0);
        upd_(L2D_REG_CTRL5, L2D_CTRL5_D4D_INT1, d4d ? L2D_CTRL5_D4D_INT1 : 0);
    } else {
        upd_(L2D_REG_CTRL5, L2D_CTRL5_LIR_INT2, cfg->latch ? L2D_CTRL5_LIR_INT2 : 0);
        upd_(L2D_REG_CTRL5, L2D_CTRL5_D4D_INT2, d4d ? L2D_CTRL5_D4D_INT2 : 0);
    }

    return true;
}

bool L2D::evtGet(l2d_evt_cfg_t* cfg, l2d_evt_gen_t gen) {
    if (!cfg) return false;
    err_ = L2D_OK;

    uint8_t cfg_a = (gen == L2D_EVT1) ? L2D_REG_INT1_CFG : L2D_REG_INT2_CFG;
    uint8_t ths_a = (gen == L2D_EVT1) ? L2D_REG_INT1_THS : L2D_REG_INT2_THS;
    uint8_t dur_a = (gen == L2D_EVT1) ? L2D_REG_INT1_DUR : L2D_REG_INT2_DUR;

    uint8_t ic = 0;
    uint8_t c5 = 0;

    if (!rd(cfg_a, ic) || !rd(ths_a, cfg->ths) || !rd(dur_a, cfg->dur) || !rd(L2D_REG_CTRL5, c5)) {
        err_ |= L2D_ERR_INT_CFG;
        return false;
    }

    cfg->xl = (ic & L2D_INT_CFG_XL) != 0;
    cfg->xh = (ic & L2D_INT_CFG_XH) != 0;
    cfg->yl = (ic & L2D_INT_CFG_YL) != 0;
    cfg->yh = (ic & L2D_INT_CFG_YH) != 0;
    cfg->zl = (ic & L2D_INT_CFG_ZL) != 0;
    cfg->zh = (ic & L2D_INT_CFG_ZH) != 0;

    bool d4d = false;
    if (gen == L2D_EVT1) {
        cfg->latch = (c5 & L2D_CTRL5_LIR_INT1) != 0;
        d4d = (c5 & L2D_CTRL5_D4D_INT1) != 0;
    } else {
        cfg->latch = (c5 & L2D_CTRL5_LIR_INT2) != 0;
        d4d = (c5 & L2D_CTRL5_D4D_INT2) != 0;
    }

    if (ic & L2D_INT_CFG_AOI) {
        if ((ic & L2D_INT_CFG_6D) && d4d) cfg->mode = L2D_EVT_4D_POS;
        else if ((ic & L2D_INT_CFG_6D) && !d4d) cfg->mode = L2D_EVT_6D_POS;
        else cfg->mode = L2D_EVT_FF;
    } else {
        if ((ic & L2D_INT_CFG_6D) && d4d) cfg->mode = L2D_EVT_4D_MOV;
        else if ((ic & L2D_INT_CFG_6D) && !d4d) cfg->mode = L2D_EVT_6D_MOV;
        else cfg->mode = L2D_EVT_WAKE;
    }

    return true;
}

bool L2D::evtSrc(l2d_evt_src_t* src, l2d_evt_gen_t gen) {
    if (!src) return false;
    err_ = L2D_OK;

    uint8_t cfg_a = (gen == L2D_EVT1) ? L2D_REG_INT1_CFG : L2D_REG_INT2_CFG;
    uint8_t src_a = (gen == L2D_EVT1) ? L2D_REG_INT1_SRC : L2D_REG_INT2_SRC;

    uint8_t ic = 0;
    uint8_t is = 0;

    if (!rd(src_a, is) || !rd(cfg_a, ic)) {
        err_ |= L2D_ERR_INT_SRC;
        return false;
    }

    src->act = (is & L2D_INT_SRC_IA) != 0;
    src->xl  = (is & L2D_INT_SRC_XL) && (ic & L2D_INT_CFG_XL);
    src->xh  = (is & L2D_INT_SRC_XH) && (ic & L2D_INT_CFG_XH);
    src->yl  = (is & L2D_INT_SRC_YL) && (ic & L2D_INT_CFG_YL);
    src->yh  = (is & L2D_INT_SRC_YH) && (ic & L2D_INT_CFG_YH);
    src->zl  = (is & L2D_INT_SRC_ZL) && (ic & L2D_INT_CFG_ZL);
    src->zh  = (is & L2D_INT_SRC_ZH) && (ic & L2D_INT_CFG_ZH);
    return true;
}

bool L2D::clickSet(l2d_click_cfg_t* cfg) {
    if (!cfg) return false;
    err_ = L2D_OK;

    uint8_t cc = 0;
    if (cfg->xs) cc |= L2D_CLICK_CFG_XS;
    if (cfg->xd) cc |= L2D_CLICK_CFG_XD;
    if (cfg->ys) cc |= L2D_CLICK_CFG_YS;
    if (cfg->yd) cc |= L2D_CLICK_CFG_YD;
    if (cfg->zs) cc |= L2D_CLICK_CFG_ZS;
    if (cfg->zd) cc |= L2D_CLICK_CFG_ZD;

    uint8_t th = cfg->ths | (cfg->latch ? 0x80 : 0x00);

    if (!wr(L2D_REG_CLICK_CFG, cc) ||
        !wr(L2D_REG_CLICK_THS, th) ||
        !wr(L2D_REG_TIME_LIMIT, cfg->tl) ||
        !wr(L2D_REG_TIME_LATENCY, cfg->lat) ||
        !wr(L2D_REG_TIME_WINDOW, cfg->win)) {
        err_ |= L2D_ERR_CLICK_CFG;
        return false;
    }

    return true;
}

bool L2D::clickGet(l2d_click_cfg_t* cfg) {
    if (!cfg) return false;
    err_ = L2D_OK;

    uint8_t cc = 0;
    uint8_t th = 0;

    if (!rd(L2D_REG_CLICK_CFG, cc) ||
        !rd(L2D_REG_CLICK_THS, th) ||
        !rd(L2D_REG_TIME_LIMIT, cfg->tl) ||
        !rd(L2D_REG_TIME_LATENCY, cfg->lat) ||
        !rd(L2D_REG_TIME_WINDOW, cfg->win)) {
        err_ |= L2D_ERR_CLICK_CFG;
        return false;
    }

    cfg->xs = (cc & L2D_CLICK_CFG_XS) != 0;
    cfg->xd = (cc & L2D_CLICK_CFG_XD) != 0;
    cfg->ys = (cc & L2D_CLICK_CFG_YS) != 0;
    cfg->yd = (cc & L2D_CLICK_CFG_YD) != 0;
    cfg->zs = (cc & L2D_CLICK_CFG_ZS) != 0;
    cfg->zd = (cc & L2D_CLICK_CFG_ZD) != 0;
    cfg->ths = th & 0x7F;
    cfg->latch = (th & 0x80) != 0;

    return true;
}

bool L2D::clickSrc(l2d_click_src_t* src) {
    if (!src) return false;
    err_ = L2D_OK;

    uint8_t s = 0;
    if (!rd(L2D_REG_CLICK_SRC, s)) {
        err_ |= L2D_ERR_CLICK_SRC;
        return false;
    }

    src->x = (s & L2D_CLICK_SRC_X) != 0;
    src->y = (s & L2D_CLICK_SRC_Y) != 0;
    src->z = (s & L2D_CLICK_SRC_Z) != 0;
    src->sign = (s & L2D_CLICK_SRC_SIGN) != 0;
    src->sc = (s & L2D_CLICK_SRC_SCLICK) != 0;
    src->dc = (s & L2D_CLICK_SRC_DCLICK) != 0;
    src->act = (s & L2D_CLICK_SRC_IA) != 0;
    return true;
}

bool L2D::intLevel(l2d_int_lvl_t lvl) {
    err_ = L2D_OK;
    uint8_t v = (lvl == L2D_INT_LOW) ? L2D_CTRL6_H_LACTIVE : 0;
    return upd_(L2D_REG_CTRL6, L2D_CTRL6_H_LACTIVE, v);
}

bool L2D::hpfCfg(l2d_hpf_t mode, uint8_t cut, bool data, bool click, bool int1, bool int2) {
    err_ = L2D_OK;

    uint8_t r = 0;
    r |= ((uint8_t)mode << 6) & L2D_CTRL2_HPM_MASK;
    r |= (cut << 4) & L2D_CTRL2_HPCF_MASK;
    if (data)  r |= L2D_CTRL2_FDS;
    if (click) r |= L2D_CTRL2_HPCLICK;
    if (int1)  r |= L2D_CTRL2_HPIS1;
    if (int2)  r |= L2D_CTRL2_HPIS2;

    if (!wr(L2D_REG_CTRL2, r)) {
        err_ |= L2D_ERR_HPF_CFG;
        return false;
    }

    return true;
}

bool L2D::hpfSet(int8_t ref) {
    err_ = L2D_OK;
    if (!wr(L2D_REG_REFERENCE, (uint8_t)ref)) {
        err_ |= L2D_ERR_HPF_CFG;
        return false;
    }
    return true;
}

int8_t L2D::hpfGet() {
    err_ = L2D_OK;
    uint8_t ref = 0;
    if (!rd(L2D_REG_REFERENCE, ref)) {
        err_ |= L2D_ERR_HPF_CFG;
        return 0;
    }
    return (int8_t)ref;
}

bool L2D::adcEn(bool adc, bool temp) {
    err_ = L2D_OK;
    const bool en = adc || temp;
    return wr(L2D_REG_TEMP_CFG, en ? L2D_TEMP_EN : 0);
}

bool L2D::adcGet(uint16_t* a1, uint16_t* a2, uint16_t* a3) {
    err_ = L2D_OK;

    uint8_t tc = 0;
    if (!rd(L2D_REG_TEMP_CFG, tc)) {
        err_ |= L2D_ERR_ADC_RD;
        return false;
    }

    if ((tc & L2D_TEMP_EN_MASK) != L2D_TEMP_EN) {
        err_ |= L2D_ERR_ADC_RD;
        return false;
    }

    uint8_t t[2] = {0};
    if (!rds(L2D_REG_OUT_TEMP_L, t, 2)) {
        err_ |= L2D_ERR_ADC_RD;
        return false;
    }

    if (a1) *a1 = 0;
    if (a2) *a2 = 0;
    if (a3) *a3 = (uint16_t)comb(t[0], t[1]); // raw temperature register value
    return true;
}

bool L2D::axes(int16_t& x, int16_t& y, int16_t& z) {
    uint8_t buf[6] = {0};
    if (!rds(L2D_REG_OUT_X_L, buf, 6)) return false;
    x = comb(buf[0], buf[1]);
    y = comb(buf[2], buf[3]);
    z = comb(buf[4], buf[5]);
    return true;
}

bool L2D::rd(uint8_t reg, uint8_t& val) {
    if (!w_) return false;

    w_->beginTransmission(addr_);
    w_->write(reg);
    if (w_->endTransmission(false) != 0) {
        err_ |= L2D_ERR_I2C_RD;
        return false;
    }

    if (w_->requestFrom(addr_, (size_t)1) != 1) {
        err_ |= L2D_ERR_I2C_RD;
        return false;
    }

    val = w_->read();
    return true;
}

bool L2D::wr(uint8_t reg, uint8_t val) {
    if (!w_) return false;

    w_->beginTransmission(addr_);
    w_->write(reg);
    w_->write(val);
    if (w_->endTransmission() != 0) {
        err_ |= L2D_ERR_I2C_WR;
        return false;
    }
    return true;
}

bool L2D::rds(uint8_t reg, uint8_t* buf, size_t len) {
    if (!w_ || !buf || len == 0) return false;

    uint8_t r = reg;
    if (len > 1) r |= L2D_REG_INC;

    w_->beginTransmission(addr_);
    w_->write(r);
    if (w_->endTransmission(false) != 0) {
        err_ |= L2D_ERR_I2C_RD;
        return false;
    }

    size_t cnt = w_->requestFrom(addr_, len);
    if (cnt != len) {
        err_ |= L2D_ERR_I2C_RD;
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        buf[i] = w_->read();
    }

    return true;
}

bool L2D::wrs(uint8_t reg, const uint8_t* buf, size_t len) {
    if (!w_ || !buf || len == 0) return false;

    uint8_t r = reg;
    if (len > 1) r |= L2D_REG_INC;

    w_->beginTransmission(addr_);
    w_->write(r);
    w_->write(buf, len);
    if (w_->endTransmission() != 0) {
        err_ |= L2D_ERR_I2C_WR;
        return false;
    }

    return true;
}

bool L2D::ok_() {
    uint8_t id = 0;
    if (!rd(L2D_REG_WHOAMI, id)) return false;
    if (id != L2D_CHIP_ID) {
        err_ = L2D_ERR_WRONG_ID;
        return false;
    }
    return true;
}

bool L2D::upd_(uint8_t reg, uint8_t mask, uint8_t val) {
    uint8_t cur = 0;
    if (!rd(reg, cur)) return false;
    cur = (cur & ~mask) | (val & mask);
    return wr(reg, cur);
}
