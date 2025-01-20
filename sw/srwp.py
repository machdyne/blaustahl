# Python example for using SRWP to communicate with Blaustahl
# see https://github.com/binqbit/serialport_srwp for protocol details

import serial
import time
import codecs

class BlaustahlSRWP:

    def __init__(self):
        self.srwp = serial.Serial('/dev/ttyACM0', 115200, timeout=1,
            rtscts=False, dsrdtr=False)
        #self.srwp.dtr = True

    def flush(self):
        while self.srwp.in_waiting:
            data = self.srwp.read(4096)
            print(data)

    def echo(self, msg):

        self.flush()

        print("*********")

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

        self.flush()

        print("*********")

        ba = bytearray()

        ba.extend(b'\x00')    # Enter SRWP mode
        ba.extend(b'\x01')    # Command: Read FRAM
        ba.extend(addr.to_bytes(4, byteorder='little'))
        ba.extend(size.to_bytes(4, byteorder='little'))

        self.srwp.write(ba)
        self.srwp.flush()

        data = self.srwp.read(size)
        print(data)

bs = BlaustahlSRWP()
bs.echo("this is a test")
#bs.read_fram(0, 100)
