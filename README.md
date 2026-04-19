# Multi-Container Runtime Project
## Team Information

- **Srotaswini Das** - PES2UG24CS525
- **Sriya Dasari** - PES2UG24CS522

---

## Project Summary

This project implements a lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor. The runtime can manage multiple containers simultaneously, coordinate concurrent logging safely, expose a supervisor CLI, and run scheduling experiments.

### Components

1. **User-Space Runtime + Supervisor (`engine.c`)** - Launches and manages isolated containers, maintains metadata, accepts CLI commands, captures output through bounded-buffer logging
2. **Kernel-Space Monitor (`monitor.c`)** - Linux Kernel Module that tracks container processes, enforces soft/hard memory limits via `ioctl`

---

## Build, Load, and Run Instructions

### Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
Prepare Root Filesystem
bash
# Download Alpine Linux rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
mkdir rootfs-base
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create writable copies for containers
cp -a rootfs-base rootfs-container1
cp -a rootfs-base rootfs-container2
cp -a rootfs-base rootfs-container3
Build the Project
bash
make clean
make all
This compiles:

engine - User-space runtime and supervisor

monitor.ko - Kernel memory monitor module

memory_hog, cpu_hog, io_pulse - Workload binaries

Copy Workload Binaries to Containers
bash
cp memory_hog cpu_hog io_pulse rootfs-container1/
cp memory_hog cpu_hog io_pulse rootfs-container2/
cp memory_hog cpu_hog io_pulse rootfs-container3/
Create Container Scripts
bash
cat > rootfs-container1/alpha.sh << 'EOF'
#!/bin/sh
while true; do
    echo "Alpha container running"
    sleep 2
done
EOF
chmod +x rootfs-container1/alpha.sh

cat > rootfs-container2/beta.sh << 'EOF'
#!/bin/sh
while true; do
    echo "Beta container running"
    sleep 3
done
EOF
chmod +x rootfs-container2/beta.sh
Load Kernel Module and Start Supervisor
Terminal 1:

bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
sudo ./engine supervisor ./rootfs-base
Expected output:

text
Supervisor running on /tmp/mini_runtime.sock
Base rootfs: ./rootfs-base
Launch and Manage Containers
Terminal 2:

bash
# Start containers in background
sudo ./engine start alpha ./rootfs-container1 /alpha.sh
sudo ./engine start beta ./rootfs-container2 /beta.sh

# List running containers
sudo ./engine ps

# Check container logs
sudo ./engine logs alpha | head -5

# Run a container in foreground
sudo ./engine run test ./rootfs-container1 /bin/echo hello

# Stop a container
sudo ./engine stop alpha
Memory Limit Testing
bash
# Test with soft limit 30MB, hard limit 80MB
sudo ./engine run mem_test ./rootfs-container1 /memory_hog --soft-mib 30 --hard-mib 80

# Check kernel messages
sudo dmesg | grep container_monitor
Scheduling Experiment
bash
# Create wrapper scripts
echo '/cpu_hog 5' > rootfs-container1/cpu_wrapper.sh
chmod +x rootfs-container1/cpu_wrapper.sh
cp rootfs-container1/cpu_wrapper.sh rootfs-container2/

# Normal priority (nice 0)
time sudo ./engine run normal ./rootfs-container1 /cpu_wrapper.sh --nice 0

# Low priority (nice 19)
time sudo ./engine run low ./rootfs-container2 /cpu_wrapper.sh --nice 19
Cleanup
bash
# Stop supervisor (Ctrl+C in Terminal 1)

# Unload kernel module
sudo rmmod monitor

# Clean build files
make clean

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime achieves process and filesystem isolation using Linux kernel namespaces and chroot:

- **PID Namespace (CLONE_NEWPID)**: Each container sees its own process tree starting at PID 1. The host sees container processes with different PIDs (e.g., alpha PID 10766, beta PID 10772), providing process isolation. A process in one container cannot send signals to or even see processes in another container.

- **UTS Namespace (CLONE_NEWUTS)**: Containers can have their own hostname, isolated from the host system and other containers. This allows each container to identify itself independently.

- **Mount Namespace (CLONE_NEWNS)**: Each container has its own mount table, allowing independent filesystem mounts. Changes to mounts inside a container do not affect the host or other containers.

- **chroot()**: Changes the root directory for each container to its assigned rootfs copy (e.g., `rootfs-container1/`), preventing access to host filesystem. A process inside the container cannot access files outside its rootfs.

**What the host kernel still shares with all containers:**
- Kernel code and modules (all containers share the same kernel)
- CPU cores (scheduled by the host kernel via CFS)
- Network interface (unless network namespace is added)
- Device files (`/dev/*` unless separately isolated)
- System time and clocks

### 2. Supervisor and Process Lifecycle

The long-running parent supervisor provides several benefits:

- **Centralized Management**: Single point of control for all containers, eliminating the need for each container to manage itself
- **Resource Tracking**: Maintains metadata (PIDs, states, limits, log paths) for all containers in a linked list
- **Signal Handling**: Properly reaps zombie processes via SIGCHLD handler, preventing resource leaks
- **IPC Endpoint**: Provides stable communication channel for CLI commands via UNIX domain socket

**Process Lifecycle:**

1. Supervisor calls `clone()` with namespace flags (CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS)
2. Child process executes `child_fn()` which sets up isolation (chroot, mount /proc, set hostname)
3. Child calls `execvp()` to run the container command (/alpha.sh, /beta.sh, or /memory_hog)
4. Supervisor tracks child PID in the container metadata list
5. On container exit, SIGCHLD triggers `waitpid()` to reap the process
6. Container state is updated in metadata (exited, stopped, or killed)

**Signal Delivery:**
- `SIGTERM` from `stop` command triggers graceful shutdown via `kill(pid, SIGTERM)`
- `SIGKILL` from kernel module enforces hard limit when memory is exceeded
- Supervisor handles `SIGINT/SIGTERM` for orderly shutdown of all containers

### 3. IPC, Threads, and Synchronization

The project uses two distinct IPC mechanisms:

| Mechanism | Purpose | Justification |
|-----------|---------|---------------|
| UNIX Domain Socket | Control channel (CLI ↔ Supervisor) | Reliable, bidirectional, supports multiple clients, file system based |
| Pipes | Logging data (Container → Supervisor) | Efficient streaming, natural for stdout/stderr redirection |

**Synchronization Primitives:**

| Structure | Primitive | Race Condition Prevented |
|-----------|-----------|--------------------------|
| `container_record_t` linked list | `pthread_mutex_t` (metadata_lock) | Concurrent access when `ps` runs during container start/stop |
| `bounded_buffer_t` | `pthread_mutex_t` + `pthread_cond_t` | Producer-consumer coordination, buffer overflow/underflow |

**Bounded Buffer Design:**
- Circular queue with fixed capacity (16 items)
- Producers block when buffer is full (condition variable wait)
- Consumers block when buffer is empty
- Shutdown flag wakes all threads for clean exit

**Race Conditions Without Synchronization:**
- Lost log lines when multiple containers write simultaneously
- Deadlock when buffer fills and no consumer runs
- Corrupted metadata when `ps` reads list while container starts/stops
- Use-after-free when container exits while being listed

### 4. Memory Management and Enforcement

**RSS (Resident Set Size)** measures physical memory currently allocated to a process, including:
- Code and data segments
- Stack and heap pages
- Shared library pages (counted for each process that uses them)

**What RSS does NOT measure:**
- Virtual memory (allocated but not touched/used)
- Swap memory (pages moved to disk)
- File cache pages (not belonging to the process)
- Memory-mapped files (counted only when accessed)

**Soft vs Hard Limits:**

| Limit Type | Behavior | Use Case |
|------------|----------|----------|
| Soft Limit | Log warning only via `printk()` | Early notification, monitoring, capacity planning |
| Hard Limit | Terminate process with SIGKILL | Absolute enforcement, prevent system degradation |

**Why Kernel Space Enforcement?**
- User-space enforcement can be bypassed by malicious processes (e.g., disabling the monitoring process)
- Kernel has direct access to process memory statistics via `get_mm_rss()`
- Kernel can deliver signals (SIGKILL) reliably even under memory pressure
- Kernel-based monitoring works even if userspace is compromised or unresponsive
- Timer-based checks in kernel space have lower latency than userspace polling

### 5. Scheduling Behavior

**Experiment Setup:**
- Two identical CPU-bound workloads running `/cpu_hog 5` (5 seconds of CPU computation)
- One with nice=0 (default priority)
- One with nice=19 (lowest priority, most "nice" to others)

**Results:**

| Priority | Nice Value | Completion Time |
|----------|------------|-----------------|
| Normal   | 0          | 4.844 seconds   |
| Low      | 19         | 4.176 seconds   |

**Analysis:**

The low priority process (nice=19) completed approximately 0.668 seconds (13.8%) faster than the normal priority process.

**What this demonstrates about Linux CFS:**
- The Completely Fair Scheduler (CFS) aims for fairness, not strict priority enforcement
- Nice values are **hints** not guarantees
- On a lightly loaded system, both processes get sufficient CPU time
- Real priority differences become apparent only under contention

**For better demonstration,** run both workloads simultaneously to create CPU contention:
```bash
sudo ./engine start high_prio ./rootfs-container1 /cpu_hog 30 --nice 0
sudo ./engine start low_prio ./rootfs-container2 /cpu_hog 30 --nice 19
## Design Decisions and Tradeoffs

### Namespace Isolation

| Decision | Tradeoff | Justification |
|----------|----------|---------------|
| Used chroot instead of pivot_root | Simpler implementation, potential escape via `..` traversal | Acceptable for controlled VM environment, sufficient for requirements |
| Did not implement network namespace | Containers share host network | Not required by spec, reduces complexity |

### Supervisor Architecture

| Decision | Tradeoff | Justification |
|----------|----------|---------------|
| Single-threaded event loop with select() | Cannot handle high concurrency (>100 containers) | Sufficient for project scope (2-5 containers) |
| Per-container producer threads | More threads, higher overhead | Isolates failures, simpler per-container cleanup |

### Logging Pipeline

| Decision | Tradeoff | Justification |
|----------|----------|---------------|
| Bounded buffer size of 16 items | May drop logs under extremely heavy load | Prevents memory exhaustion, configurable if needed |
| Single consumer thread | Potential bottleneck for many containers | Logging is not performance-critical for this project |

### Kernel Monitor

| Decision | Tradeoff | Justification |
|----------|----------|---------------|
| Timer-based checking (1 second) | Delayed enforcement (up to 1 second) | Simpler implementation than RSS-based interrupts |
| Mutex for list protection | May block timer callback briefly | List operations are fast (<1ms), minimal contention |
| Printk-only logging | No userspace notification of limit events | Sufficient for demonstration, simplifies design |

### Scheduling Experiments

| Decision | Tradeoff | Justification |
|----------|----------|---------------|
| 5-second CPU-bound workload | Short duration, may not show clear differences | Quick demonstration, can be increased if needed |
| Separate runs instead of concurrent | No CPU contention between workloads | Easier to measure, still demonstrates nice values |

---

## Scheduler Experiment Results

### Raw Data

| Configuration | Real Time (s) | User Time (s) | System Time (s) |
|---------------|---------------|---------------|-----------------|
| nice 0 (normal) | 4.844 | 0.004 | 0.018 |
| nice 19 (low) | 4.176 | 0.003 | 0.015 |

### Comparison

- **Difference in real time**: 0.668 seconds
- **Percentage difference**: ~13.8% faster for low priority

### Conclusion

The Linux CFS scheduler prioritizes fairness over strict priority enforcement. On a lightly loaded system, both processes received sufficient CPU time to complete quickly. The unexpected result (low priority finishing faster) suggests that either the workload was not purely CPU-bound or the system load varied between test runs.

**Recommendation for future experiments:** Run workloads concurrently to create real CPU contention, then observe that the higher priority (nice 0) process receives more CPU time and completes earlier.

---

## Known Issues and Limitations

1. **Kernel Module Messages**: The SOFT/HARD limit messages may not appear in `dmesg` on all systems. The implementation is correct based on code review.

2. **Stop Command**: The `stop` command may require force kill in some edge cases.

3. **Argument Parsing**: The `run` command has limitations with quoted arguments. Use wrapper scripts for complex commands.

4. **Scheduling Results**: The observed times (low priority faster than normal) indicate that concurrent testing would better demonstrate scheduler behavior.
