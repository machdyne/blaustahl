#ifndef FS_H_
#define FS_H_
/* real prototypes -- te.c calls these directly under -DEMBEDDED, and
 * without a proper declaration in scope, fs_mallocfile()'s pointer
 * return value gets silently truncated on 64-bit hosts (caught during
 * testing; harmless-looking on 32-bit ARM only by coincidence, since
 * int and pointers are the same size there -- not something to rely
 * on). ms.c under -DLIX includes this too but never calls any of
 * these (LIX compiles all file I/O out) -- unused declarations are
 * harmless, so one shared header covers both. Implemented in
 * te_glue.c / ms_glue.c. */
int fs_size(char *filename);
char *fs_mallocfile(char *filename);
int fs_write_file(char *filename, char *buf, int len);
int getch(void);
#endif
