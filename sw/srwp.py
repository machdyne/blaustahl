#!/usr/bin/env python3

# Python example for using SRWP to communicate with Blaustahl
# see https://github.com/binqbit/serialport_srwp for protocol details

import serial
import logging
import glob

class BlaustahlSRWP:
    logger = logging.getLogger(__name__)

    def __init__(self, device:str|None='/dev/ttyACM0', fram_size:int=8192):
        if device is None:
            device = self.find_device()

        self.srwp = serial.Serial(device, 115200, timeout=1, rtscts=False, dsrdtr=False)
        self.fram_size = fram_size

    @staticmethod
    def find_device():
        """
        Finds the first available /dev/ttyACM device.
        :return: The path to the device as a string.
        :raises FileNotFoundError: If no device is found.
        """
        devices = glob.glob('/dev/ttyACM*')
        if not devices:
            raise FileNotFoundError("No /dev/ttyACM device found.")
        return devices[0]

    def flush(self):
        while self.srwp.in_waiting:
            data = self.srwp.read(4096)
            self.logger.debug(f"Flushed Data: {data}")

    def echo(self, msg):
        self.flush()

        ba = bytearray()
        ml = len(msg)

        ba.extend(b'\x00')    # Enter SRWP mode
        ba.extend(b'\x00')    # Command: Echo
        ba.extend(ml.to_bytes(4, byteorder='little'))
        ba.extend(msg.encode(encoding="ascii"))

        self.srwp.write(ba)
        self.srwp.flush()

        data = self.srwp.read(ml)
        print(data)

    def read_fram(self, addr:int, size:int):
        """
        Reads `size` bytes from address `addr` on the FRAM chip.
        :param addr: Target address (4 bytes)
        :param size: Number of bytes to read
        """
        self.flush()

        ba = bytearray()
        ba.extend(b'\x00')    # Enter SRWP mode
        ba.extend(b'\x01')    # Command: Read FRAM
        ba.extend(addr.to_bytes(4, byteorder='little'))    # 4-byte address
        ba.extend(size.to_bytes(4, byteorder='little'))    # 4-byte size

        self.srwp.write(ba)
        self.srwp.flush()

        data = self.srwp.read(size)
        return data

    def write_fram(self, addr:int, data:bytes|bytearray):
        """
        Writes data to address `addr` on the FRAM chip.
        :param addr: Target address (4 bytes)
        :param data: Data to write (bytes or bytearray)
        """
        self.flush()

        ba = bytearray()
        ba.extend(b'\x00')    # Enter SRWP mode
        ba.extend(b'\x02')    # Command: Write FRAM
        ba.extend(addr.to_bytes(4, byteorder='little'))    # 4-byte address
        ba.extend(len(data).to_bytes(4, byteorder='little'))    # Data length
        ba.extend(data)    # Add data

        self.logger.debug(f"Send to FRAM: {ba.hex()}")

        self.srwp.write(ba)
        self.srwp.flush()

    def read_fram_all(self):
        """
        Reads the entire content of the FRAM chip.
        :return: All data on the FRAM chip as bytes.
        """
        return self.read_fram(0, self.fram_size)

    def clear_fram(self):
        """
        Clears the entire FRAM chip.
        """
        for addr in range(self.fram_size + 1):
            self.write_fram(addr, b'\x00')

    def is_fram_empty(self):
        """
        Checks if the entire FRAM is empty (filled with '\x00').
        :return: True if the FRAM is empty, False otherwise.
        """
        data = self.read_fram_all()
        return all(byte == 0 for byte in data)

# Main program
if __name__ == "__main__":
    from argparse import ArgumentParser

    parser = ArgumentParser(description="CLI tool for interacting with Blaustahl Storage Device using the SRWP protocol.")
    parser.add_argument("--device", type=str, default=None, help="Path to the serial device (e.g., /dev/ttyACM0). Defaults to auto-detection.")

    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # Echo command
    parser_echo = subparsers.add_parser("echo", help="Echo data back to you over the FRAM")
    parser_echo.add_argument("message", type=str, help="Message to send and receive as echo")

    # Read command
    parser_read = subparsers.add_parser("read", help="Read data from FRAM")
    parser_read.add_argument("address", type=int, help="Address to start reading from")
    parser_read.add_argument("size", type=int, help="Number of bytes to read")

    # Write command
    parser_write = subparsers.add_parser("write", help="Write data to FRAM")
    parser_write.add_argument("address", type=int, help="Address to start writing to")
    parser_write.add_argument("data", type=str, help="Data to write (as a string)")

    # Clear FRAM command
    parser_clear = subparsers.add_parser("clear", help="Clear the entire FRAM")

    # Check FRAM empty command
    parser_check = subparsers.add_parser("check", help="Check if the FRAM is empty")

    args = parser.parse_args()

    # Create an instance of BlaustahlSRWP
    bs = BlaustahlSRWP(device=args.device)

    # Execute based on the parsed arguments
    if args.command == "echo":
        print(f"Echoing: {args.message}")
        bs.echo(args.message)

    elif args.command == "read":
        print(f"Reading {args.size} bytes from address {args.address}")
        data = bs.read_fram(args.address, args.size)
        print(f"Data: {data}")

    elif args.command == "write":
        print(f"Writing data to address {args.address}")
        bs.write_fram(args.address, args.data.encode('ascii'))
        print("Write complete.")

    elif args.command == "clear":
        print("Clearing FRAM...")
        bs.clear_fram()
        print("FRAM cleared.")

    elif args.command == "check":
        print("Checking if FRAM is empty...")
        if bs.is_fram_empty():
            print("FRAM is empty.")
        else:
            print("FRAM is not empty.")

    else:
        parser.print_help()
