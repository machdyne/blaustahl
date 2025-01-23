#!/usr/bin/env python3

# Python example for using SRWP to communicate with Blaustahl
# see https://github.com/binqbit/serialport_srwp for protocol details

import serial
import logging
import glob
import time

class BlaustahlSRWP:
    logger = logging.getLogger(__name__)

    def __init__(self, device:str|None='/dev/ttyACM0', fram_size:int=8192):
        self.fram_size = fram_size

        if device is None:
            device = self.find_device()

        self.srwp = serial.Serial(device, 115200, timeout=1, rtscts=False, dsrdtr=False)
        self.srwp.flush()

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
        time.sleep(0.05)  # Pause to ensure the buffer is ready

        ba = bytearray()
        ba.extend(b'\x00')    # Enter SRWP mode
        ba.extend(b'\x01')    # Command: Read FRAM
        ba.extend(addr.to_bytes(4, byteorder='little'))    # 4-byte address
        ba.extend(size.to_bytes(4, byteorder='little'))    # 4-byte size

        self.srwp.write(ba)
        self.srwp.flush()
        time.sleep(0.05)  # Pause to allow FRAM to process

        data = self.srwp.read(size)
        return data

    def write_fram(self, addr:int, data:bytes|bytearray):
        """
        Writes data to address `addr` on the FRAM chip.
        :param addr: Target address (4 bytes)
        :param data: Data to write (bytes or bytearray)
        """
        self.flush()
        time.sleep(0.05)  # Pause to ensure the buffer is ready

        ba = bytearray()
        ba.extend(b'\x00')    # Enter SRWP mode
        ba.extend(b'\x02')    # Command: Write FRAM
        ba.extend(addr.to_bytes(4, byteorder='little'))    # 4-byte address
        ba.extend(len(data).to_bytes(4, byteorder='little'))    # Data length
        ba.extend(data)    # Add data

        self.logger.debug(f"Send to FRAM: {ba.hex()}")

        self.srwp.write(ba)
        self.srwp.flush()
        time.sleep(0.1)  # Pause to ensure data is written to FRAM

    def read_fram_retry(self, addr: int, size: int, max_retries: int = 3):
        """
        Reads `size` bytes from address `addr` on the FRAM chip with retries.
        :param addr: Starting address
        :param size: Total number of bytes to read
        :param max_retries: Maximum number of retries for incomplete reads
        :return: All data as bytes
        """
        for attempt in range(max_retries):
            data = self.read_fram(addr, size)
            if len(data) == size:
                return data  # Successful read
            self.logger.warning(f"Incomplete read: Expected {size}, got {len(data)}. Retrying... (Attempt {attempt + 1})")
            time.sleep(0.1)  # Short pause before retry
        raise IOError(f"Failed to read {size} bytes from FRAM after {max_retries} attempts")

    def read_fram_all(self):
        """
        Reads the entire content of the FRAM chip with retries.
        :return: All data on the FRAM chip as bytes.
        """
        data = bytearray()
        for offset in range(0, self.fram_size, 256):  # Default chunk size
            chunk = self.read_fram_retry(offset, min(256, self.fram_size - offset))
            data.extend(chunk)
        return bytes(data)

    def write_fram_all(self, data:bytes|bytearray):
        """
        Writes the entire content to the FRAM chip.
        :param data: Data to write (must match the size of the FRAM).
        """
        if len(data) != self.fram_size:
            raise ValueError("Data size must match the size of the FRAM.")

        self.write_fram_in_chunks(0, data)

    def read_fram_in_chunks(self, addr:int, size:int, chunk_size:int=256):
        """
        Reads `size` bytes from address `addr` on the FRAM chip in chunks.
        :param addr: Starting address
        :param size: Total number of bytes to read
        :param chunk_size: Size of each chunk to read
        :return: All data as bytes
        """
        data = bytearray()
        for offset in range(0, size, chunk_size):
            time.sleep(0.1)  # Pause between chunk reads
            chunk = self.read_fram(addr + offset, min(chunk_size, size - offset))
            data.extend(chunk)
        return bytes(data)

    def write_fram_in_chunks(self, addr:int, data:bytes|bytearray, chunk_size:int=256):
        """
        Writes data to the FRAM chip in chunks.
        :param addr: Starting address
        :param data: Data to write (bytes or bytearray)
        :param chunk_size: Size of each chunk to write
        """
        for offset in range(0, len(data), chunk_size):
            time.sleep(0.1)  # Pause between chunk writes
            chunk = data[offset:offset + chunk_size]
            self.write_fram(addr + offset, chunk)

    # Helper Functions
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

    def verify_fram(self, data:bytes|bytearray):
        """
        Verifies if the provided data matches the content of the FRAM.
        :param data: Data to verify against the FRAM.
        :return: True if the data matches, False otherwise.
        """
        fram_data = self.read_fram_all()
        for i in range(len(fram_data)):
            if fram_data[i] != data[i]:
                self.logger.error(f"Mismatch at byte {i}: FRAM={fram_data[i]:02x}, Backup={data[i]:02x}")
        return fram_data == data

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

    # Backup FRAM command
    parser_backup = subparsers.add_parser("backup", help="Backup the entire FRAM to a file")
    parser_backup.add_argument("file", type=str, help="File to save the backup")

    # Restore FRAM command
    parser_restore = subparsers.add_parser("restore", help="Restore the entire FRAM from a file")
    parser_restore.add_argument("file", type=str, help="File to read the backup from")

    # Verify FRAM command
    parser_verify = subparsers.add_parser("verify", help="Verify the entire FRAM against a file")
    parser_verify.add_argument("file", type=str, help="File to verify the FRAM content against")

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

    elif args.command == "backup":
        print(f"Backing up FRAM to {args.file}...")
        data = bs.read_fram_all()

        with open(args.file, 'wb') as f:
            f.write(data)

        print("Backup complete.")

    elif args.command == "restore":
        print(f"Restoring FRAM from {args.file}...")
        with open(args.file, 'rb') as f:
            data = f.read()

        if len(data) < bs.fram_size:
            data = data.ljust(bs.fram_size, b'\x00')

        bs.write_fram_all(data)
        print("Restore complete.")

    elif args.command == "verify":
        print(f"Verifying FRAM against {args.file}...")
        with open(args.file, 'rb') as f:
            data = f.read()

        if bs.verify_fram(data):
            print("FRAM matches the file.")
        else:
            print("FRAM does not match the file.")

    else:
        parser.print_help()
