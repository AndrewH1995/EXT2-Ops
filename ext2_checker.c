/*
 * This program takes only one command line argument: the name of an ext2 formatted virtual disk.
 * The program should implement a lightweight file system checker, which detects a small subset of
 * possible file system inconsistencies and takes appropriate actions to fix them
 */

#include <errno.h>
#include <fcntl.h>
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
struct ext2_super_block *super_block;
struct ext2_group_desc *group_desc;
struct ext2_inode *inode_table;
unsigned int *inode_bitmap;
unsigned int *block_bitmap;
int total_err;

// ---------- HELPER FUNCTIONS ----------
/**
 * a) check if the superblock and block group counters for free blocks and free inodes match the
 * number of free inodes and data blocks as indicated in the respective bitmaps. If an inconsistency
 * is detected, trust the bitmaps and update the counters.
 */
void check_counters() {
	int actual_free_inodes = super_block->s_inodes_count;
	int num_diff = 0;
	for (int i = 0; i < super_block->s_inodes_count; i++) {
		if (check_bitmap(inode_bitmap, i)) {
			actual_free_inodes--;
		}
	}
	if (super_block->s_free_inodes_count != actual_free_inodes) {
		num_diff = abs(actual_free_inodes - (int)super_block->s_free_inodes_count);
		super_block->s_free_inodes_count = actual_free_inodes;
		total_err += num_diff;
		printf("Fixed: superblock's free inodes counter was off by %d compared to the bitmap\n",
			   num_diff);
	}
	if (group_desc->bg_free_inodes_count != actual_free_inodes) {
		num_diff = abs(actual_free_inodes - (int)group_desc->bg_free_inodes_count);
		group_desc->bg_free_inodes_count = actual_free_inodes;
		total_err += num_diff;
		printf("Fixed: block group's free inodes counter was off by %d compared to the bitmap\n",
			   num_diff);
	}
	// check block bitmap
	int actual_free_blocks = super_block->s_blocks_count;
	for (int i = 0; i < super_block->s_blocks_count; i++) {
		if (check_bitmap(block_bitmap, i)) {
			actual_free_blocks--;
		}
	}
	if (super_block->s_free_blocks_count != actual_free_blocks) {
		num_diff = abs(actual_free_blocks - (int)super_block->s_free_blocks_count);
		super_block->s_free_blocks_count = actual_free_blocks;
		total_err += num_diff;
		printf("Fixed: superblock's free blocks counter was off by %d compared to the bitmap\n",
			   num_diff);
	}
	if (group_desc->bg_free_blocks_count != actual_free_blocks) {
		num_diff = abs(actual_free_blocks - (int)group_desc->bg_free_blocks_count);
		group_desc->bg_free_blocks_count = actual_free_blocks;
		total_err += num_diff;
		printf("Fixed: block group's free blocks counter was off by %d compared to the bitmap\n",
			   num_diff);
	}
}

/**
 * b) check if its inode's i_mode matches the directory entry file_type.
 * If it does not, then trust the inode's i_mode and fix the file_type to match.
 * @param  inode the inode to be checked
 * @param  dir   the dirent
 * @return       [description]
 */
void check_mode(struct ext2_inode *inode, struct ext2_dir_entry *dir) {
	if ((inode->i_mode & EXT2_S_IFREG) && (dir->file_type != EXT2_FT_REG_FILE)) {
		total_err++;
		dir->file_type = EXT2_FT_REG_FILE;
		printf("Fixed: Entry type vs inode mismatch: inode [%d]\n", dir->inode);
	} else if ((inode->i_mode & EXT2_S_IFDIR) && (dir->file_type != EXT2_FT_DIR)) {
		total_err++;
		dir->file_type = EXT2_FT_DIR;
		printf("Fixed: Entry type vs inode mismatch: inode [%d]\n", dir->inode);
	} else if ((inode->i_mode & EXT2_S_IFLNK) && (dir->file_type != EXT2_FT_SYMLINK)) {
		total_err++;
		dir->file_type = EXT2_FT_SYMLINK;
		printf("Fixed: Entry type vs inode mismatch: inode [%d]\n", dir->inode);
	}
}

/**
 * c) check if inode is marked as allocated in the inode bitmap. If it isn't, then updated the inode
 * bitmap to indicate that the inode is in use.
 * @param inode_idx 	inode index to be checked
 */
void check_allocated(unsigned short inode_idx) {
	if (check_bitmap(inode_bitmap, inode_idx - 1) == 0) {
		total_err++;
		set_bitmap(&inode_bitmap, inode_idx - 1, 1);
		super_block->s_free_inodes_count--;
		group_desc->bg_free_inodes_count--;
		printf("Fixed: inode [%d] not marked as in-use\n", inode_idx);
	}
}

/**
 * d) check if inode's i_dtime is set to 0. If it isn't, reset to 0 to indicate that the file should
 * not be marked for removal
 * @param inode_idx inode's index
 * @param inode     the inode
 */
void check_dtime(unsigned short inode_idx, struct ext2_inode *inode) {
	if (inode->i_dtime != 0) {
		total_err++;
		inode->i_dtime = 0;
		printf("Fixed: valid inode marked for deletion: [%d]\n", inode_idx);
	}
}

/**
 * e) check if inode's data blocks are allocated in the data bitmap. If any of its blocks is not
 * allocated, fix this by updating the data bitmap and the corresponding counters in the block group
 * and superblock.
 * @param inode_idx the inode idx
 * @param inode     the inode to be checked
 */
void check_block(unsigned short inode_idx, struct ext2_inode *inode) {
	int block_count = 0;
	for (int i = 0; inode->i_block[i] != 0; i++) {
		int block = inode->i_block[i];
		if (block != 0 && check_bitmap(block_bitmap, block - 1) == 0) {
			set_bitmap(&block_bitmap, block - 1, 1);
			super_block->s_free_blocks_count--;
			group_desc->bg_free_blocks_count--;
			block_count++;
		}
	}
	if (block_count > 0) {
		printf("Fixed: %d in-use data blocks not marked in data bitmap for inode: [%d]\n",
			   block_count, inode_idx);
		total_err++;
	}
}

/**
 * Recursively check each dir for b) to e)
 * @param dir       the dir_ent to check
 * @param inode_idx the inode index of dirent
 */
void check_dir(struct ext2_dir_entry *dir, unsigned short inode_idx) {
	struct ext2_dir_entry *curr_dir = dir;

	if (curr_dir->inode == 0) {
		curr_dir = (struct ext2_dir_entry *)((unsigned char *)curr_dir + curr_dir->rec_len);
	}

	int curr_rec_len = curr_dir->rec_len;

	while (curr_rec_len <= EXT2_BLOCK_SIZE) {
		struct ext2_inode *curr_inode = &inode_table[curr_dir->inode - 1];
		check_mode(curr_inode, curr_dir);
		check_allocated(curr_dir->inode);
		check_dtime(curr_dir->inode, curr_inode);
		check_block(curr_dir->inode, curr_inode);

		if (curr_dir->file_type == EXT2_FT_DIR) {
			// skip . and ..
			if (strncmp(curr_dir->name, ".", curr_dir->name_len) != 0 &&
				strncmp(curr_dir->name, "..", curr_dir->name_len) != 0) {
				for (int index = 0; index < 13; index++) {
					int block_num = curr_inode->i_block[index];
					if (block_num != 0) {
						struct ext2_dir_entry *child =
							(struct ext2_dir_entry *)(disk + (EXT2_BLOCK_SIZE * block_num));
						if (child->inode != 0) {
							check_dir(child, dir->inode);
						}
					}
				}
			}
		}
		if (curr_rec_len == EXT2_BLOCK_SIZE) {
			break;
		}
		curr_dir = (struct ext2_dir_entry *)((unsigned char *)curr_dir + curr_dir->rec_len);
		curr_rec_len += curr_dir->rec_len;
	}
}


int main(int argc, char const *argv[]) {
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <image file name>\n", argv[0]);
		exit(-1);
	}

	int result;
	total_err = 0;

	if ((result = init(&disk, argv[1])) != 0) {
		fprintf(stderr, "main: init\n");
		return result;
	}

	super_block = (struct ext2_super_block *)(disk + EXT2_BLOCK_SIZE);
	group_desc = (struct ext2_group_desc *)(disk + EXT2_BLOCK_SIZE * 2);
	inode_bitmap = (unsigned int *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_bitmap);
	block_bitmap = (unsigned int *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_block_bitmap);
	inode_table = (struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_table);

	// a)
	check_counters();

	struct ext2_inode *root_inode = &(inode_table[EXT2_ROOT_INO - 1]);
	struct ext2_dir_entry *root_dir =
		(struct ext2_dir_entry *)(disk + (EXT2_BLOCK_SIZE * root_inode->i_block[0]));
	check_dir(root_dir, EXT2_ROOT_INO);

	if (total_err > 0) {
		printf("%d file system inconsistencies repaired!\n", total_err);
	} else {
		printf("No file system inconsistencies detected!\n");
	}

	return 0;
}
