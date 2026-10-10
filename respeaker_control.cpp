#include "respeaker_control.h"
#include "respeaker_protocol.h"

#include <Arduino.h>
#include <USBCDC.h>
#include <Wire.h>
#include <atomic>

#if !CONFIG_TINYUSB_CDC_ENABLED
#error "This bridge requires TinyUSB CDC support"
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#error "Disable CDC On Boot: respeaker_control creates its own USBCDC instance"
#endif

namespace {
constexpr int kSdaPin = 5;  // XIAO D4
constexpr int kSclPin = 6;  // XIAO D5
constexpr uint8_t kRespeakerAddress = 0x2C;  // Seven-bit address, not shifted.
constexpr uint32_t kI2cFrequency = 100000;
constexpr uint16_t kI2cTimeoutMs = 100;
constexpr uint32_t kLineTimeoutMs = 1000;

USBCDC gControlCdc;
RespeakerAudioStatusProvider gAudioStatusProvider = nullptr;
RespeakerAudioStatusProvider gUsbStatusProvider = nullptr;
std::atomic<uint32_t> gCdcEpoch{0};

void cdcEvent(void *, esp_event_base_t, int32_t event, void *) {
  if (event == ARDUINO_USB_CDC_DISCONNECTED_EVENT ||
      event == ARDUINO_USB_CDC_RX_OVERFLOW_EVENT) {
    gCdcEpoch.fetch_add(1, std::memory_order_release);
  }
}

// Only the control worker writes this CDC. Partial writes are resumed, with a
// bounded timeout. Never wait inside a USB callback or the audio tasks.
void sendLine(const char *line, uint32_t epoch) {
  size_t offset = 0;
  const size_t length = strlen(line);
  const uint32_t start = millis();
  while (offset < length && gControlCdc &&
         epoch == gCdcEpoch.load(std::memory_order_acquire) && millis() - start < 250) {
    offset += gControlCdc.write(reinterpret_cast<const uint8_t *>(line) + offset,
                                length - offset);
    if (offset < length) vTaskDelay(1);
  }
}

void sendError(uint16_t sequence, const char *code, uint32_t epoch) {
  char line[80];
  snprintf(line, sizeof(line), "ERR %u %s\n", sequence, code);
  sendLine(line, epoch);
}

void sendResult(uint16_t sequence, uint8_t status, const uint8_t *data,
                size_t length, uint32_t epoch) {
  char line[respeaker::kMaxLine];
  size_t used = snprintf(line, sizeof(line), "OK %u %u ", sequence, status);
  constexpr char digits[] = "0123456789abcdef";
  if (length == 0) line[used++] = '-';
  for (size_t i = 0; i < length; ++i) {
    line[used++] = digits[data[i] >> 4];
    line[used++] = digits[data[i] & 15];
  }
  line[used++] = '\n';
  line[used] = 0;
  sendLine(line, epoch);
}

void execute(const respeaker::Request &request, uint32_t epoch) {
  if (request.operation == 'S' || request.operation == 'U') {
    uint8_t payload[respeaker::kMaxPayload];
    const auto provider = request.operation == 'S' ? gAudioStatusProvider : gUsbStatusProvider;
    const size_t size = provider ? provider(payload, sizeof(payload)) : 0;
    if (size == 0 || size > sizeof(payload)) sendError(request.sequence, "STATUS_UNAVAILABLE", epoch);
    else sendResult(request.sequence, 0, payload, size, epoch);
    return;
  }
  if (request.operation == 'P') {
    const uint8_t version[] = {'r', 'e', 's', 'p', 'e', 'a', 'k', 'e', 'r'};
    sendResult(request.sequence, 0, version, sizeof(version), epoch);
    return;
  }

  const bool read = request.operation == 'R';
  uint8_t header[3];
  respeaker::i2cHeader(request, header);
  Wire.beginTransmission(kRespeakerAddress);
  // The validated maximum (3 + 64) fits the 128-byte buffer allocated at boot.
  Wire.write(header, sizeof(header));
  if (!read) Wire.write(request.payload, request.length);

  // XMOS host transport: reads use repeated START; writes use STOP then a
  // separate one-byte status read. Do not substitute an SMBus register read.
  const uint8_t error = Wire.endTransmission(!read);
  if (error) {
    char code[32];
    snprintf(code, sizeof(code), "I2C_WRITE_%u", error);
    sendError(request.sequence, code, epoch);
    return;
  }
  const size_t expected = read ? request.length + 1 : 1;
  const size_t received = Wire.requestFrom(kRespeakerAddress, expected, true);
  if (received != expected) {
    while (Wire.available()) Wire.read();
    char code[32];
    snprintf(code, sizeof(code), "I2C_READ_%u", static_cast<unsigned>(received));
    sendError(request.sequence, code, epoch);
    return;
  }
  const uint8_t status = Wire.read();
  uint8_t payload[respeaker::kMaxPayload];
  for (size_t i = 0; i + 1 < expected; ++i) payload[i] = Wire.read();
  // Error replies from respeaker may contain uninitialized trailing bytes. Expose
  // data only on success; preserve all Respeaker status codes, including retry=64.
  sendResult(request.sequence, status, payload, status == 0 ? expected - 1 : 0, epoch);
}

void controlTask(void *) {
  char line[respeaker::kMaxLine];
  size_t used = 0;
  bool discard = false;
  uint32_t lastByte = millis();
  uint32_t epoch = gCdcEpoch.load(std::memory_order_acquire);
  while (true) {
    const uint32_t current = gCdcEpoch.load(std::memory_order_acquire);
    if (!gControlCdc || current != epoch) {
      // Discard any incomplete request after disconnect or RX queue overflow.
      while (gControlCdc.available()) gControlCdc.read();
      used = 0;
      discard = true;  // Require a newline before accepting a new session.
      epoch = current;
    } else {
      if (used && millis() - lastByte >= kLineTimeoutMs) {
        used = 0;
        discard = true;  // Never execute the suffix of a timed-out command.
        sendError(0, "RX_TIMEOUT", epoch);
      }
      // Bounded work per turn even if a host continuously sends garbage.
      for (unsigned budget = 0; budget < 256 && gControlCdc.available(); ++budget) {
        if (epoch != gCdcEpoch.load(std::memory_order_acquire)) break;
        const int ch = gControlCdc.read();
        if (ch < 0) break;
        lastByte = millis();
        if (ch == '\n') {
          if (!discard && used) {
            line[used] = 0;
            respeaker::Request request;
            if (respeaker::parseRequest(line, request)) execute(request, epoch);
            else sendError(request.sequence, "BAD_REQUEST", epoch);
          }
          used = 0;
          discard = false;
        } else if (!discard) {
          if (used == sizeof(line) - 1 || (ch < 32 && ch != '\r' && ch != '\t') || ch > 126) {
            used = 0;
            discard = true;
            sendError(0, "BAD_LINE", epoch);
          } else {
            line[used++] = static_cast<char>(ch);
          }
        }
      }
    }
    vTaskDelay(1);
  }
}
}  // namespace

bool beginRespeakerControl() {
  if (Wire.setBufferSize(128) < 128 || !Wire.begin(kSdaPin, kSclPin, kI2cFrequency)) return false;
  Wire.setTimeOut(kI2cTimeoutMs);
  if (gControlCdc.setRxBufferSize(512) < 512) return false;
  // Allow Arduino's 1200-baud / DTR-RTS bootloader request over the control CDC.
  gControlCdc.enableReboot(true);
  gControlCdc.setTxTimeoutMs(0);
  gControlCdc.onEvent(cdcEvent);
  gControlCdc.begin(115200);
  return xTaskCreatePinnedToCore(controlTask, "respeaker-control", 4096, nullptr, 1, nullptr, 0) == pdPASS;
}

void setRespeakerAudioStatusProvider(RespeakerAudioStatusProvider provider, RespeakerAudioStatusProvider usbProvider) {
  gAudioStatusProvider = provider;
  gUsbStatusProvider = usbProvider;
}
