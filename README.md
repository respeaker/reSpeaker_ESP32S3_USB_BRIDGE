# reSpeaker ESP32-S3 USB Bridge

**English** | [中文](readme_cn.md)

Arduino firmware for an ESP32-S3 USB audio and control bridge connected to a
reSpeaker XVF3800. A single native USB connection provides stereo capture, stereo playback,
and a CDC serial interface for reSpeaker XVF3800 control over I2C.

## Firmware features

- USB Audio Class 1 (UAC1) capture and playback over Full-Speed USB.
- Stereo signed 32-bit little-endian PCM, with 32-bit Philips I2S slots.
- Compile-time selection of 16 kHz or 48 kHz operation and the I2S clock source.
- Simultaneous USB capture and playback. USB and I2S use the same nominal sample rate.
- Playback buffering and software clock drift correction, with USB host volume
  and mute controls for the master and individual channels.
- Capture without playback volume processing or software resampling.
- USB CDC commands forwarded to the reSpeaker XVF3800 I2C control interface, with a Python
  host utility and read-only audio/USB diagnostics.
- A project-local TinyUSB DWC2 driver and USB descriptors that fit duplex audio
  and CDC into the ESP32-S3 hardware FIFO budget.

### Audio modes

Set `BRIDGE_MODE` in [bridge_config.h](bridge_config.h), then rebuild the complete
sketch. **Master and Slave in the macro names describe the reSpeaker XVF3800 role.**
The ESP32-S3 uses the opposite role.

| Mode | Sample rate | reSpeaker role | ESP32-S3 role | Clock source |
| --- | --- | --- | --- | --- |
| `BRIDGE_I2S_MASTER_48K` | 48 kHz | Master | Slave | reSpeaker |
| `BRIDGE_I2S_MASTER_16K` | 16 kHz | Master | Slave | reSpeaker |
| `BRIDGE_I2S_SLAVE_16K` | 16 kHz | Slave | Master | ESP32-S3 |

The current source selects `BRIDGE_I2S_SLAVE_16K`:

```cpp
#ifndef BRIDGE_MODE
#define BRIDGE_MODE BRIDGE_I2S_SLAVE_16K
#endif
```

The reSpeaker XVF3800 must run firmware configured for the matching sample rate and I2S
role. The bridge does not configure or flash the reSpeaker. Mode selection requires
recompilation; there is no runtime rate switch or 16/48 kHz conversion.

Playback uses linear interpolation to correct small clock differences and is
not bit-perfect. Host playback audio reaches the ESP32-S3 over USB, then its I2S
output feeds both the reSpeaker XVF3800 and the codec with the same stream. The reSpeaker XVF3800
receives the playback reference while the codec converts the audio for playback.
The reSpeaker XVF3800 firmware initializes and configures the codec; the ESP32-S3 bridge
provides the I2S audio stream.

See [PLAYBACK.md](PLAYBACK.md) for audio wiring, buffering, and diagnostics, and
[CONTROL.md](CONTROL.md) for I2C wiring, the Python utility, and the CDC protocol.

## Build

### Requirements

- An ESP32-S3 board with native USB; the supplied profile targets the
  Seeed Studio XIAO ESP32S3.
- Arduino IDE or Arduino CLI with the Espressif board package available.
- Arduino-ESP32 **3.3.11**, pinned in [sketch.yaml](sketch.yaml).

No separate USB audio library is required. Use the pinned core version: the
local USB driver override and descriptors depend on its TinyUSB implementation.

Keep the complete repository in a folder named `reSpeaker_ESP32S3_USB_BRIDGE`,
matching `reSpeaker_ESP32S3_USB_BRIDGE.ino`. If a downloaded ZIP adds a branch
suffix to the folder name, rename the folder before opening the sketch.

Build the entire sketch directory, including its headers, `.cpp` files,
`sketch.yaml`, and `src/tinyusb/dwc2`. Copying only the `.ino` file is insufficient.

### Arduino CLI

From the repository root:

```sh
arduino-cli compile --profile xiao_esp32s3 .
```

The profile selects the board, core version, and USB settings. Change
`BRIDGE_MODE` before compiling to select another audio mode.

### Arduino IDE

Open `reSpeaker_ESP32S3_USB_BRIDGE.ino`, install/select Arduino-ESP32 3.3.11,
and use these settings:

| Setting | Value |
| --- | --- |
| Board | Seeed Studio XIAO_ESP32S3 |
| USB Mode | USB-OTG (TinyUSB) |
| USB CDC On Boot | Disabled |
| USB Firmware MSC On Boot | Disabled |
| USB DFU On Boot | Disabled |
| CPU Frequency | 240 MHz |
| PSRAM | Disabled |

Select **Verify** to compile. The firmware creates its own CDC interface, so
CDC On Boot must remain disabled. In the pinned CLI profile, `CDCOnBoot=cdc`
corresponds to Disabled.

The control CDC accepts Arduino's bootloader reset request for USB uploads.
Audio stops during flashing and returns after restarting. If the installed
firmware cannot enter download mode automatically, hold BOOT, press and release
RESET, then release BOOT and select the download port. See [CONTROL.md](CONTROL.md)
for reset behavior and recovery.

For another ESP32-S3 board, adjust the board selection and pin constants to match
its wiring. Audio buffers and DMA use internal SRAM.
