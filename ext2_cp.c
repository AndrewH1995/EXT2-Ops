/**
 * This program takes three command line arguments. The first is the name of an ext2 formatted
 * virtual disk. The second is the path to a file on your native operating system, and the third is
 * an absolute path on your ext2 formatted disk. The program should work like cp, copying the file
 * on your native file system onto the specified location on the disk. If the source file does not
 * exist or the target is an invalid path, then your program should return the appropriate error
 * (ENOENT). If the target is a file with the same name that already exists, you should not
 * overwrite it (as cp would), just return EEXIST instead.
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ext2.h"
#include "utils.h"

unsigned char *disk;

// ---------- HELPER FUNCTIONS ----------
/**
 * Check if the given file path exist
 * @param  f_path file path on local host
 * @param  stats  stats struct
 * @return        0 on exist
 */
int check_local_file(char const *f_path, struct stat *stats) {
	if (stat(f_path, stats) == -1) {
		perror("main: stat: ");
		return -ENOENT;
	}
	if (!S_ISREG(stats->st_mode)) {
		fprintf(stderr, "check_local_file: local file [%s] needs to be a regular file.\n", f_path);
		return -ENOENT;
	}
	return 0;
}


int main(int argc, char const *argv[]) {
	if (argc != 4) {
		fprintf(stderr, "Usage: %s <image file name> <local path> <absolute path>\n", argv[0]);
		exit(-1);
	}

	int result;

	if ((result = init(&disk, argv[1])) != 0) {
		fprintf(stderr, "main: init\n");
		return result;
	}

	// check if the given local path is valid
	struct stat *stats = malloc(sizeof(struct stat)); // TODO: FREE
	if ((result = check_local_file(argv[2], stats)) < 0) {
		fprintf(stderr, "main: check_local_file\n");
		return result;
	}

	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(disk + EXT2_BLOCK_SIZE * 2);
	struct ext2_inode *inode_table =
		(struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_table);

	// parse the absolute path into the path and the dir's name
	char *path = NULL; // FREE
	char *name = NULL; // FREE
	if ((result = parse_path(argv[3], &path, &name)) != 0) {
		fprintf(stderr, "main: parse_path\n");
		return result;
	}

	// find parent dir's inode index
	int parent_idx;

	struct ext2_inode *root_inode = &(inode_table[EXT2_ROOT_INO - 1]);
	struct ext2_dir_entry *root_dir =
		(struct ext2_dir_entry *)(disk + (EXT2_BLOCK_SIZE * root_inode->i_block[0]));
	if (strncmp("/", path, strlen(path)) == 0) {
		parent_idx = 2;
	} else if ((parent_idx = find_idx(disk, basename(path), root_dir)) < 0) {
		fprintf(stderr, "main: find_idx parent\n");
		return parent_idx;
	}

	// find parent inode
	struct ext2_inode *parent_inode = &(inode_table[parent_idx - 1]);
	if (!(parent_inode->i_mode & EXT2_S_IFDIR)) {
		fprintf(stderr, "Invalid parent file type! %i\n", parent_inode->i_mode);
		return -ENOENT;
	}

	// // search for the file/lnk's inode
	int curr_idx;

	if ((curr_idx = find_idx(disk, name, root_dir)) > 0) {
		fprintf(stderr, "main: file already exists\n");
		return -EEXIST;
	}

	// create inode for the new file on disk
	int current_inode_idx;
	if ((current_inode_idx = new_inode(&disk)) < 0) {
		fprintf(stderr, "main: new_inode\n");
		return current_inode_idx;
	}
	init_inode(&disk, current_inode_idx);

	struct ext2_inode *curr_inode = &(inode_table[current_inode_idx - 1]);

	curr_inode->i_mode = EXT2_S_IFREG;
	curr_inode->i_ctime = (unsigned int)time(NULL);
	curr_inode->i_size = stats->st_size;
	curr_inode->i_links_count = 1;


	// Allocate block
	// int blocks_needed = (int)ceil(stats->st_size / EXT2_BLOCK_SIZE);
	int blocks_needed = stats->st_size / EXT2_BLOCK_SIZE;
	if (stats->st_size % EXT2_BLOCK_SIZE != 0) {
		blocks_needed++;
	}
	if (blocks_needed == 0) {
		blocks_needed++;
	}
	if (blocks_needed > group_desc->bg_free_blocks_count) {
		fprintf(stderr, "main: blocks not enough for file\n");
		return -ENOSPC;
	}
	curr_inode->i_blocks = blocks_needed;

	for (int block_num = 0; block_num < blocks_needed; block_num++) {
		int new_block_idx;
		if ((new_block_idx = new_block(&disk)) < 0) {
			fprintf(stderr, "main: new_block\n");
			return -new_block_idx;
		}

		for (int idx = 0; idx < 12; idx++) {
			if (curr_inode->i_block[idx] == 0) {
				curr_inode->i_block[idx] = new_block_idx;
				break;
			}
		}
	}

	if ((result = update_dir_entry(&disk, parent_inode, current_inode_idx, name,
								   EXT2_FT_REG_FILE)) < 0) {
		return result;
	}

	free(path);
	free(name);

	return 0;
}
