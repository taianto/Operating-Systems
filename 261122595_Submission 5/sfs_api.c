/* ======================================================================== */                                                                                                                                      
/* READ ME:                                                                 */
/* Max file name allowed set to 32 bytes, changed in test2.c from 31 to 32  */
/* as a result.                                                             */
/* ======================================================================== */


#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "sfs_api.h"
#include "disk_emu.h"

//i-Node structure
typedef struct {
    int mode;               //File permissions          
    int link_cnt;           //Count of data blocks containing file data
    int size;               //Size of file
    int pointers[12];       //Pointers to data blocks      
    int indirect_pointers;  //Indirect pointer to block containing pointers
} i_node;

//Superblock structure
typedef struct {
    char magic[16];             //            
    int block_size;             //Set to 1024 bytes            
    int file_system_size;       //Amount of blocks   
    int i_node_table_length;    //Size of i-Node table in blocks            
    int root_directory;         //Pointer to i-Node associated with root directory
} super_block;

//Open File Descriptor entry structure
typedef struct {
    i_node* inode;      //Pointer to i-Node associated will opened file
    int rwpointer;      //Location of where to start reading from/writing to
} file_descriptor;

//Directory entry structure
typedef struct {
    int i_node_num;     //Pointer to i-Node given to file               
    char *file_name;    //Name of the file                    
} dir_entry;

unsigned char bitmap[BLOCK_AMOUNT/8];           //Bitmap using a character array, covers max amount of blocks implemented                    
i_node i_node_table[INODE_AMOUNT];              //i-Node table cache - capped at 129 entries                                           
dir_entry root_directory[DIR_AMOUNT];           //Root directory cache - capped at 128 entries                   
file_descriptor open_fd_table[MAX_FD_AMOUNT];   //Open File Descriptor Table - capped at 128 entries                            
int root_directory_position;                    //Used to capture the current position of the getnextfilename() method            

int get_free_block() {
    for (int i = 0; i < BLOCK_AMOUNT/8; i++) {  //Iterates through bitmap                    
        if (bitmap[i] > 0) {                        
            return i*8 + ffs(bitmap[i]) - 1;    //Returns position of bit that represents an empty block                    
        }
    }
    printf("%s", "SFS_API: NO FREE BLOCKS FOUND\n");
    return -1;
}

void remove_bit(int block) {
    int index = (int)block/8;
    int bit_offset = block % 8;
    char current_set_of_blocks = bitmap[index];
    bitmap[index] = current_set_of_blocks & ~(1 << bit_offset);     //Turns bit to 0, meaning that the block is no longer free
}

void set_bit(int block) {
    int index = (int)block/8;
    int bit_offset = block % 8;
    char current_set_of_blocks = bitmap[index];
    bitmap[index] = current_set_of_blocks | (1 << bit_offset);      //Turns bit to 1, meaning that the block is now free
}

int size_to_blocks(int size) {          //Pretty much just gets the ceiling of input in blocks
    if (size % BLOCK_SIZE != 0) {
        return (size/BLOCK_SIZE) + 1;   
    }
    return size/BLOCK_SIZE;
}

int scan_dir_name(char* fname) {        //Scans the directory for a given file and returns index of i-Node 
    for (int i = 0; i < DIR_AMOUNT; i++) {
        if (strcmp(root_directory[i].file_name, fname) == 0) {
            return root_directory[i].i_node_num;                //Returns the index of inode for a given file
        }
    }        
    return -1;
}

int find_free_i_node () {                       //Finds index of a free i-Node
    for (int i = 0; i < INODE_AMOUNT; i++) {
        if (i_node_table[i].size == -1) {
            return i;
        }
    }
    return -1;
}

void write_directory() {        //writes directory from memory to disk using i-nodes
    i_node root_i_node = i_node_table[0];

    int dir_blocks = (size_to_blocks(sizeof(root_directory)));
    for(int i = 0; i < dir_blocks; i++) {
        write_blocks(root_i_node.pointers[i], 1, (char *) root_directory + (i*BLOCK_SIZE));
    }
}

void read_directory() {         //reads directory from disk to memory using i-nodes
    i_node root_i_node = i_node_table[0];

    int dir_blocks = (size_to_blocks(sizeof(root_directory)));
    for(int i = 0; i < dir_blocks; i++) {
        read_blocks(root_i_node.pointers[i], 1, (char *) root_directory + (i*BLOCK_SIZE));
    }
}

/* ======================================================================== */                                                                                                                                      
/*  mksfs:                                                                  */                
/*  Creates structure for the disk using the disk emulator.                 */                        
/*  Has fresh flag to indicate whether or not the file system should be     */                     
/*  created from scratch. Fresh = true, create from scratch.                */                        
/* ======================================================================== */
void mksfs(int fresh) {
    root_directory_position = -1;
    if (fresh == 1) {
        init_fresh_disk("Tairov_sfs", BLOCK_SIZE, BLOCK_AMOUNT);  //initialise a fresh disk

        for (int i = 0; i < (BLOCK_AMOUNT/8)-1; i++) {      //set every set of 8 blocks to 1 (255 = 11111111 in binary)
            bitmap[i] = 255;
        }

        super_block superblock;                         //Initialise and set data for superblock
        memcpy(superblock.magic,"0xABCD0005",9);
        superblock.block_size = BLOCK_SIZE;
        superblock.file_system_size = BLOCK_AMOUNT;
        superblock.i_node_table_length = size_to_blocks(INODE_AMOUNT);
        superblock.root_directory = 0;
        write_blocks(0, 1, &superblock);                //Write superblock to block 0 in disk
        remove_bit(0);                                  //Mark block as taken in bitmap
        
        for (int i = 0; i < INODE_AMOUNT; i++) {        //Initialise i-Nodes, root directory, and fd table

            i_node_table[i].mode = 0;                   //Initialising i-Nodes                                
            i_node_table[i].link_cnt = 0;                                                  
            i_node_table[i].size = -1;                                                  
            for (int j = 0; j < 12; j++) {                                                 
                i_node_table[i].pointers[j] = -1;                                                  
            }                                                   
            i_node_table[i].indirect_pointers = -1;                                               

            if (i < DIR_AMOUNT) {
                root_directory[i].i_node_num = -1;      //Initialising root directory entries
                root_directory[i].file_name = malloc(MAXFILENAME * sizeof(char));
                strcpy(root_directory[i].file_name, "\0");
            }

            if (i < MAX_FD_AMOUNT) {
                open_fd_table[i].inode = 0;             //Initialising open fd table entries
                open_fd_table[i].rwpointer = -1;
            }
        }
        
        int i_node_blocks = size_to_blocks(sizeof(i_node_table));       //Find how many blocks i-Node table occupies
        int dir_blocks = size_to_blocks(sizeof(root_directory));        //Find how many blocks root directory occupies
        
        int next_free_bit = get_free_block();
        for (int i = next_free_bit; i < i_node_blocks + next_free_bit; i++) {   //Remove free bits from bitmap occupied by i-Node block
            remove_bit(i);
        }

        for (int i = 0; i < dir_blocks; i++) {              //Remove free bits from bitmap occupied by directory      
            next_free_bit = get_free_block();
            i_node_table[0].pointers[i] = next_free_bit;        //Set root i-Node pointers to point at root directory blocks
            remove_bit(next_free_bit);
        }
        
        i_node_table[0].mode = 0;                   //Initialise i-Node associated with root directory             
        i_node_table[0].link_cnt = dir_blocks;                  
        i_node_table[0].size = 0;                   
        i_node_table[0].indirect_pointers = -1;                 

        write_blocks(BLOCK_AMOUNT-(size_to_blocks(sizeof(bitmap))), size_to_blocks(sizeof(bitmap)), &bitmap);   //Write bitmap to end of disk
            
        write_blocks(1,i_node_blocks,&i_node_table);    //Write i-Node table to disk
        write_directory();      //Write directory to disk
        
        printf("SFS_API: DISK CREATED & LOADED SUCCESSFULLY.\n");
    }
    else {
        init_disk("Tairov_sfs", BLOCK_SIZE, BLOCK_AMOUNT);    //Initialise premade disk
        int i_node_blocks = size_to_blocks(sizeof(i_node_table));       //Find how many blocks i-Node table occupies
        
        read_blocks(1,i_node_blocks,&i_node_table);     //Read i-Nodes into memory
        read_blocks(BLOCK_AMOUNT-(size_to_blocks(sizeof(bitmap))), size_to_blocks(sizeof(bitmap)), &bitmap);    //Read bitmap into memory
        read_directory();   //Read directory into memory
        printf("SFS_API: DISK LOADED SUCCESSFULLY.\n");
    }
}

/* ======================================================================== */                                                                                                                                      
/* getnextfilename:                                                         */    
/* Uses the global variable "root_directory_position" to iterate through    */                                                                
/* the directory, similar to a linked list. When reach the end of the,      */                                                                
/* directory return 0.                                                      */                                                            
/* ======================================================================== */
int sfs_getnextfilename(char* fname) {
    while (root_directory_position < DIR_AMOUNT - 1) {      //Iterate through directories until end
        root_directory_position++;      //Increment global variable
        
        if(strcmp(root_directory[root_directory_position].file_name,"\0") != 0) {   //If directory not empty
            strcpy(fname, root_directory[root_directory_position].file_name);   //Copy name into buffer
            return 1;
        }
    }
    root_directory_position = -1;
    return 0;
}

/* ======================================================================== */                                                                                                                                      
/* getfilesize:                                                             */    
/* Finds the size of a given file by looping through the directory and      */    
/* returning the size of the i-Node associated with the file                */                                                                          
/* ======================================================================== */
int sfs_getfilesize(const char* path) {
    int i_node_index = scan_dir_name((char *)path);     //Get index of i-Node associated with file
    if (i_node_index > 0) {                             //If i-Node exist
        return i_node_table[i_node_index].size;         //Return file size
    }
    return -1;
}

/* ======================================================================== */                                                                                                                                      
/* fopen:                                                                   */                                                
/* Opens a file, based on multiple conditions:                              */                        
/*     - File name is less than the maximum file name length                */                                        
/*     - File is not already open                                           */            
/*     - If file exists:                                                    */    
/*         - File must not be opened more than once                         */                            
/*         - Open FD Table must not be full (limited to 100 files)          */                                            
/*     - If file doesn't exist                                              */        
/*         - File must be created                                           */            
/*         - Needs a free i-Node to be allocated for the file               */                                        
/*         - Needs a free space in the root directory                       */                                                                                                                                             
/* ======================================================================== */
int sfs_fopen(char* name) {
    if (strlen(name) > MAXFILENAME) {       //Check if file name is within limit
        printf("SFS_API: FILE NAME TOO LONG.\n");
        return -1;
    }

    int index_of_inode = scan_dir_name(name); //Find index of i-Node associated with file
    if (index_of_inode < 0) {  //File doesn't exist - needs to be created:

        index_of_inode = find_free_i_node();    //Find free i-Node for file
        if (index_of_inode >= 0) {  //Free i-Node found:

            int free_directory = -1;    
            for (int i = 0; i < DIR_AMOUNT && free_directory == -1; i++) { //Find index of free space in directory
                if (strcmp(root_directory[i].file_name, "\0")==0) {     
                    free_directory = i;
                }
            }
            if (free_directory >= 0) {  //If free directory space found:
                memcpy(root_directory[free_directory].file_name, name,strlen(name)); //Set name of the file in directory
                root_directory[free_directory].i_node_num = index_of_inode;     //Assign i-Node to file in directory

                i_node_table[index_of_inode].size = 0;                       //set size of i-Node to 0   
                int i_node_blocks = size_to_blocks(sizeof(i_node_table));                           

                write_blocks(1,i_node_blocks,&i_node_table);        //Write i-Node table to disk
                write_directory();  //Write directory to disk
            }
            else {
                printf("SFS_API: MAX FILE DIRECTORY SPACE REACHED");
                return -1;
            }
        }
        else {
            printf("SFS_API: NO FREE I-NODES LEFT.\n");
            return -1;
        }
    }

    for (int i = 0; i < MAX_FD_AMOUNT; i++) { //Check if file is already open
        if (open_fd_table[i].inode == &i_node_table[index_of_inode]) {
            printf("SFS_API: FILE ALREADY OPEN\n");
            return i;
        }
    }

    int free_fd_found = -1;
    for (int i = 0; i < MAX_FD_AMOUNT && free_fd_found == -1; i++) { //Check if there is a free space in OFD table
        if (!open_fd_table[i].inode) {
            free_fd_found = i;
        }
    }

    if (free_fd_found >= 0) {
        open_fd_table[free_fd_found].inode = &i_node_table[index_of_inode];             //Set pointer to i-Node associated with file
        open_fd_table[free_fd_found].rwpointer = i_node_table[index_of_inode].size;     //Set pointer to the end of the file (append mode)

        return free_fd_found;
    }
    printf("Max Open FDs reached\n");
    return -1;
    
}

/* ======================================================================== */                                                                                                                                      
/* fclose:                                                                  */                                                
/* Closes a file, that is, only if the file exists and if it is currently   */
/* open. Sets file descriptor entry to defaults.                            */                                                                                                                                                                                                              
/* ======================================================================== */
int sfs_fclose(int fileID) {
    if (!open_fd_table[fileID].inode) {     //Check that file is in fact open
        printf("SFS_API: CANNOT CLOSE FILE; FILE NOT OPEN\n");
        return -1;
    }
    open_fd_table[fileID].inode = 0;        //Reset i-Node pointer in fd table
    open_fd_table[fileID].rwpointer = -1;   //Set pointer to -1
    return 0;
}

/* ======================================================================== */                                                                                                                                      
/* fwrite:                                                                  */                                                
/* Writes to a file, given that it is currently open.                       */                                                        
/* Procedures of writing to a file:                                         */                                    
/*     - If file needs another block(s) allocated to it:                    */                                                                
/*                                                                          */
/*         - If an indirect pointer block needs to be allocated             */                                                                        
/*                                                                          */                             
/*             - Allocate an indirect pointer block to file i-Node          */                                                        
/*                                                                          */    
/*         - Allocate block(s) to the file i-Node/Indirect BLock            */                                                                
/*                                                                          */        
/*     - Bring all blocks associated with file into memory                  */                                                            
/*     - Write to these blocks in memory                                    */                                            
/*     - Write these blocks to the disk                                     */ 
/*     - Set rw pointer to the end of file                                  */                                                                                                                                                                                                                                                      
/* ======================================================================== */
int sfs_fwrite(int fileID, const char* buf, int length) {
    if (!open_fd_table[fileID].inode) { //Check that file is open
        printf("SFS_API: CANNOT WRITE TO FILE; FILE NOT OPEN.\n");
        return -1;
    }

    i_node *file_i_node = open_fd_table[fileID].inode;  
    if (file_i_node->size >= (12*BLOCK_SIZE)+((BLOCK_SIZE/sizeof(int)*BLOCK_SIZE))) {   //Check that maximum file size isn't exceeded
        printf("SFS_API: CANNOT WRITE TO FILE; MAXIMUM FILE SIZE EXCEEDED.\n");
        return -1;
    }

    int blocks_occupied = size_to_blocks(file_i_node->size);    //How many blocks are currently occupied
    int blocks_required = size_to_blocks(open_fd_table[fileID].rwpointer+length) - blocks_occupied;     //How many blocks are required to be allocated for the write

    if (blocks_required > 0) {      //If file requires a block or more to be allocated

        int free_block;     
        int *indirect_block = (int *) malloc(BLOCK_SIZE);   
        if (blocks_occupied + blocks_required > 12) {   //If file has/needs an indirect pointer block
            if (blocks_occupied <= 12) {  //If the file needs an indirect pointer block allocated for the first time
                free_block = get_free_block();
                remove_bit(free_block);
                file_i_node->indirect_pointers = free_block;    //Set indirect pointer to point to block allocated
            }
            read_blocks(file_i_node->indirect_pointers, 1, indirect_block);     //Bring indirect pointer block into memory
        }

        for (int i = blocks_occupied; i-blocks_occupied < blocks_required; i++) {   //Allocate blocks needed to file
            free_block = get_free_block();
            if (free_block >= 0) {
                remove_bit(free_block);
                if (i < 12) {
                    file_i_node->pointers[i] = free_block;
                } 
                else {
                    indirect_block[i-12] = free_block;
                }
            } 
            else {
                printf("SFS_API: CANNOT WRITE TO FILE; NO MORE FREE BLOCKS AVAILABLE.\n");
                return -1;
            }
        }

        if (blocks_occupied + blocks_required > 12) {
            write_blocks(file_i_node->indirect_pointers, 1, indirect_block);  //Write indirect pointer block to disk (if needed)
        }
        free(indirect_block);
    
        file_i_node->link_cnt += blocks_required;
        write_blocks(BLOCK_AMOUNT-(size_to_blocks(sizeof(bitmap))), size_to_blocks(sizeof(bitmap)), &bitmap);
    } 

    int *indirect_block  = (int *) malloc(BLOCK_SIZE);
    if (blocks_occupied + blocks_required > 12) {
        read_blocks(file_i_node->indirect_pointers, 1, indirect_block);   //Bring indirect pointer block into memory (if needed)
    }
    
    void *file_blocks = malloc(BLOCK_SIZE * file_i_node->link_cnt);     
    
    for (int i = 0; i < file_i_node->link_cnt; i++) {   //Bring all blocks with file data into memory
        if (i < 12) {
            read_blocks(file_i_node->pointers[i], 1, (char *)file_blocks + (i*BLOCK_SIZE));
        }
        else {
            read_blocks(indirect_block[i-12], 1, (char *)file_blocks + (i*BLOCK_SIZE));
        }
    }

    memcpy((char *)file_blocks + open_fd_table[fileID].rwpointer, buf, length);   //Write to the file in memory from the buffer

    for (int i = 0; i < file_i_node->link_cnt; i++) {       //Write the file in memory to the disk
        if (i < 12) {
            write_blocks(file_i_node->pointers[i], 1, (char *)file_blocks + (i*BLOCK_SIZE));
        }
        else {
            write_blocks(indirect_block[i-12], 1, (char *)file_blocks + (i*BLOCK_SIZE));
        }
    }

    free(file_blocks);

    open_fd_table[fileID].rwpointer += length;      //Advance the pointer to the end of what was written
    int extra_bytes_written = open_fd_table[fileID].rwpointer - file_i_node->size;  //Calculate how much new data written to file
    if (extra_bytes_written > 0) {      //If size has increased
        file_i_node->size += extra_bytes_written;   //Write by how much the data increased
    }   

    int i_node_blocks = size_to_blocks(sizeof(i_node_table));   
    write_blocks(1,i_node_blocks,&i_node_table);    //Write updated i-Node table to disk
    write_directory();  //Write directory to disk
    return length;
}

/* ======================================================================== */                                                                                                                                      
/* fread:                                                                   */                                                
/* Reads from a file, given that it is currently open.                      */                                                         
/* Procedures of reading from a file:                                       */
/*     - Determine how many bytes will actually be read taking position of  */ 
/*         of the pointer, size of file, and the length of bytes to read    */ 
/*         into account                                                     */ 
/*     - Bring all blocks associated with file into memory                  */                                                            
/*     - Read from these blocks in memory to the buffer given               */ 
/*     - Set rw pointer to the point at which stopped reading               */                                                                                                                                                                                                                                                                                                                    
/* ======================================================================== */
int sfs_fread(int fileID, char* buf, int length) {
    if (!open_fd_table[fileID].inode) {     //Check that file is open
        printf("SFS_API: CANNOT READ FROM FILE; FILE NOT OPEN.\n");
        return -1;
    }
    i_node *file_i_node = open_fd_table[fileID].inode;  
    int bytes_available_to_read = file_i_node->size - open_fd_table[fileID].rwpointer;  //Calculate how many bytes will actually be read taking file size into account

    if (bytes_available_to_read < length) {
        length = bytes_available_to_read;       //If reading past file size, reduce amount of bytes to read
    }

    int *indirect_block  = (int *) malloc(BLOCK_SIZE);
    if (file_i_node->indirect_pointers > 0) {
        read_blocks(file_i_node->indirect_pointers, 1, indirect_block);     //Bring indirect pointer block into memory where an indirect pointer block is available
    }

    void *file_blocks = malloc(BLOCK_SIZE * file_i_node->link_cnt);
    for (int i = 0; i < file_i_node->link_cnt; i++) {       //Bring blocks of file into memory
        if (i < 12) { 
            read_blocks(file_i_node->pointers[i], 1, (char *)file_blocks + (i*BLOCK_SIZE));     
        }
        else {
            read_blocks(indirect_block[i-12], 1, (char *)file_blocks + (i*BLOCK_SIZE));     
        }
    }
    memcpy(buf, (char *)file_blocks + open_fd_table[fileID].rwpointer, length);     //Read data from memory blocks into buffer
    open_fd_table[fileID].rwpointer += length;      //Advance rw pointer to the end of data read

    free(indirect_block);
    free(file_blocks);
    return length;
}

/* ======================================================================== */                                                                                                                                      
/* fseek:                                                                   */                                                
/* Sets rw pointer of a file to the given location, only if file is open    */
/* and the location is within the file                                      */                                                                                                                                                                                                                                                                       
/* ======================================================================== */
int sfs_fseek(int fileID, int loc) {
    if (open_fd_table[fileID].inode) {  //Check that file exists
        if (open_fd_table[fileID].inode->size >= loc && loc >= 0) {     //Check that pointer is within file size boundaries
            open_fd_table[fileID].rwpointer = loc;  //Set pointer of file
            return 0;
        }
        else {
            printf("Location out of bounds for file.\n");
            return -1;
        }
    }
    printf("File not open.\n");  
    return -1;
}

/* ======================================================================== */                                                                                                                                      
/* remove:                                                                  */                                                
/* Removes file from directory, provided it exists                          */    
/* Properties assoiciate with file's i-Node entry and directory entry are   */                            
/* set to default (removing them).                                          */                                                                                                                                                                                                                                                         
/* ======================================================================== */
int sfs_remove(char* file) {
    int i_node_index = scan_dir_name(file);     //Get index of i-Node associated with file
    if (i_node_index == -1) {   //Check that file exists
        printf("SFS_API: COULD NOT REMOVE FILE; FILE DOES NOT EXIST");
        return -1;
    }
    i_node *file_i_node = &i_node_table[i_node_index];

    for (int i = 0; i < MAX_FD_AMOUNT; i++) {       //Make sure that file is not open
        if (file_i_node == open_fd_table[i].inode) {        
            printf("SFS_API: COULD NOT REMOVE FILE; FILE IS OPEN");
            return -1;        //If it is, return error
        }
    }

    int *indirect_block = (int *) malloc(BLOCK_SIZE);
    if (file_i_node->link_cnt > 12) {
        read_blocks(file_i_node->indirect_pointers, 1, indirect_block);     //Bring indirect pointer block into memory if applicable
    }

    for (int i = 0; i < file_i_node->link_cnt; i++) {   //Set free bits in bitmap
        if (i < 12) {
            set_bit(file_i_node->pointers[i]);
        }
        else {
            set_bit(indirect_block[i-12]);
        }
    }

    write_blocks(BLOCK_AMOUNT-(size_to_blocks(sizeof(bitmap))), size_to_blocks(sizeof(bitmap)), &bitmap);   //Write updated bitmap to memory

    i_node_table[i_node_index].mode = 0;        //Set i-Node back to default values
    i_node_table[i_node_index].link_cnt = 0;
    i_node_table[i_node_index].size = -1;
    for (int i = 0; i < 12; i++) {
        i_node_table[i_node_index].pointers[i] = -1;
    }
    i_node_table[i_node_index].indirect_pointers = -1;

    int i_node_blocks = size_to_blocks(sizeof(i_node_table));   

    write_blocks(1,i_node_blocks,&i_node_table);    //Write updated i-Node table to disk

    for (int i = 0; i < DIR_AMOUNT; i++) {
        if (strcmp(root_directory[i].file_name, file) == 0) {   //Set file directory entry values back to default
            strcpy(root_directory[i].file_name, "\0");
            root_directory[i].i_node_num = -1;
        }
    }    
    write_directory();  //Update directory on disk

    return 0;
}