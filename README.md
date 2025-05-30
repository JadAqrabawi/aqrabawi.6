---

# Memory Management Simulator with LRU Page Replacement

## Description

This project simulates an operating system's memory management subsystem focusing on paging and page replacement using the Least Recently Used (LRU) algorithm. It manages page tables and memory requests for user processes, handling page faults and simulating disk I/O delays when pages are swapped in or out.

The operating system simulator (`oss`) forks multiple user processes (`worker`s), which continuously send memory access requests (read/write) for random addresses. The OSS handles these requests, maintains page tables, and simulates the behavior of an OS managing physical memory frames and swapping pages to disk.

The project demonstrates core concepts such as:

* Page table management per process
* Frame allocation and replacement with LRU
* Disk I/O simulation with dirty bit optimization
* Inter-process communication using System V message queues
* Simulated system clock for timing events
* Process lifecycle management

---

## Compilation

Make sure you have a C compiler (like `gcc`) and standard build tools installed on your system.

1. Clone the repository:

```bash
git clone https://github.com/JadAqrabawi/aqrabawi.6.git
cd aqrabawi.6
```

2. Build the project using the provided Makefile:

```bash
make
```

This will compile two executables:

* `oss` — the operating system simulator
* `worker` — the user process simulation

---

## Running the Simulator

Run the OSS simulator with the following command-line options:

```
./oss [-h] [-n proc] [-s simul] [-i intervalInMsToLaunchChildren] [-f logfile]
```

* `-h` : Show help message
* `-n proc` : Maximum number of concurrent user processes (default: 18)
* `-s simul` : Simulation time in seconds (default: 5)
* `-i interval` : Interval in milliseconds between launching new user processes (default: 1000)
* `-f logfile` : Path to the log file where OSS output is saved (default: `oss_log.txt`)

Example usage:

```bash
./oss -n 10 -s 60 -i 2000 -f oss_log.txt
```

This runs the simulation with up to 10 concurrent processes for 60 seconds, launching a new process every 2000 milliseconds, and logs OSS activity to `oss_log.txt`.

---

## Important Information

* The OSS simulator logs detailed events (memory requests, page faults, page swaps, memory layout, and page tables) both to standard output and the specified log file.
* Worker processes do not write to the log file to keep logs focused on OS behavior.
* The project uses System V IPC message queues for communication between OSS and workers.
* The program cleans up all IPC resources and terminates all child processes on exit or when interrupted (e.g., CTRL+C).
* The LRU page replacement algorithm considers dirty bits to add extra simulated disk I/O time when swapping out modified pages.
* The simulation ends after the specified time or after creating 100 total processes, whichever comes first.
* The Makefile includes `clean` target to remove executables and logs:

  ```bash
  make clean
  ```
* For any issues, check the log file specified and ensure the worker executable is present in the same directory as OSS.

---

## Repository

The full source code and Makefile can be found at:
[https://github.com/JadAqrabawi/aqrabawi.6.git](https://github.com/JadAqrabawi/aqrabawi.6.git)

---# aqrabawi.6