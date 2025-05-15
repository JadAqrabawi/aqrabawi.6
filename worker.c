/*
 * worker.c - User process simulation
 * Generates memory requests (read/write) to OSS.
 * Author: Jad Aqrabawi
 * Date: 2025-05-15
 */

#define _POSIX_C_SOURCE 200809L  // Enable POSIX features (for nanosleep etc.)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <signal.h>
#include <string.h>

#define PAGE_SIZE 1024           // Size of each page in bytes (1 KB)
#define PAGE_TABLE_SIZE 32       // Number of pages per process (32 pages = 32 KB)
#define MSG_TYPE_OSS 1           // Message type used to send requests to OSS

// Define possible operations for memory access: READ or WRITE
typedef enum { READ, WRITE } Operation;

// Structure for message queue communication with OSS
typedef struct {
    long mtype;      // Message type (required by System V message queues)
    pid_t pid;       // Process ID of this worker process
    int address;     // Requested memory address
    Operation op;    // Read or Write operation
} Message;

// Global variables for message queue ID, process ID, and counters
int msgqid = -1;
int proc_id = -1;
int mem_access_count = 0;
int terminate_after = 1000; // Number of memory accesses before the worker terminates

// Signal handler to cleanly exit on interrupt signals
void cleanup_and_exit(int sig) {
    exit(0);
}

// Initialize the message queue to communicate with OSS
void init_msg_queue() {
    key_t key = ftok("oss.c", 'Q');  // Generate key using "oss.c" file and proj id 'Q'
    if (key == -1) {
        perror("ftok");               // Print error and exit on failure
        exit(EXIT_FAILURE);
    }
    msgqid = msgget(key, 0666);       // Get existing message queue with read/write permissions
    if (msgqid == -1) {
        perror("msgget");             // Print error and exit on failure
        exit(EXIT_FAILURE);
    }
}

// Generate a random memory address within this process's address space
int generate_random_address() {
    int page = rand() % PAGE_TABLE_SIZE;  // Random page number from 0 to 31
    int offset = rand() % PAGE_SIZE;       // Random offset within the page (0 to 1023)
    return page * PAGE_SIZE + offset;      // Calculate full memory address
}

// Generate a random memory operation: biased towards reads (~80% read)
Operation generate_random_operation() {
    int r = rand() % 10;
    return (r < 8) ? READ : WRITE;          // 80% reads, 20% writes
}

int main(int argc, char *argv[]) {
    // Expect exactly one command line argument: the process id
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <proc_id>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    proc_id = atoi(argv[1]);                 // Convert proc_id string to int
    srand(time(NULL) ^ getpid());            // Seed random number generator with time and PID

    // Setup signal handlers for graceful termination
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    init_msg_queue();                        // Connect to the OSS message queue

    // Main loop - generate and send memory requests continuously until termination condition met
    while (1) {
        Message msg;
        msg.mtype = MSG_TYPE_OSS;           // Set message type for OSS to receive
        msg.pid = getpid();                  // Set current process ID
        msg.address = generate_random_address();  // Generate random address to request
        msg.op = generate_random_operation();     // Random read/write operation

        // Send request message to OSS
        if (msgsnd(msgqid, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
            perror("msgsnd");
            exit(EXIT_FAILURE);
        }

        // Wait for response from OSS indicating request was fulfilled or page fault handled
        Message resp;
        if (msgrcv(msgqid, &resp, sizeof(Message) - sizeof(long), getpid(), 0) == -1) {
            perror("msgrcv");
            exit(EXIT_FAILURE);
        }

        mem_access_count++;                  // Count the completed memory access

        // Check if termination condition reached (approx every 1000 memory accesses)
        if (mem_access_count >= terminate_after) {
            break;                          // Exit loop to terminate process
        }

        // Sleep for 100-150 milliseconds to simulate time between requests
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = (100000 + (rand() % 50000)) * 1000; // Convert microseconds to nanoseconds
        nanosleep(&ts, NULL);
    }

    // Terminate gracefully (OSS will detect termination via waitpid)
    return 0;
}
