/*
 * This program takes two command line arguments. The first is the name of an
 * ext2 formatted virtual disk. The second is an absolute path on your ext2
 * formatted disk. The program should work like mkdir, creating the final
 * directory on the specified path on the disk.
 * If any component on the path to the location where the final directory is to
 * be created does not exist or if the specified directory already exists, then
 * your program should return the appropriate error (ENOENT or EEXIST).
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ext2.h"
#include "utils.h"

unsigned char *disk;

// ---------- Helper Functions ----------


int main(int argc, char const *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <image file name> <absolute path>\n", argv[0]);
		return -1;
	}

	int result;

	if ((result = init(&disk, argv[1])) != 0) {
		fprintf(stderr, "main: init\n");
		return result;
	}

	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(disk + EXT2_BLOCK_SIZE * 2);
	struct ext2_inode *inode_table =
		(struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_table);

	// parse the absolute path into the path and the dir's name
	char *path = NULL; // FREE
	char *name = NULL; // FREE
	if ((result = parse_path(argv[2], &path, &name)) != 0) {
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

	if (strncmp("/", path, strlen(path)) == 0) {
		curr_idx = 2;
	} else if ((curr_idx = find_idx(disk, name, root_dir)) > 0) {
		fprintf(stderr, "main: file already exists\n");
		return -EEXIST;
	}

	// create inode
	int new_dir_idx;
	if ((new_dir_idx = new_inode(&disk)) < 0) {
		fprintf(stderr, "main: new_inode\n");
		return new_dir_idx;
	}
	init_inode(&disk, new_dir_idx);

	int new_block_idx;
	if ((new_block_idx = new_block(&disk)) < 0) {
		fprintf(stderr, "main: new_block\n");
		return -new_block_idx;
	}

	struct ext2_inode *curr_inode = &(inode_table[new_dir_idx - 1]);


	for (int idx = 0; idx < 12; idx++) {
		if (curr_inode->i_block[idx] == 0) {
			curr_inode->i_block[idx] = new_block_idx;
			break;
		}
	}

	curr_inode->i_mode = EXT2_S_IFDIR;
	curr_inode->i_links_count += 2;
	curr_inode->i_size = EXT2_BLOCK_SIZE;

	// add . and .. in dir entry
	struct ext2_dir_entry *curr_dir =
		(struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * curr_inode->i_block[0]);
	curr_dir->inode = new_dir_idx;
	curr_dir->name_len = 1; // '.'
	strcpy(curr_dir->name, ".");
	curr_dir->rec_len = sizeof(struct ext2_dir_entry) + curr_dir->name_len;
	if (curr_dir->rec_len % 4 != 0) {
		curr_dir->rec_len += 4 - curr_dir->rec_len % 4;
	}
	curr_dir->file_type = EXT2_FT_DIR;

	curr_dir = curr_dir + curr_dir->rec_len;
	curr_dir->inode = parent_idx;
	curr_dir->name_len = 2; // '..'
	strcpy(curr_dir->name, "..");
	curr_dir->rec_len = sizeof(struct ext2_dir_entry) + curr_dir->name_len;
	if (curr_dir->rec_len % 4 != 0) {
		curr_dir->rec_len += 4 - curr_dir->rec_len % 4;
	}
	curr_dir->file_type = EXT2_FT_DIR;

	parent_inode->i_links_count++;
	group_desc->bg_used_dirs_count++;

	// update parent's dir entry
	result = update_dir_entry(&disk, parent_inode, new_dir_idx, name, EXT2_FT_DIR);
	if (result < 0) {
		return result;
	}

	free(path);
	free(name);

	return 0;
}
