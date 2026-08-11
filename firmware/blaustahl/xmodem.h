#ifndef XMODEM_H_
#define XMODEM_H_

#include <stdint.h>
#include <stdbool.h>

#include "storage.h"

/*
 * XMODEM/CRC file transfer -- moves files over the existing serial
 * connection, no USB-MSC needed.
 *
 * Blocking, deliberately: XMODEM's ACK/NAK timing doesn't compose well
 * with this firmware's normal cooperative (one-event-per-call)
 * architecture without a lot of added complexity, and a file transfer
 * is a deliberate, bounded action the user explicitly starts from the
 * CLI -- similar in spirit to "format" or "disable_encryption", not
 * something that needs to stay interruptible mid-operation. This
 * blocks core1 for the duration of the transfer; core0's USB task
 * loop is unaffected, since it runs independently on the other core.
 *
 * The handshake wait (both directions) is deliberately generous --
 * several minutes, not seconds -- because this is a human-operated
 * transfer: the user has to type the CLI command, then separately
 * switch to their terminal program and navigate its own transfer
 * menu, which routinely takes longer than a short handshake window
 * allows for. A short timeout here reads as "xmodem doesn't work" when
 * the real issue is just that the firmware gave up before the human
 * caught up.
 */

typedef enum {
	XMODEM_OK = 0,
	XMODEM_CANCELLED,		// sender sent CAN, or gave up retrying
	XMODEM_TOO_LARGE,		// file exceeded the staging buffer
	XMODEM_WRITE_FAILED,	// flash_storage_write_file() failed
	XMODEM_TIMEOUT,			// no sender ever responded to the handshake
	XMODEM_NOT_FOUND,		// (send only) named file doesn't exist on flash
	XMODEM_OUT_OF_MEMORY,	// (receive only) couldn't allocate the staging
							// buffer -- see xmodem.c for why this is a
							// real, expected possibility now, not a rare
							// edge case
} xmodem_result_t;

xmodem_result_t xmodem_receive_to_flash_file(const char *filename);

// sends `f` (FRAM, SRAM, or a flash file) to the host over XMODEM/CRC.
// For flash files, this reads directly from the mounted filesystem
// one block at a time rather than staging the whole file in RAM
// first, so there's no size cap here the way there is on receive.
xmodem_result_t xmodem_send(file_ref_t f);

#endif
