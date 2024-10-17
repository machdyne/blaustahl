# Python example for using SRWP to communicate with Blaustahl

import serial
import time
import codecs

class BlaustahlSRWP:

    def __init__(self):
        self.srwp = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
        self.srwp.dtr = 1
        time.sleep(1)

    def flush(self):
        while self.srwp.in_waiting:
            data = self.srwp.read(1024)
            print(data)

    def echo(self, msg):

        self.flush()

        print("*********")

        self.srwp.write(b'\x00')    # Enter SRWP mode
        self.srwp.write(b'\x00')    # Request SRWP echo
        self.srwp.write(len(msg).to_bytes(4, byteorder='little'))
        self.srwp.write(msg.encode(encoding="ascii"))

        while self.srwp.in_waiting:
            data = self.srwp.read(4)
            print(data)

bs = BlaustahlSRWP()
bs.echo("this is a test")
