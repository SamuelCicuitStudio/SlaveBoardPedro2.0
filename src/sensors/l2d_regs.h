/*
 * LIS2DH/LIS2DHTR register map and bit definitions (I2C/SPI accelerometer).
 *
 * Datasheet reference used for these comments:
 * - C155670.pdf (Doc ID 022516 Rev 1), "Register mapping" and "Registers Description"
 *
 * Notes from the datasheet:
 * - Registers marked Reserved must NOT be written (can damage device / break calibration).
 * - Some configuration registers are loaded at boot; writes are intended after boot completes.
 * - Multi-byte register access: set bit7 of the register address (auto-increment).
 */
#ifndef L2D_REGS_H
#define L2D_REGS_H

// -----------------------------------------------------------------------------
// I2C addressing / identity
// -----------------------------------------------------------------------------

// I2C addresses (7-bit). SA0/SDO selects the LSB of the address.
#define L2D_ADDR0              0x18  // SA0=0
#define L2D_ADDR1              0x19  // SA0=1

// WHO_AM_I (0x0F): device identification register. Expected value: 0x33.
#define L2D_REG_WHOAMI          0x0F
#define L2D_CHIP_ID             0x33

// Auto-increment flag for multi-byte reads/writes: OR this with the start register.
#define L2D_REG_INC             0x80

// -----------------------------------------------------------------------------
// Register addresses
// -----------------------------------------------------------------------------

// STATUS_REG_AUX (0x07): auxiliary status (temperature flags).
// Bit7: reserved
// Bit6: TOR  (Temperature Data Overrun) 0=no overrun, 1=new temp overwrote previous
// Bit5: reserved
// Bit4: reserved
// Bit3: reserved
// Bit2: TDA  (Temperature Data Available) 0=not available, 1=new temperature data available
// Bit1: reserved
// Bit0: reserved
#define L2D_REG_STATUS_AUX      0x07
#define L2D_STAUX_TOR           0x40  // bit6
#define L2D_STAUX_TDA           0x04  // bit2

// 0x08..0x0B: Reserved (do not write).

// OUT_TEMP_L (0x0C), OUT_TEMP_H (0x0D): temperature sensor output registers.
// - Temperature output must be enabled via TEMP_CFG_REG (0x1F) before use.
#define L2D_REG_OUT_TEMP_L      0x0C
#define L2D_REG_OUT_TEMP_H      0x0D

// INT_COUNTER_REG (0x0E): interrupt counter (8-bit counter IC7..IC0).
#define L2D_REG_INT_COUNTER     0x0E

// TEMP_CFG_REG (0x1F): temperature sensor enable.
// Bits[7:6] TEMP_EN[1:0]:
// - 00: temperature sensor disabled
// - 11: temperature sensor enabled
// Other values are not used in this datasheet.
#define L2D_REG_TEMP_CFG        0x1F
#define L2D_TEMP_EN1            0x80  // bit7
#define L2D_TEMP_EN0            0x40  // bit6
#define L2D_TEMP_EN_MASK        0xC0  // bits[7:6]

// -----------------------------------------------------------------------------
// Control registers
// -----------------------------------------------------------------------------

// CTRL_REG1 (0x20): data rate / power mode and axis enable.
// Bits[7:4] ODR[3:0]  Data rate selection:
//   0000: power-down
//   0001: 1 Hz
//   0010: 10 Hz
//   0011: 25 Hz
//   0100: 50 Hz
//   0101: 100 Hz
//   0110: 200 Hz
//   0111: 400 Hz
//   1000: low-power 1.620 kHz
//   1001: HR/normal 1.344 kHz; low-power 5.376 kHz
// Bit3 LPen: low power enable (0=normal, 1=low power)
// Bit2 Zen : Z axis enable
// Bit1 Yen : Y axis enable
// Bit0 Xen : X axis enable
#define L2D_REG_CTRL1           0x20

// CTRL_REG2 (0x21): high-pass filter configuration.
// Bits[7:6] HPM[1:0] High-pass filter mode:
//   00: normal (reset by reading REFERENCE (0x26))
//   01: reference signal for filtering
//   10: normal (alternative)
//   11: autoreset on interrupt event
// Bits[5:4] HPCF[1:0] High-pass cut-off (depends on ODR)
// Bit3 FDS     Filtered data selection (0=bypass, 1=filtered data to output/FIFO)
// Bit2 HPCLICK High-pass filter for CLICK function (0=bypass, 1=enable)
// Bit1 HPIS2   High-pass filter for AOI on INT2 (0=bypass, 1=enable)
// Bit0 HPIS1   High-pass filter for AOI on INT1 (0=bypass, 1=enable)
#define L2D_REG_CTRL2           0x21

// CTRL_REG3 (0x22): interrupt routing to INT1 pin.
// Bit7 I1_CLICK   CLICK interrupt on INT1 pin enable
// Bit6 I1_AOI1    AOI1 interrupt on INT1 pin enable
// Bit5 I1_AOI2    AOI2 interrupt on INT1 pin enable
// Bit4 I1_DRDY1   DRDY1 interrupt on INT1 pin enable
// Bit3 I1_DRDY2   DRDY2 interrupt on INT1 pin enable
// Bit2 I1_WTM     FIFO watermark interrupt on INT1 pin enable
// Bit1 I1_OVERRUN FIFO overrun interrupt on INT1 pin enable
// Bit0 reserved
#define L2D_REG_CTRL3           0x22

// CTRL_REG4 (0x23): data format / full-scale / self-test / SPI mode.
// Bit7 BDU  Block data update (0=continuous, 1=update only after LSB+MSB read)
// Bit6 BLE  Endianness (0=LSB at lower address, 1=MSB at lower address)
//          Note: datasheet states BLE is valid only in High Resolution mode.
// Bits[5:4] FS[1:0] Full scale:
//   00: +/-2g, 01: +/-4g, 10: +/-8g, 11: +/-16g
// Bit3 HR   Operating mode selection (see datasheet mode section)
// Bit2 ST1  Self test selection
// Bit1 ST0  Self test selection
// Bit0 SIM  SPI mode (0=4-wire, 1=3-wire)
#define L2D_REG_CTRL4           0x23

// CTRL_REG5 (0x24): reboot, FIFO enable, latch + 4D enable control.
// Bit7 BOOT       reboot memory content (write 1 to reboot)
// Bit6 FIFO_EN    FIFO enable
// Bit5 reserved
// Bit4 reserved
// Bit3 LIR_INT1   latch INT1_SRC (cleared by reading INT1_SRC)
// Bit2 D4D_INT1   enable 4D on INT1 when INT1_CFG.6D=1
// Bit1 LIR_INT2   latch INT2_SRC (cleared by reading INT2_SRC)
// Bit0 D4D_INT2   enable 4D on INT2 when INT2_CFG.6D=1
#define L2D_REG_CTRL5           0x24

// CTRL_REG6 (0x25): interrupt routing to INT2 pin + polarity.
// Bit7 I2_CLICKen click interrupt on INT2 pin enable
// Bit6 I2_INT1    interrupt generator 1 routed to INT2 pin
// Bit5 I2_INT2    interrupt generator 2 routed to INT2 pin
// Bit4 BOOT_I2    boot status routed to INT2 pin
// Bit3 P2_ACT     activity interrupt routed to INT2 pin
// Bit2 reserved
// Bit1 H_LACTIVE  interrupt polarity (0=active high, 1=active low)
// Bit0 reserved
#define L2D_REG_CTRL6           0x25

// REFERENCE / DATACAPTURE (0x26): reference value used by HPF and interrupt logic.
// Bits[7:0] Ref[7:0]: reference value for interrupt generation.
#define L2D_REG_REFERENCE       0x26

// STATUS_REG (0x27): data-ready and overrun flags for X/Y/Z.
// Bit7 ZYXOR: XYZ overrun
// Bit6 ZOR  : Z overrun
// Bit5 YOR  : Y overrun
// Bit4 XOR  : X overrun
// Bit3 ZYXDA: XYZ new data available
// Bit2 ZDA  : Z new data available
// Bit1 YDA  : Y new data available
// Bit0 XDA  : X new data available
#define L2D_REG_STATUS          0x27

// Output registers
// OUT_X_L/H (0x28/0x29): X-axis acceleration output (two’s complement, left-justified).
// OUT_Y_L/H (0x2A/0x2B): Y-axis acceleration output (two’s complement, left-justified).
// OUT_Z_L/H (0x2C/0x2D): Z-axis acceleration output (two’s complement, left-justified).
// The effective resolution depends on LP/normal/HR mode (see CTRL_REG1/CTRL_REG4).
#define L2D_REG_OUT_X_L          0x28
#define L2D_REG_OUT_X_H          0x29
#define L2D_REG_OUT_Y_L          0x2A
#define L2D_REG_OUT_Y_H          0x2B
#define L2D_REG_OUT_Z_L          0x2C
#define L2D_REG_OUT_Z_H          0x2D

// FIFO
// FIFO_CTRL_REG (0x2E): FIFO mode, trigger selection, FIFO threshold.
// Bits[7:6] FM[1:0] FIFO mode:
//   00 bypass, 01 FIFO, 10 stream, 11 trigger
// Bit5 TR trigger selection:
//   0: trigger event on INT1
//   1: trigger event on INT2
// Bits[4:0] FTH[4:0] FIFO watermark threshold
#define L2D_REG_FIFO_CTRL        0x2E

// FIFO_SRC_REG (0x2F): FIFO status.
// Bit7 WTM      FIFO watermark reached
// Bit6 OVRN_FIFO FIFO overrun (FIFO full)
// Bit5 EMPTY    FIFO empty
// Bits[4:0] FSS[4:0] number of samples stored in FIFO
#define L2D_REG_FIFO_SRC         0x2F

// Interrupt 1/2 configuration
// INT1_CFG (0x30) / INT2_CFG (0x34): interrupt generator configuration.
// Bit7 AOI:  AND/OR selection (see mode table below)
// Bit6 6D :  6-direction detection enable (changes meaning of axis bits)
// Bit5 ZHIE: enable Z high event (or Z-up in 6D)
// Bit4 ZLIE: enable Z low event  (or Z-down in 6D)
// Bit3 YHIE: enable Y high event (or Y-up in 6D)
// Bit2 YLIE: enable Y low event  (or Y-down in 6D)
// Bit1 XHIE: enable X high event (or X-up in 6D)
// Bit0 XLIE: enable X low event  (or X-down in 6D)
//
// AOI/6D mode table:
// AOI=0,6D=0: OR combination of interrupt events
// AOI=1,6D=0: AND combination of interrupt events
// AOI=0,6D=1: 6D movement recognition
// AOI=1,6D=1: 6D position recognition
#define L2D_REG_INT1_CFG         0x30

// INT1_SRC (0x31) / INT2_SRC (0x35): interrupt source.
// - Read-only. Reading clears IA (and de-asserts the interrupt pin if latched).
// Bit6 IA: interrupt active (1=one or more events have been generated)
// Bit5 ZH: Z high event occurred
// Bit4 ZL: Z low event occurred
// Bit3 YH: Y high event occurred
// Bit2 YL: Y low event occurred
// Bit1 XH: X high event occurred
// Bit0 XL: X low event occurred
#define L2D_REG_INT1_SRC         0x31

// INT1_THS (0x32) / INT2_THS (0x36): threshold for interrupt generator.
// Bit7: fixed 0
// Bits[6:0] THS[6:0]: threshold (LSB depends on full-scale setting)
// - 1 LSB = 16 mg @ FS=2g
// - 1 LSB = 32 mg @ FS=4g
// - 1 LSB = 62 mg @ FS=8g
// - 1 LSB = 186 mg @ FS=16g
#define L2D_REG_INT1_THS         0x32

// INT1_DURATION (0x33) / INT2_DURATION (0x37): minimum duration for event recognition.
// Bit7: fixed 0
// Bits[6:0] D[6:0]: duration value, time = N/ODR (1 LSB = 1/ODR)
#define L2D_REG_INT1_DUR         0x33
#define L2D_REG_INT2_CFG         0x34
#define L2D_REG_INT2_SRC         0x35
#define L2D_REG_INT2_THS         0x36
#define L2D_REG_INT2_DUR         0x37

// Click / Tap detection
// CLICK_CFG (0x38): enable single/double click detection per axis.
// Bits[7:6] reserved
// Bit5 ZD: enable Z double tap
// Bit4 ZS: enable Z single tap
// Bit3 YD: enable Y double tap
// Bit2 YS: enable Y single tap
// Bit1 XD: enable X double tap
// Bit0 XS: enable X single tap
#define L2D_REG_CLICK_CFG        0x38

// CLICK_SRC (0x39): click source (read-only).
// Bit7 reserved
// Bit6 IA: click interrupt active
// Bit5 DClick: double click detected
// Bit4 SClick: single click detected
// Bit3 Sign: click sign (0=positive, 1=negative)
// Bit2 Z: Z click detected
// Bit1 Y: Y click detected
// Bit0 X: X click detected
#define L2D_REG_CLICK_SRC        0x39

// CLICK_THS (0x3A): click threshold.
// Bit7 reserved
// Bits[6:0] Ths[6:0]: click threshold
#define L2D_REG_CLICK_THS        0x3A

// TIME_LIMIT (0x3B): maximum click time (tap duration window).
// TIME_LATENCY (0x3C): quiet time after a click.
// TIME_WINDOW (0x3D): time window for a second click (double click).
// (All are 8-bit values as described in the datasheet click timing section.)
#define L2D_REG_TIME_LIMIT       0x3B
#define L2D_REG_TIME_LATENCY     0x3C
#define L2D_REG_TIME_WINDOW      0x3D

// Activity / Inactivity
// Act_THS (0x3E): sleep-to-wake / return-to-sleep threshold (low-power mode).
// Bits[7:7] reserved
// Bits[6:0] Acth[6:0]: threshold (LSB depends on full-scale setting, same as INTx_THS)
#define L2D_REG_ACT_THS          0x3E

// Act_DUR (0x3F): sleep-to-wake / return-to-sleep duration.
// Bits[7:0] ActD[7:0]: duration value, time step depends on ODR (see datasheet)
#define L2D_REG_ACT_DUR          0x3F

// CTRL1 bits
#define L2D_CTRL1_ODR_MASK       0xF0  // bits[7:4] ODR[3:0]
#define L2D_CTRL1_LPEN           0x08  // bit3 LPen
#define L2D_CTRL1_ZEN            0x04  // bit2 Zen
#define L2D_CTRL1_YEN            0x02  // bit1 Yen
#define L2D_CTRL1_XEN            0x01  // bit0 Xen

// CTRL2 bits (HP filter)
#define L2D_CTRL2_HPM_MASK       0xC0  // bits[7:6] HPM[1:0]
#define L2D_CTRL2_HPCF_MASK      0x30  // bits[5:4] HPCF[1:0]
#define L2D_CTRL2_FDS            0x08  // bit3 FDS
#define L2D_CTRL2_HPCLICK        0x04  // bit2 HPCLICK
#define L2D_CTRL2_HPIS2          0x02  // bit1 HPIS2
#define L2D_CTRL2_HPIS1          0x01  // bit0 HPIS1

// CTRL3 bits (INT1 routing)
#define L2D_CTRL3_I1_CLICK       0x80  // bit7 I1_CLICK
#define L2D_CTRL3_I1_AOI1        0x40  // bit6 I1_AOI1
#define L2D_CTRL3_I1_AOI2        0x20  // bit5 I1_AOI2
#define L2D_CTRL3_I1_DRDY1       0x10  // bit4 I1_DRDY1
#define L2D_CTRL3_I1_DRDY2       0x08  // bit3 I1_DRDY2
#define L2D_CTRL3_I1_WTM         0x04  // bit2 I1_WTM
#define L2D_CTRL3_I1_OVERRUN     0x02  // bit1 I1_OVERRUN

// CTRL4 bits (scale/resolution)
#define L2D_CTRL4_BDU            0x80  // bit7 BDU
#define L2D_CTRL4_BLE            0x40  // bit6 BLE
#define L2D_CTRL4_FS_MASK        0x30  // bits[5:4] FS[1:0]
#define L2D_CTRL4_HR             0x08  // bit3 HR
#define L2D_CTRL4_ST1            0x04  // bit2 ST1
#define L2D_CTRL4_ST0            0x02  // bit1 ST0
#define L2D_CTRL4_SIM            0x01  // bit0 SIM

// CTRL5 bits
#define L2D_CTRL5_BOOT           0x80  // bit7 BOOT
#define L2D_CTRL5_FIFO_EN        0x40  // bit6 FIFO_EN
#define L2D_CTRL5_LIR_INT1       0x08  // bit3 LIR_INT1
#define L2D_CTRL5_D4D_INT1       0x04  // bit2 D4D_INT1
#define L2D_CTRL5_LIR_INT2       0x02  // bit1 LIR_INT2
#define L2D_CTRL5_D4D_INT2       0x01  // bit0 D4D_INT2

// CTRL6 bits (INT2 routing) - names per datasheet (with backward-compatible aliases)
#define L2D_CTRL6_I2_CLICKEN     0x80
#define L2D_CTRL6_I2_INT1        0x40
#define L2D_CTRL6_I2_INT2        0x20
#define L2D_CTRL6_BOOT_I2        0x10
#define L2D_CTRL6_P2_ACT         0x08
#define L2D_CTRL6_H_LACTIVE      0x02

// Backward-compatible aliases used by earlier code in this repo
#define L2D_CTRL6_I2_CLICK       L2D_CTRL6_I2_CLICKEN
#define L2D_CTRL6_I2_AOI1        L2D_CTRL6_I2_INT1
#define L2D_CTRL6_I2_AOI2        L2D_CTRL6_I2_INT2
#define L2D_CTRL6_I2_BOOT        L2D_CTRL6_BOOT_I2
#define L2D_CTRL6_I2_ACT         L2D_CTRL6_P2_ACT

// STATUS register bits
#define L2D_STATUS_ZYXOR         0x80  // bit7 ZYXOR
#define L2D_STATUS_ZOR           0x40  // bit6 ZOR
#define L2D_STATUS_YOR           0x20  // bit5 YOR
#define L2D_STATUS_XOR           0x10  // bit4 XOR
#define L2D_STATUS_ZYXDA         0x08  // bit3 ZYXDA
#define L2D_STATUS_ZDA           0x04  // bit2 ZDA
#define L2D_STATUS_YDA           0x02  // bit1 YDA
#define L2D_STATUS_XDA           0x01  // bit0 XDA

// FIFO control/source bits
#define L2D_FIFO_MODE_MASK       0xC0  // bits[7:6] FM[1:0]
#define L2D_FIFO_BYPASS          0x00  // FM=00
#define L2D_FIFO_FIFO            0x40  // FM=01
#define L2D_FIFO_STREAM          0x80  // FM=10
#define L2D_FIFO_STREAM2FIFO     0xC0  // FM=11
#define L2D_FIFO_TRIG_INT2       0x20  // bit5 TR (1=INT2, 0=INT1)
#define L2D_FIFO_WTM_MASK        0x1F  // bits[4:0] FTH[4:0]

#define L2D_FIFO_SRC_WTM         0x80  // bit7 WTM
#define L2D_FIFO_SRC_OVRN        0x40  // bit6 OVRN_FIFO
#define L2D_FIFO_SRC_EMPTY       0x20  // bit5 EMPTY
#define L2D_FIFO_SRC_FSS_MASK    0x1F  // bits[4:0] FSS[4:0]

// Interrupt config bits (INTx_CFG)
#define L2D_INT_CFG_AOI          0x80  // bit7 AOI
#define L2D_INT_CFG_6D           0x40  // bit6 6D
#define L2D_INT_CFG_ZH           0x20  // bit5 ZHIE (or ZUPE in 6D)
#define L2D_INT_CFG_ZL           0x10  // bit4 ZLIE (or ZDOWNE in 6D)
#define L2D_INT_CFG_YH           0x08  // bit3 YHIE (or YUPE in 6D)
#define L2D_INT_CFG_YL           0x04  // bit2 YLIE (or YDOWNE in 6D)
#define L2D_INT_CFG_XH           0x02  // bit1 XHIE (or XUPE in 6D)
#define L2D_INT_CFG_XL           0x01  // bit0 XLIE (or XDOWNE in 6D)

// INTx_SRC bits
#define L2D_INT_SRC_XL           0x01  // bit0 XL
#define L2D_INT_SRC_XH           0x02  // bit1 XH
#define L2D_INT_SRC_YL           0x04  // bit2 YL
#define L2D_INT_SRC_YH           0x08  // bit3 YH
#define L2D_INT_SRC_ZL           0x10  // bit4 ZL
#define L2D_INT_SRC_ZH           0x20  // bit5 ZH
#define L2D_INT_SRC_IA           0x40  // bit6 IA

// Click config/source bits
#define L2D_CLICK_CFG_ZD         0x20  // bit5 ZD
#define L2D_CLICK_CFG_ZS         0x10  // bit4 ZS
#define L2D_CLICK_CFG_YD         0x08  // bit3 YD
#define L2D_CLICK_CFG_YS         0x04  // bit2 YS
#define L2D_CLICK_CFG_XD         0x02  // bit1 XD
#define L2D_CLICK_CFG_XS         0x01  // bit0 XS

#define L2D_CLICK_SRC_X          0x01  // bit0 X
#define L2D_CLICK_SRC_Y          0x02  // bit1 Y
#define L2D_CLICK_SRC_Z          0x04  // bit2 Z
#define L2D_CLICK_SRC_SIGN       0x08  // bit3 Sign
#define L2D_CLICK_SRC_SCLICK     0x10  // bit4 SClick
#define L2D_CLICK_SRC_DCLICK     0x20  // bit5 DClick
#define L2D_CLICK_SRC_IA         0x40  // bit6 IA

// TEMP_CFG bits (TEMP_CFG_REG 0x1F)
// - Temperature sensor enabled when TEMP_EN[1:0] == 11b.
#define L2D_TEMP_EN              (L2D_TEMP_EN1 | L2D_TEMP_EN0)

#endif // L2D_REGS_H
