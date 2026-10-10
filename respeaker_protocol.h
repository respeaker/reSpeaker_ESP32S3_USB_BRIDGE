#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ASCII transport v1. Counts are DATA bytes, excluding the reSpeaker protocol status byte.
// R seq resid cmd count | W seq resid cmd hex | P seq
// All numeric fields are decimal; seq is 1..65535. cmd is 0..127.
namespace respeaker {
constexpr size_t kMaxPayload = 64;
constexpr size_t kMaxLine = 192;

struct Request {
  char operation = 0;
  uint16_t sequence = 0;
  uint8_t resource = 0;
  uint8_t command = 0;
  size_t length = 0;
  uint8_t payload[kMaxPayload] = {};
};

inline bool decimal(const char *s, unsigned limit, unsigned &value) {
  if (!s || !*s) return false;
  value = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return false;
    const unsigned digit = *s - '0';
    if (digit > limit || value > (limit - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}

inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Modifies line in place. A parsed sequence is retained on subsequent errors.
inline bool parseRequest(char *line, Request &request) {
  request = Request{};
  char *save = nullptr;
  char *op = strtok_r(line, " \t\r", &save);
  if (!op || strlen(op) != 1) return false;
  request.operation = *op;
  unsigned value;
  if (!decimal(strtok_r(nullptr, " \t\r", &save), 65535, value) || value == 0) return false;
  request.sequence = value;
  if (*op == 'P' || *op == 'S' || *op == 'U') return strtok_r(nullptr, " \t\r", &save) == nullptr;
  if (*op != 'R' && *op != 'W') return false;
  if (!decimal(strtok_r(nullptr, " \t\r", &save), 255, value)) return false;
  request.resource = value;
  if (!decimal(strtok_r(nullptr, " \t\r", &save), 127, value)) return false;
  request.command = value;
  char *body = strtok_r(nullptr, " \t\r", &save);
  if (!body || strtok_r(nullptr, " \t\r", &save)) return false;
  if (*op == 'R') {
    if (!decimal(body, kMaxPayload, value) || value == 0) return false;
    request.length = value;
  } else {
    if (strcmp(body, "-") == 0) return true;  // Explicit zero-length write.
    const size_t count = strlen(body);
    if (count % 2 || count > kMaxPayload * 2) return false;
    request.length = count / 2;
    for (size_t i = 0; i < request.length; ++i) {
      const int hi = hexDigit(body[2 * i]), lo = hexDigit(body[2 * i + 1]);
      if (hi < 0 || lo < 0) return false;
      request.payload[i] = (hi << 4) | lo;
    }
  }
  return true;
}

inline void i2cHeader(const Request &request, uint8_t header[3]) {
  const bool read = request.operation == 'R';
  header[0] = request.resource;
  header[1] = request.command | (read ? 0x80 : 0);
  header[2] = request.length + (read ? 1 : 0);  // Read includes status.
}
}  // namespace respeaker
