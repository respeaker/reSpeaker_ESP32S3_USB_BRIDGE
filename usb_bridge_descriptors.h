#pragma once
#include "bridge_config.h"
#include "tusb.h"
#include "USBAudioCardDescriptors.h"
// Match Arduino registration order/endpoint reservations: CDC, then UAC.
// Smaller CDC packets leave hardware FIFO RAM for 32-bit stereo audio.
// Arduino-ESP32 3.3.11's UAC1 headset macro has no IAD. Group AC and
// both AS interfaces so Windows binds one audio function to interfaces 2-4.
#define BRIDGE_UAC1_IAD_LEN 8
#define BRIDGE_UAC1_IAD_DESCRIPTOR \
  BRIDGE_UAC1_IAD_LEN, TUSB_DESC_INTERFACE_ASSOCIATION, 2, 3, 0x01, 0x01, 0x00, 0
#define BRIDGE_USB_CONFIGURATION \
  TUD_CONFIG_DESCRIPTOR(1, 5, 0, 9 + TUD_CDC_DESC_LEN + BRIDGE_UAC1_IAD_LEN + TUD_AUDIO10_HEADSET_STEREO_DESC_LEN(1), 0, 100), \
  TUD_CDC_DESCRIPTOR(0, 0, 0x85, 16, 0x03, 0x84, 32), \
  BRIDGE_UAC1_IAD_DESCRIPTOR, \
  TUD_AUDIO10_HEADSET_STEREO_DESCRIPTOR(2, 0, 0x01, 0x81, BRIDGE_SAMPLE_RATE, 2, 2, 4, 32, BRIDGE_SAMPLE_RATE)
