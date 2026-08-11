/*
 * Command-line mode for Blaustahl.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * A cooperative (non-blocking) line editor, not the old blocking
 * readline() -- built the same way every other mode in this firmware
 * is, one event at a time, so CTRL-T still opens the menu bar mid-line
 * instead of being swallowed the way a blocking read would swallow it.
 *
 * Commands take up to two whitespace-separated arguments, parsed once
 * per submitted line (rename <f1> <f2>, rm <filename>, etc) -- there's
 * no longer a separate "type the command, then get asked for the
 * filename on the next line" dance the way xmodem/edit briefly worked;
 * everything's on one line now, shell-style.
 *
 * The CLI is also the Scheme REPL now (BLAUSTAHL_APPS_ENABLED builds
 * only) -- there's no separate "scheme" command to enter/exit anymore.
 * Every submitted line is checked against the known command table
 * first (cli_dispatch() returns false for anything it doesn't
 * recognize); if nothing matches, the *original, untokenized* line is
 * handed to ms_glue_eval_line() instead. This has to be the original
 * line, not cmd/arg1/arg2 -- the CLI's own tokenizer is built for
 * "command name plus up to two simple arguments," and would mangle
 * S-expression syntax like "(+ 1 2)" (splitting on whitespace turns
 * that into "(+", "1", "2)", which Scheme can't parse back into
 * anything sensible). A command name always wins over a same-named
 * Scheme symbol, since the command table is checked first.
 *
 * ms's interpreter state (heap, environment, any variables defined
 * during the session) is deliberately reset on every fresh entry into
 * the CLI, not persisted across visits -- matching how the CLI's own
 * line buffer and confirmation state have always reset on re-entry,
 * and keeping an idle interpreter from costing any RAM while you're
 * off in the grid editor or viewer instead.
 *
 * Destructive commands (format, disable_encryption, rm, and rename
 * when it would overwrite an existing file) go through a generic
 * confirm step rather than a one-off "press Y" hack: dispatching a
 * command can set `pending_confirm` to a callback instead of acting
 * immediately, and the next line typed is checked against "YES"
 * before that callback runs. Anything else cancels. Confirm callbacks
 * that need to know WHICH file (rm, rename) read from a small stashed
 * buffer set at dispatch time, since the callback itself takes no
 * arguments.
 *
 * Password entry is masked (echoes '*', not the typed character) --
 * a deliberate improvement over the reference machdyne/blaustahl
 * encryption branch, which always echoed passwords in plaintext.
 * Entered password bytes are zeroed as soon as they're no longer
 * needed, rather than left sitting in a buffer indefinitely. Password
 * entry deliberately still uses its own multi-step prompt rather than
 * being typed as a plain argument -- an argument would show up in the
 * line buffer/redraw in cleartext.
 */

#include <stdio.h>
#include <string.h>

#include "blaustahl.h"
#include "vt100.h"
#include "vt100_input.h"
#include "storage.h"
#include "editor.h"
#include "xmodem.h"
#include "view.h"
#ifdef BLAUSTAHL_APPS_ENABLED
#include "te_glue.h"
#include "ms_glue.h"
// exact, verified sizeof(ms_val) on this target -- confirmed via
// arm-none-eabi-gcc -mcpu=cortex-m0plus, not a guess. See docs/ms-memory.md.
#define MS_CELL_BYTES 24
#endif
#include "cli.h"

// generous enough for a genuinely multi-line Scheme definition typed
// interactively (see the new paren-balance continuation logic below)
// without being wasteful -- anything much larger belongs in a file,
// written with `te` and run with `load`, not typed at the prompt
#define CLI_LINE_MAX 2048
#define CLI_CMD_MAX 24
#define CLI_ARG_MAX STORAGE_NAME_LEN
#define CLI_PW_MAX 40		// passwords are capped at 32 chars (see the
							// password prompts below) -- kept separate
							// from CLI_LINE_MAX so growing the line
							// buffer for multi-line input doesn't
							// also balloon this

typedef enum {
	CLI_PROMPT,			// normal command prompt
	CLI_CONFIRM,		// waiting for "YES" to confirm a destructive action
	CLI_PW_NEW1,		// waiting for a new password (FRAM currently plaintext)
	CLI_PW_NEW2,		// waiting for password confirmation
	CLI_PW_UNLOCK,		// waiting for the password to unlock encrypted FRAM
} cli_state_t;

static cli_state_t state = CLI_PROMPT;
static char line[CLI_LINE_MAX];
static int line_len = 0;

// true while accumulating a multi-line Scheme expression (see
// scan_paren_depth()/cli_yield() below) -- CLI_PROMPT covers both "a
// fresh prompt, nothing typed yet" and "mid-continuation, waiting for
// the closing paren"; this flag distinguishes the two without
// disturbing what `state` already means elsewhere (e.g. password
// masking)
static bool continuing = false;

static char pw_first[CLI_PW_MAX];	// holds the first entry while
									// awaiting confirmation
static bool pw_new_is_rotation = false;	// true if this NEW1/NEW2
											// round is changing an
											// existing password rather
											// than enabling from
											// plaintext -- decides
											// which storage function
											// cli_handle_pw_new2() calls

typedef void (*confirm_fn)(void);
static confirm_fn pending_confirm = NULL;

static char pending_rm_target[CLI_ARG_MAX];
static char pending_rename_from[CLI_ARG_MAX];
static char pending_rename_to[CLI_ARG_MAX];

static void cli_confirm_format(void) {
	bool ok = storage_format_flash();
	printf(ok ? "FORMAT OK -- ALL FLASH FILES ERASED"
	          : "FORMAT FAILED");
}

static void cli_confirm_disable_encryption(void) {
	bool ok = storage_crypt_disable();
	printf(ok ? "ENCRYPTION DISABLED -- FRAM SAVED AS PLAINTEXT"
	          : "FAILED TO DISABLE ENCRYPTION");
}

static void cli_confirm_rm(void) {
	bool ok = storage_flash_delete(pending_rm_target);
	printf(ok ? "DELETED." : "DELETE FAILED.");
}

static void cli_confirm_rename(void) {
	bool ok = storage_flash_rename(pending_rename_from, pending_rename_to);
	printf(ok ? "RENAMED." : "RENAME FAILED.");
}

static void cli_confirm_firmware_update(void) {
	// no return path from here in normal operation -- entering
	// bootloader mode is a one-way trip until either a new UF2 is
	// dropped or the device is power-cycled without one
	blaustahl_dfu();
}

#ifdef BLAUSTAHL_APPS_ENABLED
static void cli_confirm_clear(void) {
	// full teardown + fresh init, including reloading stdlib -- the
	// only way to reset the Scheme session now that it persists
	// across leaving and re-entering CLI mode (see
	// ms_glue_start_session()'s own comment). ms_glue_start_session()
	// is idempotent while a session is already active, so it must
	// see session_ready go back to false first, which end_session()
	// does, for this to actually take effect rather than no-op.
	ms_glue_end_session();
	ms_glue_start_session();
	printf("SCHEME SESSION CLEARED.");
}
#endif

// best-effort: if nothing else has an unsaved buffer pending on some
// other file, jump straight into FRAM after a successful unlock/enable
// -- mirrors the reference branch's "password entry drops you right
// into the editor" flow. If it can't (e.g. SRAM has unsaved edits),
// this just silently does nothing further; the crypto operation itself
// already succeeded regardless.
static void try_jump_to_fram(void) {
	file_ref_t fram = storage_fram_ref();
	if (storage_select(fram)) editor_open_current_file();
}

// splits a line into up to 3 whitespace-separated tokens (command +
// 2 args). Extra tokens beyond that are silently ignored. Missing
// tokens come back as empty strings, not NULL, so callers can just
// check arg1[0]/arg2[0] without a separate presence check.
static void parse_line(const char *input, char *cmd, char *arg1, char *arg2) {

	cmd[0] = arg1[0] = arg2[0] = 0;

	char *bufs[3] = { cmd, arg1, arg2 };
	int max_lens[3] = { CLI_CMD_MAX, CLI_ARG_MAX, CLI_ARG_MAX };

	const char *p = input;

	for (int tok = 0; tok < 3; tok++) {
		while (*p == ' ') p++;
		int i = 0;
		while (*p && *p != ' ' && i < max_lens[tok] - 1) bufs[tok][i++] = *p++;
		bufs[tok][i] = 0;
	}

}

// finds a flash file's full file_ref_t (including size) by name, in
// batches (same pattern as ls) rather than one storage_file_at() scan
// per candidate. Returns false if no file with that exact name exists.
static bool find_flash_file(const char *name, file_ref_t *out) {

	int n = storage_file_count();
	file_ref_t batch[STORAGE_BATCH_MAX];

	for (int start = 0; start < n; start += STORAGE_BATCH_MAX) {
		int want = n - start;
		if (want > STORAGE_BATCH_MAX) want = STORAGE_BATCH_MAX;
		int got = storage_flash_file_range(start, want, batch);
		for (int i = 0; i < got; i++) {
			if (strcmp(batch[i].name, name) == 0) {
				*out = batch[i];
				return true;
			}
		}
	}

	return false;

}

static bool cli_dispatch(const char *cmd, const char *arg1, const char *arg2) {

	if (cmd[0] == 0 || strcmp(cmd, "help") == 0) {
		printf("COMMANDS:\r\n"
		       "  format\r\n"
		       "  ls\r\n"
		       "  info\r\n"
		       "  password\r\n"
		       "  disable_encryption\r\n"
		       "  view <filename>\r\n"
		       "  xmodem_up <filename>\r\n"
		       "  xmodem_down <filename|fram|sram>\r\n"
		       "  rename <f1> <f2>\r\n"
		       "  rm <filename>\r\n"
		       "  firmware_update\r\n"
		       "  snapshot_fram\r\n"
#ifdef BLAUSTAHL_APPS_ENABLED
		       "  te <filename>\r\n"
		       "  load <filename>\r\n"
		       "  clear\r\n"
		       "\r\n"
		       "  Anything else is evaluated as Scheme.\r\n"
#endif
		       "");
		return true;
	}

	if (strcmp(cmd, "format") == 0) {
		printf("FORMAT FLASH? ALL FILES WILL BE LOST.\r\n"
		       "TYPE 'YES' TO CONFIRM, ANYTHING ELSE TO CANCEL.");
		pending_confirm = cli_confirm_format;
		state = CLI_CONFIRM;
		return true;
	}

	if (strcmp(cmd, "ls") == 0) {
		int n = storage_file_count();
		printf("%i FILES:", n);
		file_ref_t batch[STORAGE_BATCH_MAX];
		for (int start = 0; start < n; start += STORAGE_BATCH_MAX) {
			int want = n - start;
			if (want > STORAGE_BATCH_MAX) want = STORAGE_BATCH_MAX;
			int got = storage_flash_file_range(start, want, batch);
			for (int i = 0; i < got; i++)
				printf("\r\n  %-24s %u BYTES", batch[i].name, batch[i].size);
		}
		return true;
	}

	if (strcmp(cmd, "info") == 0) {

		char board_id[17];
		storage_board_unique_id(board_id, sizeof(board_id));

		const char *fram_status;
		switch (storage_crypt_status()) {
			case CRYPT_PLAINTEXT: fram_status = "PLAINTEXT";          break;
			case CRYPT_LOCKED:    fram_status = "ENCRYPTED, LOCKED";  break;
			case CRYPT_UNLOCKED:  fram_status = "ENCRYPTED, UNLOCKED"; break;
			default:               fram_status = "UNKNOWN";            break;
		}

		printf("FIRMWARE: %s\r\n"
		       "BOARD ID: %s\r\n"
		       "FRAM: %i BYTES (%s)\r\n"
		       "SRAM: %i BYTES\r\n"
		       "FLASH: %i FILES, %u/%u KB FREE\r\n",
			BLAUSTAHL_VERSION,
			board_id,
			FRAM_AVAILABLE, fram_status,
			FRAM_AVAILABLE,
			storage_file_count(),
			storage_flash_free() / 1024, storage_flash_total() / 1024);

#ifdef BLAUSTAHL_APPS_ENABLED
		uint32_t sys_total = ms_glue_system_heap_total();
		uint32_t sys_free = ms_glue_system_heap_free();
		printf("SYSTEM HEAP: %u/%u BYTES FREE", sys_free, sys_total);

		long cells_live = ms_glue_heap_live();
		long cells_total = ms_glue_heap_size();
		printf("\r\nSCHEME CELL POOL: %ld/%ld LIVE (%ld BYTES USED, %ld BYTES TOTAL)",
			cells_live, cells_total,
			cells_live * MS_CELL_BYTES, cells_total * MS_CELL_BYTES);
#endif

		return true;
	}

	if (strcmp(cmd, "password") == 0) {

		crypt_status_t st = storage_crypt_status();

		if (st == CRYPT_UNLOCKED) {
			printf("ENTER NEW PASSWORD (THIS WILL CHANGE THE EXISTING "
				"ONE), 1-32 CHARS:");
			pw_new_is_rotation = true;
			state = CLI_PW_NEW1;
		} else if (st == CRYPT_LOCKED) {
			printf("ENTER PASSWORD TO UNLOCK FRAM:");
			state = CLI_PW_UNLOCK;
		} else {
			printf("ENTER NEW PASSWORD (THIS WILL ENCRYPT FRAM), "
				"1-32 CHARS:");
			pw_new_is_rotation = false;
			state = CLI_PW_NEW1;
		}

		return true;

	}

	if (strcmp(cmd, "disable_encryption") == 0) {

		if (storage_crypt_status() != CRYPT_UNLOCKED) {
			printf("UNLOCK FIRST (password COMMAND).");
			return true;
		}

		printf("DISABLE ENCRYPTION AND SAVE FRAM AS PLAINTEXT?\r\n"
		       "TYPE 'YES' TO CONFIRM, ANYTHING ELSE TO CANCEL.");
		pending_confirm = cli_confirm_disable_encryption;
		state = CLI_CONFIRM;
		return true;

	}

	if (strcmp(cmd, "xmodem_up") == 0) {

		if (!arg1[0]) {
			printf("USAGE: xmodem_up <filename>");
			return true;
		}

		// blocking from here until the transfer finishes or times out
		// -- XMODEM's ACK/NAK timing doesn't compose with this
		// firmware's normal cooperative per-event architecture, so
		// this is a deliberate exception, the same way "format" and
		// "disable_encryption" are deliberate, bounded, user-initiated
		// actions rather than something that stays interruptible
		printf("RECEIVING '%s' VIA XMODEM/CRC -- START YOUR SENDER NOW.\r\n"
			"(THIS BLOCKS UNTIL THE TRANSFER FINISHES OR TIMES OUT.)",
			arg1);
		fflush(stdout);

		xmodem_result_t r = xmodem_receive_to_flash_file(arg1);

		printf("\r\n");
		switch (r) {
			case XMODEM_OK:           printf("RECEIVED OK.");                          break;
			case XMODEM_CANCELLED:    printf("CANCELLED BY SENDER.");                  break;
			case XMODEM_TOO_LARGE:    printf("FILE TOO LARGE FOR STAGING BUFFER.");    break;
			case XMODEM_WRITE_FAILED: printf("RECEIVED OK, BUT FAILED TO WRITE FLASH."); break;
			case XMODEM_TIMEOUT:      printf("TIMED OUT WAITING FOR SENDER.");         break;
			case XMODEM_OUT_OF_MEMORY:
#ifdef BLAUSTAHL_APPS_ENABLED
				printf("NOT ENOUGH FREE MEMORY RIGHT NOW -- "
					"TRY AGAIN, OR RUN 'clear' TO FREE UP THE SCHEME HEAP.");
#else
				printf("NOT ENOUGH FREE MEMORY RIGHT NOW -- TRY AGAIN.");
#endif
				break;
			case XMODEM_NOT_FOUND:    break;	// send-only result, unreachable here
		}
		return true;

	}

	if (strcmp(cmd, "xmodem_down") == 0) {

		if (!arg1[0]) {
			printf("USAGE: xmodem_down <filename|fram|sram>");
			return true;
		}

		file_ref_t f;

		if (strcmp(arg1, "fram") == 0) {
			f = storage_fram_ref();
		} else if (strcmp(arg1, "sram") == 0) {
			f = storage_sram_ref();
		} else if (find_flash_file(arg1, &f)) {
			// f already set by find_flash_file()
		} else {
			printf("FILE NOT FOUND: '%s'", arg1);
			return true;
		}

		// blocking, same reasoning as xmodem_up. Unlike xmodem_up
		// (which requests CRC mode itself, matching what senders like
		// sx/sz support well), this is classic checksum-mode XMODEM --
		// the original protocol, not the later CRC variant. That's
		// what a plain, unmodified `rx` expects (which is what
		// minicom's external-protocol menu actually runs).
		printf("SENDING '%s' VIA XMODEM (CHECKSUM) -- START YOUR RECEIVER NOW.\r\n"
			"(THIS BLOCKS UNTIL THE TRANSFER FINISHES OR TIMES OUT --\r\n"
			"UP TO SEVERAL MINUTES, SO TAKE YOUR TIME STARTING IT.)",
			f.name);
		fflush(stdout);

		xmodem_result_t r = xmodem_send(f);

		printf("\r\n");
		switch (r) {
			case XMODEM_OK:           printf("SENT OK.");                    break;
			case XMODEM_CANCELLED:    printf("CANCELLED BY RECEIVER.");      break;
			case XMODEM_TIMEOUT:      printf("TIMED OUT WAITING FOR RECEIVER."); break;
			case XMODEM_NOT_FOUND:    printf("FILE NOT FOUND: '%s'", arg1);  break;
			case XMODEM_TOO_LARGE:    break;	// receive-only result, unreachable here
			case XMODEM_WRITE_FAILED: break;	// receive-only result, unreachable here
			case XMODEM_OUT_OF_MEMORY: break;	// receive-only result, unreachable here
										// (send streams through a fixed
										// 128-byte buffer, nothing to allocate)
		}
		return true;

	}

	if (strcmp(cmd, "rm") == 0) {

		if (!arg1[0]) {
			printf("USAGE: rm <filename>");
			return true;
		}

		if (!storage_flash_file_exists(arg1)) {
			printf("FILE NOT FOUND: '%s'", arg1);
			return true;
		}

		printf("DELETE '%s'?\r\n"
		       "TYPE 'YES' TO CONFIRM, ANYTHING ELSE TO CANCEL.", arg1);
		strncpy(pending_rm_target, arg1, CLI_ARG_MAX - 1);
		pending_rm_target[CLI_ARG_MAX - 1] = 0;
		pending_confirm = cli_confirm_rm;
		state = CLI_CONFIRM;
		return true;

	}

	if (strcmp(cmd, "rename") == 0) {

		if (!arg1[0] || !arg2[0]) {
			printf("USAGE: rename <f1> <f2>");
			return true;
		}

		if (!storage_flash_file_exists(arg1)) {
			printf("FILE NOT FOUND: '%s'", arg1);
			return true;
		}

		if (storage_flash_file_exists(arg2)) {
			// littlefs's own rename silently replaces an existing
			// destination -- confirm before letting that happen,
			// rather than let a typo quietly destroy a different file
			printf("'%s' ALREADY EXISTS -- OVERWRITE IT?\r\n"
			       "TYPE 'YES' TO CONFIRM, ANYTHING ELSE TO CANCEL.", arg2);
			strncpy(pending_rename_from, arg1, CLI_ARG_MAX - 1);
			pending_rename_from[CLI_ARG_MAX - 1] = 0;
			strncpy(pending_rename_to, arg2, CLI_ARG_MAX - 1);
			pending_rename_to[CLI_ARG_MAX - 1] = 0;
			pending_confirm = cli_confirm_rename;
			state = CLI_CONFIRM;
			return true;
		}

		bool ok = storage_flash_rename(arg1, arg2);
		printf(ok ? "RENAMED." : "RENAME FAILED.");
		return true;

	}

	if (strcmp(cmd, "firmware_update") == 0) {
		printf("ENTER FIRMWARE UPDATE MODE? ANY UNSAVED CHANGES WILL BE LOST.\r\n"
		       "TYPE 'YES' TO CONFIRM, ANYTHING ELSE TO CANCEL.");
		pending_confirm = cli_confirm_firmware_update;
		state = CLI_CONFIRM;
		return true;
	}

	if (strcmp(cmd, "snapshot_fram") == 0) {
		bool ok = storage_snapshot_fram();
		printf(ok ? "FRAM SNAPSHOT SAVED AS fram_snapshot.bin"
		          : "SNAPSHOT FAILED (FLASH WRITE ERROR)");
		return true;
	}

	if (strcmp(cmd, "view") == 0) {

		if (!arg1[0]) {
			printf("USAGE: view <filename>");
			return true;
		}

		file_ref_t f;
		if (!find_flash_file(arg1, &f)) {
			printf("FILE NOT FOUND: '%s'", arg1);
			return true;
		}

		// unlike te, this doesn't block -- it switches to MODE_VIEW
		// and returns immediately; the normal event loop dispatches
		// subsequent keystrokes to view.c from here on. view_open()
		// tracks its own file reference independently, so this never
		// touches the global current_file (which stays dedicated to
		// the grid editor's FRAM/SRAM). The Scheme session (if any)
		// stays alive across this switch too -- see
		// ms_glue_start_session()'s own comment for why leaving CLI
		// mode no longer tears it down.
		view_open(f);
		return true;

	}

#ifdef BLAUSTAHL_APPS_ENABLED

	if (strcmp(cmd, "te") == 0) {

		if (!arg1[0]) {
			printf("USAGE: te <filename>");
			return true;
		}

		// blocking -- te takes over the terminal completely until the
		// user quits (Esc :q), same as xmodem_up/scheme
		te_glue_result_t r = te_glue_edit(arg1);

		switch (r) {
			case TE_GLUE_OK:
				// te already redrew its own screen throughout; nothing
				// further to print, just fall through to a fresh prompt
				break;
			case TE_GLUE_TOO_LARGE:
				printf("FILE TOO LARGE TO EDIT (MAX %u BYTES)",
					te_glue_max_file_size());
				break;
		}
		return true;

	}

	if (strcmp(cmd, "load") == 0) {

		if (!arg1[0]) {
			printf("USAGE: load <filename>");
			return true;
		}

		if (!storage_flash_file_exists(arg1)) {
			printf("FILE NOT FOUND: '%s'", arg1);
			return true;
		}

		// doesn't block or switch mode -- evaluates inline, same as
		// typing Scheme directly at the prompt, just reading the
		// forms from a flash file (written with te) instead of one
		// line at a time
		ms_glue_load_file(arg1);
		return true;

	}

	if (strcmp(cmd, "clear") == 0) {
		printf("CLEAR THE SCHEME SESSION? ALL CURRENT DEFINITIONS WILL BE LOST.\r\n"
		       "TYPE 'YES' TO CONFIRM, ANYTHING ELSE TO CANCEL.");
		pending_confirm = cli_confirm_clear;
		state = CLI_CONFIRM;
		return true;
	}

#endif

	return false;

}

static void cli_handle_confirm(const char *input) {

	if (strcmp(input, "YES") == 0 && pending_confirm) {
		pending_confirm();
	} else {
		printf("CANCELLED");
	}

	pending_confirm = NULL;
	state = CLI_PROMPT;

}

static void cli_handle_pw_new1(const char *input) {

	if (!input[0]) {
		printf("CANCELLED (EMPTY PASSWORD)");
		state = CLI_PROMPT;
		return;
	}

	strncpy(pw_first, input, CLI_PW_MAX - 1);
	pw_first[CLI_PW_MAX - 1] = 0;

	printf("CONFIRM PASSWORD:");
	state = CLI_PW_NEW2;

}

static void cli_handle_pw_new2(const char *input) {

	if (strcmp(input, pw_first) != 0) {
		printf("PASSWORD MISMATCH, CANCELLED.");
	} else if (pw_new_is_rotation) {
		if (storage_crypt_change_password(input)) {
			printf("PASSWORD CHANGED.");
		} else {
			printf("FAILED TO CHANGE PASSWORD.");
		}
	} else if (storage_crypt_enable(input)) {
		printf("FRAM ENCRYPTED AND UNLOCKED.");
		try_jump_to_fram();
	} else {
		printf("FAILED TO ENABLE ENCRYPTION.");
	}

	memset(pw_first, 0, sizeof(pw_first));
	state = CLI_PROMPT;

}

static void cli_handle_pw_unlock(const char *input) {

	if (storage_crypt_unlock(input)) {
		printf("UNLOCKED.");
		try_jump_to_fram();
	} else {
		printf("INCORRECT PASSWORD.");
	}

	state = CLI_PROMPT;

}

void cli_init(void) {

	line_len = 0;
	state = CLI_PROMPT;
	continuing = false;
	pending_confirm = NULL;
	pw_new_is_rotation = false;
	memset(pw_first, 0, sizeof(pw_first));

#ifdef BLAUSTAHL_APPS_ENABLED
	ms_glue_start_session();
#endif

	printf(VT100_CLEAR_HOME);
	printf(VT100_ERASE_SCREEN);
	printf("BLAUSTAHL CLI -- TYPE help FOR A LIST OF COMMANDS\r\n\r\nblaustahl> ");
	fflush(stdout);

}

// scans `text` and returns the net paren depth: positive means still
// inside open parens (an incomplete expression, waiting for more
// input), zero means balanced, negative means a stray/extra ')' (a
// genuine syntax error -- let this through to the normal evaluator,
// which already reports it clearly, rather than silently waiting
// forever for a closing paren that isn't missing at all).
//
// Deliberately a small, standalone scanner rather than calling
// ms_read() itself to test for completeness: ms_read() already
// distinguishes "no more input" from "unterminated list" internally,
// but both return NULL, and the unterminated case additionally logs
// a visible "unexpected end of input in list" error every time --
// which would print a confusing, spurious-looking error on every
// single line of legitimate multi-line input. This scanner never
// prints anything and never touches the interpreter at all.
//
// String literals ("...", with \" not ending the string) and
// ;-comments (to end of line) are skipped over correctly, so a paren
// inside either of those never affects the count. #\( and #\) are
// also skipped as a pair, so a parenthesis character literal doesn't
// miscount either.
static int scan_paren_depth(const char *text) {

	int depth = 0;
	const char *p = text;

	while (*p) {

		if (*p == ';') {
			while (*p && *p != '\n') p++;
			continue;
		}

		if (*p == '"') {
			p++;
			while (*p && *p != '"') {
				if (*p == '\\' && p[1]) p++;	// skip the escaped character
				p++;
			}
			if (*p == '"') p++;
			continue;
		}

		if (p[0] == '#' && p[1] == '\\' && p[2]) {
			p += 3;	// #\<char> -- that one character is never a
					// paren for counting purposes, even if it
					// literally is '(' or ')'
			continue;
		}

		if (*p == '(') depth++;
		else if (*p == ')') depth--;

		p++;

	}

	return depth;

}

void cli_redraw(void) {

	printf(VT100_CLEAR_HOME);
	printf(VT100_ERASE_SCREEN);
	printf("BLAUSTAHL CLI -- TYPE help FOR A LIST OF COMMANDS\r\n\r\n");
	printf(continuing ? "..........." : "blaustahl> ");

	bool masked = (state == CLI_PW_NEW1 || state == CLI_PW_NEW2 ||
		state == CLI_PW_UNLOCK);

	for (int i = 0; i < line_len; i++) {
		if (line[i] == '\n') {
			// a line break from accumulated multi-line input -- must
			// be \r\n on the wire (raw mode, no automatic CR), the
			// same reasoning as everywhere else this comes up
			printf("\r\n...........");
		} else {
			cdc_putchar(masked ? '*' : line[i]);
		}
	}

	fflush(stdout);

}

void cli_cancel_pending(void) {
	pending_confirm = NULL;
	pw_new_is_rotation = false;
	memset(pw_first, 0, sizeof(pw_first));
	memset(line, 0, sizeof(line));
	line_len = 0;
	state = CLI_PROMPT;
	continuing = false;
	// deliberately does NOT end the Scheme session here anymore --
	// see ms_glue_start_session()'s own comment for why the session
	// now persists across leaving and re-entering CLI mode, rather
	// than being torn down every time
}

void cli_yield(vt100_event_t ev) {

	if (ev.type == KEY_COPY && continuing) {
		memset(line, 0, sizeof(line));
		line_len = 0;
		continuing = false;
		printf("\r\nCANCELLED\r\nblaustahl> ");
		fflush(stdout);
		return;
	}

	if (ev.type != KEY_CHAR) return;	// arrows etc. ignored in CLI for now

	int c = ev.ch;

	bool masked = (state == CLI_PW_NEW1 || state == CLI_PW_NEW2 ||
		state == CLI_PW_UNLOCK);

	if (c == CH_CR) {

		line[line_len] = 0;
		printf("\r\n");

		if (state == CLI_PROMPT) {

			// still inside open parens (a multi-line Scheme
			// expression in progress) -- wait for more input rather
			// than dispatch a fragment. A negative depth (stray extra
			// ')') is a genuine syntax error, not missing input --
			// let that fall through to the normal evaluator below,
			// which already reports it clearly, rather than wait
			// forever for a closing paren that isn't actually missing.
			if (scan_paren_depth(line) > 0) {

				if (line_len < CLI_LINE_MAX - 1) {
					line[line_len++] = '\n';
					line[line_len] = 0;
				}
				// if genuinely out of buffer room, just stop
				// appending the newline -- the eventual dispatch (or
				// running out of space entirely) reports that clearly

				continuing = true;
				printf("...........");	// same width as "blaustahl> "
				fflush(stdout);
				return;

			}

			continuing = false;

			char cmd[CLI_CMD_MAX], arg1[CLI_ARG_MAX], arg2[CLI_ARG_MAX];
			parse_line(line, cmd, arg1, arg2);
			bool handled = cli_dispatch(cmd, arg1, arg2);

			if (!handled) {
#ifdef BLAUSTAHL_APPS_ENABLED
				// original, untokenized line -- see the file header
				// comment on why this can't be cmd/arg1/arg2
				ms_glue_eval_line(line);
#else
				printf("UNKNOWN COMMAND '%s'. TYPE help FOR A LIST.", cmd);
#endif
			}

		} else {

			switch (state) {
				case CLI_CONFIRM:   cli_handle_confirm(line);   break;
				case CLI_PW_NEW1:   cli_handle_pw_new1(line);   break;
				case CLI_PW_NEW2:   cli_handle_pw_new2(line);   break;
				case CLI_PW_UNLOCK: cli_handle_pw_unlock(line); break;
				default: break;
			}

		}

		// don't leave a typed password sitting in the line buffer any
		// longer than necessary
		memset(line, 0, sizeof(line));
		line_len = 0;

		// cat (unlike te/xmodem_up/scheme, which block) switches mode
		// and returns immediately -- printing the CLI prompt here
		// would corrupt whatever it just drew
		if (mode != MODE_CLI) return;

		printf("\r\nblaustahl> ");
		fflush(stdout);
		return;

	}

	if (c == CH_BS || c == CH_DEL) {
		if (line_len > 0) {
			line_len--;
			line[line_len] = 0;
			printf("\b \b");
			fflush(stdout);
		}
		return;
	}

	if (c >= 0x20 && c < 0x7f && line_len < CLI_LINE_MAX - 1) {
		line[line_len++] = c;
		cdc_putchar(masked ? '*' : c);
	}

}
