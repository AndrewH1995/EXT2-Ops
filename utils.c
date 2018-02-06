/*
 * Helper functions for the rest of the ext2_functions.
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

// ---------- Function Declarations ----------
int init(unsigned char **disk, char const *file_name);
int check_bitmap(unsigned int *bitmap, int index);
void set_bitmap(unsigned int **bitmap, int index, int value);
unsigned int new_inode(unsigned char **disk);
void init_inode(unsigned char **disk, unsigned int new_inode_idx);
int new_block(unsigned char **disk);
int update_dir_entry(unsigned char **disk, struct ext2_inode *parent_inode,
					  unsigned short current_idx, char *name, unsigned char type);
int parse_path(char const *absolute_path, char **path, char **name);
int find_idx(unsigned char *disk, char *name, struct ext2_dir_entry *dir);



// ---------- Function Implementations ----------

/**
 * Initialize the disk. Should be called at the start of every ext2_functions.
 * @param  disk      the global variable disk that stores the disk's info
 * @param  file_name the image file name (argv[1])
 * @return           0 on success; -1 on failure
 */
int init(unsigned char **disk, char const *file_name) {
	int fd = open(file_name, O_RDWR);
	if (fd < 0) {
		perror("init: open");
		return -EINVAL;
	}
	*disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*disk == MAP_FAILED) {
		perror("init: mmap");
		return -1;
	}
	return 0;
}


/**
 * Check if the index on the bitmap is free
 * @param  bitmap the given bitmap (inode or block)
 * @param  index  index on the block
 * @return        0 on free, 1 on used
 */
int check_bitmap(unsigned int *bitmap, int index) {
	return (((unsigned char *)bitmap)[index / 8] >> (index % 8)) & 0x1;
}


/**
 * Set or reset the bitmap
 * @param bitmap  	the bitmap pointer
 * @param index 	index to set or unset
 * @param value  	1 to set, 0 to unset
 */
void set_bitmap(unsigned int **bitmap, int index, int value) {
	if (value == 1) { // set
		*(((unsigned char *)*bitmap) + (index / 8)) |= (1 << (index % 8));
	} else { // unset
		*(((unsigned char *)*bitmap) + (index / 8)) &= ~(1 << (index % 8));
	}
}

/**
 * Allocate and return a new inode
 * @param disk	the disk
 * @return 		the new inode index
 * 				errno on failure
 */
unsigned int new_inode(unsigned char **disk) {
	int free_inode_idx = 0;
	struct ext2_super_block *super_block = (struct ext2_super_block *)(*disk + EXT2_BLOCK_SIZE);
	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(*disk + (2 * EXT2_BLOCK_SIZE));
	unsigned int *inode_bitmap =
		(unsigned int *)(*disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_bitmap);

	// loop over inode indices to check for a free inode on the bitmap
	for (free_inode_idx = EXT2_GOOD_OLD_FIRST_INO; free_inode_idx < super_block->s_inodes_count;
		 free_inode_idx++) {
		if (check_bitmap(inode_bitmap, free_inode_idx) == 0) {
			break;
		}
	}
	if (free_inode_idx == super_block->s_inodes_count) {
		fprintf(stderr, "no free inode left\n");
		return -ENOSPC;
	}
	set_bitmap(&inode_bitmap, free_inode_idx, 1);

	super_block->s_free_inodes_count--;
	group_desc->bg_free_inodes_count--;

	return free_inode_idx + 1;
}

/**
 * Initialize the new inode.
 * NOTE: i_mode, i_blocks, i_size, i_links_count, i_block need to be set
 * @param disk          the disk
 * @param new_inode_idx index of the new inode
 */
void init_inode(unsigned char **disk, unsigned int new_inode_idx) {
	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(*disk + (2 * EXT2_BLOCK_SIZE));
	struct ext2_inode *inode_table =
		(struct ext2_inode *)(*disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_table);
	struct ext2_inode *inode = &(inode_table[new_inode_idx]);

	inode->i_mode = 0;
	// inode->i_blocks = 0;
	inode->i_size = 0;
	inode->i_links_count = 0;
	// inode->i_block = 0;
	// inode->extra = 0;

	inode->i_atime = (unsigned int)time(NULL);
	inode->i_ctime = (unsigned int)time(NULL);
	inode->i_mtime = 0;
	inode->i_dtime = 0;

	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_flags = 0;
	inode->osd1 = 0;
	inode->i_generation = 0;
	inode->i_file_acl = 0;
	inode->i_dir_acl = 0;
	inode->i_faddr = 0;
}


/**
 * Allocate a new block on the disk
 * @param  disk the disk
 * @return      the block index
 */
int new_block(unsigned char **disk) {
	int free_block_idx = 0;

	struct ext2_super_block *super_block = (struct ext2_super_block *)(*disk + EXT2_BLOCK_SIZE);
	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(*disk + (2 * EXT2_BLOCK_SIZE));
	unsigned int *block_bitmap =
		(unsigned int *)(*disk + EXT2_BLOCK_SIZE * group_desc->bg_block_bitmap);

	for (free_block_idx = 0; free_block_idx < super_block->s_blocks_count; free_block_idx++) {
		if (!check_bitmap(block_bitmap, free_block_idx)) {
			break;
		}
	}
	if (free_block_idx == super_block->s_blocks_count) {
		fprintf(stderr, "no free block left\n");
		return -ENOSPC;
	}
	set_bitmap(&block_bitmap, free_block_idx, 1);

	super_block->s_free_blocks_count--;
	group_desc->bg_free_blocks_count--;

	return free_block_idx + 1;
}



/**
 * update the parent directory given the current index
 * @param parent_inode parent inode
 * @param current_idx  the current entry's inode index
 * @param name         the current entry's name
 * @param type         dirent type for the current entry
 * @return			   0 on success, errno on failure
 */
int update_dir_entry(unsigned char **disk, struct ext2_inode *parent_inode,
					  unsigned short current_idx, char *name, unsigned char type) {

	// loop from the end to find a free or not full block
	for (int i = 11; i >= 0; i--) {
		if (parent_inode->i_block[i] != 0) { // may be not full
			int dir_block_num = parent_inode->i_block[i];

			struct ext2_dir_entry *dir =
				(struct ext2_dir_entry *)(*disk + EXT2_BLOCK_SIZE * dir_block_num);
			int curr_len = dir->rec_len;

			while (curr_len <= EXT2_BLOCK_SIZE) {
				if (curr_len == EXT2_BLOCK_SIZE) { // last entry. check if entry has padding
												   // new entry's size
					int new_size = sizeof(struct ext2_dir_entry) + strlen(name);
					if (new_size % 4 != 0) {
						new_size += 4 - new_size % 4;
					}

					// current last entry's real size (w/o padding)
					int last_ent_size = sizeof(struct ext2_dir_entry) + dir->name_len;
					if (last_ent_size % 4 != 0) {
						last_ent_size = 4 * (last_ent_size / 4) + 4;
					}

					int space_left = dir->rec_len - last_ent_size;
					if (space_left - new_size >= 0) { // enough space left
						// rm padding for prev last entry
						dir->rec_len = last_ent_size;

						dir = (struct ext2_dir_entry *)((unsigned char *)dir + (dir->rec_len));
						dir->file_type = type;
						dir->inode = current_idx;
						dir->name_len = strlen(name);
						strncpy(dir->name, name, dir->name_len);

						dir->rec_len = space_left;
						if (dir->rec_len % 4 != 0) {
							dir->rec_len = 4 * (dir->rec_len / 4) + 4;
						}

					} else { // not enought space in the current block
						// allocate new block and make inode block pointer point to the new block
						int block_num = new_block(disk);
						if (block_num < 0) {
							return block_num;
						}
						parent_inode->i_block[i + 1] = block_num;
						dir = (struct ext2_dir_entry *)(*disk + EXT2_BLOCK_SIZE * block_num);
						dir->file_type = type;
						dir->inode = current_idx;
						dir->name_len = strlen(name);
						strncpy(dir->name, name, dir->name_len);
						dir->rec_len = EXT2_BLOCK_SIZE;
						parent_inode->i_size += EXT2_BLOCK_SIZE;
					}
					return 0;
				}

				dir = (struct ext2_dir_entry *)((unsigned char *)dir + dir->rec_len);
				curr_len += dir->rec_len;
			}
		}
	}
	return 0;
}


/**
 * Parse the given absolute path to the path and the file/dir name
 * @param  absolute_path the given absolute path
 * @param  path          the path to file
 * @param  name          the file/dir name
 * @return               0 on success
 * 						 -1 on failure
 */
int parse_path(char const *absolute_path, char **path, char **name) {
	if (absolute_path[0] != '/') {
		fprintf(stderr, "%s is not absolute\n", absolute_path);
		return -EINVAL;
	}
	int len = strlen(absolute_path);
	char *abs_path = (char *)absolute_path;
	// rm last '/'
	//
	if (len > 1 && abs_path[len - 1] == '/') {
		abs_path[len - 1] = '\0';
		len--;
	}
	// get name
	char *idx = strrchr(abs_path, '/') + 1;
	if (!(*name = malloc(sizeof(char) * (strlen(idx) + 1)))) {
		perror("parse_path: malloc");
		return -1;
	}
	strncpy(*name, idx, strlen(idx));
	// get path
	abs_path[len - strlen(*name) - 1] = '\0';
	if (strlen(abs_path) == 0) {
		abs_path = "/";
	}
	if (!(*path = malloc(sizeof(char) * (strlen(abs_path) + 1)))) {
		perror("parse_path: malloc");
		return -1;
	}
	strcpy(*path, abs_path);

	return 0;
}


/**
 * Find the given name's node index
 * @param  disk disk
 * @param  name target
 * @param  dir  dirent
 * @return      node index
 */
int find_idx(unsigned char *disk, char *name, struct ext2_dir_entry *dir) {
	int result;
	if (strncmp(dir->name, name, dir->name_len) == 0) {
		return dir->inode;
	}
	struct ext2_dir_entry *curr_dir = dir;

	struct ext2_group_desc *group_desc = (struct ext2_group_desc *)(disk + EXT2_BLOCK_SIZE * 2);
	struct ext2_inode *inode_table =
		(struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * group_desc->bg_inode_table);

	if (curr_dir->inode == 0) {
		curr_dir = (struct ext2_dir_entry *)((unsigned char *)curr_dir + curr_dir->rec_len);
	}

	int curr_rec_len = curr_dir->rec_len;

	while (curr_rec_len <= EXT2_BLOCK_SIZE) {
		struct ext2_inode *curr_inode = &inode_table[curr_dir->inode - 1];

		if (strncmp(curr_dir->name, name, curr_dir->name_len) == 0) {
			return curr_dir->inode;
		}


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
							if ((result = find_idx(disk, name, child)) > 0) {
								return result;
							}
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
	return -ENOENT;
}
