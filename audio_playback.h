#pragma once
#include "bridge_config.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include <cmath>

// Call all methods under the same task critical section on ESP32.
// Host-testable stereo FIFO + adaptive linear interpolator, independent of USB.
class AudioPlayback {
 public:
  static constexpr size_t kCapacity = 4096;
  static constexpr size_t kBlockFrames = BRIDGE_FRAMES_PER_MS;
  static constexpr size_t kTarget = 12 * kBlockFrames; // 12 ms prefill.
  static constexpr uint32_t kOne = 1U << 24;
  struct Stats {
    uint32_t received = 0, dropped = 0, underruns = 0, rendered = 0;
    uint32_t highWater = 0;
    int32_t correctionPpm = 0;
  } stats;

  void setActive(bool value) {
    active_ = value;
    reset();
    ++generation_;
  }
  void reset() {
    read_ = write_ = 0;
    phase_ = 0;
    primed_ = false;
    filtered_ = kTarget;
    integral_ = 0;
    tailSize_ = 0;
    stats.correctionPpm = 0;
  }
  uint32_t generation() const { return generation_; }
  size_t available() const { return write_ - read_; }
  bool active() const { return active_; }

  // Accept arbitrary USB chunks, including a stereo frame split across calls.
  void append(const void *data, size_t bytes) {
    if (!active_) return;
    auto p = static_cast<const uint8_t *>(data);
    while (bytes) {
      const size_t n = std::min(bytes, sizeof(tail_) - tailSize_);
      memcpy(tail_ + tailSize_, p, n);
      tailSize_ += n;
      p += n;
      bytes -= n;
      if (tailSize_ == sizeof(tail_)) {
        ++stats.received;
        if (available() == kCapacity) {
          ++stats.dropped;  // Drop complete NEW frames; never shift L/R.
        } else {
          memcpy(ring_[write_++ & (kCapacity - 1)], tail_, sizeof(tail_));
          stats.highWater = std::max<uint32_t>(stats.highWater, available());
        }
        tailSize_ = 0;
      }
    }
  }

  // One block is 1 ms of the selected I2S clock.
  bool render(int32_t output[kBlockFrames][2]) {
    memset(output, 0, kBlockFrames * 2 * sizeof(int32_t));
    if (!active_) return false;
    if (!primed_) {
      if (available() < kTarget) return false;
      primed_ = true;
      filtered_ = kTarget;
    }
    // Low-pass the bursty USB FIFO level; PI corrects oscillator mismatch.
    // Clamp to +/-5000 ppm. This is clock drift correction, not 44.1->48 kHz SRC.
    filtered_ += (static_cast<double>(available()) - filtered_) / 256.0;
    // Normalize level error to 48 kHz frames so PI dynamics stay the same.
    const double error = (filtered_ - kTarget) * (48000.0 / BRIDGE_SAMPLE_RATE);
    integral_ = std::max(-0.004, std::min(0.004, integral_ + error * 1e-9));
    const double correction = std::max(-0.005, std::min(0.005, error * 8e-6 + integral_));
    const uint32_t step = static_cast<uint32_t>(kOne * (1.0 + correction) + 0.5);
    stats.correctionPpm = static_cast<int32_t>(correction * 1e6);
    const size_t needed = ((static_cast<uint64_t>(phase_) + static_cast<uint64_t>(kBlockFrames) * step) >> 24) + 2;
    if (available() < needed) {
      ++stats.underruns;
      reset();  // Refill before restarting; never replay old samples.
      return false;
    }
    for (size_t i = 0; i < kBlockFrames; ++i) {
      const auto &a = ring_[read_ & (kCapacity - 1)];
      const auto &b = ring_[(read_ + 1) & (kCapacity - 1)];
      for (size_t ch = 0; ch < 2; ++ch) {
        const int64_t delta = static_cast<int64_t>(b[ch]) - a[ch];
        output[i][ch] = static_cast<int32_t>(a[ch] + delta * phase_ / kOne);
      }
      const uint32_t next = phase_ + step;
      read_ += next >> 24;
      phase_ = next & (kOne - 1);
    }
    stats.rendered += kBlockFrames;
    return true;
  }

 private:
  alignas(4) int32_t ring_[kCapacity][2] = {};
  uint32_t read_ = 0, write_ = 0, phase_ = 0, generation_ = 0;
  bool active_ = false, primed_ = false;
  double filtered_ = kTarget, integral_ = 0;
  uint8_t tail_[8] = {};
  size_t tailSize_ = 0;
};
