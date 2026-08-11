#ifndef MS_GLUE_H_
#define MS_GLUE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Wires machdyne/ms (Machdyne Scheme, the -DLIX embedded build) into
 * the CLI itself -- the CLI IS the Scheme REPL now, not a separate
 * mode reached by typing "scheme". See cli.c's file header for the
 * full "how do we tell a command from an expression" story.
 *
 * ms_glue_start_session() / ms_glue_end_session() malloc/free the
 * interpreter's heap and protect stack (ms_init_lix()/ms_deinit(),
 * patched into ms.c -- see patches/ms-lix-embedding-fixes.patch), the
 * same way an old "scheme" session used to cost nothing while idle --
 * called once when entering the CLI and once when leaving it, not
 * once per line.
 *
 * Standard library loading happens automatically on session start
 * (see ms_glue_start_session() in ms_glue.c) -- the memory budget was
 * confirmed on real hardware to comfortably support this. If it ever
 * fails (a tight-memory edge case, not the normal path),
 * ms_glue_load_stdlib() is retried automatically the next time the
 * CLI is entered; there's no user-facing command for it, since normal
 * startup already covers it.
 *
 * ms_glue_eval_line() evaluates a single line (which may contain more
 * than one top-level form, same as before) and prints results/errors
 * directly, the same way cli_dispatch() prints its own output
 * directly rather than returning a string.
 *
 * ms_glue_load_file() loads and evaluates a flash file as Scheme
 * source, quietly (no per-form result printing, matching ordinary
 * Lisp `load` semantics -- unlike eval_line, which is deliberately a
 * REPL that prints every result) -- for programs written with `te`
 * and run with the CLI's `load` command.
 */

void ms_glue_start_session(void);
void ms_glue_end_session(void);
void ms_glue_eval_line(const char *line);
void ms_glue_load_file(const char *filename);

// loads the standard library into the current session on demand --
// see the long comment on this function in ms_glue.c. Returns false
// if loading it panicked.
bool ms_glue_load_stdlib(void);

// the Scheme interpreter's own fixed cell pool: how many cells are
// currently live, and the pool's total configured capacity
// (MS_HEAP_SIZE). Does not include general, separately-malloc'd
// memory (e.g. string/symbol storage) -- see docs/ms-memory.md.
long ms_glue_heap_live(void);
long ms_glue_heap_size(void);

// the general system heap (the malloc() arena everything shares, not
// just Scheme's own cell pool) -- see docs/ms-memory.md for why this
// exists: distinguishing "Scheme's fixed pool ran out" from "the
// general allocator itself is the bottleneck" was the whole point of
// adding it.
uint32_t ms_glue_system_heap_total(void);
uint32_t ms_glue_system_heap_free(void);

#endif
