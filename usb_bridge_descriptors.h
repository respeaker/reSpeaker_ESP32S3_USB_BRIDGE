#pragma once
#include "bridge_config.h"
#include "tusb.h"
#include "USBAudioCardDescriptors.h"
// Match Arduino registration order/endpoint reservations: CDC, then UAC.
// Smaller CDC packets leave hardware FIFO RAM for 32-bit stereo audio.
#define BRIDGE_USB_CONFIGURATION \
  TUD_CONFIG_DESCRIPTOR(1, 5, 0, 9 + TUD_CDC_DESC_LEN + TUD_AUDIO10_HEADSET_STEREO_DESC_LEN(1), 0, 100), \
  TUD_CDC_DESCRIPTOR(0, 0, 0x85, 16, 0x03, 0x84, 32), \
  TUD_AUDIO10_HEADSET_STEREO_DESCRIPTOR(2, 0, 0x01, 0x81, BRIDGE_SAMPLE_RATE, 2, 2, 4, 32, BRIDGE_SAMPLE_RATE)
