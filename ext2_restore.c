/*
 * This program takes two command line arguments. The first is the name of an ext2 formatted virtual
 * disk, and the second is an absolute path to a file or link (not a directory!) on that disk. The
 * program should be the exact opposite of rm, restoring the specified file that has been previous
 * removed. If the file does not exist (it may have been overwritten), or if it is a directory, then
 * your program should return the appropriate error.
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

	struct ext2_super_block *super_block = (struct ext2_super_block *)(disk + EXT2_BLOCK_SIZE);
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

	if ((curr_idx = find_idx(disk, name, root_dir)) > 0) {
		fprintf(stderr, "main: file already exists\n");
		return -EEXIST;
	}

	unsigned int *inode_bitmap =
		(unsigned int *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_bitmap);
	unsigned int *block_bitmap =
		(unsigned int *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_block_bitmap);

	// loop over block to check each parent block's entry for gaps
	for (int i = 0; i < 12; i++) {
	    int block_num = inode_table[parent_idx - 1].i_block[i];
	    if (block_num != 0) {
			// head of the potential gap containing dir_ent
	        struct ext2_dir_entry *head = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * block_num);
			// for looping over each dirs in gap
			struct ext2_dir_entry *curr_dir = head;

	        int curr_rec_len = head->rec_len;

	        while (curr_rec_len <= EXT2_BLOCK_SIZE) {
	            int real_size = sizeof(struct ext2_dir_entry) + head->name_len;
	            if (real_size % 4 != 0) {
	                real_size += 4 - real_size % 4;
	            }

	            curr_dir = (struct ext2_dir_entry *)((char *)head + real_size);
	            int gap_counter = real_size;
	            int head_len_total = head->rec_len;

	            while (gap_counter < head_len_total) {
	                if (strncmp(curr_dir->name, name, curr_dir->name_len) == 0) {
						if (check_bitmap(inode_bitmap, curr_dir->inode - 1) == 1) {
	                        fprintf(stderr,
	                                "the inode has already been taken. restore inpossible\n");
	                        return -ENOENT;
	                    }
	                    // check if dtime != 0
	                    struct ext2_inode *restored_inode = &inode_table[curr_dir->inode - 1];
	                    if (restored_inode->i_dtime == 0) {
	                        fprintf(stderr, "the inode was not deleted\n");
	                        return -ENOENT;
	                    }
	                    // updates
	                    super_block->s_free_inodes_count--;
	                    group_desc->bg_free_inodes_count--;
	                    set_bitmap(&inode_bitmap, curr_dir->inode - 1, 1);

	                    curr_dir->rec_len = head_len_total - gap_counter;
	                    head->rec_len = gap_counter;

	                    restored_inode->i_links_count++;
	                    restored_inode->i_dtime = 0;
	                    restored_inode->i_mtime = (unsigned int)time(NULL);

	                    for (int i = 0; i < 12; i++) {
	                        if (restored_inode->i_block[i] != 0) {
	                            set_bitmap(&block_bitmap, restored_inode->i_block[i] - 1, 1);
	                            super_block->s_free_blocks_count--;
	                            group_desc->bg_free_blocks_count--;
	                        }
	                    }
						return 0;
	                }
	                real_size = sizeof(struct ext2_dir_entry) + curr_dir->name_len;
	                if (real_size % 4 != 0) {
	                    real_size += 4 - real_size % 4;
	                }
	                curr_dir = (struct ext2_dir_entry *)((char *)curr_dir + real_size);
	                gap_counter += real_size;
	            }
	            if (curr_rec_len == EXT2_BLOCK_SIZE)
	                break;
	            head = (struct ext2_dir_entry *)((char *)head + head->rec_len);
	            curr_rec_len += head->rec_len;
	        }
	    }
	}



	return 0;
}
