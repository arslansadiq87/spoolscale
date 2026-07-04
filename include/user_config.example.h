#pragma once

// Copy this file to include/user_config.h for local builds.
// Keep real Wi-Fi credentials out of Git. include/user_config.h is ignored.

// -------------------- Wi-Fi --------------------
// Leave WIFI_SSID empty to run without Wi-Fi.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// The web UI is available at http://<device-ip>/ and, when mDNS works,
// http://scale.local/
#define DEVICE_NAME "ESP32 Scale"
#define MDNS_HOSTNAME "scale"

// -------------------- ESP32-C3 Super Mini GPIO --------------------
// HX711 load-cell amplifier
#define HX711_DOUT_PIN 4
#define HX711_SCK_PIN 5

// SSD1306 OLED over I2C
#define OLED_SDA_PIN 8
#define OLED_SCL_PIN 9
#define OLED_I2C_ADDRESS 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 32

// TTP223 touch input used to wake the OLED
#define TTP223_PIN 6

// Optional physical tare button. Use -1 to disable.
// Wire the button from the configured GPIO to GND.
#define TARE_BUTTON_PIN -1

// -------------------- Scale Defaults --------------------
// Start with 1.0, then calibrate from the web UI.
// The calibrated value is saved in ESP32 preferences after calibration.
#define HX711_CALIBRATION_FACTOR 1.0f
#define SCALE_WEIGHT_DEADBAND_KG 0.015f
#define SCALE_FILTER_ALPHA 0.22f

// -------------------- Timing --------------------
#define SCALE_SENSOR_POLL_MS 80
#define SCALE_OLED_REFRESH_MS 250
#define SCALE_WIFI_RETRY_MS 10000
#define SCALE_WIFI_CONNECT_TIMEOUT_MS 12000
#define SCALE_OLED_WAKE_WINDOW_MS 60000
#define SCALE_BUTTON_DEBOUNCE_MS 250

// -------------------- Sampling --------------------
#define SCALE_TARE_SAMPLES 12
#define SCALE_CALIBRATION_SAMPLES 12
