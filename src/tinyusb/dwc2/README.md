# Project-local TinyUSB DWC2 driver

Upstream: https://github.com/hathach/tinyusb/blob/0.21.0/src/portable/synopsys/dwc2/dcd_dwc2.c
Original SHA-256: `96e6a20e599f97fc9fa057e672612977f5732465911988df7cbeccdd9028dcb7`
MIT license retained in source. Tested with Arduino-ESP32 3.3.11.

Local changes: qualify the SDK header include; restrict target to ESP32-S3;
replace double-packet RX FIFO budgeting with the documented single-packet
minimum via `usb_fifo_budget.h`. All setup/global-NAK/per-endpoint status
space remains reserved. No transfer, interrupt or endpoint logic is edited.
Arduino compiles this source recursively from `src`; its DCD definitions
satisfy linking before the prebuilt archive's DCD object is needed. Verify
that the map uses this object and does not extract `dcd_dwc2.c.obj` from the
SDK archive when changing toolchain versions. No global Arduino files change.

The USB configuration callback in `usb_bridge_descriptors.cpp` also reduces
CDC notification maximum packet size to 16 bytes and CDC bulk packets to 32
bytes. Both are required for the 253/256-word total FIFO budget. Single-packet
RX buffering has less interrupt-latency tolerance than double buffering;
validate simultaneous capture/playback with CDC traffic on hardware.
