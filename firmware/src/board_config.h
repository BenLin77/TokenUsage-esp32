#pragma once

#define LGFX_USE_V1

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

static constexpr int LCD_CS = 39;
static constexpr int LCD_DC = 38;
static constexpr int LCD_RD = 14;
static constexpr int LCD_WR = 45;

static constexpr int BOARD_I2C_SDA = 2;
static constexpr int BOARD_I2C_SCL = 1;
static constexpr uint32_t BOARD_I2C_FREQ = 400000;

static constexpr int TOUCH_SDA = 41;
static constexpr int TOUCH_SCL = 40;
static constexpr int TOUCH_INT = 42;
static constexpr uint8_t TOUCH_I2C_ADDR = 0x14;
static constexpr int TOUCH_I2C_PORT = 1;
static constexpr uint32_t TOUCH_I2C_FREQ = 400000;

static constexpr uint8_t XL9555_ADDR = 0x20;
static constexpr uint8_t XL9555_OUTPUT_PORT0_REG = 2;
static constexpr uint8_t XL9555_CONFIG_PORT0_REG = 6;

static constexpr uint16_t XL9555_TP_RST_IO = 0x0001;
static constexpr uint16_t XL9555_LCD_RST_IO = 0x0002;
static constexpr uint16_t XL9555_BL_CTR_IO = 0x0008;
static constexpr uint16_t XL9555_LED1_IO = 0x0010;

#if defined(LCD_PANEL_ILI9488)
using DashboardPanel = lgfx::Panel_ILI9488;
#elif defined(LCD_PANEL_ILI9486)
using DashboardPanel = lgfx::Panel_ILI9486;
#else
using DashboardPanel = lgfx::Panel_ST7796;
#endif

inline bool xl9555Write(uint8_t reg, const uint8_t* data, size_t len) {
  Wire.beginTransmission(XL9555_ADDR);
  Wire.write(reg);
  for (size_t i = 0; i < len; ++i) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission() == 0;
}

inline bool xl9555ReadOutputs(uint8_t data[2]) {
  Wire.beginTransmission(XL9555_ADDR);
  Wire.write(XL9555_OUTPUT_PORT0_REG);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(XL9555_ADDR), 2) != 2) return false;
  data[0] = Wire.read();
  data[1] = Wire.read();
  return true;
}

inline bool xl9555PinWrite(uint16_t pin, bool level) {
  uint8_t data[2] = {0, 0};
  if (!xl9555ReadOutputs(data)) return false;

  if (pin <= 0x00FF) {
    if (level) {
      data[0] |= static_cast<uint8_t>(pin);
    } else {
      data[0] &= ~static_cast<uint8_t>(pin);
    }
  } else {
    uint8_t mask = static_cast<uint8_t>(pin >> 8);
    if (level) {
      data[1] |= mask;
    } else {
      data[1] &= ~mask;
    }
  }

  return xl9555Write(XL9555_OUTPUT_PORT0_REG, data, 2);
}

inline bool boardInitIoExpander() {
  // Mirrors the vendor BSP: XL9555 config = 0xF003.
  uint8_t config[2] = {0x03, 0xF0};
  if (!xl9555Write(XL9555_CONFIG_PORT0_REG, config, 2)) return false;
  bool ok = true;
  ok &= xl9555PinWrite(XL9555_BL_CTR_IO, false);
  ok &= xl9555PinWrite(XL9555_LED1_IO, true);
  return ok;
}

inline bool boardResetLcd() {
  bool ok = true;
  ok &= xl9555PinWrite(XL9555_LCD_RST_IO, true);
  delay(10);
  ok &= xl9555PinWrite(XL9555_LCD_RST_IO, false);
  delay(50);
  ok &= xl9555PinWrite(XL9555_LCD_RST_IO, true);
  delay(200);
  return ok;
}

inline bool boardResetTouch() {
  bool ok = true;
  ok &= xl9555PinWrite(XL9555_TP_RST_IO, false);
  delay(200);
  ok &= xl9555PinWrite(XL9555_TP_RST_IO, true);
  delay(200);
  ok &= xl9555PinWrite(XL9555_TP_RST_IO, false);
  delay(200);
  ok &= xl9555PinWrite(XL9555_TP_RST_IO, true);
  delay(200);
  return ok;
}

inline bool boardSetBacklight(bool enabled) {
  return xl9555PinWrite(XL9555_BL_CTR_IO, enabled);
}

// On-board LED1 (XL9555 P4), used as a low-quota alert indicator.
inline bool boardSetLed(bool enabled) {
  return xl9555PinWrite(XL9555_LED1_IO, enabled);
}

class DashboardDisplay : public lgfx::LGFX_Device {
  DashboardPanel panel_;
  lgfx::Bus_Parallel16 bus_;
  lgfx::Touch_GT911 touch_;

 public:
  DashboardDisplay() {
    {
      auto cfg = bus_.config();
      cfg.port = 0;
      cfg.freq_write = 25000000;
      cfg.pin_wr = LCD_WR;
      cfg.pin_rd = LCD_RD;
      cfg.pin_rs = LCD_DC;
      cfg.pin_d0 = 13;
      cfg.pin_d1 = 12;
      cfg.pin_d2 = 11;
      cfg.pin_d3 = 10;
      cfg.pin_d4 = 9;
      cfg.pin_d5 = 46;
      cfg.pin_d6 = 3;
      cfg.pin_d7 = 8;
      cfg.pin_d8 = 18;
      cfg.pin_d9 = 17;
      cfg.pin_d10 = 16;
      cfg.pin_d11 = 15;
      cfg.pin_d12 = 7;
      cfg.pin_d13 = 6;
      cfg.pin_d14 = 5;
      cfg.pin_d15 = 4;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }

    {
      auto cfg = panel_.config();
      cfg.pin_cs = LCD_CS;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 320;
      cfg.memory_height = 480;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = true;
      cfg.bus_shared = true;
      panel_.config(cfg);
    }

    {
      auto cfg = touch_.config();
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 479;
      cfg.pin_int = TOUCH_INT;
      cfg.pin_rst = -1;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = TOUCH_I2C_PORT;
      cfg.i2c_addr = TOUCH_I2C_ADDR;
      cfg.pin_sda = TOUCH_SDA;
      cfg.pin_scl = TOUCH_SCL;
      cfg.freq = TOUCH_I2C_FREQ;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }

    setPanel(&panel_);
  }
};
