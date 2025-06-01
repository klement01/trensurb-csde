"""Writes ASDO/CSDE info to MIFARE Classic 1K tag."""

import argparse
import enum
import functools
import sys

import serial
import serial.tools.list_ports


def main() -> int:
    """Writes ASDO/CSDE info to MIFARE Classic 1K tag."""

    # Parse command line arguments.
    parser = argparse.ArgumentParser(prog="TagWriter")

    parser.add_argument("--baudrate", type=int, default=115_200)
    parser.add_argument("--port")

    parser.add_argument("--disable", action="store_true")
    parser.add_argument("--approach-direction", choices=["N", "S"], required=True)
    parser.add_argument("--version", type=int, default=0)
    parser.add_argument("--application-code", type=int, default=1)
    parser.add_argument(
        "--open-left", dest="csde", action="append_const", const=CSDE.LEFT
    )
    parser.add_argument(
        "--open-right", dest="csde", action="append_const", const=CSDE.RIGHT
    )
    parser.add_argument("--stop-zone-length", type=int, required=True)
    parser.add_argument("--station-id", type=int, required=True)
    parser.add_argument("--platform-id", type=int, required=True)

    args = parser.parse_args()

    # Generates block to be writen to tag.
    block_to_write = make_mifare_clasic_1k_asdo_csde_block(
        block_id=0,
        asdo_enabled=not args.disable,
        approach_direction=ApproachDirection.NORTH
        if args.approach_direction == "N"
        else ApproachDirection.SOUTH,
        version=args.version,
        application_code=args.application_code,
        csde=functools.reduce(lambda x, y: x | y, args.csde or [], CSDE.NONE),
        stop_zone_length=args.stop_zone_length,
        station_id=args.station_id,
        platform_id=args.platform_id,
    )

    # Try to find a connected Arduino.
    if args.port is None:
        arduino_ports = list(serial.tools.list_ports.grep("Arduino"))
        try:
            arduino_port = arduino_ports[0]
        except IndexError:
            print("No Arduino detected", file=sys.stderr)
            return 1
        else:
            port = arduino_port.device
    else:
        port = args.port

    with serial.Serial(port, args.baudrate) as ser:
        print("Connected to:", arduino_port)

        print(
            "Block to write:",
            " ".join(f"{hex(i).upper()[2:]:0>2}" for i in block_to_write),
        )

        # Wait for ready signal.
        writer_ready_signal = ser.readline().decode("utf-8").strip()
        if writer_ready_signal != "WRITER READY":
            print("Writer not detected", file=sys.stderr)
            return 1

        # Send bytes and check for echo.
        print("Writer detected, sending block...")
        ser.write(block_to_write)

        block_echo = ser.read(len(block_to_write))
        if block_to_write != block_echo:
            ser.write(0x00)
            print("Block echo failed", file=sys.stderr)
            return 1

        ser.write([0x40])  # Magic OK code.
        print("Block sent, waiting for tag...")

        # Waits for ending signal.
        writer_done_signal = ser.readline().decode("utf-8").strip()
        if writer_done_signal != "WRITE DONE":
            print("Error while writing:", writer_done_signal, file=sys.stderr)
            return 1
        print("Done!")

    return 0


class ApproachDirection(enum.Enum):
    """Direction train approaches station."""

    NORTH = int("01", base=2)
    SOUTH = int("10", base=2)


class CSDE(enum.Flag):
    """Doors enabled in stopping zone."""

    NONE = int("00", base=2)
    LEFT = int("01", base=2)
    RIGHT = int("10", base=2)
    BOTH = int("11", base=2)


def make_mifare_clasic_1k_asdo_csde_block(
    block_id: int,
    asdo_enabled: bool,
    approach_direction: ApproachDirection,
    version: int,
    application_code: int,
    csde: CSDE,
    stop_zone_length: int,
    station_id: int,
    platform_id: int,
) -> bytes:
    """Make a MIFARE Classic 1K block for ASDO/CSDE application
    following the specification outlined in the project."""

    b = bytearray(16)

    b[0] |= block_id << 4 & 0xF0
    b[0] |= asdo_enabled << 3 & 0x08
    b[0] |= approach_direction.value << 1 & 0x06
    b[0] |= version >> 1 & 0x01

    b[1] |= version << 7 & 0x80
    b[1] |= application_code & 0x7F

    b[2] |= csde.value << 6 & 0xC0
    b[2] |= stop_zone_length >> 4 & 0x3F

    b[3] |= stop_zone_length << 4 & 0xF0
    b[3] |= station_id >> 11 & 0x0F

    b[4] |= station_id >> 3 & 0xFF

    b[5] |= station_id << 5 & 0xF0
    b[5] |= platform_id & 0x0F
    
    return bytes(b)


if __name__ == "__main__":
    sys.exit(main())
