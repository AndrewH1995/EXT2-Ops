/*
 * This program takes two command line arguments. The first is the name of an ext2 formatted virtual
 * disk, and the second is an absolute path to a file or link (not a directory) on that disk. The
 * program should work like rm, removing the specified file from the disk. If the file does not
 * exist or if it is a directory, then your program should return the appropriate error. Once again,
 * please read the specifications of ext2 carefully, to figure out what needs to actually happen
 * when a file or link is removed (e.g., no need to zero out data blocks, must set i_dtime in the
 * inode, removing a directory entry need not shift the directory entries after the one being
 * deleted, etc.).
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
#include <time.h>
#include <unistd.h>

#include "ext2.h"
#include "utils.h"

unsigned char *disk;

// --------- HELPER FUNCTIONS ----------
/**
 * Remove an inode by decreasing its i_links_count and see if i_links_count = 0
 * @param disk             disk
 * @param target_inode_idx the inode's index
 */
void rm_inode(unsigned char **disk, unsigned int target_inode_idx) {
	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(*disk + (2 * EXT2_BLOCK_SIZE));
	struct ext2_super_block *super_block = (struct ext2_super_block *)(*disk + EXT2_BLOCK_SIZE);
	struct ext2_inode *inode_table =
		(struct ext2_inode *)(*disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_table);
	unsigned int *inode_bitmap =
		(unsigned int *)(*disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_bitmap);
	struct ext2_inode *inode = &(inode_table[target_inode_idx]);


	inode->i_links_count--;
	if (inode->i_links_count == 0) {
		inode->i_dtime = (unsigned int)time(NULL);
		set_bitmap(&inode_bitmap, target_inode_idx, 0);
		super_block->s_free_inodes_count++;
		group_desc->bg_free_inodes_count++;
	}
}

/**
 * remove block from bitmap
 * @param disk         disk
 * @param target_inode target inode
 */
void rm_block(unsigned char **disk, struct ext2_inode *target_inode) {
	struct ext2_super_block *super_block = (struct ext2_super_block *)(*disk + EXT2_BLOCK_SIZE);
	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(*disk + (2 * EXT2_BLOCK_SIZE));
	unsigned int *block_bitmap =
		(unsigned int *)(*disk + EXT2_BLOCK_SIZE * group_desc->bg_block_bitmap);
	for (int i = 0; target_inode->i_block[i] != 0; i++) {
		set_bitmap(&block_bitmap, target_inode->i_block[i] - 1, 0);
		super_block->s_free_blocks_count++;
		group_desc->bg_free_blocks_count++;
	}
}

/**
 * Free the parent's block containing target
 * @param disk         disk
 * @param parent_inode parent inode
 * @param curr_idx     target index
 * @param target_name  target name
 */
void free_block(unsigned char **disk, struct ext2_inode *parent_inode, int curr_idx,
				char *target_name) {
	struct ext2_super_block *super_block = (struct ext2_super_block *)(*disk + EXT2_BLOCK_SIZE);
	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(*disk + (2 * EXT2_BLOCK_SIZE));
	unsigned int *block_bitmap =
		(unsigned int *)(*disk + EXT2_BLOCK_SIZE * group_desc->bg_block_bitmap);

	// loop over each block in parent node
	for (int i = 0; parent_inode->i_block[i] != 0; i++) {
		struct ext2_dir_entry *prev_dir = NULL;
		int dir_block_num = parent_inode->i_block[i];
		struct ext2_dir_entry *curr_dir =
			(struct ext2_dir_entry *)(*disk + EXT2_BLOCK_SIZE * dir_block_num);

		int curr_len = 0;
		while (curr_len < EXT2_BLOCK_SIZE) {
			if (strncmp(curr_dir->name, target_name, curr_dir->name_len) == 0) {
				if (prev_dir != NULL) {
					prev_dir->rec_len += curr_dir->rec_len;
					break;
				} else { // no prev_dir. set whole block to 0
					parent_inode->i_block[i] = 0;
					set_bitmap(&block_bitmap, curr_idx, 0);
					super_block->s_free_blocks_count++;
					group_desc->bg_free_blocks_count++;
				}
			} else {
				prev_dir = curr_dir;
			}
			curr_dir = (struct ext2_dir_entry *)((unsigned char *)curr_dir + curr_dir->rec_len);
			curr_len += curr_dir->rec_len;
		}
	}
}


int main(int argc, char const *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <image file name> <absolute path>\n", argv[0]);
		exit(-1);
	}

	int result;

	if ((result = init(&disk, argv[1])) != 0) {
		fprintf(stderr, "main: init\n");
		return result;
	}

	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(disk + EXT2_BLOCK_SIZE * 2);
	struct ext2_inode *inode_table =
		(struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_table);

	// parse the absolute path into the path and the new file's name
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

	if ((curr_idx = find_idx(disk, name, root_dir)) < 0) {
		fprintf(stderr, "file does not exist\n");
		return -ENOENT;
	}

	// find curr inode
	struct ext2_inode *curr_inode = &(inode_table[curr_idx - 1]);
	if (!(curr_inode->i_mode & EXT2_S_IFLNK || curr_inode->i_mode & EXT2_S_IFREG)) {
		fprintf(stderr, "Invalid parent file type! %i\n", curr_inode->i_mode);
		return -ENOENT;
	}

	// free curr from its parent's block
	free_block(&disk, parent_inode, curr_idx, name);

	// rm current inode
	rm_inode(&disk, curr_idx - 1);
	if (curr_inode->i_links_count == 0) { // completely remove current block
		rm_block(&disk, curr_inode);
	}


	return 0;
}
