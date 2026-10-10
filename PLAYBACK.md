# USB playback and I2S audio

**English** | [中文](playback_cn.md)

The firmware provides stereo USB capture and playback alongside CDC control.
USB and I2S use the same nominal sample rate, selected at build time in
`bridge_config.h`. Both directions carry signed 32-bit PCM in left/right order.
See [README.md](README.md) for mode selection and build instructions.

## Wiring and audio modes

All pin directions below are relative to the ESP32-S3.

| ESP32-S3 pin | Connected signal | Direction |
| --- | --- | --- |
| GPIO8 | BCLK | Input when reSpeaker XVF3800 is master; output when ESP32-S3 is master |
| GPIO7 | WS / LRCLK | Input when reSpeaker XVF3800 is master; output when ESP32-S3 is master |
| GPIO43 | Audio DOUT | Input: reSpeaker XVF3800 to USB capture |
| GPIO44 | reSpeaker XVF3800 audio DIN and codec audio DIN | Output: the same USB playback stream to both receivers |
| GND | GND | Common ground |

I2C uses GPIO5 for SDA and GPIO6 for SCL; see [CONTROL.md](CONTROL.md).
Use 3.3 V signal levels. Native USB D−/D+ are GPIO19/20; connect through the
board's native USB interface rather than an external USB-to-UART adapter.
The ESP32-S3 bridge does not connect or generate MCLK; any codec MCLK requirement
must be met by the board's clocking arrangement.

The format is standard Philips I2S: two 32-bit slots per frame, WS low for left,
WS high for right, and the MSB one BCLK after the WS transition.

| Sample rate | BCLK | WS | Frames per 1 ms block | Playback prefill |
| --- | --- | --- | --- | --- |
| 48 kHz | 3.072 MHz | 48 kHz | 48 | 576 frames / 12 ms |
| 16 kHz | 1.024 MHz | 16 kHz | 16 | 192 frames / 12 ms |

For reSpeaker XVF3800 master modes, the default configuration uses I2S0 TX and I2S1 RX as
ESP32-S3 slaves sharing external BCLK and WS. For reSpeaker XVF3800 slave mode, paired I2S0
TX/RX channels make the ESP32-S3 the clock master. Configure the reSpeaker XVF3800 for the
matching role and rate before use; only one device may drive BCLK and WS.

GPIO43 and GPIO44 remain the capture input and playback output in every mode.
Change the pin constants in `reSpeaker_ESP32S3_USB_BRIDGE.ino` for different
hardware. Never connect two audio outputs together.

## Playback path, codec configuration, and host setup

Host playback audio arrives over USB and is transmitted on the ESP32-S3's I2S
output (GPIO44). This data line feeds the reSpeaker XVF3800 audio input and the codec audio
input in parallel, so both receive the same playback stream. The reSpeaker XVF3800 uses
the stream as the far-end reference, and the codec converts it for playback.

The reSpeaker XVF3800 firmware initializes and configures the codec. The ESP32-S3 bridge
supplies the I2S audio data; the codec's playback data comes directly from that
shared data line, rather than being forwarded through the reSpeaker XVF3800.

Correct I2S output from the ESP32-S3 does not by itself guarantee speaker output.
If playback is silent, verify GPIO44 at both receivers, the required codec clocks,
and codec initialization, output configuration, mute, and volume in the reSpeaker XVF3800
firmware. Check the reSpeaker XVF3800 reference input configuration separately.

After flashing or changing modes, reconnect USB and reselect the input, output,
and CDC port. Product names depend on the selected mode:

| Mode | USB product name |
| --- | --- |
| reSpeaker XVF3800 master, 48 kHz | `BRIDGE I2S Master 48k` |
| reSpeaker XVF3800 master, 16 kHz | `BRIDGE I2S Master 16k` |
| reSpeaker XVF3800 slave, 16 kHz | `BRIDGE I2S Slave 16k` |

Each mode has a distinct USB serial string. Some host audio menus display
`TinyUSB UAC1` instead of the USB product name. Select stereo 32-bit audio at the
compiled sample rate. Master/channel volume and mute apply to playback only.

## Playback buffering and clock matching

A dedicated USB reader task is the sole consumer of the TinyUSB audio OUT FIFO.
It starts after `USB.begin()`, waits for USB configuration and audio-class mount,
and performs bounded reads before submitting PCM to the playback buffer.
The firmware does not call `gUac.begin()`, which would create another FIFO
consumer in the pinned core. USBAudioCard still provides audio registration,
capture writes, and volume/mute handling.

Playback processing works as follows:

1. Incoming bytes are assembled into complete stereo frames and placed in a
   4096-frame application FIFO (32 KiB).
2. The TX task follows I2S DMA consumption in 1 ms blocks. Playback starts after
   12 ms of data has accumulated.
3. A filtered FIFO level drives a PI controller that adjusts the fractional
   sampling step within ±5000 ppm. Both channels share the same interpolation
   phase, using 24-bit fractional precision and 64-bit intermediate arithmetic.
4. USB master and per-channel volume/mute are applied after resampling.

USB playback uses an adaptive OUT endpoint and capture uses an asynchronous IN
endpoint. There is no explicit playback feedback endpoint. Software rate
correction handles small differences between clocks at the same nominal rate;
it does not provide arbitrary sample-rate conversion.

**Playback is not bit-perfect.** Linear interpolation changes samples and affects
high-frequency response. It is a clock drift correction mechanism, not a
band-limited asynchronous sample-rate converter.

The buffering contribution includes approximately 12 ms of application prefill
and up to approximately 8 ms of TX DMA, plus host and USB buffering. This is not
a measured end-to-end latency and excludes reSpeaker XVF3800 and codec processing.

Capture preserves the incoming 32-bit samples without playback volume or
software resampling. Its application ring holds 2048 frames. Data received while
capture is inactive is discarded; USB backpressure is handled separately.
An absent input clock does not cause the firmware to synthesize capture samples.

## Stop, underrun, and recovery

- Inactive playback, suspend, and USB disconnection result in silence on TX.
- A playback session change disables TX, fills TX DMA with zeros, and restarts
  TX without stopping RX. Already transmitted samples cannot be recalled.
- DMA automatic clearing prevents old blocks from being replayed indefinitely
  when the task cannot supply new data.
- FIFO underruns clear the remaining data and restart prebuffering. Overflow
  drops complete new frames and increments a counter.
- I2S writes have a 20 ms timeout. A write failure clears playback buffering and
  resets TX DMA; when transmission resumes, playback prebuffers again.
- CDC/I2C work runs in a separate task and is not performed in audio callbacks.

Debugger statistics include `gPlayback.stats.received`, `rendered`, `dropped`,
`underruns`, `highWater`, and `correctionPpm`. `gPlayback.available()` reports
queued frames. `gStats.txTimeouts` and `txErrors` count I2S write failures.

## Audio status

Run these commands from `host`, replacing the port with your device:

```sh
python3 bridge_host.py --list-ports
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --audio-status
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --usb-status
```

Both queries read ESP32 state without I2C access or volume changes. Compare
snapshots taken roughly one second apart while playing non-silent audio.

The `S seq` audio response contains these fields in wire order. Each occupies
one little-endian 32-bit word; only `master_volume_db` is signed.

| Field | Meaning |
| --- | --- |
| `flags` | Bit 0: playback active; bit 1: separate I2S controllers. Active playback normally gives 3 in the default reSpeaker XVF3800 master arrangement, or 1 in reSpeaker XVF3800 slave mode. |
| `usb_callbacks` | Number of PCM submissions from the dedicated USB reader to the playback buffer |
| `usb_bytes` | Bytes submitted to the playback buffer |
| `usb_peak` | Sample peak of the most recently submitted USB block |
| `capture_frames` | Frames read from I2S RX |
| `rendered_frames` | Playback frames produced by the resampler |
| `tx_bytes` | Bytes handed to the I2S driver, including silence; not confirmed wire output |
| `tx_peak` | Peak of the latest generated TX block after volume processing |
| `tx_dma_completions` | TX DMA completion callback count |
| `tx_timeouts` | I2S TX timeouts |
| `tx_errors` | Other I2S TX errors |
| `fifo_frames` | Current playback FIFO occupancy in frames |
| `dropped_frames` | Frames dropped on playback FIFO overflow |
| `underruns` | Playback FIFO underrun count |
| `mute_mask` | Bit 0: master mute; bit 1: left mute; bit 2: right mute |
| `master_volume_db` | Master volume in dB; 0 means no attenuation. Per-channel volumes are not included. |

Counters can wrap at 32 bits. Peaks describe recent blocks and may be zero during
silent passages. Snapshots use separate locks for different modules and are not
atomic across the complete audio pipeline. Nonzero software peaks and DMA
activity do not prove correct external I2S reception; verify the physical signal
when diagnosing a routing or wiring problem.

## USB OUT status

The `U seq` response contains these unsigned little-endian 32-bit fields in wire
order. Current firmware returns all 16; the host accepts older responses with
only the first eight.

| Field | Meaning |
| --- | --- |
| `reader_loops` | Dedicated USB reader loop count |
| `fifo_high_water` | Largest observed TinyUSB OUT FIFO occupancy, in bytes |
| `out_packets` | Audio receive callback count after FIFO reception and transfer rearming |
| `out_packet_bytes` | Bytes reported through those receive callbacks |
| `out_endpoint` | Endpoint of the latest receive callback; expected to be 1 during playback |
| `read_bytes` | Total bytes read from the TinyUSB OUT FIFO |
| `usb_flags` | Bit 0: USB configured; bit 1: audio class mounted; bit 2: suspended. Normally 3 while running. |
| `reader_stack_min` | Minimum remaining reader-task stack, in the units of the ESP-IDF FreeRTOS API |
| `doepctl1` | OUT endpoint 1 control register |
| `doeptsiz1` | OUT endpoint 1 transfer-size register |
| `doepint1` | OUT endpoint 1 interrupt register |
| `gintsts` | Global interrupt status register |
| `gintmsk` | Global interrupt mask register |
| `dsts` | Device status register |
| `dctl` | Device control register |
| `grxfsiz` | Hardware receive FIFO size in 32-bit words |

Register reads do not pop FIFO data or change hardware state and are not atomic
across registers. Packet counts are software callbacks, not USB analyzer
measurements. An endpoint value of 0 before any callback does not mean the
endpoint is absent.

If reader loops increase but packet counts and FIFO occupancy remain zero,
check host playback, endpoint setup, and the USB stack. If packets arrive but
`read_bytes` does not grow, investigate FIFO reading. If reads grow, use audio
status to check prebuffering, rendering, mute, volume, and TX progress.

## Hardware USB FIFO budget

The ESP32-S3 USB controller has 256 words of FIFO RAM. The project-local TinyUSB
0.21.0 DWC2 driver reserves one maximum receive packet plus setup/status space.
CDC notification packets are 16 bytes and CDC bulk packets are 32 bytes, leaving
room for stereo audio in both directions.

| Sample rate | Maximum audio packet | RX FIFO | Total RX + IN allocation |
| --- | --- | --- | --- |
| 48 kHz | 392 bytes | 127 words | 253 / 256 words |
| 16 kHz | 136 bytes | 63 words | 125 / 256 words |

These hardware FIFOs are separate from the application audio buffers. Do not add
USB classes or increase packet sizes without recalculating the allocation.
Single-packet RX buffering has less interrupt-latency tolerance than double
buffering, so validate duplex audio with concurrent CDC traffic.

The descriptor test uses the same configuration definition as the firmware.
When changing core/toolchain versions, verify that the local DWC2 object is
linked instead of the prebuilt SDK driver. See
[src/tinyusb/dwc2/README.md](src/tinyusb/dwc2/README.md) for source provenance and
the override mechanism.

## Validation

Project records report working 48 kHz capture, playback, and CDC control on
hardware. The 16 kHz modes still require hardware validation; selecting a mode
in the source does not establish that it has been tested.

The playback simulation covers frame assembly, interpolation, silence on stop,
overflow, underrun recovery, and three simulated minutes at each of 0, ±100, and
±1000 ppm clock drift with USB data arriving in 2 ms bursts.

Run it from the repository root with a C++17 compiler that supports the undefined
behavior sanitizer. The following explicitly selects the 48 kHz test mode:

```sh
c++ -std=c++17 -O2 -fsanitize=undefined -Wall -Wextra -Werror \
  -DBRIDGE_MODE=0 tests/playback_test.cpp -o /tmp/xvf-playback-test
/tmp/xvf-playback-test
```

Repeat with `-DBRIDGE_MODE=1` and `-DBRIDGE_MODE=2` to exercise the 16 kHz playback
configuration. These are software simulations, not tests of physical I2S roles.
Control and Python test commands are in [CONTROL.md](CONTROL.md#tests-and-hardware-checks).

For hardware acceptance:

1. Confirm that both USB audio input and output enumerate at the compiled rate.
2. Play distinct left/right tones. Decode GPIO44 with a logic analyzer to verify
   Philips I2S framing, slot width, channel order, and levels at both the reSpeaker XVF3800
   and codec inputs. Check reSpeaker XVF3800 reference reception and codec/speaker output,
   including codec configuration by the reSpeaker XVF3800 firmware.
3. Verify capture with known stereo input, and check playback volume and mute.
4. Run simultaneous capture and playback for at least 30 minutes while reading
   supported reSpeaker XVF3800 parameters over CDC. Watch FIFO drops, underruns, I2S errors,
   and channel continuity.
5. Exercise playback-only and capture-only operation, stream close/reopen, USB
   reconnect, host suspend/resume, and external clock loss/recovery in reSpeaker XVF3800 master
   modes. Check that control remains usable and stale audio does not repeat.

On Linux, `aplay -l` and `arecord -l` list devices. For a 48 kHz build, example
commands are:

```sh
aplay -D hw:CARD,DEV stereo_48k_s32.wav
arecord -D hw:CARD,DEV -t wav -f S32_LE -r 48000 -c 2 -d 10 capture.wav
```

Replace `CARD,DEV` with the device selection. Supply a stereo signed 32-bit WAV
at the compiled rate; for a 16 kHz build, use a 16 kHz file and `-r 16000`.
