#include "ext2.h"

#ifndef EXT2_UTIL
#define EXT2_UTIL

int init(unsigned char **disk, char const *file_name);
int check_bitmap(unsigned int *bitmap, int index);
void set_bitmap(unsigned int **bitmap, int index, int value);
unsigned int new_inode(unsigned char **disk);
void init_inode(unsigned char **disk, unsigned int new_inode_idx);
int new_block(unsigned char **disk);
int update_dir_entry(unsigned char **disk, struct ext2_inode *parent_inode, unsigned short current_idx, char *name,
                      unsigned char type);
int parse_path(char const *absolute_path, char **path, char **name);
int find_idx(unsigned char *disk, char *name, struct ext2_dir_entry *dir);


#endif // EXT2_UTIL
