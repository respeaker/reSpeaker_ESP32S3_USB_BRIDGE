# reSpeaker XVF3800 control over USB CDC and I2C

**English** | [中文](control_cn.md)

The bridge exposes a USB CDC serial interface alongside USB audio capture and
playback. Control requests travel from the computer through the ESP32-S3 to the
reSpeaker XVF3800 over I2C. The ESP32-S3 is always the I2C master, regardless of the selected
I2S audio mode.

## Wiring and configuration

| Signal | ESP32-S3 | reSpeaker XVF3800 |
| --- | --- | --- |
| SDA | GPIO5 / XIAO D4 | Control interface SDA |
| SCL | GPIO6 / XIAO D5 | Control interface SCL |
| Ground | GND | GND |

The defaults in `respeaker_control.cpp` are a **7-bit address of 0x2C**, a 100 kHz bus,
and a 100 ms I2C timeout. Change `kSdaPin`, `kSclPin`, or `kRespeakerAddress` there if
required. Do not shift the address.

Connect to the reSpeaker XVF3800 I2C **slave control interface**, not the bus on which the
reSpeaker XVF3800 controls its codec as master. Use a common ground and SDA/SCL pull-ups to
3.3 V. Existing board pull-ups may be sufficient; otherwise, 4.7 kΩ is a starting
point. Avoid connecting another active master to the same control bus.

The reSpeaker XVF3800 firmware must enable I2C control and implement the requested commands.
The bridge does not write reSpeaker XVF3800 parameters at startup.

Build settings are documented in [README.md](README.md). Select USB-OTG (TinyUSB)
and disable CDC On Boot: the firmware explicitly creates `USBCDC gControlCdc`.
The control port carries protocol messages, not debug logs, and does not use
UART0 on GPIO43/44.

CDC bootloader reset is enabled. Arduino's 1200-baud request or the core's
recognized DTR/RTS reset sequence restarts the board into ROM download mode.
Use 115200 baud for normal control traffic. Uploading interrupts audio and
re-enumerates USB; after flashing and restarting, audio and control CDC return.
USB starts before I2S initialization so an I2S initialization failure still
leaves the CDC bootloader request available.

For the first upload from firmware with CDC reset disabled, or if the application
cannot service USB, hold BOOT, press and release RESET, then release BOOT and
select the download port. Automatic port switching depends on the host upload
tool and should be verified on hardware.

## Python host utility

From the repository root, enter `host` and install the dependency:

```sh
cd host
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
python3 bridge_host.py --list-ports
python3 bridge_host.py --list
```

On Windows Command Prompt, activate the environment with
`.venv\Scripts\activate.bat` instead of `source`.

Run the following examples from `host`. Replace `/dev/cu.usbmodemXXXX` with your
port, such as `/dev/ttyACM0` on Linux or `COM5` on Windows. Close other programs
that have the port open.

```sh
# Check the ESP32 bridge without accessing I2C.
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --ping

# Read the reSpeaker XVF3800 firmware version.
python3 bridge_host.py --port /dev/cu.usbmodemXXXX VERSION

# Read and write parameters.
python3 bridge_host.py --port /dev/cu.usbmodemXXXX AUDIO_MGR_MIC_GAIN
python3 bridge_host.py --port /dev/cu.usbmodemXXXX AUDIO_MGR_MIC_GAIN --values 1.0
python3 bridge_host.py --port /dev/cu.usbmodemXXXX PP_AGCONOFF --values 1
python3 bridge_host.py --port /dev/cu.usbmodemXXXX AEC_FIXEDBEAMSAZIMUTH_VALUES --values 0.0 1.5707963

# These commands require reSpeaker XVF3800 firmware that implements them.
python3 bridge_host.py --port /dev/cu.usbmodemXXXX DOA_VALUE
python3 bridge_host.py --port /dev/cu.usbmodemXXXX LED_COLOR --values 0x00FF00

# Read ESP32 diagnostics without accessing I2C.
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --audio-status
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --usb-status
```

Command names are case-insensitive. `host/parameters.json` contains a snapshot
of 120 parameter definitions extracted from `xvf_host.py`. A catalog entry does
not guarantee that the connected reSpeaker XVF3800 firmware implements that command.

Supported types are `uint8`, `uint16`, `uint32`, `int32`, `float`, `radians`, and
`char`. Values use little-endian encoding. The utility checks value counts,
read/write permissions, and integer ranges; the device checks command-specific
semantic limits. Pass angles in radians. Fractional values are rejected for
integer parameters rather than truncated.

To use another parameter catalog:

```sh
python3 bridge_host.py --parameters-source /path/to/xvf_host.py \
  --port /dev/cu.usbmodemXXXX VERSION
```

The utility extracts the `PARAMETERS` literal without executing or importing the
source script. An alternate parameters JSON file is also supported. PyUSB and
libusb are not required. The original script's USB vendor transfers cannot
address this CDC bridge; use `bridge_host.py` with `--port`.

## Serial protocol v1

The port uses 115200 baud as CDC line coding; this does not set the USB audio
rate or I2C clock. Assert DTR, send an empty line to synchronize after opening,
and terminate requests with LF or CRLF. Allow only one outstanding request and
one program using the port. The firmware sends no startup banner.

Requests are ASCII:

```text
P seq
R seq resid cmd data_length
W seq resid cmd hex_payload
S seq
U seq
```

| Field or command | Meaning |
| --- | --- |
| `seq` | Decimal sequence number, 1–65535 |
| `resid` | Decimal resource ID, 0–255 |
| `cmd` | Decimal command ID, 0–127, without the I2C read flag |
| `data_length` | 1–64 data bytes, excluding the status byte |
| `hex_payload` | Up to 64 bytes of contiguous hexadecimal digits, without `0x`; `-` means an empty write |
| `P` | Bridge ping; no I2C access |
| `S` | Audio status; no I2C access |
| `U` | USB OUT status; no I2C access |

Responses are:

```text
OK seq status hex_data
ERR seq bridge_error
```

For I2C commands, `OK` means the transport completed; **status 0 is required for
command success**. A successful write returns `OK seq 0 -`. A successful read
returns hexadecimal data. Nonzero reSpeaker XVF3800 status responses carry `-` instead of data.
Ping returns `OK seq 0 7265737065616b6572`, where the payload is ASCII `respeaker`, even if the
reSpeaker XVF3800 is disconnected. Errors without a recoverable sequence number use 0.

The host and firmware must both use the `respeaker` handshake signature.
Reflash older firmware before using this host tool; the previous signature is
not accepted.

Example version read (the example response represents version 3.2.1):

```text
R 1 48 0 3
OK 1 0 030201
```

Example AGC enable:

```text
W 2 17 10 01000000
OK 2 0 -
```

`S` returns 64 bytes: 16 little-endian 32-bit fields, with the final field signed.
`U` returns 64 bytes: 16 little-endian unsigned 32-bit fields. The host also
accepts older 32-byte USB status responses. Field definitions and interpretation
are in [PLAYBACK.md](PLAYBACK.md#audio-status).

Invalid characters or excessive line length produce `BAD_LINE`; malformed
requests produce `BAD_REQUEST`. An incomplete line with no new byte for one
second produces `RX_TIMEOUT`, after which input is discarded through the next
newline. Disconnects and CDC RX overflow also require newline resynchronization.
The host utility synchronizes and pings automatically.

Sequence numbers match replies to requests. They do not deduplicate writes or
guarantee exactly-once execution across disconnects.

## I2C transactions and retries

- **Read:** write `[resid, cmd | 0x80, data_length + 1]`, issue a repeated START,
  then read `data_length + 1` bytes. The first byte is status, followed by data.
- **Write:** write `[resid, cmd, payload_length, payload...]`, issue STOP, then
  perform a separate one-byte status read.
- The firmware forwards reSpeaker XVF3800 status without retrying. The Python utility retries
  only explicit status `64 / RETRY`, with 10 ms between attempts and a default
  limit of 100 total attempts. Other device statuses fail immediately.
- `I2C_WRITE_2` usually indicates an address NACK; `I2C_WRITE_5` indicates a write
  timeout. `I2C_READ_n` means the received byte count differed from the expected
  count. A repeated-START failure may appear as `I2C_READ_0` because the Wire API
  cannot distinguish every cause on this path.
- Transport failures and response timeouts do not automatically reissue device
  writes. A command may already have executed before its reply was lost.
  Check the device state before manually retrying. The local ping handshake may
  retry because it has no device side effects.

A dedicated control task on core 0, priority 1, serializes I2C and CDC work.
Audio callbacks do not execute I2C requests. Partial CDC replies are resumed
with a bounded 250 ms send window, and replies from an old CDC session are
suppressed after a session change.

## Tests and hardware checks

Run the host tests and firmware control simulation from the repository root
with Python 3 and a C++17 compiler. These tests require neither a board nor
pyserial and do not send commands to hardware.

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
c++ -std=c++17 -Wall -Wextra -Werror \
  -I tests/fakes tests/control_test.cpp -o /tmp/xvf-control-test
/tmp/xvf-control-test
```

The tests cover parameter encoding, permissions, retries, response matching,
timeouts, diagnostic decoding, parser boundaries, I2C transaction ordering,
short reads, error handling, partial CDC writes, and session changes.

On hardware, verify audio and CDC enumeration, ping, a `VERSION` read, and a
supported parameter write followed by readback. Exercise control traffic during
simultaneous capture and playback. Check recovery from I2C disconnection, USB
reconnection, and host suspend/resume. Audio validation status is recorded in
[PLAYBACK.md](PLAYBACK.md#validation).
