#include "usb_bridge_descriptors.h"

// Override the Arduino core's weak callback without modifying the installed
// core. Keep this configuration limited to the bridge's CDC + UAC interfaces.
extern "C" uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  static const uint8_t configuration[] = {BRIDGE_USB_CONFIGURATION};
  return index == 0 ? configuration : nullptr;
}
