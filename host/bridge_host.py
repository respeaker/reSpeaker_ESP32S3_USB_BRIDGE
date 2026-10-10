#!/usr/bin/env python3
"""Device control over the ESP32-S3 USB audio + CDC / I2C bridge.

Loads command definitions from a JSON catalog or a Python PARAMETERS literal.
Only an explicit device status 64 permits retrying a control transaction.
"""

import argparse
import ast
import json
from pathlib import Path
import secrets
import struct
import sys
import time

MAX_PAYLOAD = 64
FORMATS = {"uint8": "B", "uint16": "H", "uint32": "I", "int32": "i",
           "float": "f", "radians": "f", "char": "B"}
STATUS_NAMES = {0: "SUCCESS", 2: "BAD_COMMAND", 3: "DATA_LENGTH_ERROR",
                5: "BAD_RESOURCE", 6: "MALFORMED_PACKET", 7: "COMMAND_IGNORED",
                64: "RETRY", 65: "WRONG_COMMAND_ID", 66: "WRONG_COMMAND_LEN",
                67: "WRONG_PAYLOAD", 68: "QUEUE_FULL", 71: "RESOURCE_ERROR"}


def load_parameters(source=None):
    """Read a JSON snapshot or safely extract a Python literal; never import it."""
    path = Path(source) if source else Path(__file__).with_name("parameters.json")
    if path.suffix == ".py":
        for node in ast.parse(path.read_text(encoding="utf-8")).body:
            if isinstance(node, ast.Assign) and any(
                    isinstance(t, ast.Name) and t.id == "PARAMETERS" for t in node.targets):
                parameters = ast.literal_eval(node.value)
                break
        else:
            raise ValueError("No literal PARAMETERS dictionary in source")
    else:
        parameters = json.loads(path.read_text(encoding="utf-8"))["parameters"]
    for name, entry in parameters.items():
        resid, cmd, count, access, kind, description = entry
        if (not isinstance(name, str) or not 0 <= resid <= 255 or not 0 <= cmd <= 127
                or access not in ("ro", "wo", "rw") or kind not in FORMATS
                or not 1 <= count * struct.calcsize(FORMATS[kind]) <= MAX_PAYLOAD):
            raise ValueError(f"Unsupported parameter definition: {name}")
    return parameters


def encode_values(entry, values):
    _, _, count, access, kind, _ = entry
    if access == "ro":
        raise ValueError("Command is read-only")
    if len(values) != count:
        raise ValueError(f"Expected {count} values, got {len(values)}")
    if kind in ("float", "radians"):
        parsed = [float(v) for v in values]
    else:
        # Never route integer values through float: uint32 and signed values
        # must retain their exact bit pattern; fractional inputs are rejected.
        parsed = [int(v[1:], 16) if v.startswith("$") else
                  int(v, 16) if v.lower().lstrip("+-").startswith("0x") else int(v, 10)
                  for v in values]
    return struct.pack("<" + FORMATS[kind] * count, *parsed)


def decode_values(entry, data):
    _, _, count, _, kind, _ = entry
    expected = count * struct.calcsize(FORMATS[kind])
    if len(data) != expected:
        raise ValueError(f"Expected {expected} data bytes, got {len(data)}")
    if kind == "char":
        return data.rstrip(b"\0").decode("utf-8", errors="replace")
    return struct.unpack("<" + FORMATS[kind] * count, data)


class BridgeError(RuntimeError):
    pass


class CdcBridge:
    """One outstanding request per port. A timeout leaves writes indeterminate."""

    def __init__(self, serial_port, timeout=2.0):
        self.serial = serial_port
        self.timeout = timeout
        self.sequence = secrets.randbelow(65535) + 1

    def exchange(self, operation, fields=""):
        sequence = self.sequence
        self.sequence = sequence % 65535 + 1
        request = f"{operation} {sequence}{(' ' + fields) if fields else ''}\n".encode("ascii")
        if self.serial.write(request) != len(request):
            raise BridgeError("Incomplete CDC write; outcome unknown, not retried")
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            line = self.serial.read_until(b"\n", 192)
            if not line:
                continue
            if not line.endswith(b"\n"):
                raise BridgeError("Truncated CDC response; outcome unknown, not retried")
            try:
                fields = line.decode("ascii").split()
                reply_sequence = int(fields[1])
            except (UnicodeError, ValueError, IndexError) as exc:
                raise BridgeError(f"Malformed CDC response: {line!r}") from exc
            if reply_sequence != sequence:
                continue  # Includes sequence=0 parser errors during initial resync.
            if len(fields) == 3 and fields[0] == "ERR":
                raise BridgeError(f"Bridge error {fields[2]}; outcome may be unknown, not retried")
            if len(fields) != 4 or fields[0] != "OK":
                raise BridgeError(f"Malformed CDC response: {line!r}")
            try:
                status = int(fields[2])
                data = b"" if fields[3] == "-" else bytes.fromhex(fields[3])
                if not 0 <= status <= 255 or len(data) > MAX_PAYLOAD:
                    raise ValueError("response out of range")
            except ValueError as exc:
                raise BridgeError(f"Malformed CDC response: {line!r}") from exc
            return status, data
        raise BridgeError("CDC response timed out; outcome unknown, not retried")

    def audio_status(self):
        status, payload = self.exchange("S")
        if status != 0 or len(payload) != 64:
            raise BridgeError("Audio status requires the diagnostic firmware (64-byte response)")
        names = ("flags", "usb_callbacks", "usb_bytes", "usb_peak", "capture_frames",
                 "rendered_frames", "tx_bytes", "tx_peak", "tx_dma_completions",
                 "tx_timeouts", "tx_errors", "fifo_frames", "dropped_frames",
                 "underruns", "mute_mask", "master_volume_db")
        values = list(struct.unpack("<16I", payload))
        if values[15] >= 0x80000000:
            values[15] -= 0x100000000
        return dict(zip(names, values))

    def usb_status(self):
        status, payload = self.exchange("U")
        if status != 0 or len(payload) not in (32, 64):
            raise BridgeError("USB status requires the USB OUT reader firmware")
        names = ("reader_loops", "fifo_high_water", "out_packets", "out_packet_bytes",
                 "out_endpoint", "read_bytes", "usb_flags", "reader_stack_min",
                 "doepctl1", "doeptsiz1", "doepint1", "gintsts", "gintmsk",
                 "dsts", "dctl", "grxfsiz")
        return dict(zip(names, struct.unpack("<%dI" % (len(payload) // 4), payload)))

    def handshake(self):
        # Newline resynchronizes the device after partial input or disconnect.
        # Only this local, side-effect-free PING may retry a transport timeout.
        for attempt in range(3):
            self.serial.reset_input_buffer()
            self.serial.write(b"\n")
            try:
                # Match the firmware handshake signature exactly (lowercase ASCII).
                if self.exchange("P") != (0, b"respeaker"):
                    raise BridgeError('Ping failed, please check whether "reSpeaker_ESP32S3_USB_BRIDGE.ino" was flashed successfully.')
                return
            except BridgeError:
                if attempt == 2:
                    raise
                time.sleep(0.05)

    def command(self, entry, values=None, attempts=100):
        resource, command, count, access, kind, _ = entry
        if values is None:
            if access == "wo":
                raise ValueError("Command is write-only; supply --values")
            operation = "R"
            body = str(count * struct.calcsize(FORMATS[kind]))
        else:
            operation = "W"
            body = encode_values(entry, values).hex() or "-"
        for attempt in range(attempts):
            status, data = self.exchange(operation, f"{resource} {command} {body}")
            if status == 0:
                if operation == "W":
                    if data:
                        raise BridgeError("Unexpected payload in write acknowledgement")
                    return None
                return decode_values(entry, data)
            if status != 64:
                raise BridgeError(f"Device status {status} ({STATUS_NAMES.get(status, 'UNKNOWN')})")
            if attempt + 1 < attempts:
                time.sleep(0.01)
        raise BridgeError(f"Device still reports RETRY after {attempts} attempts")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("COMMAND", nargs="?", type=str.upper)
    parser.add_argument("--port", help="e.g. /dev/cu.usbmodemXXXX, /dev/ttyACM0 or COM5")
    parser.add_argument("--values", nargs="+", help="write values; decimal, 0xFF, '$FF', float")
    parser.add_argument("-l", "--list", action="store_true")
    parser.add_argument("--list-ports", action="store_true")
    parser.add_argument("--usb-status", action="store_true", help="read TinyUSB OUT diagnostics; no I2C access")
    parser.add_argument("--audio-status", action="store_true", help="read ESP32 audio diagnostics; no I2C access")
    parser.add_argument("--ping", action="store_true", help="check ESP32 bridge only, without I2C")
    parser.add_argument("--parameters-source", help="alternate Python PARAMETERS source or parameters JSON")
    args = parser.parse_args()
    try:
        parameters = load_parameters(args.parameters_source)
        if args.list:
            for name, (resid, cmd, count, access, kind, description) in parameters.items():
                print(f"{name:38} {resid:3} {cmd:3} {access} {count:2} {kind:7} {description}")
            return 0
        if not args.list_ports:
            if not args.port:
                parser.error("--port is required (use --list-ports to discover ports)")
            if not args.ping and not args.audio_status and not args.usb_status:
                if args.COMMAND not in parameters:
                    parser.error("Specify a command from --list")
                entry = parameters[args.COMMAND]
                # Validate before opening the port or sending any request.
                if args.values is not None:
                    encode_values(entry, args.values)
                elif entry[3] == "wo":
                    raise ValueError("Command is write-only; supply --values")
        import serial
        if args.list_ports:
            from serial.tools import list_ports
            for port in list_ports.comports():
                print(f"{port.device}\t{port.description}\t{port.hwid}")
            return 0
        # Baud rate is CDC line coding only; it does not set I2C or audio clocks.
        with serial.Serial(args.port, 115200, timeout=0.25, write_timeout=2) as port:
            bridge = CdcBridge(port)
            bridge.handshake()
            if args.usb_status:
                print(json.dumps(bridge.usb_status(), indent=2))
                return 0
            if args.audio_status:
                print(json.dumps(bridge.audio_status(), indent=2))
                return 0
            if args.ping:
                print("Ping OK! (I2C device not probed)")
                return 0
            result = bridge.command(entry, args.values)
            if result is None:
                print(f"{args.COMMAND}: OK (device status 0)")
            else:
                print(f"{args.COMMAND}: {result}")
        return 0
    except ImportError:
        print("Install pyserial: python3 -m pip install -r requirements.txt", file=sys.stderr)
        return 1
    except (OSError, ValueError, SyntaxError, KeyError, struct.error, BridgeError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
