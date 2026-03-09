#pragma once

#include <Arduino.h>

// Console over USB CDC
constexpr uint32_t CONSOLE_BAUD = 115200;

// Mirror telemetry link (FAST_STATE frames)
constexpr uint32_t MIRROR_BAUD = 921600;
// Teensy 4.0 Serial3: TX14 / RX15
#define MIRROR_SERIAL Serial3
constexpr uint8_t MIRROR_TX_PIN = 14;
constexpr uint8_t MIRROR_RX_PIN = 15;

// Planned CRSF telemetry UART
constexpr uint32_t CRSF_BAUD = 420000;
// Teensy 4.0 Serial2: TX8 / RX7
#define CRSF_SERIAL Serial2
constexpr uint8_t CRSF_TX_PIN = 8;
constexpr uint8_t CRSF_RX_PIN = 7;

// Planned GPS UART (u-blox UBX NAV-PVT)
constexpr uint32_t GPS_BAUD = 115200;
// Teensy 4.0 Serial1: TX1 / RX0
#define GPS_SERIAL Serial1
constexpr uint8_t GPS_TX_PIN = 1;
constexpr uint8_t GPS_RX_PIN = 0;

// I2C (IMU / baro)
// Teensy 4.0 Wire default: SDA=18, SCL=19
constexpr uint8_t I2C_SDA_PIN = 18;
constexpr uint8_t I2C_SCL_PIN = 19;
constexpr uint32_t I2C_BUS_HZ = 1000000;
