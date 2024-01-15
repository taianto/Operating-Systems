#ifndef SFS_API_H
#define SFS_API_H

//Defining some constants for the file system
#define BLOCK_SIZE 1024
#define BLOCK_AMOUNT 2000
#define DIR_AMOUNT 128
#define MAX_FD_AMOUNT 128
#define INODE_AMOUNT 129        //129 because since maximum directory files is 128, and first i-Node is for the directory
#define MAXFILENAME 32

void mksfs(int);
int sfs_getnextfilename(char*);
int sfs_getfilesize(const char*);
int sfs_fopen(char*);
int sfs_fclose(int);
int sfs_fwrite(int, const char*, int);
int sfs_fread(int, char*, int);
int sfs_fseek(int, int);
int sfs_remove(char*);

//Added functions
int get_free_block();
void set_bit(int);
void remove_bit(int);
int size_to_blocks(int);
int scan_dir_name(char* fname);
int find_free_i_node();
void write_directory();
void read_directory();

#endif
