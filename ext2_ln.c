/*
 * This program takes three command line arguments. The first is the src_name of an ext2 formatted
 * virtual disk. The other two are absolute paths on your ext2 formatted disk. The program should
 * work like ln, creating a link from the first specified file to the second specified src_path. This
 * program should handle any exceptional circumstances, for example: if the source file does not
 * exist (ENOENT), if the link src_name already exists (EEXIST), if a hardlink refers to a directory
 * (EISDIR), etc. then your program should return the appropriate error code. Additionally, this
 * command may take a "-s" flag, after the disk image argument. When this flag is used, your program
 * must create a symlink instead (other arguments remain the same).
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

#include "ext2.h"
#include "utils.h"

unsigned char *disk;

// ---------- HELPER FUNCTIONS ----------


int main(int argc, char const *argv[]) {
	if (argc < 4 || argc > 5) {
		fprintf(stderr, "Usage: %s <image file src_name> [-s] <src path> <dest dest_path>\n",
				argv[0]);
		return -EINVAL;
	}

	int soft_link = 0;
	char const *src_full_path = argv[2];
	char const *dest_full_path = argv[3];

	if (argc == 5) {
		if (strncmp(argv[2], "-s", 2) == 0) {
			soft_link = 1;
			src_full_path = argv[3];
			dest_full_path = argv[4];
		} else {
			fprintf(stderr, "Usage: %s <image file src_name> [-s] <src path> <dest dest_path>\n",
					argv[0]);
			return -EINVAL;
		}
	}
	unsigned long src_len = strlen(src_full_path);

	int result;

	if ((result = init(&disk, argv[1])) != 0) {
		fprintf(stderr, "main: init\n");
		return result;
	}
	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(disk + EXT2_BLOCK_SIZE * 2);
	struct ext2_inode *inode_table =
		(struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_table);

	// parse the absolute src_path into the src_path and the dir's src_name
	char *src_path = NULL;	 // FREE
	char *src_name = NULL; // FREE
	if ((result = parse_path(src_full_path, &src_path, &src_name)) != 0) {
		fprintf(stderr, "main: parse_path\n");
		return result;
	}

	// find parent dir's inode index

	struct ext2_inode *root_inode = &(inode_table[EXT2_ROOT_INO - 1]);
	struct ext2_dir_entry *root_dir =
		(struct ext2_dir_entry *)(disk + (EXT2_BLOCK_SIZE * root_inode->i_block[0]));

	// // search for the file/lnk's inode
	int src_idx;

	if (strncmp("/", src_path, strlen(src_path)) == 0) {
		src_idx = 2;
	} else if ((src_idx = find_idx(disk, src_name, root_dir)) < 0) {
		fprintf(stderr, "main: src file does not exists\n");
		return -ENOENT;
	}


	// parse the absolute dest_path into the dest_path and the dir's dest_lnk
	char *dest_path = NULL;	 // FREE
	char *dest_lnk = NULL; // FREE
	if ((result = parse_path(dest_full_path, &dest_path, &dest_lnk)) != 0) {
		fprintf(stderr, "main: parse_path\n");
		return result;
	}

	// find parent dir's inode index
	int dest_parent_idx;

	if (strncmp("/", dest_path, strlen(dest_path)) == 0) {
		dest_parent_idx = 2;
	} else if ((dest_parent_idx = find_idx(disk, basename(dest_path), root_dir)) < 0) {
		fprintf(stderr, "main: find_idx parent\n");
		return dest_parent_idx;
	}

	// find parent inode
	struct ext2_inode *parent_inode = &(inode_table[dest_parent_idx - 1]);
	if (!(parent_inode->i_mode & EXT2_S_IFDIR)) {
		fprintf(stderr, "Invalid parent file type! %i\n", parent_inode->i_mode);
		return -ENOENT;
	}

	// // search for the file/lnk's inode
	int dest_idx;

	if (strncmp("/", dest_path, strlen(dest_path)) == 0) {
		dest_idx = 2;
	} else if ((dest_idx = find_idx(disk, dest_lnk, root_dir)) > 0) {
		fprintf(stderr, "main: dest file already exists\n");
		return -EEXIST;
	}

	// find parent inode
	struct ext2_inode *dest_parent_inode = &(inode_table[dest_parent_idx - 1]);
	if (!(dest_parent_inode->i_mode & EXT2_S_IFDIR)) {
		fprintf(stderr, "Invalid parent file type! %i\n", dest_parent_inode->i_mode);
		return -ENOENT;
	}


	if (soft_link) {
		// Soft link
		int soft_lnk_idx;
		if ((soft_lnk_idx = new_inode(&disk)) < 0) {
			fprintf(stderr, "main: new_inode\n");
			return soft_lnk_idx;
		}
		init_inode(&disk, soft_lnk_idx);

		struct ext2_inode *soft_lnk_inode = &(inode_table[soft_lnk_idx - 1]);

		soft_lnk_inode->i_mode = EXT2_S_IFLNK;
		soft_lnk_inode->i_ctime = (unsigned int)time(NULL);
		soft_lnk_inode->i_size = src_len;
		soft_lnk_inode->i_links_count = 2;

		// Allocate block
		// int blocks_needed = (int)ceil(strlen(src_full_path) / EXT2_BLOCK_SIZE);
		int blocks_needed = strlen(src_full_path) / EXT2_BLOCK_SIZE;
		if (strlen(src_full_path) % EXT2_BLOCK_SIZE != 0) {
			blocks_needed++;
		}
		if (blocks_needed == 0) {
			blocks_needed++;
		}
		if (blocks_needed > group_desc->bg_free_blocks_count) {
			fprintf(stderr, "main: blocks not enough for file\n");
			return -ENOSPC;
		}
		soft_lnk_inode->i_blocks = blocks_needed;

		for (int block_num = 0; block_num < blocks_needed; block_num++) {
			int new_block_idx;
			if ((new_block_idx = new_block(&disk)) < 0) {
				fprintf(stderr, "main: new_block\n");
				return -new_block_idx;
			}

			for (int idx = 0; idx < 12; idx++) {
				if (soft_lnk_inode->i_block[idx] == 0) {
					soft_lnk_inode->i_block[idx] = new_block_idx;
					break;
				}
			}
		}

		result = update_dir_entry(&disk, dest_parent_inode, soft_lnk_idx, dest_lnk, EXT2_FT_SYMLINK);
		if (result < 0) {
			return result;
		}


	} else {
		// Hard link
		result = update_dir_entry(&disk, dest_parent_inode, src_idx, dest_lnk, EXT2_FT_REG_FILE);
		if (result < 0) {
			return result;
		}
	}


	free(src_path);
	free(src_name);
	free(dest_path);
	free(dest_lnk);

	return 0;
}
