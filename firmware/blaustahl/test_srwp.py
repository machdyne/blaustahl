#!/usr/bin/env python3
"""
SRWP (Simple Read/Write Protocol) test suite for Blaustahl.

Protocol reference: https://github.com/binqbit/serialport_srwp
Firmware reference: docs/srwp.md in this repository

Runs the same test suite against either:
  - Real hardware, over a serial port: --port /dev/ttyACM0
  - The firmware's srwp.c compiled standalone for testing on a
    desktop, driven over a pipe: --exec ./driver

SAFETY: this test suite writes to every corner of FRAM, including
address 0 and the very last byte -- if run against real hardware, it
WILL overwrite whatever is currently stored there. To make that safe,
every run backs up the full 8KB via SRWP's own read command BEFORE
touching anything, and restores it afterward in a `finally` block, so
the restore still happens even if a test fails or raises partway
through. The backup is also written to a local timestamped file as a
second line of defence, in case restore itself can't complete (e.g.
the device disconnects mid-test).

Usage:
    pip install pyserial
    python3 test_srwp.py --port /dev/ttyACM0
    python3 test_srwp.py --exec ./driver          # test build, no hardware
    python3 test_srwp.py --port /dev/ttyACM0 --skip-backup   # DANGEROUS, see below
"""

import argparse
import os
import struct
import sys
import time

FRAM_SIZE = 8192

CMD_TEST = 0x00
CMD_READ = 0x01
CMD_WRITE = 0x02
CMD_SIZE = 0x0a


# --------------------------------------------------------------------
# Transports -- either a real serial port, or a subprocess pipe to a
# desktop-compiled test build of srwp.c. Both expose the same tiny
# read(n)/write(data) interface, so everything above this point is
# transport-agnostic.
# --------------------------------------------------------------------

class SerialTransport:

	def __init__(self, port, baud=115200):
		import serial
		self.ser = serial.Serial(port, baud, timeout=5)
		# let the OS/driver settle after opening -- some USB CDC
		# stacks reset the device on port-open
		time.sleep(0.3)
		self.ser.reset_input_buffer()

	def write(self, data):
		self.ser.write(data)
		self.ser.flush()

	def read(self, n, timeout=5):
		self.ser.timeout = timeout
		data = self.ser.read(n)
		if len(data) != n:
			raise TimeoutError(f"expected {n} bytes, got {len(data)}: {data!r}")
		return data

	def close(self):
		self.ser.close()


class PipeTransport:
	"""Drives a standalone test build of srwp.c over stdin/stdout for
	one persistent subprocess covering the whole test run -- the
	driver's own loop (see the harness in this repo) calls srwp() once
	per command it sees, exactly matching how editor_yield() invokes
	it on real hardware, but the underlying process (and its simulated
	FRAM chip) persists across every command, matching how the real
	chip persists across every srwp() call too."""

	def __init__(self, exec_path):
		import subprocess
		self.proc = subprocess.Popen(
			[exec_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
			stderr=subprocess.PIPE, bufsize=0)

	def write(self, data):
		self.proc.stdin.write(data)
		self.proc.stdin.flush()

	def read(self, n, timeout=5):
		deadline = time.time() + timeout
		buf = b""
		while len(buf) < n:
			if time.time() > deadline:
				raise TimeoutError(f"expected {n} bytes, got {len(buf)}: {buf!r}")
			chunk = self.proc.stdout.read(n - len(buf))
			if chunk:
				buf += chunk
			else:
				time.sleep(0.005)
		return buf

	def close(self):
		try:
			self.proc.stdin.write(b"\xff")	# non-zero byte -- driver's
											# test loop exits on this
			self.proc.stdin.close()
			self.proc.wait(timeout=5)
		except Exception:
			pass


# --------------------------------------------------------------------
# SRWP client -- one persistent transport for the client's entire
# lifetime, matching how a real serial connection is used in practice
# (opened once, many commands sent over time) and how srwp() itself
# behaves on real hardware: each call handles exactly one command, but
# the underlying FRAM chip and the firmware process both persist
# continuously across every call, not just within one.
# --------------------------------------------------------------------

class SRWPClient:

	def __init__(self, transport):
		self.t = transport

	def test(self, data):
		self.t.write(bytes([0x00, CMD_TEST]) + struct.pack("<I", len(data)) + data)
		return self.t.read(len(data))

	def read(self, addr, length):
		self.t.write(bytes([0x00, CMD_READ]) + struct.pack("<II", addr, length))
		return self.t.read(length)

	def write(self, addr, data):
		self.t.write(bytes([0x00, CMD_WRITE]) + struct.pack("<II", addr, len(data)) + data)

	def size(self):
		self.t.write(bytes([0x00, CMD_SIZE]))
		(v,) = struct.unpack("<I", self.t.read(4))
		return v


# --------------------------------------------------------------------
# Backup / restore -- see the SAFETY note in the module docstring.
# --------------------------------------------------------------------

def backup_fram(client):
	print("Backing up all 8192 bytes of FRAM before testing...")
	data = client.read(0, FRAM_SIZE)
	assert len(data) == FRAM_SIZE
	fname = f"srwp_fram_backup_{int(time.time())}.bin"
	with open(fname, "wb") as f:
		f.write(data)
	print(f"Backup saved to {fname} ({FRAM_SIZE} bytes)")
	return data, fname


def restore_fram(client, backup_data):
	print("Restoring original FRAM content...")
	client.write(0, backup_data)
	readback = client.read(0, FRAM_SIZE)
	if readback == backup_data:
		print("Restore verified byte-exact.")
		return True
	else:
		diff = sum(1 for a, b in zip(readback, backup_data) if a != b)
		print(f"WARNING: restore verification FAILED -- {diff} bytes differ "
			f"from the backup. The backup file has NOT been deleted; "
			f"restore it manually if needed.")
		return False


# --------------------------------------------------------------------
# Tests -- each takes a fresh SRWPClient and returns True/False.
# All FRAM addresses used here are safe to overwrite ONLY because
# backup/restore wraps the whole run (see main()).
# --------------------------------------------------------------------

results = []

def check(name, cond):
	print(f"  [{name}: {'OK' if cond else 'FAIL'}]")
	results.append((name, cond))


def run_tests(client):

	# --- CMD_TEST: basic echo ---
	out = client.test(b"hello world")
	check("CMD_TEST echoes exact data", out == b"hello world")

	out = client.test(b"")
	check("CMD_TEST with zero length", out == b"")

	data = bytes((i % 256) for i in range(500))
	out = client.test(data)
	check("CMD_TEST across multiple chunks (500 bytes)", out == data)

	# --- CMD_SIZE ---
	sz = client.size()
	check("CMD_SIZE reports 8192 (full physical chip)", sz == FRAM_SIZE)

	# --- CMD_WRITE / CMD_READ round trips ---
	payload = bytes(range(256)) * 2
	client.write(100, payload)
	readback = client.read(100, len(payload))
	check("CMD_WRITE then CMD_READ round trip is byte-exact", readback == payload)

	client.write(0, b"START")
	readback = client.read(0, 5)
	check("write/read at address 0", readback == b"START")

	client.write(FRAM_SIZE - 1, b"\x42")
	readback = client.read(FRAM_SIZE - 1, 1)
	check("write/read at the last valid byte", readback == b"\x42")

	client.write(FRAM_SIZE - 2, b"\xaa\xbb")
	readback = client.read(FRAM_SIZE - 2, 2)
	check("read exactly up to the boundary", readback == b"\xaa\xbb")

	# --- out-of-bounds behavior ---
	client.write(FRAM_SIZE - 4, b"\x11\x22\x33\x44")
	out = client.read(FRAM_SIZE - 4, 10)
	check("out-of-bounds read is zero-padded to the requested length",
		out == b"\x11\x22\x33\x44" + b"\x00" * 6)

	out = client.read(FRAM_SIZE + 1000, 20)
	check("fully out-of-bounds read returns correct-length zeros, doesn't hang",
		out == b"\x00" * 20)

	client.write(FRAM_SIZE - 4, b"\x11\x22\x33\x44\x55\x66\x67\x68")
	readback = client.read(FRAM_SIZE - 4, 4)
	check("out-of-bounds write doesn't corrupt the in-bounds portion",
		readback == b"\x11\x22\x33\x44")
	check("session still responsive after an out-of-bounds write",
		client.test(b"still alive") == b"still alive")

	# --- sequencing ---
	client.write(0, b"AAAA")
	client.write(4, b"BBBB")
	readback = client.read(0, 8)
	check("multiple sequential commands don't desync the stream",
		readback == b"AAAABBBB")

	# --- larger transfer, many chunks ---
	big = bytes((i * 7) % 256 for i in range(4000))
	client.write(0, big)
	readback = client.read(0, len(big))
	check("large transfer (4000 bytes) is byte-exact", readback == big)

	# --- malformed length must not hang or crash the session ---
	client.t.write(bytes([0x00, CMD_READ]) + struct.pack("<II", 0, 0xFFFFFFFF))
	out = client.test(b"ping")
	check("absurd length (0xFFFFFFFF) aborted safely, session still works",
		out == b"ping")


def main():

	ap = argparse.ArgumentParser(description=__doc__,
		formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--port", help="serial port, e.g. /dev/ttyACM0 or COM3")
	ap.add_argument("--baud", type=int, default=115200)
	ap.add_argument("--exec", dest="exec_path",
		help="path to a standalone srwp.c test build, for testing without hardware")
	ap.add_argument("--skip-backup", action="store_true",
		help="DANGEROUS: skip the FRAM backup/restore. Only use this "
			"against a device you don't mind wiping, e.g. a fresh unit "
			"with no data on it yet.")
	args = ap.parse_args()

	if not args.port and not args.exec_path:
		ap.error("specify either --port (real hardware) or --exec (test build)")
	if args.port and args.exec_path:
		ap.error("specify only one of --port or --exec")

	if args.port:
		transport = SerialTransport(args.port, args.baud)
	else:
		transport = PipeTransport(args.exec_path)

	client = SRWPClient(transport)

	backup_data = None
	if not args.skip_backup:
		backup_data, backup_fname = backup_fram(client)
	else:
		print("--skip-backup given: FRAM content will NOT be preserved. "
			"Proceeding in 3 seconds -- Ctrl-C now to abort.")
		time.sleep(3)

	print()
	print("Running SRWP test suite...")
	print()

	try:
		run_tests(client)
	finally:
		if backup_data is not None:
			print()
			restored_ok = restore_fram(client, backup_data)
			if restored_ok:
				os.remove(backup_fname)
				print(f"Restore verified -- {backup_fname} removed.")

	print()
	passed = sum(1 for _, ok in results if ok)
	total = len(results)

	transport.close()

	if passed == total:
		print(f"ALL {total} TESTS PASSED")
		sys.exit(0)
	else:
		print(f"{total - passed} OF {total} TESTS FAILED:")
		for name, ok in results:
			if not ok:
				print(f"  - {name}")
		sys.exit(1)


if __name__ == "__main__":
	main()
