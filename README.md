# Multi-Container Runtime Project

## Team Information

- Srotaswini Das-PES2UG24CS525
- Sriya Dasari-PES2UG24CS522

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
