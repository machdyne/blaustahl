/*
 * XMODEM/CRC receiver.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 */

#include <string.h>
#include <stdlib.h>

#include "pico/time.h"

#include "blaustahl.h"
#include "flash_storage.h"
#include "storage.h"
#include "xmodem.h"

#define X_SOH   0x01
#define X_STX   0x02
#define X_EOT   0x04
#define X_ACK   0x06
#define X_NAK   0x15
#define X_CAN   0x18
#define X_CTRLZ 0x1a

#define XMODEM_BLOCK_SIZE 128
// matches te's own max file size -- no reason to allow uploading a
// file larger than what can actually be edited on-device. Dynamically
// allocated for the duration of a single transfer (see
// xmodem_receive_to_flash_file()) rather than reserved permanently:
// the previous 64KB static reservation was the single largest
// consumer of this firmware's general heap arena, competing directly
// with the Scheme interpreter (and anything else using malloc()) even
// though it only actually did anything during the rare moments an
// upload was in progress.
#define XMODEM_STAGING_SIZE (32u * 1024u)

// handshake wait budget: this is a HUMAN-operated transfer, not two
// programs starting in lockstep -- the user has to type the CLI
// command, then separately switch to their terminal program and find
// its own transfer menu, which routinely takes well over a minute in
// practice. 60 tries * 3s = 3 minutes of patience before giving up.
#define XMODEM_HANDSHAKE_TRIES 60
#define XMODEM_HANDSHAKE_TRY_MS 3000

static uint16_t crc16_update(uint16_t crc, uint8_t byte) {

	crc = (uint16_t)(crc ^ ((uint16_t)byte << 8));

	for (int i = 0; i < 8; i++) {
		if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
		else crc = (uint16_t)(crc << 1);
	}

	return crc;

}

static int xmodem_getchar_timeout(uint32_t timeout_ms) {

	absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

	while (!time_reached(deadline)) {
		int c = cdc_getchar();
		if (c != -1) return c;
	}

	return -1;

}

static void flush_input(void) {
	// drain trailing bytes (e.g. a sender's second CAN) so they don't
	// leak into the next CLI prompt
	while (xmodem_getchar_timeout(200) != -1) { }
}

xmodem_result_t xmodem_receive_to_flash_file(const char *filename) {

	// ensure flash is mounted before starting -- fails fast here if
	// flash is genuinely broken, rather than after a full transfer
	// only to have the final write silently fail. Idempotent: a no-op
	// if something already mounted flash earlier this session.
	storage_init();

	// allocated fresh for this transfer, freed on every exit path
	// below (see the cleanup: label) -- not a permanent reservation.
	// A failed allocation here is a real, expected possibility now
	// (competing with whatever else -- notably the Scheme
	// interpreter -- currently has the general heap arena), not a
	// rare edge case, so it's handled as an ordinary result code
	// rather than left to whatever the platform's malloc() failure
	// behavior happens to be.
	uint8_t *staging = malloc(XMODEM_STAGING_SIZE);
	if (!staging) return XMODEM_OUT_OF_MEMORY;

	xmodem_result_t result;

	uint32_t total = 0;
	uint32_t last_block_size = XMODEM_BLOCK_SIZE;
	uint8_t expected_block = 1;

	// establish the transfer: request CRC mode ('C'), retry until the
	// sender responds with a block header or gives up
	int c = -1;
	for (int tries = 0; tries < XMODEM_HANDSHAKE_TRIES; tries++) {
		if (!cdc_putchar_reliable('C')) { result = XMODEM_TIMEOUT; goto cleanup; }
		c = xmodem_getchar_timeout(XMODEM_HANDSHAKE_TRY_MS);
		if (c == X_SOH || c == X_STX || c == X_EOT) break;
		if (c == X_CAN) { flush_input(); result = XMODEM_CANCELLED; goto cleanup; }
		c = -1;
	}
	if (c == -1) { result = XMODEM_TIMEOUT; goto cleanup; }

	while (1) {

		if (c == X_EOT) {
			if (!cdc_putchar_reliable(X_ACK)) { result = XMODEM_TIMEOUT; goto cleanup; }
			break;
		}

		if (c == X_CAN) {
			flush_input();
			result = XMODEM_CANCELLED;
			goto cleanup;
		}

		if (c != X_SOH && c != X_STX) {
			// unexpected byte where a block header was expected
			if (!cdc_putchar_reliable(X_NAK)) { result = XMODEM_TIMEOUT; goto cleanup; }
			c = xmodem_getchar_timeout(3000);
			if (c == -1) { result = XMODEM_TIMEOUT; goto cleanup; }
			continue;
		}

		uint32_t block_size = (c == X_STX) ? 1024u : (uint32_t)XMODEM_BLOCK_SIZE;

		int blk      = xmodem_getchar_timeout(1000);
		int blk_comp = xmodem_getchar_timeout(1000);

		static uint8_t block_data[1024];
		uint16_t crc = 0;
		bool timed_out = false;

		for (uint32_t i = 0; i < block_size; i++) {
			int b = xmodem_getchar_timeout(1000);
			if (b == -1) { timed_out = true; break; }
			block_data[i] = (uint8_t)b;
			crc = crc16_update(crc, (uint8_t)b);
		}

		int crc_hi = timed_out ? -1 : xmodem_getchar_timeout(1000);
		int crc_lo = timed_out ? -1 : xmodem_getchar_timeout(1000);

		bool block_ok = !timed_out && blk != -1 && blk_comp != -1 &&
			crc_hi != -1 && crc_lo != -1 &&
			((blk + blk_comp) == 255) &&
			((uint16_t)((crc_hi << 8) | crc_lo) == crc);

		if (!block_ok) {
			if (!cdc_putchar_reliable(X_NAK)) { result = XMODEM_TIMEOUT; goto cleanup; }
			c = xmodem_getchar_timeout(3000);
			if (c == -1) { result = XMODEM_TIMEOUT; goto cleanup; }
			continue;
		}

		if ((uint8_t)blk == expected_block) {

			if (total + block_size > XMODEM_STAGING_SIZE) {
				cdc_putchar_reliable(X_CAN);
				cdc_putchar_reliable(X_CAN);
				flush_input();
				result = XMODEM_TOO_LARGE;
				goto cleanup;
			}

			memcpy(&staging[total], block_data, block_size);
			total += block_size;
			last_block_size = block_size;
			expected_block++;

		}
		// else: (uint8_t)blk == expected_block - 1 -- a duplicate
		// retransmit of a block we already have (our ACK was lost).
		// ACK it again without re-appending -- standard XMODEM
		// receiver behavior, keeps the sender's window in sync.

		if (!cdc_putchar_reliable(X_ACK)) { result = XMODEM_TIMEOUT; goto cleanup; }
		c = xmodem_getchar_timeout(3000);
		if (c == -1) { result = XMODEM_TIMEOUT; goto cleanup; }

	}

	// classic XMODEM pads the final block with CTRL-Z (or NUL) up to
	// the block boundary -- trim that padding, but only ever look
	// within the last block actually received, since 0x1A/0x00 could
	// legitimately appear in real binary content earlier in the file
	{
		uint32_t trimmed = total;
		uint32_t block_start = (total >= last_block_size) ? total - last_block_size : 0;

		while (trimmed > block_start &&
				(staging[trimmed - 1] == X_CTRLZ || staging[trimmed - 1] == 0x00))
			trimmed--;

		result = flash_storage_write_file(filename, (const char *)staging, trimmed)
			? XMODEM_OK : XMODEM_WRITE_FAILED;
	}

cleanup:
	free(staging);
	return result;

}

xmodem_result_t xmodem_send(file_ref_t f) {

	// ensure flash is mounted before starting, same reasoning as
	// receive: fail fast rather than after the handshake succeeds.
	// Harmless no-op for FRAM/SRAM sends.
	storage_init();

	uint32_t file_size = f.size;

	// wait for the receiver to initiate with NAK (0x15) -- classic
	// XMODEM's checksum-mode request, and the only handshake byte a
	// plain, unmodified `rx` ever sends (which is what minicom's
	// external-protocol menu actually runs). Retries here are the
	// RECEIVER's responsibility (they keep sending NAK until data
	// starts flowing); we just wait.
	int c = -1;
	for (int tries = 0; tries < XMODEM_HANDSHAKE_TRIES; tries++) {
		c = xmodem_getchar_timeout(XMODEM_HANDSHAKE_TRY_MS);
		if (c == X_NAK) break;
		if (c == X_CAN) { flush_input(); return XMODEM_CANCELLED; }
		c = -1;
	}
	if (c == -1) return XMODEM_TIMEOUT;

	uint8_t block_num = 1;
	uint32_t offset = 0;
	uint8_t block_data[XMODEM_BLOCK_SIZE];

	while (offset < file_size) {

		uint32_t chunk = file_size - offset;
		if (chunk > XMODEM_BLOCK_SIZE) chunk = XMODEM_BLOCK_SIZE;

		uint32_t got = storage_read(f, offset, (char *)block_data, chunk);

		// pad a short final block with CTRL-Z up to the full 128
		// bytes -- classic XMODEM convention, mirrors what receive
		// trims back off on the way in
		for (uint32_t i = got; i < XMODEM_BLOCK_SIZE; i++)
			block_data[i] = X_CTRLZ;

		uint8_t checksum = 0;
		for (uint32_t i = 0; i < XMODEM_BLOCK_SIZE; i++)
			checksum = (uint8_t)(checksum + block_data[i]);

		bool acked = false;

		for (int attempt = 0; attempt < 10 && !acked; attempt++) {

			bool ok = true;
			ok = ok && cdc_putchar_reliable(X_SOH);
			ok = ok && cdc_putchar_reliable(block_num);
			ok = ok && cdc_putchar_reliable((uint8_t)(255 - block_num));
			for (uint32_t i = 0; ok && i < XMODEM_BLOCK_SIZE; i++)
				ok = cdc_putchar_reliable(block_data[i]);
			ok = ok && cdc_putchar_reliable(checksum);

			// couldn't even get the block out -- the host has stopped
			// reading entirely (e.g. disconnected mid-transfer), not
			// a recoverable NAK/timeout a retry would fix
			if (!ok) return XMODEM_TIMEOUT;

			int resp = xmodem_getchar_timeout(5000);

			if (resp == X_ACK) { acked = true; break; }
			if (resp == X_CAN) { flush_input(); return XMODEM_CANCELLED; }
			// NAK, timeout, or anything else unexpected -- retry the
			// same block rather than advancing

		}

		if (!acked) return XMODEM_TIMEOUT;

		offset += got;
		block_num++;

	}

	bool eot_acked = false;
	for (int attempt = 0; attempt < 10 && !eot_acked; attempt++) {
		if (!cdc_putchar_reliable(X_EOT)) return XMODEM_TIMEOUT;
		int resp = xmodem_getchar_timeout(3000);
		if (resp == X_ACK) { eot_acked = true; break; }
		if (resp == X_CAN) { flush_input(); return XMODEM_CANCELLED; }
	}
	if (!eot_acked) return XMODEM_TIMEOUT;

	return XMODEM_OK;

}
