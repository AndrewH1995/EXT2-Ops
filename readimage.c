#include "ext2.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

unsigned char *disk;

// ---------- Helper Function Declarations ----------
int check_inode(int inode_count, struct ext2_inode *inode);
int print_bitmap(unsigned char *bitmap, int size);
char get_inode_type(unsigned short mode);
char get_dir_type(unsigned char type);

// ---------- Helper Functions ----------
/**
 * check if the inode is ok
 * @param  inode_count 	the inode count
 * @param  inode       	the current inode struct
 * @return             	1: inode ok
 * 						0: skip this inode
 */
int check_inode(int inode_count, struct ext2_inode *inode) {
	return (inode_count == 1 || inode_count > 10) && inode->i_size > 0;
}

/**
 * print out a given bit map.
 * @param  bitmap the bitmap
 * @param  size   the size of the bitmap
 * @return        0 on success
 */
int print_bitmap(unsigned char *bitmap, int size) {
	for (int i = 0; i < size; i++) {
		if (i % 8 == 0) {
			printf(" ");
		}
		printf("%d", bitmap[i / 8] >> (i % 8) & 0x1);
	}
	return 0;
}

/**
 * get the type of the inode
 * @param  mode inode.i_mode
 * @return      the inode type
 * 				'f': file
 * 				'd': dir
 * 				'l': link
 * 				-1: error
 */
char get_inode_type(unsigned short mode) {
	if (mode & EXT2_S_IFREG) {
		return 'f';
	} else if (mode & EXT2_S_IFDIR) {
		return 'd';
	} else if (mode & EXT2_S_IFLNK) {
		return 'l';
	}
	return -1;
}

/**
 * get the type of the directory
 * @param  type dir.file_type
 * @return      the file type
 * 				'f': file
 * 				'd': dir
 * 				'l': link
 * 				-1: error
 */
char get_dir_type(unsigned char type) {
	if (type == EXT2_FT_REG_FILE)
		return 'f';
	else if (type == EXT2_FT_DIR)
		return 'd';
	else if (type == EXT2_FT_SYMLINK)
		return 'l';
	return -1;
}

// ---------- MAIN ----------

int main(int argc, char **argv) {

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <image file name>\n", argv[0]);
		exit(1);
	}
	int fd = open(argv[1], O_RDWR);

	disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (disk == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	struct ext2_super_block *super_block =
		(struct ext2_super_block *)(disk + 1024);
	struct ext2_group_desc *group_desc =
		(struct ext2_group_desc *)(disk + 1024 * 2);

	printf("Inodes: %d\n", super_block->s_inodes_count);
	printf("Blocks: %d\n", super_block->s_blocks_count);

	printf("Block group:\n");
	printf("    block bitmap: %d\n", group_desc->bg_block_bitmap);
	printf("    inode bitmap: %d\n", group_desc->bg_inode_bitmap);
	printf("    inode table: %d\n", group_desc->bg_inode_table);
	printf("    free blocks: %d\n", group_desc->bg_free_blocks_count);
	printf("    free inodes: %d\n", group_desc->bg_free_inodes_count);
	printf("    used_idrs: %d\n", group_desc->bg_used_dirs_count);


	// pointer to the block bitmap
	unsigned char *block_bitmap =
		(unsigned char *)(disk + 1024 * group_desc->bg_block_bitmap);

	printf("Block bitmap:");
	print_bitmap(block_bitmap, super_block->s_blocks_count);
	printf("\n");

	// pointer to the inode bitmap
	unsigned char *inode_bitmap =
		(unsigned char *)(disk + 1024 * group_desc->bg_inode_bitmap);

	printf("Inode bitmap: ");
	print_bitmap(inode_bitmap, super_block->s_inodes_count);
	printf("\n");

	// pointer to the first inode
	struct ext2_inode *inodes =
		(struct ext2_inode *)(disk + 1024 * group_desc->bg_inode_table);

	printf("\nInodes:\n");
	for (int i = 0; i < super_block->s_inodes_count; i++) {
		struct ext2_inode *current_inode = &inodes[i];
		if (check_inode(i, current_inode)) {
			char type = get_inode_type(current_inode->i_mode);
			if (type == -1) {
				fprintf(stderr, "Invalid file type in inode: %d\n", i);
				exit(-1);
			}
			int size = current_inode->i_size;
			int links_count = current_inode->i_links_count;
			int blocks = current_inode->i_blocks;

			printf("[%d] type: %c size: %d links: %d blocks: %d\n", i + 1, type,
				   size, links_count, blocks);

			printf("[%d] Blocks: ", i + 1);
			for (int j = 0; current_inode->i_block[j] != 0; j++) {
				printf(" %d", current_inode->i_block[j]);
			}
			printf("\n");
		}
	}


	printf("\nDirectory Blocks:\n");

	for (int i = 0; i < super_block->s_inodes_count; i++) {
		struct ext2_inode *current_inode = &inodes[i];

		if (check_inode(i, current_inode) &&
			get_inode_type(current_inode->i_mode) == 'd') {

			for (int j = 0; current_inode->i_block[j] != 0; j++) {
				printf("   DIR BLOCK NUM: %d (for inode %d)\n",
					   current_inode->i_block[j], i + 1);

				struct ext2_dir_entry *dir_base =
					(struct ext2_dir_entry *)(disk + 1024 * current_inode->i_block[j]);

				unsigned short curr_len = 0;
				while (curr_len < current_inode->i_size) {
					struct ext2_dir_entry *dir =
						(struct ext2_dir_entry *)((unsigned char *)dir_base + curr_len);

					int inode = dir->inode;
					int rec_len = dir->rec_len;
					int name_len = dir->name_len;
					char type = get_dir_type(dir->file_type);
					if (type == -1) {
						fprintf(stderr, "Invalid file type in block: %s\n", dir->name);
						exit(-1);
					}

					char name[name_len + 1];
					strncpy(name, dir->name, name_len);
					name[name_len] = '\0';

					printf("Inode: %d rec_len: %d name_len: %d type= %c "
						   "name=%s \n",
						   inode, rec_len, name_len, type, name);

					curr_len += dir->rec_len;
				}
			}
		}
	}

	return 0;
}
