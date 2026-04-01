# HockeyHelmetCam

ESP32-S3 hockey helmet camera firmware for the Freenove ESP32-S3 WROOM board.

## Features
- **Wi-Fi hotspot**: Creates "HelmetCam" network — no router needed on the rink
- **Live stream**: MJPEG video at http://192.168.4.1 on your phone browser
- **BLE control**: Send commands from any BLE terminal app (nRF Connect, etc.)
- **SD recording**: Save JPEG frames to microSD at ~5 FPS
- **Photo capture**: Via web UI, BLE command, or onboard boot button
- **LED status**: WS2812 color-coded (green=ready, blue=streaming, purple=recording)

## Setup

### Arduino IDE
1. Install Arduino IDE 2.x
2. Add ESP32 board URL in Preferences:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Install "esp32 by Espressif Systems" from Boards Manager
4. Install the Freenove WS2812 library:
   Sketch → Include Library → Add .ZIP Library → select
   `Freenove_ESP32_S3_WROOM_Board-main/C/Libraries/Freenove_WS2812_Lib_for_ESP32-2.0.0.zip`

### Board Settings
- Board: ESP32S3 Dev Module
- Port: (your USB serial port)
- USB CDC On Boot: Disabled
- CPU Frequency: 240MHz (WiFi)
- Flash Mode: QIO 80MHz
- Flash Size: 8MB (64Mb)
- Partition Scheme: 8M with spiffs (3MB APP/1.5MB SPIFFS)
- PSRAM: OPI PSRAM
- Upload Mode: UART0 / Hardware CDC
- Upload Speed: 921600

### Hardware
- Plug OV2640 camera into FPC connector
- Insert 32GB microSD card (FAT32)
- Connect USB-C to UART port for programming
- For battery: LiPo → LiPo Rider Plus → SPDT switch → board 5V + GND pins

## Usage

### Phone Browser (Wi-Fi)
1. Connect phone to "HelmetCam" Wi-Fi (password: hockey123)
2. Open http://192.168.4.1 in browser
3. Use buttons to record, take photos, change resolution

### BLE Commands (send from nRF Connect app)
| Command   | Action                         |
|-----------|--------------------------------|
| PHOTO     | Take and save a photo          |
| RECORD    | Start recording to SD          |
| STOP      | Stop recording                 |
| STATUS    | Get camera/SD/recording status |
| RES:VGA   | Set 640x480 resolution         |
| RES:SVGA  | Set 800x600 resolution         |
| RES:XGA   | Set 1024x768 resolution        |
| RES:HD    | Set 1280x720 resolution        |
| FLIP      | Toggle vertical flip           |
| MIRROR    | Toggle horizontal mirror       |

### Boot Button (GPIO0)
- **Short press**: Take a photo
- **Long press (2+ seconds)**: Start/stop recording

## LED Colors
| Color  | Meaning                    |
|--------|----------------------------|
| Yellow | Booting                    |
| Green  | Ready / standby            |
| Blue   | Wi-Fi streaming active     |
| Purple | Recording to SD (blinks)   |
| White  | Photo captured (flash)     |
| Red    | Error (camera/SD failed)   |

## SD Card Structure
```
/photos/IMG_0.jpg, IMG_1.jpg, ...
/videos/REC_1/frame_0.jpg, frame_1.jpg, ...
/videos/REC_2/frame_0.jpg, frame_1.jpg, ...
```

## Customization
- Change Wi-Fi name/password: edit `ap_ssid` and `ap_password` in the .ino
- Change recording FPS: edit `RECORD_INTERVAL_MS` (200ms = ~5fps, 100ms = ~10fps)
- Adjust image: edit the sensor settings in `initCamera()` (brightness, contrast, etc.)
- Mount orientation: set `set_vflip` and `set_hmirror` in `initCamera()` based on how the camera sits on your helmet
