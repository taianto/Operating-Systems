/* ======================================================================== */                                                                                                                                      
/*  Name: Anton Tairov                                                      */                
/*  ID: 261122595                                                           */        
/* ======================================================================== */

#ifndef __SUT_H__
#define __SUT_H__
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include<ucontext.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "queue.h"

#define STACK_SIZE 1024*1024

typedef void (*sut_task_f)();

//Task Control Block
//Stores necessary information about task
typedef struct task {
    ucontext_t *context;
    char operation[10];
    int fd;
    char *buf;
    int size;
    bool io_indicator;
    bool yield_indicator;
}TCB;

pthread_t *iexec;
pthread_t *cexec;
struct queue task_ready_queue;
struct queue wait_queue;
ucontext_t *main_cx;
pthread_mutex_t cexec_mutex;
pthread_mutex_t iexec_mutex;
TCB *current_cexec_task;
bool queue_enabled;
bool finished;
int thread_count;

/* ======================================================================== */                                                                                                                                      
/*  C-EXECUTOR:                                                             */                
/*  Is responsible for choosing what task to run next, and  which task is   */    
/* put into wait queue and which task is yielded based on information       */
/* stored in the TCB. Tasks are run using FIFO within the queue.            */                                                 
/* ======================================================================== */
void *c_executor() {
    struct timespec s,ns;       //Initialise variables for nanosleep()
    s.tv_sec = 0;
    ns.tv_nsec = 100000L;

    while(!finished) {          //Executor runs until tasks are finished & is empty
        pthread_mutex_lock(&cexec_mutex);
        struct queue_entry *task = queue_peek_front(&task_ready_queue);     //Check if there is a task in the wait queue - atomic
        pthread_mutex_unlock(&cexec_mutex);
        if (task) {                                     //If task is at head:
            pthread_mutex_lock(&cexec_mutex);
            queue_pop_head(&task_ready_queue);          //Pop head TCB out of queue - atomic
            pthread_mutex_unlock(&cexec_mutex);

            TCB *ut = (TCB *)task->data;
            current_cexec_task = ut;                    //Set global current task pointer to popped task

            swapcontext(main_cx, ut->context);          //Swap context to context of task
                                                        //When returned to main context:
            if (ut->io_indicator) {                     //If IO task:
                pthread_mutex_lock(&iexec_mutex);
                queue_insert_tail(&wait_queue, queue_new_node(ut)); //Place task into wait queue - atomic
                pthread_mutex_unlock(&iexec_mutex);
            }
            else if (ut->yield_indicator) {             //Else if task is yielding
                pthread_mutex_lock(&cexec_mutex);
                queue_insert_tail(&task_ready_queue, queue_new_node(ut));   //Place task at the end of task ready queue - atomic
                pthread_mutex_unlock(&cexec_mutex);
                ut->yield_indicator = 0;                                    //Task indicator set to false
            }
            queue_enabled = true;       //C-EXEC ran a task and can now quit if needed
        } else if (queue_enabled) {     //If empty and enabled:
            nanosleep(&s,&ns);          //Sleep for 100 micro seconds
            if (thread_count == 0) {    //If there are no other user threads, quit
                finished = true;
            }
        }
    }
    return 0;
}

/* ======================================================================== */                                                                                                                                      
/*  I-EXECUTOR:                                                            */                
/*  Is responsible for computing IO tasks. Makes use of the wait queue,     */                        
/*  tasks that are waiting for an IO response are taken out of the wait     */                        
/*  queue and replaced into the task ready queue once the IO operation      */                        
/*  has finished and results of the operation are updated                   */        
/* ======================================================================== */
void *i_executor() {
    struct timespec s,ns;     //Initialise variables for nanosleep()
    s.tv_sec = 0;
    ns.tv_nsec = 100000L;
    struct queue_entry *task;
    task = (struct queue_entry *)malloc(sizeof(struct queue_entry));
    while(!finished) {          //Executor runs until tasks are finished & is empty
        pthread_mutex_lock(&iexec_mutex);
        task = queue_peek_front(&wait_queue);    //Check head of wait queue
        pthread_mutex_unlock(&iexec_mutex);
        if (task) {                             //If head of wait queue is not empty:
            pthread_mutex_lock(&iexec_mutex);
            queue_pop_head(&wait_queue);        //pop item off wait queue
            pthread_mutex_unlock(&iexec_mutex);
            
            TCB *ut = (TCB *)task->data;        //Pointer to TCB in wait queue
            if (strcmp(ut->operation, "open") == 0) {       //If operation is "open"      
                int fd = open(ut->buf, O_RDWR | O_CREAT, 0666);  //Open file, if none exists create with the name of file and set permissions
                if (fd > 0) {   //If open successful
                    ut->fd = fd;   //Save fd
                }
                else if (fd == -1) { //If open unsuccessful
                    ut->fd = fd;
                }
            }
            else if (strcmp(ut->operation, "write") == 0) {   //If operation is "write"
                ssize_t status = write(ut->fd, ut->buf, ut->size);    //Write to file, save status
                if (status == -1) {   //if write unsuccessful
                    printf("Could not write to file file\n");
                }
            }
            else if (strcmp(ut->operation, "close") == 0) {  //If operation is "close"
                int status = close(ut->fd);   //Close file
                if (status == -1) {     //If close unsuccessful
                    printf("Could not close file\n");
                }
            }
            else if (strcmp(ut->operation, "read") == 0) {   //If operation is "read"
                int status = read(ut->fd, ut->buf, ut->size);  //read from file
                if (status == -1) {  //If read unsuccessful
                    ut->fd = status;  //Set "fd" as -1, serves as an indicator wether read was unsuccessful
                }
                else ut->fd = 0;  // read successful
            }
            else  {
                printf("Unknown Error");
            }
                ut->io_indicator = 0;    //Set io_indicator to false for putting back into queue

                pthread_mutex_lock(&cexec_mutex);
                queue_insert_tail(&task_ready_queue, queue_new_node(ut));  //place task back into task ready queue - atomic
                pthread_mutex_unlock(&cexec_mutex);
                queue_enabled = true;    //Indication that the first task has completed
        } else if (queue_enabled) {
            nanosleep(&s,&ns);           //Sleep for 100 micro seconds
            if (thread_count == 0) {     //If first task completed and wait queue is empty and no other tasks exists
                finished = true;     //exit
                free(task);    //free allocated memory for task
            }
        }
        
    }
    return 0;
}

//Initialisation process for SUT
void sut_init() {    
    finished = false;        //Set queue and executor booleans to false
    queue_enabled = false;

    cexec = (pthread_t*)malloc(sizeof(pthread_t));     //Allocate memory for threads
    iexec = (pthread_t*)malloc(sizeof(pthread_t));

    pthread_create(cexec, NULL, c_executor, NULL);   //Create threads
    pthread_create(iexec, NULL, i_executor, NULL);
    
    task_ready_queue = queue_create();      //Create task ready queue          
    queue_init(&task_ready_queue);          //Initialise task ready queue
    wait_queue = queue_create();            //Create wait queue
    queue_init(&wait_queue);                //Initialise wait queue
    
    main_cx = (ucontext_t *)malloc(sizeof(ucontext_t));   //Allocate memory for main context
    getcontext(main_cx);        //Save main context

    pthread_mutex_init(&cexec_mutex, PTHREAD_MUTEX_DEFAULT);   //Create mutexes for kernel threads
    pthread_mutex_init(&iexec_mutex, PTHREAD_MUTEX_DEFAULT);
       
    current_cexec_task = (TCB *)malloc(sizeof(TCB)); //Allocate memory for current task TCB 
    
}

//Creating new context
bool sut_create(sut_task_f fn) {
    TCB *task;
    task = (TCB *)malloc(sizeof(TCB));    //Allocate memory for TCB
    task->context = (ucontext_t *)malloc(sizeof(ucontext_t));    //Allocate memory for context

    if( getcontext(task->context) == -1)  {     //Create context
        return 0;
    }

    char *uc_s;
    uc_s = (char *)malloc(sizeof(char) * (STACK_SIZE));   //Allocate stack memory for context

    task->context->uc_stack.ss_sp = uc_s;
    task->context->uc_stack.ss_size = sizeof(char) * (STACK_SIZE);
    task->context->uc_stack.ss_flags = 0;
    task->context->uc_link = main_cx;       //Come back to main_cx once finished
    strcpy(task->operation,"exec");         //Set default operation to "exec"  
    task->fd = 0;                           //Default fd
    task->buf = (char *)malloc(sizeof(char) * (50));   //Alocate space for buffer
    task->size = 0;                         //Default size of buffer 0
    task->yield_indicator = false;          //Default yield_indicator
    task->io_indicator = false;             //Default IO_indicator

    thread_count++;                         //Increase user thread count

    makecontext(task->context, fn, 0);      //Create context to a function given

    pthread_mutex_lock(&cexec_mutex);
    queue_insert_tail(&task_ready_queue, queue_new_node(task));     //Put TCB of task into task ready queue - atomic
    pthread_mutex_unlock(&cexec_mutex);
            
    return 0;
}

//Yield context
void sut_yield() {
    getcontext(current_cexec_task->context);    //Save context
    current_cexec_task->yield_indicator = 1;    //Indicate context is yielding for when context switches to c-exec
    swapcontext(current_cexec_task->context, main_cx); //Swap context to main_cx
    return;
}

//Exit context
void sut_exit() {
    thread_count--;      //Reduce number of user threads - needed for executors   
    setcontext(main_cx); //Switch context to main immediately
}

//Open operation for context
int sut_open(char *fname) {                                                                        
    strcpy(current_cexec_task->operation,"open");                    //Set operation to "open"                                                                      
    strcpy(current_cexec_task->buf,fname);                           //Use buffer as holder for file name                
    current_cexec_task->io_indicator = true;                         //Set IO indicator to true                    
    current_cexec_task->fd = -1;                                     //Set default fd to -1           
    current_cexec_task->size = 0;                                    //Set default size to 0           
    getcontext(current_cexec_task->context);                        //Save current context
    swapcontext(current_cexec_task->context, main_cx);              //Swap context to main context

    return current_cexec_task->fd;                                  //Upon returning from task ready queue, return fd result 
}

//Write operation for context
void sut_write(int fd, char *buf, int size) {
    strcpy(current_cexec_task->operation,"write");      //Set operation to "write"                                       
    strcpy(current_cexec_task->buf,buf);                //Copy given buffer into current task buffer     
    current_cexec_task->fd = fd;                        //Set fd of current task to fd given
    current_cexec_task->size = size;                    //Set size of current task to size given
    current_cexec_task->io_indicator = true;            //Set IO indicator to true      
    getcontext(current_cexec_task->context);            //Save user context
    swapcontext(current_cexec_task->context, main_cx);  //Swap context to main context
    return;
}

//Close operation for context
void sut_close(int fd) {                                
    getcontext(current_cexec_task->context);         //Save current context                     
    strcpy(current_cexec_task->operation,"close");   //Set operation to "close"                           
    current_cexec_task->fd = fd;                     //Set current task fd to fd given  
    current_cexec_task->io_indicator = true;         //Set IO indicator to true                    
    swapcontext(current_cexec_task->context, main_cx); //Swap context to main context
    return;
}

//Read operation for context
char *sut_read(int fd, char *buf, int size){
    strcpy(current_cexec_task->operation,"read");          //Set operation to "read"                  
    current_cexec_task->buf = buf;                         //Set buffer pointer of current task to buffer given    
    current_cexec_task->fd = fd;                           //Set fd of current task to fd given     
    current_cexec_task->size = size;                       //Set buffer size of current task to given buffer size         
    current_cexec_task->io_indicator = true;               //Set IO indicator to true                 
    getcontext(current_cexec_task->context);               //Save current context                
    swapcontext(current_cexec_task->context, main_cx);     //Swap to main context

    if (current_cexec_task->fd == 0) {     //Upon return, if fd was set to 0 then read was sucessfull
        return "Read Successfully";
    }
    return NULL;                           //Else read not successful
}

//Joins threads and exit
void sut_shutdown() {
    pthread_join(*cexec,NULL);            //Join kernel threads                       
    pthread_join(*iexec,NULL);                                 
    free(iexec);                         //Free allocated memory          
    free(cexec);                                    
    free(main_cx);                                  
    free(current_cexec_task);                                   
}

#endif
