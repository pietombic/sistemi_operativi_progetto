# MultiPandOS Project

### 1. Compilation

To compile the project and generate the necessary build files, run the following CMake commands from the project root directory:

```bash
cmake -B build
cmake --build build
```

### 2. Execution

This project requires the uRISCV emulator. Please ensure it is installed on your system. ([uRISCV repository](https://github.com/virtualsquare/uriscv/))

Once the config_machine.json is ready, follow these steps to launch the PandOS shell:

1. **Launch the Emulator**: Execute the uRISCV emulator from your terminal:

```bash
uriscv
```

2. **Load Configuration**: Open the generated config_machine.json file within the emulator interface.
3. **Start Execution**: Press the "Play" button (or the appropriate start command within the emulator GUI) to turn on the machine and begin execution.
4. **Open Terminal**: Navigate to the "Window" menu and select "Terminal 0". The PandOS shell prompt should now be visible.

## Phase 1: Queue Management Implementation

We successfully completed the first phase of the project by implementing the second layer, the Queue Manager.

This involved developing core functionalities for:

- **PCB (Process Control Block) Management**: Handling the allocation and deallocation of PCBs.
- **PCB Queue Management**: Managing the queues of processes.
- **PCT (Process Control Table) Tree Management**: Implementing the control structure for processes.

The underlying list structures were designed and built following the robust Double Linked List model used within the Linux Kernel.

This phase was a highly rewarding and significant learning experience, as it marked the first time the entire team worked with and implemented this specific Kernel-style list model.


## Phase 2: Kernel

Phase 2 implements the OS kernel: exception handler, scheduler, interrupt handler, and the ten system calls defined by the specification.

### Folder structure

The implementation is organized into several modules, each handling a specific part of the kernel:

- **`main.c`**: The entry point of the kernel that invokes the initialization routine.
- **`initial.c`**: Handles the kernel initialization, setting up the global state (ready queue, semaphores, process count), initializing the PCB and ASL layers from Phase 1, and creating the first process.
- **`scheduler.c`**: Implements the priority-based round-robin scheduler. It selects the next process to run or manages the idle state (HALT, WAIT, or PANIC).
- **`exception.c`**: Contains the unified exception handler that routes interrupts, system calls, and memory faults to their respective handlers.
- **`interrupt.c`**: Manages all device interrupts (terminals, disks, etc.) and the pseudo-clock timer, performing the necessary V operations on device semaphores.
- **`functions.c`**: Provides utility functions for process management, including recursive process termination (for `TERMPROCESS`), PID-based process lookup via tree traversal, and manual state copying.
- **`headers/`**: Contains the internal headers for the Phase 2 modules.

### Scheduler

The scheduler implements a **priority-based round-robin** policy. On each invocation it dequeues the highest-priority process from `readyQueue`, arms the time slice via `setTIMER(TIMESLICE)`, and dispatches it with `LDST`.

When the ready queue is empty the scheduler distinguishes three cases:

- **No living processes** (`processCount == 0`): the kernel executes `HALT`.
- **Soft-blocked processes** (`softBlockCount > 0`): the kernel enters `WAIT` with interrupts enabled, waiting for a device or the pseudo-clock to unblock a process.
- **Deadlock** (living processes but none soft-blocked or ready): the kernel executes `PANIC`.

CPU time is tracked by snapshotting the clock (`STCK`) at the start of each quantum and accumulating the delta into `p_time` on every preemption or block.

### Exception handler

The unified entry point `exceptionHandler` reads the saved state from `BIOSDATAPAGE` and routes the exception based on the `cause` field:

| Type                | Condition               | Action                                                                                |
| ------------------- | ----------------------- | ------------------------------------------------------------------------------------- |
| Interrupt           | `CAUSE_IS_INT`          | `interruptHandler()`                                                                  |
| System call         | excCode 8 or 11 (ecall) | advance PC by one word, `systemCallHandler()`                                         |
| Memory access fault | excCode 1, 5 or 7       | `passUpOrDie(PGFAULTEXCEPT)` if in user space, otherwise `passUpOrDie(GENERALEXCEPT)` |
| Other               | any other excCode       | `passUpOrDie(GENERALEXCEPT)`                                                          |

`passUpOrDie` terminates the current process if it has no `p_supportStruct`, otherwise it delegates to the support level (phase 3) by loading the exception context stored in that structure.

### Interrupt handler

The `interruptHandler` manages all hardware and timer interrupts. It first updates the CPU time of the `currentProcess` using `STCK`.

- **PLT (Processor Local Timer)**: When the time slice expires, the current process's state is saved, its time slice is reset, and it is re-inserted into the `readyQueue`. The scheduler is then called to pick the next process.
- **Interval Timer (Pseudo-clock)**: Every 100ms, all processes blocked on the pseudo-clock semaphore (`deviceSemaphores[48]`) are unblocked and moved to the `readyQueue`.
- **Device Interrupts**: For each device line (disks, flash, network, etc.), the handler identifies the interrupting device, acknowledges the interrupt (sending `ACK`), and unblocks the process waiting on the corresponding device semaphore, returning the device's status in `reg_a0`.
- **Terminals**: Handled separately as they provide independent transmission (TX) and reception (RX) sub-devices, each with its own semaphore.

### System calls

The ten system calls are dispatched by `systemCallHandler`, which first checks that negative service codes (kernel-level calls) are invoked from M-mode (kernel mode), then switches on `reg_a0`:

| Code | Name            | Description                                                                                                                                                                 |
| ---- | --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| -1   | `CREATEPROCESS` | Allocates a new PCB, initializes it with the state/priority/support pointer passed by the caller, and inserts it as a child of the current process in the process tree.     |
| -2   | `TERMPROCESS`   | Terminates the process identified by the PID in `reg_a1` (0 = self) and its entire progeny via a recursive tree traversal.                                                  |
| -3   | `PASSEREN`      | P operation on a semaphore: decrements the counter and blocks the process if it goes negative.                                                                              |
| -4   | `VERHOGEN`      | V operation on a semaphore: increments the counter and unblocks the first waiting process if any.                                                                           |
| -5   | `DOIO`          | Writes a command to a device register and blocks the process on the corresponding device semaphore waiting for the interrupt. Terminals have separate TX and RX semaphores. |
| -6   | `GETTIME`       | Returns the accumulated CPU time of the current process.                                                                                                                    |
| -7   | `CLOCKWAIT`     | Blocks the process on the pseudo-clock semaphore until the next 100 ms timer tick.                                                                                          |
| -8   | `GETSUPPORTPTR` | Returns the `support_t` pointer of the current process, or NULL if none was set at creation time.                                                                           |
| -9   | `GETPROCESSID`  | Returns the PID of the current process (`reg_a1 == 0`) or of its parent (`reg_a1 != 0`), 0 if no parent exists.                                                             |
| -10  | `YIELD`         | Voluntarily relinquishes the CPU: the process is re-inserted into the ready queue and the scheduler picks the next one.                                                     |

### Notable implementation choices

- **PID lookup via process tree**: finding a process by PID is done with a DFS over the process tree rooted at `rootProcess`. Every live process is always reachable because `CREATEPROCESS` always inserts the new process as a child of the caller.
- **Unified soft-block semaphore check**: `isSoftBlockSemaphore` identifies all semaphores that contribute to `softBlockCount` (devices 0–47 and pseudo-clock 48) with a single range check on `deviceSemaphores`, removing the need to treat the pseudo-clock as a special case.
- **Manual `copyState`**: the bare-metal toolchain (`-ffreestanding -nostdlib`) does not provide `memcpy`. Direct struct assignment of large types like `state_t` causes the compiler to emit a `memcpy` call, so the copy is done with a manual word-by-word loop instead.

---

## Phase 3: Support Level

Phase 3 implements the Support Level: the layer between the kernel and user processes (U-proc). It provides virtual memory via demand paging, terminal I/O, and process management for user-level programs.

### Folder structure

- **`initProc.c`**: Instantiator process (`test()`). Initializes shared data structures, launches the shell (ASID 1), and waits for it to terminate.
- **`vmSupport.c`**: Pager — TLB exception handler that implements demand paging using a swap pool backed by flash devices.
- **`sysSupport.c`**: General exception handler that dispatches positive syscalls (SYS2–SYS6) and handles program traps from U-procs.
- **`headers/`**: Internal headers for the Phase 3 modules.

### Instantiator process

`test()` is the entry point of the Support Level. It runs as a kernel-mode process and performs the following startup sequence:

1. **`initSwapStructs()`**: initializes the 16-frame swap pool and its mutex semaphore.
2. Initializes all mutual-exclusion semaphores to 1 (free): one per flash device (`flashMutex[]`), one for terminal TX, one for terminal RX.
3. Zeroes the synchronization semaphores: `masterSemaphore` (test waits on this for the shell) and `shellSemaphore` (shell waits on this for each child program).
4. Launches the shell as a U-proc with ASID 1 via `launchUproc(1)`.
5. Blocks on `masterSemaphore` until the shell signals it on termination.
6. Terminates via `TERMPROCESS`.

Each U-proc is given a `support_t` containing its private 32-entry page table and the two exception handler contexts (TLB and general). Both handlers run in kernel-mode with interrupts enabled.

### Demand paging (Pager)

The pager implements on-demand loading of pages from flash into a fixed pool of 16 physical frames (the swap pool), located at `SWAPPOOLSTART = 0x20020000`.

Each U-proc has a virtual address space of 32 pages (128 KB):
- Pages 0–30: text and data, VPN `0x80000 + i` (KUSEG)
- Page 31: stack, VPN `0xBFFFF` (top of KUSEG)

All pages start as invalid (V=0, D=1). On the first access the hardware generates a TLB exception, which the kernel forwards to `pager()` via `passUpOrDie`.

**Page fault handling algorithm:**

| Step | Action |
| ---- | ------ |
| 1 | Read the faulting VPN from `sup_exceptState[PGFAULTEXCEPT].entry_hi` |
| 2 | Acquire `swapPoolSem` (mutex protecting the entire swap pool) |
| 3 | Select victim frame with **FIFO round-robin** (`swapFrameClock`) |
| 4 | If occupied: invalidate its PTE (V=0), flush from TLB atomically, write frame to flash (eviction) |
| 5 | Read the requested page from flash into the frame |
| 6 | Update swap pool metadata (ASID, page number, PTE pointer) |
| 7 | Update PTE: set V=1, D=1, PFN = physical address of frame |
| 8 | Update TLB entry in place (TLBP + TLBWI) if already cached |
| 9 | Release `swapPoolSem` |
| 10 | `LDST` back to the faulting instruction (now succeeds) |

TLB-Modification faults (write to a read-only page) are treated as program errors and trigger process termination.

**Flash I/O** is performed via `DOIO` (SYS5 of the kernel), with each device protected by its own `flashMutex`. Flash device *i* stores the backing image for the U-proc with ASID *i+1*. The block index in the flash corresponds directly to the logical page number.

### System calls

User processes invoke syscalls with a positive number in `reg_a0` via `ECALL`. The general exception handler (`supportGeneralExHandler`) checks the exception code and dispatches to the correct handler:

| Code | Name | Description |
| ---- | ---- | ----------- |
| 2 | `TERMINATE` | U-proc terminates normally. Signals `masterSemaphore` (ASID 1) or `shellSemaphore` (ASID 2–8), then calls `TERMPROCESS`. |
| 4 | `WRITETERMINAL` | Writes a string of up to 128 chars to terminal 0. `reg_a1` = virtual address of string, `reg_a2` = length. Returns chars written or negative error code. Protected by `termMutexTX`. |
| 5 | `READTERMINAL` | Reads a line from terminal 0 into a buffer at `reg_a1`. Stops on newline or 128 chars. Returns chars read or negative error code. Protected by `termMutexRX`. |
| 6 | `EXECUTE` | Shell-only (ASID 1). Initializes a new U-proc with the ASID in `reg_a1` (2–UPROCMAX), creates it via `CREATEPROCESS`, then blocks on `shellSemaphore` until the child terminates. |

Any unknown syscall number or non-syscall exception (program trap) causes the process to be terminated and its parent unblocked.

### Notable implementation choices

- **Separate TX/RX mutexes**: terminal 0 has independent transmit and receive channels, so `termMutexTX` and `termMutexRX` are kept separate. This allows a process to read and another to write simultaneously without unnecessary blocking.
- **Atomic TLB invalidation**: `updateTLBEntry` disables interrupts during the TLBP + TLBWI sequence to prevent a concurrent TLB access between the PTE invalidation and the TLB flush, which could expose a stale valid entry to another process.
- **FIFO eviction without dirty-bit optimization**: all frames are always written back to flash on eviction (D=1 is set unconditionally). This simplifies the pager at the cost of one extra flash write per eviction for pages that were not modified.
- **Shell/child synchronization via semaphore**: `sys6_execute` blocks the shell on `shellSemaphore` (P) and the child signals it (V) in `sys2_terminate` or `programTrapHandler` before calling `TERMPROCESS`. This ensures the shell never proceeds to the next command before the previous program has fully terminated.
- **ASID-based flash assignment**: U-proc with ASID *i* uses flash device *i−1*. The pager computes the device address as `START_DEVREG + (IL_FLASH − IL_DISK) * 0x80 + (asid−1) * 0x10`, following the uRISCV device register layout.

---

Stay hungry, stay foolish
