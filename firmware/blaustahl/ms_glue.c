/*
 * Machdyne Scheme (ms) integration -- the CLI itself is the REPL now.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * Uses the patched embedding API (see patches/ms-lix-embedding-fixes.patch):
 *   - ms_init_lix(true)/ms_deinit() malloc/free the heap, protect stack,
 *     and load the standard library (ms_stdlib.h -- map, reduce, filter,
 *     append, caar/cadr, etc; plain ms_init() alone does NOT load this),
 *     so an idle interpreter costs nothing while the CLI isn't in use --
 *     called once on cli_init() and once on leaving the CLI, not once
 *     per line.
 *   - ms_panic_before_try()/setjmp(ms_panic_recovery) let a Scheme-level
 *     panic (car of a non-pair, calling a non-procedure, heap
 *     exhaustion, etc -- the COMMON case while experimenting
 *     interactively, not a rare failure) be caught and reported without
 *     killing the firmware process.
 *
 * IMPORTANT: per ms.c's own documented contract, setjmp(ms_panic_recovery)
 * must be called DIRECTLY in the same stack frame that goes on to call
 * ms_eval() -- not in a helper that sets it up and returns before the
 * protected work runs, since longjmp() can only target a still-live
 * stack frame. That's why the setjmp lives directly in
 * ms_glue_eval_line() itself, fresh on every call, rather than once in
 * ms_glue_start_session() -- start_session() returns immediately after
 * ms_init(), which would leave any later panic jumping into an already-
 * exited frame.
 *
 * (exit) is caught the same way (longjmp code 2, vs 1 for a genuine
 * panic) but has nothing to "exit" back to anymore -- there's no
 * separate Scheme mode to leave, the CLI itself is always here. It
 * just prints a brief note and the CLI carries on normally, rather
 * than doing anything surprising to the firmware's navigation.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>
#include <malloc.h>

#include "blaustahl.h"
#include "flash_storage.h"
#include "ms_glue.h"

// general system heap (the malloc() arena everything shares -- not
// just Scheme's own fixed cell pool). Exposed here, not just in
// cli.c's `info` command, specifically so ms_glue_start_session() can
// print it BEFORE attempting any allocation -- if something panics
// during that allocation, this is the one number that still gets
// seen, since `info` can never be reached if session start itself
// never returns. Same widely-used pico-sdk idiom either way: total is
// the gap between the end of .bss/.data and the start of the stack
// per the default linker script, free is that total minus whatever
// mallinfo() reports as currently allocated.
uint32_t __attribute__((weak)) ms_glue_system_heap_total(void) {
	extern char __StackLimit, __bss_end__;
	return (uint32_t)(&__StackLimit - &__bss_end__);
}

uint32_t __attribute__((weak)) ms_glue_system_heap_free(void) {
	struct mallinfo m = mallinfo();
	return ms_glue_system_heap_total() - (uint32_t)m.uordblks;
}

typedef struct ms_val ms_val;

extern ms_val *ms_global_env;
extern void ms_init_lix(bool stdlib);
extern void ms_deinit(void);
extern ms_val *ms_read(const char **s);
extern ms_val *ms_eval(ms_val *x, ms_val *env);
extern void ms_print(ms_val *v, bool readable);
extern void ms_load_string(const char *src, ms_val *env);

extern jmp_buf ms_panic_recovery;
extern bool ms_exit_requested;
extern int ms_exit_code;
extern void ms_panic_before_try(void);
extern void ms_panic_after_recover(void);
extern void ms_panic_disarm(void);

// getch() (declared in fs.h, called by te.c under its own embedded
// build flag) is implemented once, in te_glue.c -- te_glue.c and
// ms_glue.c are always compiled together (both gated by the same
// BLAUSTAHL_ENABLE_APPS option), so there is exactly one definition
// in the final binary. ms_glue.c itself no longer needs getch() at
// all -- there's no blocking read_line() anymore, the CLI's own
// cooperative line editor in cli.c handles all input now, and just
// hands ms_glue_eval_line() a complete line once Enter is pressed.

// true only once ms_init_lix() has actually run this session -- false
// the whole time if ms_glue_start_session()'s pre-check above refused
// to attempt it. Every other function below checks this first, so a
// refused session fails safely (a clear message) rather than
// operating on an interpreter that was never actually set up.
static bool session_ready = false;

void ms_glue_start_session(void) {

	// idempotent: once a session is up, re-entering the CLI (e.g.
	// after switching to FRAM/SRAM/VIEWER/FILES/HELP via the menu and
	// back) must NOT reset it. ms_init() unconditionally wipes
	// ms_heap_live/ms_symbols/environment state on every call --
	// malloc guard or not -- so calling through to it again here
	// would silently discard every definition the session has made,
	// even though the heap itself was never freed. The session now
	// persists for the whole power-on lifetime once started, until
	// either the CLI's `clear` command (see cli.c) or a reboot -- not
	// torn down just because CLI mode itself was left and re-entered.
	if (session_ready) return;

	// print this BEFORE attempting any allocation at all -- if what
	// follows panics (this firmware links against the pico-sdk's
	// default malloc() wrapper, which panics immediately and
	// unrecoverably on ANY failed allocation anywhere, via
	// PICO_MALLOC_PANIC -- a hard failure ms.c's own, catchable
	// setjmp-based panic handling below never even gets a chance to
	// see), this is the one number that still made it out.
	uint32_t sys_free = ms_glue_system_heap_free();
	uint32_t sys_total = ms_glue_system_heap_total();
	printf("SCHEME: SYSTEM HEAP %u/%u BYTES FREE BEFORE INIT\r\n", sys_free, sys_total);
	fflush(stdout);

	// best-effort pre-check: MS_HEAP_SIZE is a compile-time constant
	// (ms_gc_heap_size() returns it correctly even before ms_init()
	// has ever run), so this can be checked before attempting the
	// allocation at all. NOT a complete check -- ms_init() also
	// allocates a separate MS_PROTECT_STACK_SIZE-sized block, which
	// isn't currently queryable the same way, so this can still miss
	// a failure caused by THAT allocation specifically. Still worth
	// having: it catches the single most likely failure (the cell
	// pool itself, generally the larger of the two) before ever
	// reaching the pico-sdk's own hard, unrecoverable panic.
	extern long ms_gc_heap_size(void);
	long needed = ms_gc_heap_size() * 24;	// verified bytes/cell on this
											// target, see docs/ms-memory.md
	if ((uint32_t)needed > sys_free) {
		printf("SCHEME: REFUSING TO INIT -- NEEDS ~%ld BYTES FOR THE CELL "
			"POOL ALONE, ONLY %u FREE. REDUCE MS_HEAP_SIZE.\r\n",
			needed, sys_free);
		fflush(stdout);
		return;	// session_ready stays false
	}

	// bare init first (no stdlib) -- this is the allocation the
	// pre-check above was sized against, and keeping it separate from
	// stdlib loading means a failure in either one is diagnosable on
	// its own rather than lumped together
	ms_init_lix(false);
	session_ready = true;

	// load stdlib automatically -- real hardware testing (see
	// docs/ms-memory.md) confirmed the memory budget comfortably
	// supports this after freeing xmodem's old static 64KB
	// reservation. Goes through ms_glue_load_stdlib() rather than
	// calling ms_init_lix(true) directly: stdlib loading does its own
	// additional allocations (symbol/string interning for every
	// definition) beyond the base cell pool the pre-check above
	// already accounted for, so it needs its own panic protection --
	// if loading it specifically is what fails, that's caught
	// gracefully here and the session still remains usable with
	// native builtins only, rather than an unprotected call taking
	// down the whole session start.
	if (!ms_glue_load_stdlib()) {
		printf("SCHEME: STDLIB FAILED TO LOAD -- CONTINUING WITHOUT IT "
			"(NATIVE BUILTINS STILL WORK; LEAVE AND RE-ENTER THE CLI TO RETRY).\r\n");
		fflush(stdout);
	}

}

void ms_glue_end_session(void) {
	if (!session_ready) return;
	ms_panic_disarm();
	ms_deinit();
	session_ready = false;
}

// loads the standard library into the CURRENT, already-running
// session. Called automatically from ms_glue_start_session() right
// after the base interpreter is up -- this function exists on its
// own (rather than being inlined there) because it needs its own
// panic protection: stdlib loading does its own additional
// allocations (symbol/string interning for every definition) beyond
// the base cell pool, so it can fail independently of the base init
// succeeding.
//
// Verified this is safe to call after ms_glue_start_session() has
// already run: ms_init_lix()'s underlying ms_init() guards its
// malloc() calls (skips re-allocating an already-allocated heap) but
// unconditionally resets ms_heap_live/ms_symbols/environment state
// either way, so calling it again with stdlib=true correctly loads
// the stdlib into a fresh interpreter state, same as if it had been
// requested from the start -- confirmed empirically, not just from
// reading the guard logic.
//
// Returns false if loading it panicked (most likely out of memory);
// the panic is still caught and reported via ms_log(), same as any
// other panic -- this just also gives the caller a clean yes/no to
// react to.
bool ms_glue_load_stdlib(void) {

	if (!session_ready) {
		printf("SCHEME SESSION NEVER INITIALIZED -- SEE THE MESSAGE ABOVE.");
		fflush(stdout);
		return false;
	}

	ms_panic_before_try();
	int sig = setjmp(ms_panic_recovery);

	if (sig == 0) {
		ms_init_lix(true);
		fflush(stdout);
		return true;
	}

	if (sig == 2) {
		// (exit) during stdlib loading would be unusual, but handle
		// it the same way as everywhere else rather than leave it
		// unhandled
		printf("(exit) called while loading stdlib.\r\n");
	} else {
		ms_panic_after_recover();
	}

	fflush(stdout);
	return false;

}

// thin wrappers around ms.c's own ms_gc_live_count()/ms_gc_heap_size()
// -- exposed here so cli.c (which only ever talks to ms.c through
// this file) can report them in `info` without reaching into ms.c's
// externs directly.
extern long ms_gc_live_count(void);
extern long ms_gc_heap_size(void);

long ms_glue_heap_live(void) { return ms_gc_live_count(); }
long ms_glue_heap_size(void) { return ms_gc_heap_size(); }

void ms_glue_eval_line(const char *line) {

	if (!session_ready) {
		printf("SCHEME SESSION NEVER INITIALIZED -- SEE THE MESSAGE ABOVE.\r\n");
		fflush(stdout);
		return;
	}

	// setjmp() directly in this frame, fresh every call -- see the
	// file header for why this can't be hoisted into a helper or
	// called once for the whole session
	ms_panic_before_try();
	int sig = setjmp(ms_panic_recovery);

	if (sig == 0) {

		// evaluate every top-level form on the line, matching the
		// desktop REPL's own documented behavior (e.g. "(+ 1 2) (+ 3 4)"
		// prints two results)
		const char *p = line;
		while (1) {
			ms_val *form = ms_read(&p);
			if (!form) break;
			ms_val *result = ms_eval(form, ms_global_env);
			ms_print(result, true);
			printf("\r\n");
		}

	} else if (sig == 2) {

		// (exit) was called -- nothing to exit back to anymore, the
		// CLI itself is always here. Report it plainly and carry on;
		// cli.c prints the next prompt right after this returns.
		printf("(exit) has no effect here -- the CLI is always available.\r\n");
		(void)ms_exit_requested;
		(void)ms_exit_code;

	} else {

		// a panic was caught; ms_log() already printed what went
		// wrong (to stderr, which in this build shares the same wire
		// as stdout -- that's fine here, this IS the interactive UI
		// right now)
		ms_panic_after_recover();

	}

	fflush(stdout);

}

// generous but bounded -- matches te's own max file size, since that's
// the tool used to write these programs in the first place; no point
// allowing a load of something larger than what could actually be
// edited on-device
#define MS_LOAD_MAX_SIZE 32768

void ms_glue_load_file(const char *filename) {

	if (!session_ready) {
		printf("SCHEME SESSION NEVER INITIALIZED -- SEE THE MESSAGE ABOVE.");
		fflush(stdout);
		return;
	}

	uint32_t size;
	if (!flash_storage_file_size(filename, &size)) {
		printf("FILE NOT FOUND: '%s'", filename);
		return;
	}

	if (size > MS_LOAD_MAX_SIZE) {
		printf("FILE TOO LARGE TO LOAD (%u BYTES, MAX %u)", size, MS_LOAD_MAX_SIZE);
		return;
	}

	static char buf[MS_LOAD_MAX_SIZE + 1];
	uint32_t got = flash_storage_read(filename, 0, buf, size);
	buf[got] = 0;

	// same fresh-setjmp-per-call pattern as eval_line, and for the
	// same reason -- a buggy or incomplete user program is the COMMON
	// case here, not a rare failure, and shouldn't be able to take
	// down the CLI session
	ms_panic_before_try();
	int sig = setjmp(ms_panic_recovery);

	if (sig == 0) {

		ms_load_string(buf, ms_global_env);
		printf("LOADED '%s' OK", filename);

	} else if (sig == 2) {

		printf("(exit) CALLED WHILE LOADING '%s' -- LOAD STOPPED THERE.", filename);
		(void)ms_exit_requested;
		(void)ms_exit_code;

	} else {

		ms_panic_after_recover();
		printf("LOAD ABORTED: PANIC WHILE EVALUATING '%s'", filename);

	}

	fflush(stdout);

}
