#pragma once

#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0
#error "Select Tools > USB Mode > USB-OTG (TinyUSB)"
#endif

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || ARDUINO_USB_CDC_ON_BOOT != 0
#error "Select Tools > USB CDC On Boot > Disabled: the bridge creates its own CDC port"
#endif

// Upload Mode only changes Arduino upload properties; it has no dedicated
// compiler macro. The core exposes the selected board options in ARDUINO_FQBN.
#ifndef ARDUINO_FQBN
#error "ARDUINO_FQBN is required to check Tools > Upload Mode > USB-OTG CDC (TinyUSB)"
#else
namespace bridge_build {
constexpr bool hasUsbCdcUploadMode(const char *fqbn) {
  constexpr char option[] = "UploadMode=cdc";
  for (unsigned i = 0; fqbn[i]; ++i) {
    if (i && fqbn[i - 1] != ':' && fqbn[i - 1] != ',') continue;
    unsigned j = 0;
    while (option[j] && fqbn[i + j] && option[j] == fqbn[i + j]) ++j;
    if (!option[j] && (fqbn[i + j] == ',' || fqbn[i + j] == '\0')) return true;
  }
  return false;
}
static_assert(hasUsbCdcUploadMode(ARDUINO_FQBN),
              "Select Tools > Upload Mode > USB-OTG CDC (TinyUSB); CLI requires UploadMode=cdc");
}  // namespace bridge_build
#endif
