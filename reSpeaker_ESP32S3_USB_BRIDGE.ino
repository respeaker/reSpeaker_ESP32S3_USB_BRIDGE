#include <Arduino.h>
#include "bridge_config.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "USB.h"
#include "USBAudioCard.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "tusb.h"
#include "soc/usb_dwc_struct.h"
#include "respeaker_control.h"
#include "audio_playback.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "reSpeaker_ESP32S3_USB_BRIDGE requires an ESP32-S3"
#endif

#ifndef ARDUINO_USB_MODE
#error "The selected board does not expose a native USB interface"
#elif ARDUINO_USB_MODE == 1
#error "Select Tools > USB Mode > USB-OTG (TinyUSB)"
#endif

#if !defined(CONFIG_TINYUSB_AUDIO_ENABLED) || !CONFIG_TINYUSB_AUDIO_ENABLED
#error "TinyUSB Audio is disabled; use Arduino-ESP32 3.3.11 or newer"
#endif

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#error "Disable USB CDC On Boot: respeaker_control creates the control CDC port explicitly"
#endif

#if defined(ARDUINO_USB_MSC_ON_BOOT) && ARDUINO_USB_MSC_ON_BOOT
#error "Disable Tools > USB MSC On Boot"
#endif

#if defined(ARDUINO_USB_DFU_ON_BOOT) && ARDUINO_USB_DFU_ON_BOOT
#error "Disable Tools > USB DFU On Boot"
#endif

namespace {

constexpr uint32_t kSampleRate = BRIDGE_SAMPLE_RATE;
constexpr size_t kChannels = 2;
constexpr size_t kBytesPerSample = sizeof(int32_t);
constexpr size_t kBytesPerFrame = kChannels * kBytesPerSample;
// One DMA block is 1 ms at the selected rate. Peripheral sharing and
// clock ownership are selected centrally in bridge_config.h.
constexpr size_t kDmaFrames = BRIDGE_FRAMES_PER_MS;
constexpr size_t kRingFrames = 2048;  // About 43 ms at 48 kHz; must remain a power of two.
constexpr size_t kUsbMaxPacketFrames = (kSampleRate + 999) / 1000 + 1;
constexpr size_t kUsbMaxPacketBytes = kUsbMaxPacketFrames * kBytesPerFrame;

static_assert((kRingFrames & (kRingFrames - 1)) == 0, "ring size must be a power of two");
static_assert(kUsbMaxPacketBytes <= 392, "USB FIFO budget exceeded");

constexpr gpio_num_t kBclkPin = GPIO_NUM_8;
constexpr gpio_num_t kFsyncPin = GPIO_NUM_7;
// Same capture wiring as the local TDM bridge. The GitHub I2S-UAC
// example instead uses GPIO44 for DIN; change this pin for that wiring.
constexpr gpio_num_t kHostDinPin = GPIO_NUM_43;
constexpr gpio_num_t kHostDoutPin = GPIO_NUM_44;

struct BridgeStats {
  uint32_t capturedFrames = 0;
  uint32_t queuedFrames = 0;
  uint32_t sentBytes = 0;
  uint32_t usbPlaybackCallbacks = 0;
  uint32_t usbPlaybackBytes = 0;
  uint32_t usbPlaybackPeak = 0;
  uint32_t txBytes = 0;
  uint32_t txPeak = 0;
  uint32_t txDmaCompletions = 0;
  uint32_t rxErrors = 0;
  uint32_t txErrors = 0;
  uint32_t txTimeouts = 0;
  uint32_t rxQueueOverflows = 0;
  uint32_t ringOverruns = 0;
  uint32_t usbBackpressure = 0;
  uint32_t maxRingFrames = 0;
};

i2s_chan_handle_t gRxChannel = nullptr;
i2s_chan_handle_t gTxChannel = nullptr;
AudioPlayback gPlayback;
portMUX_TYPE gPlaybackMux = portMUX_INITIALIZER_UNLOCKED;
bool gSpeakerEnabled = false;
volatile uint32_t gUsbOutPackets = 0, gUsbOutPacketBytes = 0, gUsbOutEndpoint = 0;
portMUX_TYPE gUsbIsrMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> gUsbReadLoops{0}, gUsbFifoHighWater{0}, gUsbReadBytes{0};
TaskHandle_t gUsbReaderTask = nullptr;
portMUX_TYPE gStatsMux = portMUX_INITIALIZER_UNLOCKED;
volatile BridgeStats gStats;

alignas(4) int32_t gAudioRing[kRingFrames][kChannels];
std::atomic<uint32_t> gRingWriteFrame{0};
std::atomic<uint32_t> gRingReadFrame{0};
// Bit 0: interface enabled; upper bits: generation. A single atomic snapshot
// prevents mixing the enable flag of one session with another generation.
std::atomic<uint32_t> gStreamState{0};
const char *volatile gFatalOperation = nullptr;
volatile esp_err_t gFatalError = ESP_OK;

USBAudioCard gUac(kSampleRate, UAC_BPS_32, UAC_SPK_STEREO, UAC_MIC_STEREO);

void incrementTaskCounter(volatile uint32_t &counter, uint32_t amount = 1) {
  portENTER_CRITICAL(&gStatsMux);
  counter += amount;
  portEXIT_CRITICAL(&gStatsMux);
}

void updateRingHighWater(uint32_t frames) {
  portENTER_CRITICAL(&gStatsMux);
  if (frames > gStats.maxRingFrames) {
    gStats.maxRingFrames = frames;
  }
  portEXIT_CRITICAL(&gStatsMux);
}

bool IRAM_ATTR onRxQueueOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  portENTER_CRITICAL_ISR(&gStatsMux);
  ++gStats.rxQueueOverflows;
  portEXIT_CRITICAL_ISR(&gStatsMux);
  return false;
}

[[noreturn]] void fail(const char *operation, esp_err_t error = ESP_FAIL) {
  gFatalOperation = operation;
  gFatalError = error;
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  while (true) {
#ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
#endif
    delay(150);
  }
}

void check(const char *operation, esp_err_t error) {
  if (error != ESP_OK) {
    fail(operation, error);
  }
}

void discardRing() {
  const uint32_t writeFrame = gRingWriteFrame.load(std::memory_order_acquire);
  gRingReadFrame.store(writeFrame, std::memory_order_release);
}

bool pushRing(const int32_t frames[][kChannels], size_t frameCount) {
  const uint32_t writeFrame = gRingWriteFrame.load(std::memory_order_relaxed);
  const uint32_t readFrame = gRingReadFrame.load(std::memory_order_acquire);
  const uint32_t usedFrames = writeFrame - readFrame;
  if (frameCount > kRingFrames - usedFrames) {
    incrementTaskCounter(gStats.ringOverruns, static_cast<uint32_t>(frameCount));
    return false;
  }

  for (size_t frame = 0; frame < frameCount; ++frame) {
    memcpy(gAudioRing[(writeFrame + frame) & (kRingFrames - 1)], frames[frame], kBytesPerFrame);
  }

  gRingWriteFrame.store(writeFrame + frameCount, std::memory_order_release);
  incrementTaskCounter(gStats.queuedFrames, static_cast<uint32_t>(frameCount));
  updateRingHighWater(usedFrames + static_cast<uint32_t>(frameCount));
  return true;
}

size_t popRing(int32_t destination[][kChannels], size_t maxFrames) {
  const uint32_t readFrame = gRingReadFrame.load(std::memory_order_relaxed);
  const uint32_t writeFrame = gRingWriteFrame.load(std::memory_order_acquire);
  const size_t frameCount = std::min<size_t>(writeFrame - readFrame, maxFrames);

  for (size_t frame = 0; frame < frameCount; ++frame) {
    memcpy(destination[frame], gAudioRing[(readFrame + frame) & (kRingFrames - 1)], kBytesPerFrame);
  }

  gRingReadFrame.store(readFrame + frameCount, std::memory_order_release);
  return frameCount;
}

bool IRAM_ATTR onTxSent(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  portENTER_CRITICAL_ISR(&gStatsMux);
  ++gStats.txDmaCompletions;
  portEXIT_CRITICAL_ISR(&gStatsMux);
  return false;
}

void configureI2s() {
  i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(
      I2S_NUM_0, BRIDGE_ESP_I2S_MASTER ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
  channelConfig.dma_desc_num = 8;
  channelConfig.dma_frame_num = kDmaFrames;
  channelConfig.auto_clear_after_cb = true;
#if BRIDGE_SEPARATE_I2S
  check("allocate I2S0 TX", i2s_new_channel(&channelConfig, &gTxChannel, nullptr));
  channelConfig.id = I2S_NUM_1;
  check("allocate I2S1 RX", i2s_new_channel(&channelConfig, nullptr, &gRxChannel));
#else
  check("allocate I2S TX/RX", i2s_new_channel(&channelConfig, &gTxChannel, &gRxChannel));
#endif

  i2s_std_config_t config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = kBclkPin,
          .ws = kFsyncPin,
          .dout = kHostDoutPin,
          .din = kHostDinPin,
          .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
      },
  };
  config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  // Philips I2S: WS low = left, WS high = right, MSB one BCLK after WS.
  // In slave mode the external source determines the actual sample rate.
  // Keep unused data directions disconnected for each simplex peripheral.
  i2s_std_config_t txConfig = config;
  txConfig.gpio_cfg.din = I2S_GPIO_UNUSED;
  i2s_std_config_t rxConfig = config;
  rxConfig.gpio_cfg.dout = I2S_GPIO_UNUSED;
#if BRIDGE_SEPARATE_I2S
  check("initialize I2S TX", i2s_channel_init_std_mode(gTxChannel, &txConfig));
  config = rxConfig;
#else
  check("initialize I2S TX", i2s_channel_init_std_mode(gTxChannel, &config));
#endif
  check("initialize I2S RX", i2s_channel_init_std_mode(gRxChannel, &config));
  i2s_event_callbacks_t callbacks = {};
  callbacks.on_recv_q_ovf = onRxQueueOverflow;
  check("register I2S RX callback",
        i2s_channel_register_event_callback(gRxChannel, &callbacks, nullptr));
  i2s_event_callbacks_t txCallbacks = {};
  txCallbacks.on_sent = onTxSent;
  check("register I2S TX callback", i2s_channel_register_event_callback(gTxChannel, &txCallbacks, nullptr));
  static int32_t silence[8 * kDmaFrames][kChannels] = {};
  size_t loaded = 0;
  check("preload I2S TX", i2s_channel_preload_data(gTxChannel, silence, sizeof(silence), &loaded));
  if (loaded != sizeof(silence)) fail("short TX preload");
  check("enable I2S RX", i2s_channel_enable(gRxChannel));
  check("enable I2S TX", i2s_channel_enable(gTxChannel));
}

void rxTask(void *) {
  static int32_t rawFrames[kDmaFrames][kChannels];

  while (true) {
    const uint32_t state = gStreamState.load(std::memory_order_acquire);
    size_t bytesRead = 0;
    const esp_err_t error =
        i2s_channel_read(gRxChannel, rawFrames, sizeof(rawFrames), &bytesRead, 100);
    if ((error != ESP_OK && error != ESP_ERR_TIMEOUT) || (bytesRead % kBytesPerFrame) != 0) {
      incrementTaskCounter(gStats.rxErrors);
      continue;
    }

    const size_t frameCount = bytesRead / kBytesPerFrame;
    if (frameCount == 0) {
      continue;
    }

    incrementTaskCounter(gStats.capturedFrames, static_cast<uint32_t>(frameCount));

    if ((state & 1U) && state == gStreamState.load(std::memory_order_acquire) &&
        tud_mounted() && !tud_suspended()) {
      pushRing(rawFrames, frameCount);
    }
  }
}

void usbWriterTask(void *) {
  static int32_t stagingFrames[kDmaFrames][kChannels];
  size_t pendingOffset = 0;
  size_t pendingBytes = 0;
  uint32_t observedGeneration = gStreamState.load(std::memory_order_acquire);

  while (true) {
    const uint32_t generation = gStreamState.load(std::memory_order_acquire);
    if (!(generation & 1U) || generation != observedGeneration ||
        !tud_mounted() || tud_suspended()) {
      pendingOffset = 0;
      pendingBytes = 0;
      discardRing();
      observedGeneration = generation;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    if (pendingBytes == 0) {
      const size_t frameCount = popRing(stagingFrames, kDmaFrames);
      if (frameCount == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      pendingOffset = 0;
      pendingBytes = frameCount * kBytesPerFrame;
    }

    const uint16_t written = gUac.write(
        reinterpret_cast<const uint8_t *>(stagingFrames) + pendingOffset,
        static_cast<uint16_t>(pendingBytes));
    if (written == 0) {
      incrementTaskCounter(gStats.usbBackpressure);
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    pendingOffset += written;
    pendingBytes -= written;
    incrementTaskCounter(gStats.sentBytes, written);
  }
}

uint32_t pcmPeak(const void *data, size_t bytes) {
  uint32_t peak = 0;
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i + 4 <= bytes; i += 4) {
    int32_t sample;
    memcpy(&sample, p + i, 4);
    const uint32_t magnitude = sample < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(sample)) : sample;
    peak = std::max(peak, magnitude);
  }
  return peak;
}

// Called only by our USB OUT reader task, never wait for I2S here.
void speakerData(void *data, uint16_t length) {
  const uint32_t peak = pcmPeak(data, length);
  portENTER_CRITICAL(&gStatsMux);
  ++gStats.usbPlaybackCallbacks;
  gStats.usbPlaybackBytes += length;
  gStats.usbPlaybackPeak = peak;
  portEXIT_CRITICAL(&gStatsMux);
  portENTER_CRITICAL(&gPlaybackMux);
  if (tud_mounted() && !tud_suspended()) gPlayback.append(data, length);
  portEXIT_CRITICAL(&gPlaybackMux);
}

// Sole consumer of TinyUSB's OUT FIFO. Do not call gUac.begin(): in this
// core it would create a second FIFO reader. Constructor registers descriptors;
// volume, events and capture write() do not require the wrapper receive task.
void usbReaderTask(void *) {
  alignas(4) uint8_t buffer[2 * kDmaFrames * kBytesPerFrame];
  while (true) {
    gUsbReadLoops.fetch_add(1, std::memory_order_relaxed);
    if (tud_mounted() && tud_audio_mounted() && !tud_suspended()) {
      for (unsigned budget = 0; budget < 4; ++budget) {
        const uint16_t available = tud_audio_available();
        if (available > gUsbFifoHighWater.load(std::memory_order_relaxed)) {
          gUsbFifoHighWater.store(available, std::memory_order_relaxed);
        }
        if (!available) break;
        const uint16_t bytes = tud_audio_read(buffer, sizeof(buffer));
        if (!bytes) break;
        gUsbReadBytes.fetch_add(bytes, std::memory_order_relaxed);
        speakerData(buffer, bytes);
      }
    }
    vTaskDelay(1);
  }
}

// Reset only TX; RX and the external master clock remain independent.
void clearTxDma() {
  check("disable I2S TX", i2s_channel_disable(gTxChannel));
  static int32_t silence[8 * kDmaFrames][kChannels] = {};
  size_t loaded = 0;
  check("clear I2S TX", i2s_channel_preload_data(gTxChannel, silence, sizeof(silence), &loaded));
  if (loaded != sizeof(silence)) fail("short TX clear");
  check("restart I2S TX", i2s_channel_enable(gTxChannel));
}

void txTask(void *) {
  int32_t block[kDmaFrames][kChannels] = {};
  size_t offset = 0;
  uint32_t generation = UINT32_MAX;
  while (true) {
    portENTER_CRITICAL(&gPlaybackMux);
    const uint32_t current = gPlayback.generation();
    portEXIT_CRITICAL(&gPlaybackMux);
    if (current != generation) {
      clearTxDma();
      offset = 0;
      generation = current;
    }
    if (offset == 0) {
      portENTER_CRITICAL(&gPlaybackMux);
      gPlayback.render(block);
      portEXIT_CRITICAL(&gPlaybackMux);
      // Apply master/channel volume and mute to complete, aligned frames.
      gUac.applyVolume(block, sizeof(block));
      const uint32_t peak = pcmPeak(block, sizeof(block));
      portENTER_CRITICAL(&gStatsMux);
      gStats.txPeak = peak;
      portEXIT_CRITICAL(&gStatsMux);
    }
    size_t written = 0;
    const esp_err_t error = i2s_channel_write(gTxChannel,
        reinterpret_cast<uint8_t *>(block) + offset, sizeof(block) - offset, &written, 20);
    incrementTaskCounter(gStats.txBytes, written);
    offset += written;
    if (offset == sizeof(block)) offset = 0;
    if (error != ESP_OK) {
      incrementTaskCounter(error == ESP_ERR_TIMEOUT ? gStats.txTimeouts : gStats.txErrors);
      // Clock absent/stalled: remove queued audio so restoration starts fresh.
      portENTER_CRITICAL(&gPlaybackMux);
      gPlayback.reset();
      portEXIT_CRITICAL(&gPlaybackMux);
      clearTxDma();
      offset = 0;
      vTaskDelay(1);
    }
  }
}

void usbEventCallback(void *, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
  if (eventBase == ARDUINO_USB_EVENTS) {
    if (eventId == ARDUINO_USB_STOPPED_EVENT || eventId == ARDUINO_USB_SUSPEND_EVENT ||
        eventId == ARDUINO_USB_RESUME_EVENT) {
      portENTER_CRITICAL(&gPlaybackMux);
      if (eventId == ARDUINO_USB_STOPPED_EVENT) gSpeakerEnabled = false;
      gPlayback.setActive(eventId == ARDUINO_USB_RESUME_EVENT && gSpeakerEnabled);
      portEXIT_CRITICAL(&gPlaybackMux);
    }
    if (eventId == ARDUINO_USB_STOPPED_EVENT) {
      const uint32_t state = gStreamState.load(std::memory_order_relaxed);
      gStreamState.store((state + 2U) & ~1U, std::memory_order_release);
    }
    return;
  }

  if (eventBase != ARDUINO_USB_AUDIO_CARD_EVENTS || eventData == nullptr ||
      eventId != ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT) {
    return;
  }

  const auto *data = static_cast<arduino_usb_audio_card_event_data_t *>(eventData);
  if (data->interface_enable.interface == UAC_INTERFACE_SPK) {
    portENTER_CRITICAL(&gPlaybackMux);
    gSpeakerEnabled = data->interface_enable.enable;
    gPlayback.setActive(gSpeakerEnabled);
    portEXIT_CRITICAL(&gPlaybackMux);
    return;
  }
  if (data->interface_enable.interface != UAC_INTERFACE_MIC) return;

  const uint32_t state = gStreamState.load(std::memory_order_relaxed);
  gStreamState.store(((state + 2U) & ~1U) | (data->interface_enable.enable ? 1U : 0U),
                     std::memory_order_release);
}

size_t audioStatus(uint8_t *destination, size_t capacity) {
  uint32_t words[16] = {};
  if (capacity < sizeof(words)) return 0;
  portENTER_CRITICAL(&gPlaybackMux);
  words[0] = (gPlayback.active() ? 1U : 0U) | (BRIDGE_SEPARATE_I2S ? 2U : 0U);
  words[5] = gPlayback.stats.rendered;
  words[11] = gPlayback.available();
  words[12] = gPlayback.stats.dropped;
  words[13] = gPlayback.stats.underruns;
  portEXIT_CRITICAL(&gPlaybackMux);
  portENTER_CRITICAL(&gStatsMux);
  words[1] = gStats.usbPlaybackCallbacks;
  words[2] = gStats.usbPlaybackBytes;
  words[3] = gStats.usbPlaybackPeak;
  words[4] = gStats.capturedFrames;
  words[6] = gStats.txBytes;
  words[7] = gStats.txPeak;
  words[8] = gStats.txDmaCompletions;
  words[9] = gStats.txTimeouts;
  words[10] = gStats.txErrors;
  portEXIT_CRITICAL(&gStatsMux);
  words[14] = (gUac.mute(UAC_CHAN_MASTER) ? 1U : 0U) |
              (gUac.mute(UAC_CHAN_LEFT) ? 2U : 0U) |
              (gUac.mute(UAC_CHAN_RIGHT) ? 4U : 0U);
  words[15] = static_cast<uint32_t>(static_cast<int32_t>(gUac.volume(UAC_CHAN_MASTER)));
  memcpy(destination, words, sizeof(words));
  return sizeof(words);
}

size_t usbStatus(uint8_t *destination, size_t capacity) {
  uint32_t packets, packetBytes, endpoint;
  portENTER_CRITICAL(&gUsbIsrMux);
  packets = gUsbOutPackets;
  packetBytes = gUsbOutPacketBytes;
  endpoint = gUsbOutEndpoint;
  portEXIT_CRITICAL(&gUsbIsrMux);
  const uint32_t words[] = {
      gUsbReadLoops.load(std::memory_order_relaxed),
      gUsbFifoHighWater.load(std::memory_order_relaxed),
      packets,
      packetBytes,
      endpoint,
      gUsbReadBytes.load(std::memory_order_relaxed),
      static_cast<uint32_t>((tud_mounted() ? 1U : 0U) | (tud_audio_mounted() ? 2U : 0U) |
                            (tud_suspended() ? 4U : 0U)),
      gUsbReaderTask ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(gUsbReaderTask)) : 0U,
      // UAC uses duplex endpoint 1 (verified by the descriptor test).
      // These registers are non-destructive reads; never read GRXSTSP here,
      // since that would pop the USB receive status FIFO.
      USB_DWC.out_eps[0].doepctl_reg.val,
      USB_DWC.out_eps[0].doeptsiz_reg.val,
      USB_DWC.out_eps[0].doepint_reg.val,
      USB_DWC.gintsts_reg.val,
      USB_DWC.gintmsk_reg.val,
      USB_DWC.dsts_reg.val,
      USB_DWC.dctl_reg.val,
      USB_DWC.grxfsiz_reg.val,
  };
  if (capacity < sizeof(words)) return 0;
  memcpy(destination, words, sizeof(words));
  return sizeof(words);
}

void createBridgeTasks() {
  if (xTaskCreatePinnedToCore(txTask, "i2s-playback", 4096, nullptr, 4, nullptr, 1) != pdPASS) {
    fail("create I2S TX task");
  }
  if (xTaskCreatePinnedToCore(rxTask, "i2s-capture", 4096, nullptr, 5, nullptr, 1) != pdPASS) {
    fail("create I2S RX task");
  }
  if (xTaskCreatePinnedToCore(usbWriterTask, "uac-writer", 4096, nullptr, 4, nullptr, 0) !=
      pdPASS) {
    fail("create UAC writer task");
  }
}

}  // namespace

// TinyUSB 0.21 weak ISR hook: counts completed packets after FIFO insertion.
// No reads, allocation, logging, blocking or audio processing in interrupt context.
extern "C" bool tud_audio_rx_done_isr(uint8_t, uint16_t bytes, uint8_t function,
                                     uint8_t endpoint, uint8_t) {
  if (function == 0) {
    portENTER_CRITICAL_ISR(&gUsbIsrMux);
    ++gUsbOutPackets;
    gUsbOutPacketBytes += bytes;
    gUsbOutEndpoint = endpoint;
    portEXIT_CRITICAL_ISR(&gUsbIsrMux);
  }
  return true;
}

void setup() {
  setRespeakerAudioStatusProvider(audioStatus, usbStatus);
  if (!beginRespeakerControl()) {
    fail("start respeaker I2C/CDC control");
  }
  gUac.onEvent(usbEventCallback);

  // Advertise IAD support for the CDC + audio composite device on Windows.
  USB.usbClass(0xEF);
  USB.usbSubClass(0x02);
  USB.usbProtocol(0x01);
  USB.manufacturerName("reSpeaker ESP32S3 USB Bridge");
  USB.productName(BRIDGE_USB_PRODUCT);
  USB.serialNumber(BRIDGE_USB_SERIAL);
  USB.firmwareVersion(BRIDGE_FIRMWARE_VERSION);
  USB.onEvent(usbEventCallback);
  if (!USB.begin()) {
    fail("start USB");
  }
  // Keep CDC bootloader requests available if I2S initialization enters fail().
  configureI2s();
  createBridgeTasks();

  if (xTaskCreatePinnedToCore(usbReaderTask, "uac-out-reader", 4096, nullptr, 5,
                            &gUsbReaderTask, 0) != pdPASS) {
    fail("start USB OUT reader");
  }
}

void loop() {
  delay(1000);
}
