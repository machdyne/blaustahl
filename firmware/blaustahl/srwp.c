/*
 * Simple Read/Write Protocol (SRWP) for FRAM.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * Protocol: https://github.com/binqbit/serialport_srwp -- reimplemented
 * here from that spec, not copied from any reference source. See
 * docs/srwp.md for the full command reference and design notes.
 *
 * Invoked from editor.c's central input loop: every byte read over
 * CDC is checked there before anything else touches it, and a leading
 * 0x00 hands off to srwp() to read and process exactly one command
 * before returning. srwp() only ever sees the bytes AFTER that
 * marker -- it never reads the marker itself.
 *
 * Deliberately encryption-unaware, by design: this operates on the
 * full, raw 8KB FRAM chip (all of it, not just the encrypted
 * firmware's usable content region), completely bypassing storage.c's
 * buffer mode and any encryption layered on top of it. A read returns
 * whatever is physically on the chip -- ciphertext, if FRAM happens to
 * be encrypted -- and a write goes straight to the chip. If FRAM is
 * encrypted, writing through SRWP will corrupt it beyond recovery
 * (the AEAD tag stops matching), exactly as writing garbage over any
 * ciphertext would. That's an accepted, deliberate tradeoff for now,
 * not an oversight -- extending the protocol itself (e.g. to be
 * password-aware) is a possible future direction, not something this
 * revision attempts.
 *
 * The one nod to the rest of the firmware: editor_notify_srwp_write()
 * arms a one-time status-bar warning after a write, since a write here
 * can leave the grid editor's own FRAM buffer silently stale. It
 * doesn't correct that buffer -- SRWP has no reason to know or care
 * whether one exists -- it just flags it for a human to notice.
 *
 * HARDENING NOTES (relative to the original binqbit-derived implementation):
 *
 * 1. No unbounded stack allocation. The original used `uint8_t
 *    buf[len]` (a VLA) sized directly from an attacker/host-controlled
 *    length, with no bounds check at all -- a real stack-smashing risk
 *    on an embedded target with a small, unknown core1 stack budget
 *    (see storage.c's own crypt_scratch comment on the same concern).
 *    Every transfer here streams through a small, fixed, static
 *    SRWP_CHUNK_SIZE buffer instead, regardless of how large the
 *    requested length is.
 *
 * 2. Bounds-checked against the full physical FRAM size (8192 bytes),
 *    with overflow-safe arithmetic (addr+len is never computed
 *    directly and compared, which could wrap past UINT32_MAX and
 *    incorrectly pass). Since the protocol itself has no error
 *    response, out-of-bounds reads are zero-padded to the requested
 *    length (so the host always gets the reply length it expects,
 *    rather than hanging waiting for bytes that will never come), and
 *    out-of-bounds writes are safely discarded byte-by-byte -- still
 *    fully drained from the input stream so the next command doesn't
 *    desync, just not written to the chip.
 *
 * 3. Reliable multi-byte reads. The original read addr/len with a
 *    single tud_cdc_read(buf, 4) call and gave up immediately (silently
 *    losing any bytes already read) if fewer than 4 arrived in that one
 *    call -- a real desync risk under ordinary USB packet
 *    fragmentation. Every multi-byte value here accumulates through a
 *    proper byte-at-a-time loop with a bounded per-byte timeout
 *    instead (srwp_read_bytes()), the same "block with a timeout, not
 *    a busy-fail on any gap" philosophy already established by
 *    xmodem.c's xmodem_getchar_timeout() elsewhere in this firmware.
 *    Once the leading 0x00 has committed the host to a command, it's
 *    correct to wait a reasonable bounded time for the rest of it
 *    rather than abandon the parse on the first timing hiccup.
 *
 * 4. Portable, explicit little-endian decoding. The original read
 *    raw bytes directly into a uint32_t via a pointer cast
 *    (`uint32_t buf[1]; tud_cdc_read(buf, 4)`), relying on
 *    implementation-defined pointer-aliasing behavior that happens to
 *    work on this specific little-endian target but is fragile and
 *    non-obvious. srwp_read_u32()/srwp_write_u32() assemble/emit the
 *    four bytes explicitly instead.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/time.h"
#include "tusb.h"

#include "blaustahl.h"
#include "editor.h"
#include "fram.h"
#include "srwp.h"

#define CMD_TEST  0x00
#define CMD_READ  0x01
#define CMD_WRITE 0x02
#define CMD_SIZE  0x0a		// firmware-specific extension, not part of
							// the documented upstream protocol -- see
							// docs/srwp.md

#define SRWP_FRAM_SIZE 8192	// full physical chip capacity -- deliberately
							// NOT FRAM_AVAILABLE (the smaller,
							// metadata-excluded region storage.c uses):
							// SRWP is raw and encryption-unaware by
							// design, see file header

// once a command has started (the leading 0x00 marker was already
// consumed by the caller), it's reasonable to wait a bounded time per
// byte for the rest of it rather than abandon the parse on any single
// timing gap -- matches xmodem.c's own established philosophy for the
// same reason.
#define SRWP_BYTE_TIMEOUT_MS 3000

// transfers stream through this fixed-size buffer regardless of the
// requested length -- never sized from a host-controlled value. 128
// matches xmodem.c's own block size, a size already proven reasonable
// on this target.
#define SRWP_CHUNK_SIZE 128
static uint8_t chunk_buf[SRWP_CHUNK_SIZE];

static int srwp_getchar_timeout(uint32_t timeout_ms) {

	absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

	while (!time_reached(deadline)) {
		int c = cdc_getchar();
		if (c != -1) return c;
	}

	return -1;

}

// accumulates exactly `len` bytes with a per-byte timeout, unlike the
// original single-shot tud_cdc_read() calls this replaces -- see
// hardening note 3 above.
static bool srwp_read_bytes(uint8_t *buf, uint32_t len) {

	for (uint32_t i = 0; i < len; i++) {
		int c = srwp_getchar_timeout(SRWP_BYTE_TIMEOUT_MS);
		if (c < 0) return false;
		buf[i] = (uint8_t)c;
	}

	return true;

}

static bool srwp_read_u32(uint32_t *out) {

	uint8_t b[4];
	if (!srwp_read_bytes(b, 4)) return false;

	// explicit little-endian assembly -- see hardening note 4 above
	*out = (uint32_t)b[0]
		| ((uint32_t)b[1] << 8)
		| ((uint32_t)b[2] << 16)
		| ((uint32_t)b[3] << 24);

	return true;

}

static void srwp_write_bytes(const uint8_t *buf, uint32_t len) {
	for (uint32_t i = 0; i < len; i++) tud_cdc_write_char(buf[i]);
	tud_cdc_write_flush();
}

static void srwp_write_u32(uint32_t v) {
	uint8_t b[4] = {
		(uint8_t)(v & 0xff),
		(uint8_t)((v >> 8) & 0xff),
		(uint8_t)((v >> 16) & 0xff),
		(uint8_t)((v >> 24) & 0xff),
	};
	srwp_write_bytes(b, 4);
}

// CMD_TEST: echo `len` bytes back exactly as received. No FRAM
// involved at all, so the only concern is a host-controlled length
// large enough to be clearly unreasonable -- capped at the chip's own
// capacity as a sanity bound, streamed through the fixed chunk buffer
// regardless of size either way.
static void cmd_test(void) {

	uint32_t len;
	if (!srwp_read_u32(&len)) return;
	if (len > SRWP_FRAM_SIZE) return;	// clearly malformed -- abort,
										// don't attempt to drain or reply

	uint32_t remaining = len;
	while (remaining > 0) {
		uint32_t chunk = remaining < SRWP_CHUNK_SIZE ? remaining : SRWP_CHUNK_SIZE;
		if (!srwp_read_bytes(chunk_buf, chunk)) return;
		srwp_write_bytes(chunk_buf, chunk);
		remaining -= chunk;
	}

}

// CMD_READ: always replies with exactly `len` bytes, even if the
// request runs past the chip -- the portion beyond SRWP_FRAM_SIZE is
// zero-padded rather than the reply being short, since the protocol
// has no way to signal "here's less than you asked for" and a host
// waiting on a fixed-length reply that never fully arrives would just
// hang.
static void cmd_read(void) {

	uint32_t addr, len;
	if (!srwp_read_u32(&addr)) return;
	if (!srwp_read_u32(&len)) return;
	if (len > SRWP_FRAM_SIZE) return;	// clearly malformed -- abort

	uint32_t valid_len = 0;
	if (addr < SRWP_FRAM_SIZE) {
		uint32_t avail = SRWP_FRAM_SIZE - addr;
		valid_len = len < avail ? len : avail;
	}

	uint32_t offset = 0;

	while (offset < valid_len) {
		uint32_t chunk = valid_len - offset;
		if (chunk > SRWP_CHUNK_SIZE) chunk = SRWP_CHUNK_SIZE;
		fram_read((char *)chunk_buf, (int)(addr + offset), (int)chunk);
		srwp_write_bytes(chunk_buf, chunk);
		offset += chunk;
	}

	if (valid_len < len) {

		// pad the out-of-bounds remainder with zeros, chunk_buf
		// reused as a source of zero bytes
		uint32_t pad = len - valid_len;
		for (uint32_t i = 0; i < SRWP_CHUNK_SIZE && i < pad; i++) chunk_buf[i] = 0;

		while (pad > 0) {
			uint32_t chunk = pad < SRWP_CHUNK_SIZE ? pad : SRWP_CHUNK_SIZE;
			srwp_write_bytes(chunk_buf, chunk);
			pad -= chunk;
		}

	}

}

// CMD_WRITE: always fully drains `len` bytes from the input stream
// even when part or all of the range falls outside the chip -- the
// protocol gives no way to signal a partial failure (this command has
// no reply at all), so out-of-range bytes are simply discarded rather
// than written, while still being consumed so the byte stream stays
// in sync for whatever command comes next.
static void cmd_write(void) {

	uint32_t addr, len;
	if (!srwp_read_u32(&addr)) return;
	if (!srwp_read_u32(&len)) return;
	if (len > SRWP_FRAM_SIZE) return;	// clearly malformed -- abort
										// (nothing received yet to drain)

	uint32_t offset = 0;
	bool wrote_anything = false;

	while (offset < len) {

		uint32_t chunk = len - offset;
		if (chunk > SRWP_CHUNK_SIZE) chunk = SRWP_CHUNK_SIZE;

		if (!srwp_read_bytes(chunk_buf, chunk)) return;

		for (uint32_t i = 0; i < chunk; i++) {
			uint32_t byte_addr = addr + offset + i;
			if (byte_addr < SRWP_FRAM_SIZE) {
				fram_write((int)byte_addr, chunk_buf[i]);
				wrote_anything = true;
			}
		}

		offset += chunk;

	}

	if (wrote_anything) editor_notify_srwp_write();

}

// CMD_SIZE (firmware-specific extension): reports the full physical
// chip capacity, matching SRWP's raw, encryption-unaware access model
// -- deliberately not the smaller, metadata-excluded FRAM_AVAILABLE
// figure used elsewhere in the firmware.
static void cmd_size(void) {
	srwp_write_u32(SRWP_FRAM_SIZE);
}

void srwp(void) {

	blaustahl_led(LED_IDLE);

	int cmd = srwp_getchar_timeout(SRWP_BYTE_TIMEOUT_MS);
	if (cmd < 0) return;	// host sent the marker but nothing followed
							// in time -- give up quietly, no reply
							// expected for an incomplete command

	switch (cmd) {

		case CMD_TEST:
			cmd_test();
			break;

		case CMD_READ:
			blaustahl_led(LED_READ);
			cmd_read();
			break;

		case CMD_WRITE:
			blaustahl_led(LED_WRITE);
			cmd_write();
			break;

		case CMD_SIZE:
			blaustahl_led(LED_READ);
			cmd_size();
			break;

		default:
			// unknown command code -- nothing sensible to do without
			// knowing its shape; matches the original's own safe
			// default of simply not processing it further
			break;

	}

}
