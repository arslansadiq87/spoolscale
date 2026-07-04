# ESP32-C3 Weighing Scale

PlatformIO Arduino firmware for an ESP32-C3 Super Mini, HX711 load-cell amplifier, SSD1306 OLED, and optional TTP223 touch wake input.

## Setup

1. Copy `include/user_config.example.h` to `include/user_config.h`.
2. Put your Wi-Fi SSID/password and GPIO choices in `include/user_config.h`.
3. Build and upload with PlatformIO.

`include/user_config.h` is ignored by Git so real Wi-Fi credentials are not published.

## Default Pin Map

| Module | Signal | GPIO |
| --- | --- | --- |
| HX711 | DOUT | 4 |
| HX711 | SCK | 5 |
| OLED | SDA | 8 |
| OLED | SCL | 9 |
| TTP223 | OUT | 6 |
| Tare button | Input | disabled |

The default OLED config is `128x32` at I2C address `0x3C`.

## Build

```powershell
C:\.platformio\penv\Scripts\pio.exe run
```

Upload:

```powershell
C:\.platformio\penv\Scripts\pio.exe run --target upload
```

If your `pio` command points to an older PlatformIO Core, use the full path above or update PlatformIO.
