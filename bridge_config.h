#pragma once

// Names describe the respeaker xvf3800 role. ESP32-S3 always uses the opposite role.
#define BRIDGE_I2S_MASTER_48K 0
#define BRIDGE_I2S_MASTER_16K 1
#define BRIDGE_I2S_SLAVE_16K  2

// Edit this one macro, then rebuild/upload the whole sketch directory.
// #define BRIDGE_MODE BRIDGE_I2S_MASTER_48K
#ifndef BRIDGE_MODE
#error "Please define BRIDGE_MODE in bridge_config.h"
#endif

#if BRIDGE_MODE == BRIDGE_I2S_MASTER_48K
#define BRIDGE_SAMPLE_RATE 48000
#define BRIDGE_ESP_I2S_MASTER 0
#define BRIDGE_USB_PRODUCT "BRIDGE I2S Master 48k"
#define BRIDGE_USB_SERIAL "BRIDGE_I2S_MASTER_48K"
#elif BRIDGE_MODE == BRIDGE_I2S_MASTER_16K
#define BRIDGE_SAMPLE_RATE 16000
#define BRIDGE_ESP_I2S_MASTER 0
#define BRIDGE_USB_PRODUCT "BRIDGE I2S Master 16k"
#define BRIDGE_USB_SERIAL "BRIDGE_I2S_MASTER_16K"
#elif BRIDGE_MODE == BRIDGE_I2S_SLAVE_16K
#define BRIDGE_SAMPLE_RATE 16000
#define BRIDGE_ESP_I2S_MASTER 1
#define BRIDGE_USB_PRODUCT "BRIDGE I2S Slave 16k"
#define BRIDGE_USB_SERIAL "BRIDGE_I2S_SLAVE_16K"
#else
#error "Invalid BRIDGE_MODE"
#endif

#define BRIDGE_FIRMWARE_VERSION 0x0100 // v1.0.0

#define BRIDGE_FRAMES_PER_MS (BRIDGE_SAMPLE_RATE / 1000)
#ifndef BRIDGE_SEPARATE_I2S
#define BRIDGE_SEPARATE_I2S (!BRIDGE_ESP_I2S_MASTER)
#endif
#if BRIDGE_ESP_I2S_MASTER && BRIDGE_SEPARATE_I2S
#error "ESP32 master requires shared I2S0 TX/RX to generate one BCLK/WS pair"
#endif
