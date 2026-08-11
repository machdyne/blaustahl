/*
 * Builds a littlefs filesystem image, in memory, matching this
 * project's EXACT partition parameters (see flash_storage.c) -- same
 * littlefs source, same block_size/prog_size/block_count/block_cycles,
 * so the resulting image is guaranteed mountable by the real firmware,
 * not just "should be compatible."
 *
 * Populates it with every regular file found in a given directory
 * (non-recursive -- littlefs's root directory is flat here, matching
 * how this firmware's own flash file listing works: the browser only
 * ever lists root-level files, so even if this tool packed files from
 * subdirectories, the firmware couldn't show them anyway), then writes
 * the whole image out as a raw binary, ready for bin2uf2.
 *
 * Every skipped entry is reported with the specific reason (not a
 * regular file, name too long, etc) rather than silently discarded --
 * and if the result would be an image with zero files in it, this
 * exits with an error instead of happily writing out something
 * useless. An empty "pre-populated" image is always a mistake, not a
 * valid result: there's no reason to run this tool at all if you
 * didn't want any files loaded.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

#include "lfs.h"

#define FS_BLOCK_SIZE   4096u
#define FS_PROG_SIZE    256u
#define FS_BLOCK_COUNT  512u	// 2MB partition (2*1024*1024 / 4096)
#define FS_IMAGE_SIZE   (FS_BLOCK_SIZE * FS_BLOCK_COUNT)

static uint8_t *image;

static int img_read(const struct lfs_config *c, lfs_block_t block,
		lfs_off_t off, void *buffer, lfs_size_t size) {
	(void)c;
	memcpy(buffer, &image[block * FS_BLOCK_SIZE + off], size);
	return 0;
}

static int img_prog(const struct lfs_config *c, lfs_block_t block,
		lfs_off_t off, const void *buffer, lfs_size_t size) {
	(void)c;
	memcpy(&image[block * FS_BLOCK_SIZE + off], buffer, size);
	return 0;
}

static int img_erase(const struct lfs_config *c, lfs_block_t block) {
	(void)c;
	memset(&image[block * FS_BLOCK_SIZE], 0xff, FS_BLOCK_SIZE);
	return 0;
}

static int img_sync(const struct lfs_config *c) {
	(void)c;
	return 0;
}

static uint8_t read_buf[FS_PROG_SIZE];
static uint8_t prog_buf[FS_PROG_SIZE];
static uint8_t lookahead_buf[64];

static const struct lfs_config cfg = {
	.read = img_read,
	.prog = img_prog,
	.erase = img_erase,
	.sync = img_sync,

	.read_size = FS_PROG_SIZE,
	.prog_size = FS_PROG_SIZE,
	.block_size = FS_BLOCK_SIZE,
	.block_count = FS_BLOCK_COUNT,
	.block_cycles = 500,

	.cache_size = FS_PROG_SIZE,
	.lookahead_size = sizeof(lookahead_buf),

	.read_buffer = read_buf,
	.prog_buffer = prog_buf,
	.lookahead_buffer = lookahead_buf,
};

int main(int argc, char **argv) {

	if (argc != 3) {
		fprintf(stderr, "usage: %s <input-dir> <output.bin>\n", argv[0]);
		return 1;
	}

	const char *input_dir = argv[1];
	const char *output_path = argv[2];

	image = malloc(FS_IMAGE_SIZE);
	if (!image) {
		fprintf(stderr, "error: couldn't allocate %u bytes for the image\n",
			FS_IMAGE_SIZE);
		return 1;
	}
	memset(image, 0xff, FS_IMAGE_SIZE);

	lfs_t lfs;

	if (lfs_format(&lfs, &cfg) != 0) {
		fprintf(stderr, "error: lfs_format failed\n");
		return 1;
	}
	if (lfs_mount(&lfs, &cfg) != 0) {
		fprintf(stderr, "error: lfs_mount failed\n");
		return 1;
	}

	DIR *dir = opendir(input_dir);
	if (!dir) {
		fprintf(stderr, "error: couldn't open directory '%s'\n", input_dir);
		return 1;
	}

	int file_count = 0;
	int skipped_count = 0;
	struct dirent *ent;

	while ((ent = readdir(dir)) != NULL) {

		// "." and ".." are always present in a directory listing --
		// skip them quietly, they're not worth reporting
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		char full_path[4096];
		snprintf(full_path, sizeof(full_path), "%s/%s", input_dir, ent->d_name);

		struct stat st;
		if (stat(full_path, &st) != 0) {
			fprintf(stderr, "  skipping %-31s -- couldn't stat it\n", ent->d_name);
			skipped_count++;
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			fprintf(stderr, "  skipping %-31s -- it's a directory "
				"(this tool doesn't recurse -- flatten your files into "
				"one directory first; the firmware's own file browser "
				"can't see subdirectories either way)\n", ent->d_name);
			skipped_count++;
			continue;
		}

		if (!S_ISREG(st.st_mode)) {
			fprintf(stderr, "  skipping %-31s -- not a regular file\n", ent->d_name);
			skipped_count++;
			continue;
		}

		if (strlen(ent->d_name) >= 32) {
			fprintf(stderr, "  skipping %-31s -- name too long "
				"(max 31 chars, matching STORAGE_NAME_LEN)\n", ent->d_name);
			skipped_count++;
			continue;
		}

		FILE *src = fopen(full_path, "rb");
		if (!src) {
			fprintf(stderr, "  skipping %-31s -- couldn't open it for reading\n",
				ent->d_name);
			skipped_count++;
			continue;
		}

		lfs_file_t lf;
		if (lfs_file_open(&lfs, &lf, ent->d_name,
				LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != 0) {
			fprintf(stderr, "  skipping %-31s -- couldn't create it in the image\n",
				ent->d_name);
			fclose(src);
			skipped_count++;
			continue;
		}

		char buf[4096];
		size_t got;
		lfs_ssize_t total_written = 0;
		bool write_failed = false;

		while ((got = fread(buf, 1, sizeof(buf), src)) > 0) {
			lfs_ssize_t w = lfs_file_write(&lfs, &lf, buf, got);
			if (w < 0 || (size_t)w != got) { write_failed = true; break; }
			total_written += w;
		}

		lfs_file_close(&lfs, &lf);
		fclose(src);

		if (write_failed) {
			fprintf(stderr, "error: failed writing '%s' into the image "
				"(image may be full -- max %u bytes total)\n",
				ent->d_name, FS_IMAGE_SIZE);
			return 1;
		}

		printf("  added    %-31s %ld bytes\n", ent->d_name, (long)total_written);
		file_count++;

	}

	closedir(dir);
	lfs_unmount(&lfs);

	if (file_count == 0) {
		fprintf(stderr, "\nerror: no files were added -- refusing to write an "
			"empty image (that's never what you actually want).\n");
		if (skipped_count > 0) {
			fprintf(stderr, "%d entries were found in '%s' but all of them were "
				"skipped -- see the reasons listed above.\n", skipped_count, input_dir);
		} else {
			fprintf(stderr, "'%s' appears to be empty, or doesn't exist.\n", input_dir);
		}
		return 1;
	}

	FILE *out = fopen(output_path, "wb");
	if (!out) {
		fprintf(stderr, "error: couldn't open '%s' for writing\n", output_path);
		return 1;
	}
	fwrite(image, 1, FS_IMAGE_SIZE, out);
	fclose(out);

	printf("wrote %s (%u bytes, %d files, %d skipped)\n",
		output_path, FS_IMAGE_SIZE, file_count, skipped_count);

	return 0;

}
