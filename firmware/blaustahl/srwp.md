# SRWP -- Simple Read/Write Protocol

SRWP gives a host machine raw, byte-level read/write access to
Blaustahl's FRAM over the same USB CDC (serial) connection used for
the interactive terminal UI. It's the primary machine-to-machine API
for the device: with the USB Composite firmware variant and its
separate vendor-class interface removed, CDC is the only USB
interface Blaustahl exposes, and SRWP is how a program -- as opposed
to a human typing at a terminal -- talks to it.

Protocol origin: [binqbit/serialport_srwp](https://github.com/binqbit/serialport_srwp).
This document describes Blaustahl's own implementation, including
where and why it deliberately diverges from or extends that reference.

## Design: raw, encryption-unaware, all 8KB

SRWP operates directly on the full physical FRAM chip -- all 8192
bytes, address 0 through 8191 -- with no awareness of:

- **FRAM encryption.** If FRAM is encrypted, an SRWP read returns raw
  ciphertext, not plaintext. An SRWP write goes straight to the chip;
  if FRAM happens to be encrypted at the time, that write will corrupt
  it beyond recovery (the AEAD authentication tag will no longer
  match the ciphertext, and there is no way to recover the original
  content). This is a deliberate, accepted tradeoff for the current
  protocol revision, not an oversight -- extending SRWP itself to be
  password/encryption-aware is a possible future direction, not
  something this revision attempts.
- **The grid editor's buffer mode.** The interactive editor stages
  edits in its own RAM buffer before committing them to FRAM (see
  storage.c). SRWP reads and writes bypass that buffer entirely,
  talking straight to the chip. A write from SRWP while the editor has
  an active, uncommitted buffer will not be reflected in that buffer,
  and a later commit from the editor will silently overwrite whatever
  SRWP just wrote. The firmware surfaces a one-time warning
  ("SRWP WROTE FRAM") on the grid editor's status line the next time
  it redraws, to catch a human's attention -- see
  `editor_notify_srwp_write()` in editor.c. It does not attempt to
  reconcile the buffer automatically.

Given both of the above, SRWP is best understood as a way to treat
FRAM as a flat 8KB block device, similar in spirit to writing directly
to a disk's raw device node -- not as a way to interact with
Blaustahl's own file/encryption model. If your use case needs
encryption-aware access, use the interactive terminal UI (grid editor
or CLI) instead, at least until SRWP is extended to support it.

## Wire framing

Every SRWP command starts with a single `0x00` byte. This is checked
centrally, before any other input handling, in `editor_yield()`
(editor.c) -- the very first byte of every read is checked, and if
it's `0x00`, control hands off to `srwp()` to read and process exactly
one full command before returning. This means SRWP can be sent at any
time, from any UI mode (grid editor, viewer, file browser, CLI,
help screen, menu) -- it always takes priority over whatever the
interactive UI happens to be doing when the byte arrives, since the
leading `0x00` is not a meaningful keystroke on its own.

The consequence: an SRWP session and an interactive human session
cannot usefully coexist moment-to-moment on the same serial
connection. Use one or the other at a time.

All multi-byte integers (addresses, lengths) are 4-byte,
**little-endian** encoded.

## Commands

### CMD_TEST (`0x00`)

Request: `0x00 0x00 <len:u32> <data:len bytes>`
Response: `<data:len bytes>` -- the same bytes, echoed back exactly.

A pure loopback test. Touches no FRAM at all; useful for verifying the
connection and framing are working before attempting a real
read/write.

### CMD_READ (`0x01`)

Request: `0x00 0x01 <addr:u32> <len:u32>`
Response: `<data:len bytes>`

Reads `len` bytes starting at `addr`. Always replies with exactly
`len` bytes, even if the requested range runs past the end of FRAM --
any portion beyond address 8191 is zero-filled in the response rather
than the reply being short. (The protocol has no way to signal "here's
less than you asked for"; a host waiting on a fixed-length reply that
never fully arrives would simply hang. Zero-padding avoids that.)

### CMD_WRITE (`0x02`)

Request: `0x00 0x02 <addr:u32> <len:u32> <data:len bytes>`
Response: none.

Writes `len` bytes starting at `addr`. Bytes that would fall at or
past address 8192 are read from the host (so the stream stays framed
correctly for whatever command comes next) but discarded rather than
written. Since this command has no reply at all, there is no way to
signal that part of a write was rejected -- if you write near the end
of FRAM, read the result back afterward to confirm what actually
landed.

### CMD_SIZE (`0x0a`) -- firmware-specific extension

Request: `0x00 0x0a`
Response: `<size:u32>` -- always `8192`.

Not part of the documented upstream protocol. Included because it's
useful and was already present in earlier versions of this firmware;
kept as a Blaustahl-specific extension. A host that only implements
the three commands above can safely ignore this one.

## Hardening notes (relative to the original implementation)

The following issues existed in the SRWP implementation this firmware
started from, and were fixed as part of hardening it into the
project's primary API surface:

1. **Unbounded stack allocation.** The original sized a buffer
   directly from the host-supplied length (`uint8_t buf[len]`, a
   variable-length array) with no bounds check at all -- a genuine
   stack-smashing risk on an embedded target with a small, largely
   unknown stack budget. Every transfer now streams through a small,
   fixed 128-byte static buffer instead, regardless of the requested
   length.

2. **No bounds checking against FRAM's actual size.** Addresses and
   lengths are now checked against the full 8192-byte chip, with the
   comparison written to avoid integer overflow (an address and length
   that together overflow past `UINT32_MAX` can no longer be
   miscomputed as "in range").

3. **Fragile multi-byte reads.** The original read 4-byte values
   (addresses, lengths) with a single, one-shot read call and gave up
   immediately -- silently losing whatever partial data had already
   arrived -- if fewer than 4 bytes came back in that one call. Under
   ordinary USB packet fragmentation this could desync the parser
   mid-command. Every multi-byte value is now accumulated through a
   proper byte-at-a-time loop with a bounded per-byte timeout, the
   same philosophy already used by this firmware's XMODEM
   implementation: once a command has genuinely started, it's correct
   to wait a reasonable bounded time for the rest of it, rather than
   abandon the parse on the first timing hiccup.

4. **Implementation-defined byte-order handling.** The original read
   raw bytes directly into a `uint32_t` via a pointer cast, relying on
   the host and device happening to agree on byte order through
   undefined/implementation-defined pointer-aliasing behavior.
   Little-endian encoding/decoding is now explicit.

## Testing

See `tools/test_srwp.py` for a standalone, repeatable test suite that
exercises all of the above -- basic echo, round-trip read/write,
boundary addresses (0, 8191, exactly at the 8192 edge),
out-of-bounds reads and writes, command sequencing, larger multi-chunk
transfers, and a deliberately malformed length to confirm the firmware
aborts safely rather than hanging or crashing.

**The test suite writes to FRAM, including address 0 and the very
last byte.** Run against real hardware, it backs up the full 8KB via
SRWP's own read command before touching anything, and restores it
afterward -- including if a test fails partway through -- so it's
safe to run against a device that already has real data on it. The
backup is also written to a local timestamped file as a second line
of defence. See the script's own docstring for usage.

```sh
pip install pyserial
python3 tools/test_srwp.py --port /dev/ttyACM0
```
