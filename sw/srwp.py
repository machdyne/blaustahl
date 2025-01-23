# Python example for using SRWP to communicate with Blaustahl
# see https://github.com/binqbit/serialport_srwp for protocol details

import serial
import sys
import time
import codecs
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

    def read_fram(self, addr, size):
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

    def write_fram(self, addr, data):
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

    c = input("This demo will overwrite FRAM, type 'CONTINUE' to continue: ")
    if c != "CONTINUE": sys.exit()

    bs = BlaustahlSRWP(device='/dev/ttyACM0')
    bs.clear_fram()

    # Echo test
    bs.echo("This is an echo test")

    # FRAM test: Write and read
    test_address = 0
    test_data = b'This is a write test'

    print("\nWriting data to FRAM:")
    bs.write_fram(test_address, test_data)

    print("\nReading data from FRAM:")
    read_data = bs.read_fram(test_address, len(test_data))

    # Verify the read data
    if read_data == test_data:
        print("\n✅ Data was written and read correctly!")
    else:
        print(f"\n❌ Data mismatch! Expected: {test_data.hex()}, Read: {read_data.hex()}")

    print(f"\n{bs.read_fram_all()}")
