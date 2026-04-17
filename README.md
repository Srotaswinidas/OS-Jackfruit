# Multi-Container Runtime Project

## Team Information

- **Srotaswini Das** - PES2UG24CS525
- **Sriya Dasari** - PES2UG24CS522

---
## Project Explanation

This project implements a **lightweight container runtime from scratch in C**, similar to the core concepts behind Docker. The system can run multiple isolated Linux containers simultaneously, monitor their memory usage, and manage their lifecycle.

### What This Project Does

1. **Process Isolation**: Each container runs in its own Linux namespaces (PID, UTS, Mount), making it think it has its own separate system.

2. **Resource Limits**: A kernel module monitors memory usage and enforces soft/hard limits - warning at soft limit and killing at hard limit.

3. **Lifecycle Management**: A supervisor daemon manages containers with commands to start, stop, list, and view logs.

4. **Log Collection**: All container output is captured through a bounded-buffer logging pipeline and written to persistent log files.

### Key Features Implemented

| Feature | Implementation |
|---------|----------------|
| Multi-container management | Supervisor tracks multiple containers with unique PIDs |
| Process isolation | clone() with CLONE_NEWPID, CLONE_NEWUTS, CLONE_NEWNS |
| Filesystem isolation | chroot() into per-container rootfs |
| Memory limits | Kernel module with RSS monitoring and SIGKILL enforcement |
| Logging | Bounded buffer with producer-consumer threads |
| IPC | UNIX domain socket for control, pipes for logging |
| Scheduling | nice() values to demonstrate Linux CFS behavior |

---

## Build, Load, and Run Instructions

### Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
Root Filesystem Setup
bash
# Download Alpine Linux rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
mkdir rootfs-base
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create writable copies for containers
cp -a rootfs-base rootfs-container1
cp -a rootfs-base rootfs-container2
cp -a rootfs-base rootfs-container3

# Copy workload binaries into containers
cp memory_hog cpu_hog io_pulse rootfs-container1/
cp memory_hog cpu_hog io_pulse rootfs-container2/
cp memory_hog cpu_hog io_pulse rootfs-container3/
Build the Project
bash
make clean
make all
This compiles:

engine - User-space runtime and supervisor

monitor.ko - Kernel memory monitor module

memory_hog, cpu_hog, io_pulse - Workload binaries

Load Kernel Module
bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
Start Supervisor
bash
sudo ./engine supervisor ./rootfs-base
Expected output:

text
Supervisor running on /tmp/mini_runtime.sock
Base rootfs: ./rootfs-base
Launch Containers
bash
# Start containers in background
sudo ./engine start alpha ./rootfs-container1 /alpha.sh
sudo ./engine start beta ./rootfs-container2 /beta.sh

# List running containers
sudo ./engine ps

# Run a container in foreground
sudo ./engine run test ./rootfs-container1 /bin/echo "Hello"

# Check container logs
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
Memory Limit Testing
bash
# Test soft limit (30MB soft, 100MB hard)
sudo ./engine run soft_test ./rootfs-container1 /memory_hog --soft-mib 30 --hard-mib 100

# Test hard limit (10MB soft, 50MB hard)
sudo ./engine run hard_test ./rootfs-container2 /memory_hog --soft-mib 10 --hard-mib 50

# Check kernel messages
sudo dmesg | grep container_monitor
Scheduling Experiment
bash
# Normal priority (nice 0)
time sudo ./engine run normal ./rootfs-container1 /cpu_hog 5 --nice 0

# Low priority (nice 19)
time sudo ./engine run low ./rootfs-container2 /cpu_hog 5 --nice 19
Cleanup
bash
# Stop supervisor (Ctrl+C in supervisor terminal)

# Unload kernel module
sudo rmmod monitor

# Clean build files
make clean
