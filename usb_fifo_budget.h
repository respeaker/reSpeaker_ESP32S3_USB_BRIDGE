#pragma once
#include <stdint.h>
// DWC2 slave RX minimum: setup/control + global NAK + one packet/status
// + status storage for every OUT endpoint. Units are 32-bit FIFO words.
static inline uint16_t bridge_rx_fifo_words(uint16_t packet_bytes, uint8_t ep_count) {
  return (uint16_t)(13 + 1 + ((packet_bytes + 3) / 4 + 1) + 2 * ep_count);
}
