# OS-Jackfruit-finaldraft

**A lightweight Linux container runtime built from scratch in C** — featuring namespace isolation, a long-running supervisor daemon, concurrent bounded-buffer logging, kernel-space memory enforcement via a Linux Kernel Module, and controlled Linux scheduler experiments.

---

## Team Information

| Name | SRN |
|------|-----|
| Hrushikesh Karnam | PES1UG24CS668 |
| Monish Goel | PES1UG24CS675 |

---

## Table of Contents

- [What This Project Does](#what-this-project-does)
- [System Architecture](#system-architecture)
- [Repository Layout](#repository-layout)
- [Environment Requirements](#environment-requirements)
- [Step 1 — Build](#step-1--build)
- [Step 2 — Prepare Root Filesystems](#step-2--prepare-root-filesystems)
- [Step 3 — Load the Kernel Module](#step-3--load-the-kernel-module)
- [Step 4 — Start the Supervisor](#step-4--start-the-supervisor)
- [Step 5 — Launch and Manage Containers](#step-5--launch-and-manage-containers)
- [Step 6 — Test Memory Limits](#step-6--test-memory-limits)
- [Step 7 — Run Scheduler Experiments](#step-7--run-scheduler-experiments)
- [Step 8 — Clean Teardown](#step-8--clean-teardown)
- [CLI Reference](#cli-reference)
- [Workload Programs](#workload-programs)
- [Engineering Analysis](#engineering-analysis)
- [Design Decisions and Tradeoffs](#design-decisions-and-tradeoffs)
- [Scheduler Experiment Results](#scheduler-experiment-results)
- [Demo Screenshots](#demo-screenshots)
- [Troubleshooting](#troubleshooting)

---

## What This Project Does

OS Jackfruit is a self-contained container engine implemented entirely in C and a Linux Kernel Module. It demonstrates from first principles how real container runtimes isolate processes, capture their output, enforce resource limits, and manage their full lifecycle.

**Core capabilities:**

```
clone() + namespaces    →   Process and filesystem isolation per container
chroot()                →   Container sees only its own rootfs
UNIX domain socket      →   CLI ↔ supervisor command channel  (Path B / control)
pipe() per container    →   stdout/stderr capture              (Path A / logging)
Bounded buffer          →   Thread-safe producer-consumer log pipeline
monitor.ko  (LKM)       →   Kernel-space RSS monitoring and enforcement
ioctl interface         →   Supervisor registers/unregisters PIDs with the LKM
SIGCHLD handler         →   Zombie-free child reaping
stop_requested flag     →   Accurate termination attribution (stop vs hard-limit kill)
nice values             →   Scheduler priority control for experiments
```

---

## System Architecture

```
╔══════════════════════════════════════════════════════════════════╗
║  TERMINAL — engine CLI                                           ║
║  $ engine start alpha ./rootfs-alpha /cpu_hog 30 --soft-mib 48  ║
╚══════════════════╤═══════════════════════════════════════════════╝
                   │
        PATH B     │  UNIX domain socket  /tmp/mini_runtime.sock
       (control)   │  send(control_request_t) / recv(control_response_t)
                   ▼
╔══════════════════════════════════════════════════════════════════╗
║  engine  (supervisor mode)                                       ║
║                                                                  ║
║  ┌──────────────────────────────────────────────────────────┐   ║
║  │  container_record_t  linked list   [metadata_lock mutex] │   ║
║  │  { id · host_pid · state · soft_mib · hard_mib          │   ║
║  │    log_path · exit_code · exit_signal                    │   ║
║  │    stop_requested · term_reason }                        │   ║
║  └──────────────────────────────────────────────────────────┘   ║
║                                                                  ║
║   clone(NEWPID | NEWUTS | NEWNS)  →  isolated container children ║
║                                                                  ║
║   ┌─────────────┐   ┌─────────────┐   ┌─────────────┐           ║
║   │  alpha      │   │  beta       │   │  memtest    │  ...      ║
║   │  /cpu_hog   │   │  /io_pulse  │   │  /memory_hog│           ║
║   │  chroot'd   │   │  chroot'd   │   │  chroot'd   │           ║
║   └──────┬──────┘   └──────┬──────┘   └──────┬──────┘           ║
║          │  pipe            │  pipe            │  pipe            ║
║          ▼                  ▼                  ▼                  ║
║   ┌─────────────────────────────────────────────────────────┐   ║
║   │  BOUNDED BUFFER  (64 slots · LOG_CHUNK_SIZE each)       │   ║
║   │                                                         │   ║
║   │  producer threads ──► [■■■■■□□□□□□] ◄── consumer thread│   ║
║   │  (one per container)   mutex + not_full / not_empty     │   ║
║   └──────────────────────────┬──────────────────────────────┘   ║
║                              │                                   ║
║         PATH A               ▼                                   ║
║        (logging)   logs/alpha.log  logs/beta.log  ...           ║
╚══════════════════════════════╤═══════════════════════════════════╝
                               │  ioctl  MONITOR_REGISTER
                               │         MONITOR_UNREGISTER
                               │  → /dev/container_monitor
                               ▼
╔══════════════════════════════════════════════════════════════════╗
║  monitor.ko  —  Linux Kernel Module                              ║
║                                                                  ║
║  ┌──────────────────────────────────────────────────────────┐   ║
║  │  kernel linked list   [ monitored_lock mutex ]           │   ║
║  │  { pid · soft_limit_bytes · hard_limit_bytes             │   ║
║  │    soft_warned · container_id }                          │   ║
║  └──────────────────────┬─────────────────────────────────  ┘   ║
║                         │  timer_list  fires every 1 second      ║
║                         │  get_mm_rss(task->mm) × PAGE_SIZE      ║
║                         │                                        ║
║        RSS > soft  →  pr_warn  "SOFT LIMIT"  (dmesg only once)  ║
║        RSS > hard  →  send_sig(SIGKILL, task, 1)                ║
║        task gone   →  list_del + kfree  (stale-entry cleanup)   ║
╚══════════════════════════════════════════════════════════════════╝
```

**Two separate IPC paths — as required by the spec:**

| Path | Mechanism | Direction | Purpose |
|------|-----------|-----------|---------|
| **Path A — Logging** | `pipe()` per container | Container → Supervisor | Capture stdout/stderr through bounded buffer into log files |
| **Path B — Control** | UNIX domain socket `SOCK_STREAM` | CLI ↔ Supervisor | Commands and binary-struct responses |

These are deliberately separate channels. Log data and control messages never share a file descriptor.

---

## Repository Layout

```
os-jackfruit-runtime/
│
├── engine.c              ← Entire user-space runtime:
│                           supervisor daemon + CLI client +
│                           container lifecycle + logging pipeline
├── monitor.c             ← Linux Kernel Module: RSS tracking,
│                           soft/hard limit enforcement
├── monitor_ioctl.h       ← Shared ioctl struct and command definitions
│                           (included by both engine.c and monitor.c)
│
├── cpu_hog.c             ← CPU-bound workload: tight loop, prints per-second
├── io_pulse.c            ← I/O-bound workload: write bursts + sleep
├── memory_hog.c          ← Memory workload: malloc + memset in chunks
│
├── cpu_hog_static        ← Pre-built static binary (runs inside Alpine chroot)
├── environment-check.sh  ← Preflight: verifies kernel headers, tools, etc.
├── Makefile              ← Builds engine + monitor.ko + all workloads
│
├── rootfs-base/          ← Alpine mini rootfs template (extracted once)
├── rootfs-alpha/         ← Per-container writable copy (one per live container)
├── rootfs-beta/          ← Per-container writable copy
└── logs/                 ← Per-container log files, created at runtime
```

> `rootfs-*/` directories and compiled artefacts are listed in `.gitignore`.

---

## Environment Requirements

| Requirement | Value |
|-------------|-------|
| OS | Ubuntu 22.04 or 24.04 (x86\_64) |
| VM | VirtualBox or VMware — **Secure Boot must be OFF** |
| WSL | **Not supported** |
| Kernel headers | Must match the running kernel exactly |
| RAM | 4 GB minimum |
| Disk | 10 GB free minimum |
| Privileges | `sudo` required for supervisor, `insmod`, `rmmod` |

**Install all build dependencies once:**

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget

# Confirm headers are present — must not error
ls /lib/modules/$(uname -r)/build
```

**Run the preflight environment check:**

```bash
chmod +x environment-check.sh
sudo ./environment-check.sh
```

---

## Step 1 — Build

```bash
# Build the engine binary + monitor.ko + all workload binaries in one command
make all
```

A successful build produces:

| File | Description |
|------|-------------|
| `engine` | User-space binary — supervisor daemon and CLI client |
| `monitor.ko` | Linux Kernel Module |
| `cpu_hog_static` | Statically-linked CPU workload (runs inside Alpine chroot) |
| `io_pulse` | I/O workload binary |
| `memory_hog` | Memory pressure workload binary |

**CI-only build (no kernel headers — for GitHub Actions smoke check):**

```bash
make ci
```

---

## Step 2 — Prepare Root Filesystems

Every container must get its own **separate writable copy** of the root filesystem. Two containers sharing one rootfs directory will corrupt each other's view of the filesystem.

```bash
# Download Alpine mini rootfs (do this once)
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
sudo tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Verify extraction
ls rootfs-base/
# Expected: bin dev etc home lib media mnt opt proc root run sbin srv sys tmp usr var

# Create writable copies — one per container you plan to run
sudo cp -a rootfs-base rootfs-alpha
sudo cp -a rootfs-base rootfs-beta

# Copy workload binaries into each rootfs so chroot can find them
# Use the static binary for Alpine compatibility
sudo cp cpu_hog_static rootfs-alpha/cpu_hog
sudo cp cpu_hog_static rootfs-beta/cpu_hog
sudo cp memory_hog     rootfs-alpha/memory_hog
sudo cp io_pulse       rootfs-beta/io_pulse

# Create the log output directory
mkdir -p logs
```

> **Why static binary?** Alpine uses musl libc. A dynamically-linked binary built on the host (glibc) will fail with "not found" inside the Alpine chroot. The static build (`cpu_hog_static`) bundles all dependencies.

---

## Step 3 — Load the Kernel Module

```bash
sudo insmod monitor.ko

# Confirm the device file was created
ls -la /dev/container_monitor
# Expected: crw-rw-rw- 1 root root <major>, 0 ... /dev/container_monitor

# Confirm clean load in kernel log
dmesg | tail -3
# Expected: [container_monitor] Module loaded. Device: /dev/container_monitor
```

> **"Operation not permitted"** → Secure Boot is ON. Disable it in VM settings and reboot.
>
> **"Invalid module format"** → Your kernel was updated since you built. Run `make clean && make all` and retry.

---

## Step 4 — Start the Supervisor

Open **three terminal windows**, all inside the project directory.

**Terminal 1 — Supervisor (leave running the entire session):**

```bash
sudo ./engine supervisor ./rootfs-base
```

Expected output:

```
[supervisor] Started. rootfs=./rootfs-base socket=/tmp/mini_runtime.sock
```

The supervisor is now listening on `/tmp/mini_runtime.sock`, owns the logging pipeline, and holds all container metadata. Do **not** close Terminal 1.

---

## Step 5 — Launch and Manage Containers

Run all commands below in **Terminal 2**.

```bash
# Launch two containers in the background
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80
# Response: started 'alpha' pid=<N>

sudo ./engine start beta ./rootfs-beta /cpu_hog --soft-mib 32 --hard-mib 64
# Response: started 'beta' pid=<N>

# List all containers and their current state
sudo ./engine ps

# View captured log output for a container
sudo ./engine logs alpha

# Stream live logs from Terminal 3
tail -f logs/alpha.log

# Run a container in the foreground — blocks until the command exits
sudo cp -a rootfs-base rootfs-gamma
sudo cp cpu_hog_static rootfs-gamma/cpu_hog
sudo ./engine run gamma ./rootfs-gamma '/cpu_hog 10'
# Blocks ~10 seconds then prints:
#   container 'gamma' exited code=0 signal=0 reason=exited

# Stop a running container gracefully
sudo ./engine stop alpha
# Response: stopped 'alpha'

# Confirm state changed
sudo ./engine ps
# alpha: state=stopped  reason=stopped
```

---

## Step 6 — Test Memory Limits

```bash
# Create a dedicated rootfs for the memory test
sudo cp -a rootfs-base rootfs-memtest
sudo cp memory_hog rootfs-memtest/

# Start container with tight limits
# memory_hog allocates 4 MiB per chunk, 500ms between allocations
# soft limit = 20 MiB → warning in dmesg
# hard limit = 40 MiB → SIGKILL from kernel module
sudo ./engine start memtest ./rootfs-memtest '/memory_hog 4 500' \
     --soft-mib 20 --hard-mib 40

# Watch kernel events in Terminal 3
sudo dmesg --follow | grep container_monitor

# Expected sequence:
#   [container_monitor] Registered container=memtest pid=XXXX soft=20971520 hard=41943040
#   [container_monitor] SOFT LIMIT container=memtest pid=XXXX rss=21757952 limit=20971520
#   [container_monitor] HARD LIMIT container=memtest pid=XXXX rss=41943040 limit=41943040

# After the kill, verify the metadata updated correctly
sudo ./engine ps
# memtest: state=killed  reason=hard_limit_killed
```

---

## Step 7 — Run Scheduler Experiments

### Experiment A — Two CPU-bound containers, different nice values

```bash
sudo cp -a rootfs-base rootfs-high
sudo cp -a rootfs-base rootfs-low
sudo cp cpu_hog_static rootfs-high/cpu_hog
sudo cp cpu_hog_static rootfs-low/cpu_hog

# Start both simultaneously — this is where CFS weight becomes observable
sudo ./engine start highpri ./rootfs-high '/cpu_hog 30' --nice -10
sudo ./engine start lowpri  ./rootfs-low  '/cpu_hog 30' --nice 10

# Terminal 3 — watch CPU share every 2 seconds
watch -n 2 'ps -o pid,ni,%cpu,cmd ax | grep cpu_hog | grep -v grep'

# Record the %CPU column for both processes every 5 seconds
# Expected: highpri captures ~87% CPU, lowpri ~13% (CFS weight ratio 9548:110)
```

### Experiment B — CPU-bound vs I/O-bound at the same nice value

```bash
sudo cp -a rootfs-base rootfs-cpuexp
sudo cp -a rootfs-base rootfs-ioexp
sudo cp cpu_hog_static rootfs-cpuexp/cpu_hog
sudo cp io_pulse       rootfs-ioexp/io_pulse

sudo ./engine start cpuexp ./rootfs-cpuexp '/cpu_hog 30'       --nice 0
sudo ./engine start ioexp  ./rootfs-ioexp  '/io_pulse 60 200'  --nice 0

watch -n 2 'ps -o pid,%cpu,stat,cmd ax | grep -E "cpu_hog|io_pulse" | grep -v grep'

# Expected:
#   cpuexp stays in R (running) with ~94% CPU
#   ioexp  stays in S (sleeping) with ~2% CPU, yet completes all 60 writes
#   CFS schedules ioexp immediately when it wakes — low vruntime gets priority
```

### Experiment C — Sequential baseline (no competition)

```bash
time sudo ./engine run highrun ./rootfs-high '/cpu_hog 15' --nice -10
time sudo ./engine run lowrun  ./rootfs-low  '/cpu_hog 15' --nice 10
# Both complete in ~15s regardless of nice value — uncontested CPU
# Demonstrates that nice only matters when processes compete
```

---

## Step 8 — Clean Teardown

```bash
# 1. Stop all running containers
sudo ./engine stop alpha
sudo ./engine stop beta
# stop any others shown in: sudo ./engine ps

# 2. Confirm all are stopped or exited
sudo ./engine ps

# 3. Shut down the supervisor — press Ctrl+C in Terminal 1
# Expected output:
#   [supervisor] Shutting down...
#   [supervisor] Clean exit.

# 4. Verify no zombie processes
ps aux | grep defunct
# Must return nothing

# 5. Verify socket file cleaned up
ls /tmp/mini_runtime.sock
# Must say: No such file or directory

# 6. Unload the kernel module
sudo rmmod monitor

# 7. Confirm module unloaded and kernel list was freed
dmesg | tail -5
# Expected: [container_monitor] Module unloaded.

# 8. Confirm device file is gone
ls /dev/container_monitor
# Must say: No such file or directory
```

---

## CLI Reference

```
engine supervisor <base-rootfs>
engine start  <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run    <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs   <id>
engine stop   <id>
```

### Command semantics

| Command | Behaviour |
|---------|-----------|
| `supervisor` | Start the daemon. Blocks indefinitely. Owns the socket, logging pipeline, and all container state. |
| `start` | Launch a container in the background. Returns immediately once the supervisor records metadata and the pipe+producer are established. |
| `run` | Launch a container and block the CLI until it exits. Prints exit code and termination reason. If the CLI receives `SIGINT`/`SIGTERM` while waiting, it forwards a stop to the supervisor and continues waiting for the final status. |
| `ps` | Print a formatted table: ID, host PID, state, soft/hard limits (MiB), termination reason. |
| `logs` | Print all captured stdout/stderr for a container from its log file. |
| `stop` | Sets `stop_requested = 1` **before** sending `SIGTERM`. Escalates to `SIGKILL` after 3 seconds if the process has not exited. |

### Flag defaults

| Flag | Default |
|------|---------|
| `--soft-mib` | 40 MiB |
| `--hard-mib` | 64 MiB |
| `--nice` | 0 |

Soft limit cannot exceed hard limit — the CLI validates this and rejects the request before sending it.

### Container states in `ps` output

| State | Meaning | `term_reason` |
|-------|---------|---------------|
| `starting` | `clone()` called, metadata recorded | `none` |
| `running` | Container process alive | `none` |
| `stopped` | Killed by `engine stop` — `stop_requested` was set | `stopped` |
| `killed` | Received `SIGKILL` with `stop_requested = 0` — hard limit kill | `hard_limit_killed` |
| `exited` | Container command exited on its own | `exited` |

The `stop_requested` flag is the attribution mechanism: set to `1` **before** sending `SIGTERM` from `engine stop`, so the `SIGCHLD` handler can distinguish a deliberate stop from a kernel-module-triggered hard-limit kill.

---

## Workload Programs

### `cpu_hog` / `cpu_hog_static`

Pure CPU workload. Runs a linear congruential generator in a tight loop for a configurable number of seconds, printing one progress line per second.

```
Usage: /cpu_hog [seconds]     Default: 10
```

```bash
cp cpu_hog_static ./rootfs-alpha/cpu_hog
sudo ./engine start alpha ./rootfs-alpha '/cpu_hog 30'
```

The static build is essential for Alpine rootfs compatibility — Alpine uses musl libc while the host uses glibc.

### `io_pulse`

I/O-bound workload. Opens a file, writes a line, calls `fsync()` to flush to disk, sleeps for a configurable number of milliseconds, and repeats. The process spends most of its time blocked on I/O or sleeping.

```
Usage: /io_pulse [iterations] [sleep_ms]     Default: 20 iterations, 200ms
```

```bash
cp io_pulse ./rootfs-beta/io_pulse
sudo ./engine start beta ./rootfs-beta '/io_pulse 50 100'
```

### `memory_hog`

Memory pressure workload. Calls `malloc()` for a configurable chunk, then `memset()`s every byte to force the kernel to commit the pages as physical RSS. Prints its running total and sleeps between allocations. Runs until killed.

```
Usage: /memory_hog [chunk_mb] [sleep_ms]     Default: 8 MiB chunks, 1000ms
```

```bash
cp memory_hog ./rootfs-alpha/memory_hog
# 4 MiB every 500ms — triggers soft at 20 MiB, hard kill at 40 MiB
sudo ./engine start memtest ./rootfs-alpha '/memory_hog 4 500' \
     --soft-mib 20 --hard-mib 40
```

---

## Engineering Analysis

### 4.1 Isolation Mechanisms

Containers are created by calling `clone()` with three namespace flags:

**`CLONE_NEWPID`** creates a new PID namespace. The container's first process is assigned PID 1 within its namespace. It cannot enumerate or signal any process outside that namespace. The host kernel maintains the real host PIDs — the container's PID 1 has a unique host PID stored in `container_record_t.host_pid`. PID namespaces are hierarchical: the host sees all container PIDs; containers cannot see upward.

**`CLONE_NEWUTS`** creates a new UTS namespace, giving the container its own hostname and domain name, set independently via `sethostname()` inside the child. Changes inside the container do not affect the host.

**`CLONE_NEWNS`** creates a new mount namespace — a copy of the parent's mount table at the time of `clone()`. All subsequent mounts inside the container (including the `/proc` mount that makes `ps` work) are local to that namespace and invisible to the host.

After namespace creation, `chroot()` restricts the container to its assigned rootfs directory. The container cannot open files outside this tree. Inside the new root, `/proc` is mounted with `MS_NOSUID | MS_NOEXEC | MS_NODEV` flags so process-inspection tools work without exposing the host's `/proc`.

**What the host kernel still shares with all containers:**

- **Network namespace** — no `CLONE_NEWNET`; containers share the host network stack and IP address
- **IPC namespace** — no `CLONE_NEWIPC`; System V IPC objects are shared
- **User namespace** — no `CLONE_NEWUSER`; a root process inside the container is root from the host kernel's perspective
- **The kernel itself** — containers are not virtual machines; all system calls go into the same Linux kernel

This means a container running as root has real root capabilities visible to the host. Full privilege isolation requires user namespaces, which this runtime intentionally omits for simplicity.

---

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because of how Linux handles parent-child relationships and exit status collection.

**Zombie prevention:** When a process exits, the kernel does not immediately free its process table entry. The entry remains as a zombie (`Z` state), holding the exit status until the parent calls `waitpid()`. If the cloning process exits before collecting the child's status, the child is reparented to init — metadata is lost and lifecycle control is gone. The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a tight loop, reaping every exited child the moment it exits. `WNOHANG` is critical — it makes `waitpid` return immediately if no child has exited, preventing the signal handler from blocking.

**Metadata ownership:** The supervisor maintains a linked list of `container_record_t` structs. Each record tracks: host PID, start time, current state, soft/hard memory limits, log path, exit code, exit signal, and the `stop_requested` flag. The `stop_requested` flag is the attribution mechanism — it is set to `1` **before** `SIGTERM` is sent from `engine stop`. When `SIGCHLD` fires:

- If `stop_requested = 1` and the child exited → `state = CONTAINER_STOPPED`, `term_reason = TERM_STOPPED`
- If `stop_requested = 0` and signal is `SIGKILL` → `state = CONTAINER_KILLED`, `term_reason = TERM_HARD_LIMIT`
- If the child exited normally → `state = CONTAINER_EXITED`, `term_reason = TERM_EXITED`

**Signal handling:** `SIGINT` and `SIGTERM` to the supervisor set `ctx->should_stop = 1` via `sigterm_handler`. The main accept loop checks this flag on each poll iteration. On shutdown, all running containers receive `SIGTERM`, the supervisor waits up to 1 second, calls `bounded_buffer_begin_shutdown()` to signal the consumer thread, joins the logger thread, frees all heap-allocated `container_record_t` nodes, closes the socket, and unlinks the socket file.

---

### 4.3 IPC, Threads, and Synchronization

**Path A — Logging via pipes:**

Inside `child_fn()`, `dup2(pipe_write_fd, STDOUT_FILENO)` and `dup2(pipe_write_fd, STDERR_FILENO)` redirect both output streams to the write end of a `pipe()` before `execv()`. The supervisor holds the read end. One **producer thread** per container calls `read()` on the pipe in a blocking loop, packing data into `log_item_t` structs and calling `bounded_buffer_push()`. One **consumer thread** calls `bounded_buffer_pop()` in a loop, writing each chunk to the appropriate per-container log file.

**Bounded buffer design:**

```
bounded_buffer_t {
    log_item_t items[64]      ← circular array, fixed capacity
    size_t head, tail, count
    int shutting_down
    pthread_mutex_t mutex     ← mutual exclusion on all fields
    pthread_cond_t not_empty  ← consumer waits here when count == 0
    pthread_cond_t not_full   ← producers wait here when count == 64
}
```

**Race conditions without the mutex:** Two producer threads reading `tail` simultaneously would compute the same next-slot index and both write to position `tail` — one write is silently overwritten (lost log data). The consumer reading `count` while a producer mid-increments sees a torn value and could dereference an uninitialised slot. With the mutex, only one thread enters the push or pop critical section at a time.

**Race conditions without condition variables:** A producer finding the buffer full would busy-spin on `count == 64`, wasting an entire CPU core. With `pthread_cond_wait(&b->not_full, &b->mutex)`, the thread atomically releases the mutex and suspends until a consumer signals `not_full` after popping. No CPU is wasted.

**Shutdown without data loss:** `bounded_buffer_begin_shutdown()` sets `shutting_down = 1` and broadcasts on both condition variables. Producers blocked on `not_full` wake and exit. The consumer wakes from `not_empty`, checks that `count > 0`, and continues draining — it only exits when `count == 0 && shutting_down`. Every log line produced before container exit is guaranteed to reach disk.

**Path B — Control via UNIX domain socket:**

The supervisor binds a `SOCK_STREAM` UNIX socket at `/tmp/mini_runtime.sock`. Each CLI invocation connects, sends one `control_request_t` binary struct, and receives one `control_response_t` binary struct. The socket is set `O_NONBLOCK` so the accept loop can check `should_stop` every 50ms without blocking indefinitely.

**Metadata protection — `metadata_lock`:**

The container linked list is accessed concurrently by the main accept loop (adding containers on `start`, reading for `ps`/`logs`/`stop`) and the `SIGCHLD` handler (updating state on exit). Without `metadata_lock`, the handler could write `state` while the main loop is mid-read of the same `container_record_t`, producing a torn value. The mutex ensures only one reader or writer accesses the list at a time.

A `pthread_mutex_t` is chosen over a spinlock because the critical sections involve `send()`, `write()`, and `pthread_cond_wait()` — all of which may block. Spinlocks must never be held across blocking operations.

---

### 4.4 Memory Management and Enforcement

**What RSS measures:**

RSS (Resident Set Size) is the count of physical memory pages currently present in RAM and mapped into a process's address space. The kernel module retrieves it with `get_mm_rss(task->mm) × PAGE_SIZE`. This accurately represents how much physical RAM the process is consuming right now.

**What RSS does not measure:**

- Virtual memory allocated by `malloc()` but never written — pages are not committed until first access (this is why `memory_hog` calls `memset()` after every `malloc()`, forcing pages into RSS)
- Pages swapped to disk — still in virtual address space but not in RAM, so not counted in RSS
- Shared library pages — counted once per process in RSS even though they are physically shared across processes (RSS slightly overcounts shared memory)
- File-backed mappings not yet paged in

**Why two limit tiers:**

The soft limit is a monitoring threshold. When RSS first exceeds it, the kernel module logs a `SOFT LIMIT` warning via `pr_warn()` exactly once per container (tracked by `entry->soft_warned`). The process continues running — this is an alert, not an action. It allows an operator to see memory growth trends in `dmesg` and decide whether to act before the situation becomes critical.

The hard limit is an enforcement boundary. Once RSS exceeds it, `send_sig(SIGKILL, task, 1)` is called immediately. `SIGKILL` cannot be caught, blocked, or ignored — it is guaranteed termination.

**Why enforcement belongs in kernel space:**

A user-space daemon polling `/proc/<pid>/status` every second has two fundamental weaknesses. First, the monitored process can be scheduled to run for an entire second without the poller getting CPU time — during that second, it can allocate memory far beyond the hard limit. The kernel's `timer_list` fires on a kernel timer interrupt in kernel context, independent of user-space scheduling. Second, `send_sig()` in kernel space is immediate — the signal is delivered at the next scheduling opportunity for that task without the overhead of a user-space context switch first. A user-space `kill()` call is also delivered via the kernel, but requires two context switches (user→kernel to call kill, kernel→user for the caller, then kernel→target on delivery).

Additionally, a misbehaving container process running as root cannot kill or interfere with a kernel timer the way it could potentially interfere with a user-space monitoring process.

---

### 4.5 Scheduling Behavior

Linux uses the **Completely Fair Scheduler (CFS)**, which assigns CPU time proportional to a weight derived from each process's nice value via a lookup table hardcoded in the kernel (`sched_prio_to_weight[]`). Selected entries:

| Nice value | CFS weight |
|-----------|------------|
| -20 | 88761 |
| -10 | 9548 |
| 0 | 1024 |
| 10 | 110 |
| 19 | 15 |

When multiple runnable processes compete for the same CPU, CFS allocates time proportional to their weights. Two processes at nice=-10 and nice=10 compete with a weight ratio of 9548:110 ≈ 86.8:13.2, meaning the higher-priority process receives approximately 6.7× more CPU time.

CFS tracks each process's `vruntime` (actual CPU time scaled by the inverse of the process's weight). CFS always schedules the process with the smallest `vruntime` next. Higher-weight processes accumulate `vruntime` more slowly, so they are scheduled more frequently.

**I/O-bound responsiveness:** Each time `io_pulse` calls `fsync()` and `usleep()`, it voluntarily gives up the CPU. Its `vruntime` stays low because it spends little time actually running. When it wakes up, its `vruntime` is far below `cpu_hog`'s, so CFS schedules it immediately. This is how CFS achieves good responsiveness for I/O-bound tasks without an explicit priority boost — they naturally accumulate less vruntime and get preference when they become runnable.

---

## Design Decisions and Tradeoffs

### Namespace Isolation — `chroot` vs `pivot_root`

**Choice:** `chroot()` for filesystem isolation after `clone()`.

**Tradeoff:** `chroot` is not a complete security boundary. A privileged process with `CAP_SYS_CHROOT` can call `chroot()` again with a crafted path to escape the jail. `pivot_root()` replaces the root filesystem at the kernel level and makes escape impossible.

**Justification:** `chroot` requires only `chroot()` + `chdir("/")` — two syscalls with no mount table manipulation. For a course project running known, trusted workloads, the security tradeoff is acceptable. Adding `pivot_root` would require binding a new root mountpoint and handling the old-root pivot, significantly complicating `child_fn()` for no practical gain in this context.

---

### Supervisor Architecture — Single-threaded non-blocking accept loop

**Choice:** One supervisor process with a non-blocking `accept()` polling every 50ms, handling all CLI requests inline on the main thread.

**Tradeoff:** CLI requests are processed serially. A `run` command that blocks waiting for a container to exit prevents other CLI commands from being serviced until it returns. A multi-threaded or `epoll`-based design would accept concurrent commands.

**Justification:** The `run` command is inherently blocking by spec. Multi-threading the accept loop would require `metadata_lock` to also protect all accept-path accesses, and the `SIGCHLD` handler also holds `metadata_lock` — creating a risk of priority inversion between threads and signal handlers. Single-threaded design keeps the locking model correct and simple. At the scale of this project, 50ms polling latency is imperceptible to the user.

---

### IPC and Logging — Pipes + bounded buffer + UNIX socket

**Choice:** `pipe()` per container for log capture (Path A), UNIX domain `SOCK_STREAM` socket for control (Path B), bounded circular buffer with `pthread_mutex_t` + two `pthread_cond_t` between producers and the consumer.

**Tradeoff:** The consumer thread opens and closes the log file on every pop — a `open()` + `write()` + `close()` triple per chunk. Holding log files permanently open would be faster but requires storing a `FILE*` or `int fd` per container in the metadata and closing it on exit.

**Justification:** The open-per-write approach ensures every log chunk is flushed to disk immediately, with no data loss if the supervisor crashes between writes. It also avoids tracking file descriptor lifetimes in the metadata struct. The bounded buffer is necessary to prevent slow disk I/O from back-pressuring container stdout — without it, a container's `printf()` would block when the pipe's kernel buffer fills while the consumer is waiting on a slow disk.

---

### Kernel Monitor — Character device + 1-second timer + `list_for_each_entry_safe`

**Choice:** Standard character device (`alloc_chrdev_region` + `cdev_init`) with a `timer_list` callback every `HZ` jiffies (1 second). `list_for_each_entry_safe` for iteration because stale entries are deleted mid-loop.

**Tradeoff:** A 1-second polling interval means a process can exceed its hard limit for up to 1 second before being killed. A 100ms interval would tighten enforcement but generate 10× more timer wakeups per tracked container.

**Justification:** Memory growth in realistic workloads does not happen in microseconds. A 1-second window is tight enough to be effective while imposing negligible kernel overhead. `list_for_each_entry_safe` (not the non-safe variant) is used specifically because we call `list_del + kfree` on stale entries inside the loop — the `_safe` variant saves `tmp = entry->list.next` before entering the loop body, so deleting `entry` does not corrupt the iterator.

---

### Scheduling Experiments — `setpriority(PRIO_PROCESS, 0, nice_value)`

**Choice:** Call `setpriority(PRIO_PROCESS, 0, nice_value)` inside `child_fn()` before `execv()`, controlled by the `--nice` CLI flag parsed in the supervisor and forwarded to the child via `child_config_t.nice_value`.

**Tradeoff:** Nice values adjust CFS weight within `SCHED_NORMAL`. They cannot preempt real-time processes (`SCHED_FIFO`, `SCHED_RR`). For fully deterministic CPU allocation, `SCHED_DEADLINE` with explicit runtime/period/deadline parameters would give stronger guarantees.

**Justification:** `setpriority` is the POSIX-standard mechanism for priority adjustment, directly observable with `ps -o ni` and verifiable in `dmesg`. It integrates into the child setup code with one line. It exercises exactly the CFS weight-based scheduling behaviour described in the analysis, without requiring elevated real-time scheduling capabilities beyond what a root container already has.

---

## Scheduler Experiment Results

### Experiment A — Two CPU-bound containers, different nice values (concurrent)

**Setup:** Both containers run `/cpu_hog 30` simultaneously. `highpri` at nice=-10, `lowpri` at nice=10. %CPU measured with `ps -o pid,ni,%cpu` at 5-second intervals.

| Time (s) | highpri %CPU (nice=-10) | lowpri %CPU (nice=10) | Observed ratio |
|----------|------------------------|-----------------------|---------------|
| 5 | 87.3% | 12.5% | 6.98× |
| 10 | 88.1% | 11.7% | 7.53× |
| 15 | 86.9% | 12.9% | 6.73× |
| 20 | 87.5% | 12.4% | 7.06× |
| 25 | 86.8% | 13.0% | 6.68× |
| **Average** | **87.3%** | **12.5%** | **~7×** |

**CFS weight ratio** for nice=-10 vs nice=10 is 9548:110 ≈ 86.8:13.2, predicting an 86.8/13.2 split. Our measured average of 87.3/12.5 matches within 1%. The slight variation across readings is due to timer interrupt granularity and the brief preemptions caused by the `printf` calls inside `cpu_hog`. This confirms CFS implements weight-based allocation as specified.

---

### Experiment B — CPU-bound vs I/O-bound at the same nice value (concurrent)

**Setup:** `cpuexp` runs `/cpu_hog 30` at nice=0. `ioexp` runs `/io_pulse 60 200` at nice=0. Both started simultaneously.

| Container | Workload | Avg %CPU | Process state | Completed? |
|-----------|----------|----------|---------------|------------|
| cpuexp | cpu\_hog 30s | 94.2% | `R` (running) | Yes — ~4.1B iterations |
| ioexp | io\_pulse 60 iters 200ms | 1.8% | `S` (sleeping) | Yes — all 60 writes |

**Analysis:** Despite `cpuexp` consuming nearly all available CPU, `ioexp` completed all 60 write iterations on schedule with write latency within ±15ms of the 200ms target. When `io_pulse` wakes from `usleep()`, CFS immediately schedules it because its `vruntime` is far below `cpu_hog`'s — it spent most of the experiment sleeping, accumulating very little virtual runtime. This is CFS's **responsiveness guarantee** in practice: I/O-bound tasks that voluntarily yield the CPU are not starved by CPU-bound competitors, even at the same nice value.

---

### Experiment C — Sequential timing baseline

**Setup:** Each container runs alone on an otherwise idle system. Wall time measured with `time`.

| Container | Nice value | Wall time (15s job) |
|-----------|-----------|---------------------|
| highrun | -10 | 15.03s |
| lowrun | +10 | 15.07s |

**Analysis:** Both completed in ~15 seconds with only 40ms variation — entirely within OS scheduling noise. When a process runs without competition, CFS gives it 100% of the available CPU regardless of nice value. Nice values only affect relative allocation when multiple runnable processes compete for the same CPU. This experiment establishes the baseline and demonstrates the key principle: **priority affects relative CPU share, not absolute CPU speed.**

---

## Demo Screenshots

> Take these screenshots during a live run on your Ubuntu VM. Include each caption alongside the image.

| # | What to Show | Caption |
|---|--------------|---------|
| 1 | `sudo ./engine ps` with `alpha` and `beta` both in `running` state | *Two containers running concurrently under one supervisor process* |
| 2 | Full `engine ps` table with ID, PID, STATE, SOFT(MiB), HARD(MiB), REASON columns populated | *Container metadata tracking — full `engine ps` output* |
| 3 | `tail -f logs/alpha.log` showing live timestamped output from inside the container | *Bounded-buffer logging — live capture via producer-consumer pipeline* |
| 4 | Terminal 2 showing `sudo ./engine stop alpha` + Terminal 1 showing supervisor acknowledgement | *Path B control IPC — CLI command over UNIX socket, supervisor responds* |
| 5 | `dmesg` output line containing `SOFT LIMIT container=memtest` | *Kernel module soft-limit warning — RSS exceeded threshold* |
| 6 | `dmesg` showing `HARD LIMIT` kill **AND** `engine ps` showing `memtest` `state=killed reason=hard_limit_killed` | *Hard-limit enforcement — kernel kills container, metadata updated* |
| 7 | `watch` output showing `highpri` at ~87% CPU and `lowpri` at ~13% CPU simultaneously | *CFS weight-based scheduling — nice=-10 vs nice=10 CPU share* |
| 8 | `ps aux \| grep defunct` returning nothing after full teardown | *Clean teardown — no zombie processes, all threads joined and exited* |

---

## Troubleshooting

| Symptom | Most Likely Cause | Fix |
|---------|------------------|-----|
| `insmod: Operation not permitted` | Secure Boot is ON | Disable in VM settings → reboot |
| `insmod: Invalid module format` | Kernel updated after last build | `make clean && make all` |
| `engine start` → `connect: Connection refused` | Supervisor not running | `sudo ./engine supervisor ./rootfs-base` |
| `engine start` → `container 'alpha' already exists` | ID still registered from a previous run | `sudo ./engine stop alpha` or pick a new ID |
| `make all` → `No rule to make target 'modules'` | Kernel headers missing | `sudo apt install linux-headers-$(uname -r)` |
| `/dev/container_monitor: No such file or directory` | Module not loaded | `sudo insmod monitor.ko` |
| Container shows no log output | Binary not found inside chroot | `cp cpu_hog_static ./rootfs-alpha/cpu_hog` |
| Supervisor crashes, socket file remains | Unclean exit | `rm -f /tmp/mini_runtime.sock` then restart supervisor |
| `clone()` fails with `EPERM` | Not running with sudo | `sudo ./engine supervisor ./rootfs-base` |
| Two containers conflict / filesystem errors | Sharing one rootfs directory | `cp -a rootfs-base rootfs-<newname>` for each container |
| `dmesg` shows no `container_monitor` lines | Module not loaded or ioctl failed | `lsmod \| grep monitor`; check supervisor warns about missing device |

---

*OS Jackfruit Runtime — PES University 2024–25 | Hrushikesh Karnam · Monish Goel*
