/*
 * oss.c - Operating System Simulator
 * Implements memory management with LRU page replacement.
 * Author: Jad Aqrabawi
 * Date: 2025-05-15
 */

#define _POSIX_C_SOURCE 200809L  // Enable POSIX extensions (for nanosleep, etc.)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>

// Maximum number of concurrent user processes allowed
#define MAX_PROCESSES 18
// Maximum total processes to create before terminating simulation
#define MAX_TOTAL_PROCS 100
// Number of pages per process (each process has 32 pages)
#define PAGE_TABLE_SIZE 32
// Total simulated memory size (128 KB)
#define MEMORY_SIZE 131072          
// Size of each frame (1 KB)
#define FRAME_SIZE 1024
// Total number of frames in memory (128 frames)
#define NUM_FRAMES (MEMORY_SIZE / FRAME_SIZE) 
// Simulated disk IO time in nanoseconds (14 milliseconds)
#define DISK_IO_TIME_NS 14000000    
// Message type for communication from workers to OSS
#define MSG_TYPE_OSS 1

// Defines possible memory operations: READ or WRITE
typedef enum {
    READ,
    WRITE
} Operation;

// Message structure for IPC communication
typedef struct {
    long mtype;         // Message type (required for System V message queues)
    pid_t pid;          // PID of the worker sending the message
    int address;        // Requested memory address
    Operation op;       // Requested operation (read/write)
} Message;

// Frame structure representing a physical frame in memory
typedef struct {
    bool occupied;         // Whether frame is currently occupied
    bool dirty;            // Dirty bit - whether frame has been written to
    unsigned int page_number; // Page number loaded in this frame
    pid_t owner_pid;       // PID of the process owning this frame
    unsigned int last_ref_sec;  // Last referenced time seconds part
    unsigned int last_ref_ns;   // Last referenced time nanoseconds part
} Frame;

// Page table structure holding frame indices for each page
typedef struct {
    int frames[PAGE_TABLE_SIZE]; // For each page: frame number or -1 if not loaded
} PageTable;

// Process state enumeration
typedef enum {
    PROCESS_READY,       // Process ready to run
    PROCESS_BLOCKED,     // Process blocked waiting for IO
    PROCESS_TERMINATED   // Process terminated
} ProcState;

// Process Control Block (PCB) structure to track each process info
typedef struct {
    pid_t pid;               // PID of the process
    int id;                  // Logical process id
    ProcState state;         // Current state of the process
    PageTable page_table;    // Page table for this process
    int mem_accesses;        // Count of memory accesses performed
    int page_faults;         // Count of page faults encountered
    unsigned int last_access_sec;  // Last access time seconds part
    unsigned int last_access_ns;   // Last access time nanoseconds part
} PCB;

// IORequest structure represents a pending disk IO request
typedef struct {
    pid_t pid;                  // PID of requesting process
    int page_num;               // Page number requested
    Operation op;               // Operation type (read/write)
    unsigned int request_start_sec;  // Time when request started (seconds)
    unsigned int request_start_ns;   // Time when request started (nanoseconds)
} IORequest;

// Global variables
Frame frame_table[NUM_FRAMES];             // The frame table representing physical memory
PCB process_table[MAX_PROCESSES];          // Table of process control blocks

unsigned int sim_clock_sec = 0;            // Simulated clock seconds part
unsigned int sim_clock_ns = 0;             // Simulated clock nanoseconds part

int msgqid = -1;                           // Message queue ID for IPC
FILE *logfile = NULL;                      // Log file pointer

int active_processes = 0;                  // Number of active user processes
int total_created = 0;                     // Total number of processes created

IORequest io_queue[MAX_PROCESSES];         // Circular queue for IO requests
int io_queue_start = 0;                    // IO queue start index
int io_queue_end = 0;                      // IO queue end index

char logfile_name[256] = "oss_log.txt";    // Default logfile name

// Configurable parameters with defaults
int max_processes = 18;                    // Max concurrent user processes allowed
int max_simulation_seconds = 5;            // Max simulation real-time seconds
int launch_interval_ms = 1000;             // Interval between launching new processes (ms)

// Forward declarations of functions implemented below
void cleanup_and_exit(int sig);
void increment_clock(unsigned int ns);
void print_log(const char *fmt, ...);
void print_memory_layout();
void print_page_tables();
int find_free_frame();
int lru_replace_frame();
void enqueue_io_request(IORequest req);
bool io_queue_empty();
IORequest *peek_io_request();
IORequest dequeue_io_request();
void check_io_completion();
void launch_process(int proc_id);
void receive_message();
void release_frames(int proc_id);
void print_usage(const char *progname);

// Initialize process table entries to default values
void init_process_table() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].pid = 0;
        process_table[i].id = i;
        process_table[i].state = PROCESS_TERMINATED; // Initially terminated
        process_table[i].mem_accesses = 0;
        process_table[i].page_faults = 0;
        process_table[i].last_access_sec = 0;
        process_table[i].last_access_ns = 0;
        for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
            process_table[i].page_table.frames[p] = -1; // No frames allocated
        }
    }
}

// Initialize the frame table to empty and clean
void init_frame_table() {
    for (int i = 0; i < NUM_FRAMES; i++) {
        frame_table[i].occupied = false;
        frame_table[i].dirty = false;
        frame_table[i].owner_pid = 0;
        frame_table[i].page_number = 0;
        frame_table[i].last_ref_sec = 0;
        frame_table[i].last_ref_ns = 0;
    }
}

// Main function - entry point of the simulation
int main(int argc, char *argv[]) {
    // Parse command-line arguments
    int opt;
    while ((opt = getopt(argc, argv, "hn:s:i:f:")) != -1) {
        switch (opt) {
            case 'h':  // Help message
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            case 'n':  // Maximum concurrent processes to run
                max_processes = atoi(optarg);
                if (max_processes > MAX_PROCESSES) max_processes = MAX_PROCESSES;
                break;
            case 's':  // Maximum simulation time in seconds
                max_simulation_seconds = atoi(optarg);
                break;
            case 'i':  // Interval in milliseconds between launching new processes
                launch_interval_ms = atoi(optarg);
                break;
            case 'f':  // Log file name
                strncpy(logfile_name, optarg, sizeof(logfile_name)-1);
                logfile_name[sizeof(logfile_name)-1] = '\0';
                break;
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Setup signal handlers for clean termination
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    // Open logfile for writing; exit on failure
    logfile = fopen(logfile_name, "w");
    if (!logfile) {
        perror("Failed to open logfile");
        exit(EXIT_FAILURE);
    }

    // Initialize process and frame tables
    init_process_table();
    init_frame_table();

    // Create or get the message queue used for IPC with workers
    key_t key = ftok("oss.c", 'Q');
    if (key == -1) {
        perror("ftok");
        exit(EXIT_FAILURE);
    }
    msgqid = msgget(key, IPC_CREAT | 0666);
    if (msgqid == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }

    print_log("OSS starting simulation.\n");
    unsigned int last_print_sec = 0;

    // Record real start time for terminating after max real seconds
    time_t real_start = time(NULL);
    unsigned int last_launch_time_ms = 0;
    int next_proc_id_to_launch = 0;

    // Main simulation loop - runs until max total processes or time exceeded
    while (total_created < MAX_TOTAL_PROCS &&
           (time(NULL) - real_start) < (time_t)max_simulation_seconds) {

        // Calculate current simulated time in milliseconds
        unsigned int current_ms = sim_clock_sec * 1000 + sim_clock_ns / 1000000;

        // Launch new user process if allowed by max and interval
        if (active_processes < max_processes && next_proc_id_to_launch < max_processes) {
            if (current_ms - last_launch_time_ms >= (unsigned int)launch_interval_ms) {
                launch_process(next_proc_id_to_launch);
                next_proc_id_to_launch++;
                last_launch_time_ms = current_ms;
            }
        }

        // Check if any IO request has completed and handle it
        check_io_completion();

        // Receive memory requests or termination messages from workers
        receive_message();

        // Print memory layout and page tables every simulated second
        if (sim_clock_sec > last_print_sec) {
            print_memory_layout();
            print_page_tables();
            last_print_sec = sim_clock_sec;
        }

        // Increment simulated clock by 100 nanoseconds per loop iteration
        increment_clock(100);
    }

    // After main loop, wait for remaining children to finish
    while (active_processes > 0) {
        check_io_completion();
        receive_message();
        increment_clock(100);
    }

    print_log("OSS simulation ending.\n");

    // Cleanup resources and exit cleanly
    cleanup_and_exit(0);
    return 0;
}

// Print usage message for OSS executable
void print_usage(const char *progname) {
    printf("Usage: %s [-h] [-n proc] [-s simul] [-i intervalInMsToLaunchChildren] [-f logfile]\n", progname);
    printf("  -h\tShow this help message\n");
    printf("  -n proc\tMax concurrent user processes (default 18)\n");
    printf("  -s simul\tSimulation time seconds (default 5)\n");
    printf("  -i interval\tInterval in ms to launch children (default 1000)\n");
    printf("  -f logfile\tLog file name (default oss_log.txt)\n");
}

// Logging helper function - logs to both file and stdout
#include <stdarg.h>
void print_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(logfile, fmt, args);
    fflush(logfile);
    va_end(args);

    va_start(args, fmt);
    vprintf(fmt, args);
    fflush(stdout);
    va_end(args);
}

// Increment the simulated clock by given nanoseconds
void increment_clock(unsigned int ns) {
    sim_clock_ns += ns;
    while (sim_clock_ns >= 1000000000) {
        sim_clock_ns -= 1000000000;
        sim_clock_sec++;
    }
}

// Print current memory frame table status
void print_memory_layout() {
    print_log("Current memory layout at time %u:%u\n", sim_clock_sec, sim_clock_ns);
    print_log("Frame  Occupied DirtyBit LastRefS LastRefNS\n");
    for (int i = 0; i < NUM_FRAMES; i++) {
        print_log(" %4d   %s     %d       %u      %u\n",
                  i,
                  frame_table[i].occupied ? "Yes" : "No",
                  frame_table[i].dirty ? 1 : 0,
                  frame_table[i].last_ref_sec,
                  frame_table[i].last_ref_ns);
    }
}

// Print page tables of all active processes
void print_page_tables() {
    for (int i = 0; i < max_processes; i++) {
        if (process_table[i].state != PROCESS_TERMINATED) {
            print_log("P%d page table: [ ", i);
            for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
                print_log("%d ", process_table[i].page_table.frames[p]);
            }
            print_log("]\n");
        }
    }
}

// Find a free frame in the frame table, return index or -1 if none free
int find_free_frame() {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (!frame_table[i].occupied) return i;
    }
    return -1;
}

// Use Least Recently Used algorithm to select frame to replace
int lru_replace_frame() {
    unsigned long long oldest_time = ULLONG_MAX;
    int victim = -1;
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (frame_table[i].occupied) {
            unsigned long long ref_time = (unsigned long long)frame_table[i].last_ref_sec * 1000000000ULL
                                        + frame_table[i].last_ref_ns;
            if (ref_time < oldest_time) {
                oldest_time = ref_time;
                victim = i;
            }
        }
    }
    return victim;
}

// Add a disk IO request to the circular IO queue
void enqueue_io_request(IORequest req) {
    io_queue[io_queue_end] = req;
    io_queue_end = (io_queue_end + 1) % MAX_PROCESSES;
}

// Check if the IO queue is empty
bool io_queue_empty() {
    return io_queue_start == io_queue_end;
}

// Peek at the front IO request without removing it
IORequest *peek_io_request() {
    if (io_queue_empty()) return NULL;
    return &io_queue[io_queue_start];
}

// Remove and return the front IO request from the queue
IORequest dequeue_io_request() {
    IORequest req = io_queue[io_queue_start];
    io_queue_start = (io_queue_start + 1) % MAX_PROCESSES;
    return req;
}

// Check if the IO request at the head of the queue has completed
void check_io_completion() {
    if (io_queue_empty()) return;

    IORequest *req = peek_io_request();
    unsigned long long request_time_ns = (unsigned long long)req->request_start_sec * 1000000000ULL + req->request_start_ns;
    unsigned long long current_time_ns = (unsigned long long)sim_clock_sec * 1000000000ULL + sim_clock_ns;

    // If enough simulated time has passed, complete IO
    if (current_time_ns - request_time_ns >= DISK_IO_TIME_NS) {
        IORequest finished_req = dequeue_io_request();

        // Find process index for this request
        int pcb_index = -1;
        for (int i = 0; i < max_processes; i++) {
            if (process_table[i].pid == finished_req.pid) {
                pcb_index = i;
                break;
            }
        }
        if (pcb_index == -1) return; // Process ended meanwhile

        // Try to find free frame or pick victim with LRU
        int frame_num = find_free_frame();
        if (frame_num == -1) {
            frame_num = lru_replace_frame();
            Frame *victim_frame = &frame_table[frame_num];

            // If dirty bit set, add extra IO time to clock
            if (victim_frame->dirty) {
                print_log("oss: Dirty bit set on frame %d, adding disk write time.\n", frame_num);
                increment_clock(DISK_IO_TIME_NS);
            }

            // Remove victim frame from owning process page table
            for (int i = 0; i < max_processes; i++) {
                for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
                    if (process_table[i].page_table.frames[p] == frame_num) {
                        process_table[i].page_table.frames[p] = -1;
                        break;
                    }
                }
            }

            print_log("oss: Clearing frame %d and swapping in pid %d page %d\n", frame_num, finished_req.pid, finished_req.page_num);
        }

        // Fill the frame with requested page
        frame_table[frame_num].occupied = true;
        frame_table[frame_num].dirty = (finished_req.op == WRITE);
        frame_table[frame_num].page_number = finished_req.page_num;
        frame_table[frame_num].owner_pid = finished_req.pid;
        frame_table[frame_num].last_ref_sec = sim_clock_sec;
        frame_table[frame_num].last_ref_ns = sim_clock_ns;

        // Update process page table to map page to new frame
        process_table[pcb_index].page_table.frames[finished_req.page_num] = frame_num;

        // Unblock the process
        process_table[pcb_index].state = PROCESS_READY;
        print_log("oss: Completed I/O for pid %d page %d\n", finished_req.pid, finished_req.page_num);

        // Notify process IO completion via message queue
        Message resp;
        resp.mtype = finished_req.pid;
        resp.pid = 0;
        resp.address = finished_req.page_num * FRAME_SIZE;
        resp.op = finished_req.op;
        if (msgsnd(msgqid, &resp, sizeof(Message) - sizeof(long), 0) == -1) {
            perror("msgsnd");
        }
    }
}

// Fork and exec a worker process with given proc_id
void launch_process(int proc_id) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        // In child process - exec worker with proc_id as argument
        char proc_id_str[16];
        snprintf(proc_id_str, sizeof(proc_id_str), "%d", proc_id);
        execl("./worker", "./worker", proc_id_str, NULL);
        perror("execl worker failed");
        exit(EXIT_FAILURE);
    } else {
        // In parent - record worker info and update bookkeeping
        process_table[proc_id].pid = pid;
        process_table[proc_id].state = PROCESS_READY;
        process_table[proc_id].mem_accesses = 0;
        process_table[proc_id].page_faults = 0;
        for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
            process_table[proc_id].page_table.frames[p] = -1;
        }
        active_processes++;
        total_created++;
        print_log("oss: Launched process P%d with pid %d\n", proc_id, pid);
    }
}

// Receive messages from workers (memory requests or terminations)
void receive_message() {
    Message msg;
    ssize_t ret = msgrcv(msgqid, &msg, sizeof(Message) - sizeof(long), MSG_TYPE_OSS, IPC_NOWAIT);
    if (ret == -1) {
        if (errno != ENOMSG) {
            perror("msgrcv");
        }
        return; // No messages to process now
    }

    // Find process index by pid
    int pcb_index = -1;
    for (int i = 0; i < max_processes; i++) {
        if (process_table[i].pid == msg.pid) {
            pcb_index = i;
            break;
        }
    }
    if (pcb_index == -1) {
        print_log("oss: Received message from unknown pid %d\n", msg.pid);
        return;
    }
    if (process_table[pcb_index].state == PROCESS_TERMINATED) return;

    // Update memory access count for process
    process_table[pcb_index].mem_accesses++;

    // Calculate requested page number and check frame mapping
    unsigned int page_num = msg.address / FRAME_SIZE;
    int frame_num = process_table[pcb_index].page_table.frames[page_num];

    print_log("oss: P%d requesting %s of address %d at time %u:%u\n",
              pcb_index, msg.op == READ ? "read" : "write", msg.address, sim_clock_sec, sim_clock_ns);

    // If page not loaded in a frame or frame not occupied, page fault occurs
    if (frame_num == -1 || !frame_table[frame_num].occupied) {
        process_table[pcb_index].page_faults++;
        process_table[pcb_index].state = PROCESS_BLOCKED;

        // Queue IO request for bringing page into memory
        IORequest req = {
            .pid = msg.pid,
            .page_num = page_num,
            .op = msg.op,
            .request_start_sec = sim_clock_sec,
            .request_start_ns = sim_clock_ns
        };
        enqueue_io_request(req);

        print_log("oss: Address %d is not in a frame, pagefault\n", msg.address);
        return; // Process blocked, do not reply yet
    }

    // No page fault: update frame last reference and dirty bit if writing
    Frame *frame = &frame_table[frame_num];
    frame->last_ref_sec = sim_clock_sec;
    frame->last_ref_ns = sim_clock_ns;
    if (msg.op == WRITE) {
        frame->dirty = true;
    }

    print_log("oss: Address %d in frame %d, %s data to P%d at time %u:%u\n",
              msg.address, frame_num, msg.op == READ ? "giving" : "writing", pcb_index, sim_clock_sec, sim_clock_ns);

    // Increment simulated clock for handling this request
    increment_clock(100);

    // Send reply message back to worker so it continues
    Message resp;
    resp.mtype = msg.pid;
    resp.pid = 0;
    resp.address = msg.address;
    resp.op = msg.op;

    if (msgsnd(msgqid, &resp, sizeof(Message) - sizeof(long), 0) == -1) {
        perror("msgsnd");
    }
}

// Release all frames owned by a terminated process
void release_frames(int proc_id) {
    for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
        int f = process_table[proc_id].page_table.frames[p];
        if (f != -1) {
            frame_table[f].occupied = false;
            frame_table[f].dirty = false;
            frame_table[f].owner_pid = 0;
            frame_table[f].page_number = 0;
            frame_table[f].last_ref_sec = 0;
            frame_table[f].last_ref_ns = 0;
            process_table[proc_id].page_table.frames[p] = -1;
        }
    }
}

// Clean up resources and exit simulation
void cleanup_and_exit(int sig) {
    print_log("oss: Cleaning up and exiting...\n");

    // Terminate all remaining child processes
    for (int i = 0; i < max_processes; i++) {
        if (process_table[i].state != PROCESS_TERMINATED && process_table[i].pid > 0) {
            kill(process_table[i].pid, SIGTERM);
            waitpid(process_table[i].pid, NULL, 0);
        }
    }

    // Remove message queue
    if (msgqid != -1) {
        msgctl(msgqid, IPC_RMID, NULL);
    }

    // Close logfile if open
    if (logfile) {
        fclose(logfile);
    }

    // Exit with success or failure depending on signal
    exit(sig == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
