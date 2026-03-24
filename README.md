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


---

## Phase 2: Kernel

Phase 2 implements the OS kernel: exception handler, scheduler, interrupt handler, and the ten system calls defined by the specification.

### Scheduler

The scheduler implements a **priority-based round-robin** policy. On each invocation it dequeues the highest-priority process from `readyQueue`, arms the time slice via `setTIMER(TIMESLICE)`, and dispatches it with `LDST`.

When the ready queue is empty the scheduler distinguishes three cases:
- **No living processes** (`processCount == 0`): the kernel executes `HALT`.
- **Soft-blocked processes** (`softBlockCount > 0`): the kernel enters `WAIT` with interrupts enabled, waiting for a device or the pseudo-clock to unblock a process.
- **Deadlock** (living processes but none soft-blocked or ready): the kernel executes `PANIC`.

CPU time is tracked by snapshotting the clock (`STCK`) at the start of each quantum and accumulating the delta into `p_time` on every preemption or block.

### Exception handler

The unified entry point `exceptionHandler` reads the saved state from `BIOSDATAPAGE` and routes the exception based on the `cause` field:

| Type | Condition | Action |
|---|---|---|
| Interrupt | `CAUSE_IS_INT` | `interruptHandler()` |
| System call | excCode 8 or 11 (ecall) | advance PC by one word, `systemCallHandler()` |
| Memory access fault | excCode 1, 5 or 7 | `passUpOrDie(PGFAULTEXCEPT)` if in user space, otherwise `passUpOrDie(GENERALEXCEPT)` |
| Other | any other excCode | `passUpOrDie(GENERALEXCEPT)` |

`passUpOrDie` terminates the current process if it has no `p_supportStruct`, otherwise it delegates to the support level (phase 3) by loading the exception context stored in that structure.

### System calls

The ten system calls are dispatched by `systemCallHandler`, which first checks that negative service codes (kernel-level calls) are invoked from M-mode (kernel mode), then switches on `reg_a0`:

| Code | Name | Description |
|---|---|---|
| -1 | `CREATEPROCESS` | Allocates a new PCB, initializes it with the state/priority/support pointer passed by the caller, and inserts it as a child of the current process in the process tree. |
| -2 | `TERMPROCESS` | Terminates the process identified by the PID in `reg_a1` (0 = self) and its entire progeny via a recursive tree traversal. |
| -3 | `PASSEREN` | P operation on a semaphore: decrements the counter and blocks the process if it goes negative. |
| -4 | `VERHOGEN` | V operation on a semaphore: increments the counter and unblocks the first waiting process if any. |
| -5 | `DOIO` | Writes a command to a device register and blocks the process on the corresponding device semaphore waiting for the interrupt. Terminals have separate TX and RX semaphores. |
| -6 | `GETTIME` | Returns the accumulated CPU time of the current process. |
| -7 | `CLOCKWAIT` | Blocks the process on the pseudo-clock semaphore until the next 100 ms timer tick. |
| -8 | `GETSUPPORTPTR` | Returns the `support_t` pointer of the current process, or NULL if none was set at creation time. |
| -9 | `GETPROCESSID` | Returns the PID of the current process (`reg_a1 == 0`) or of its parent (`reg_a1 != 0`), 0 if no parent exists. |
| -10 | `YIELD` | Voluntarily relinquishes the CPU: the process is re-inserted into the ready queue and the scheduler picks the next one. |

### Notable implementation choices

- **PID lookup via process tree**: instead of maintaining a separate `activeProcesses[]` array, finding a process by PID is done with a DFS over the process tree rooted at `rootProcess`. Every live process is always reachable because `CREATEPROCESS` always inserts the new process as a child of the caller.
- **Unified soft-block semaphore check**: `isSoftBlockSemaphore` identifies all semaphores that contribute to `softBlockCount` (devices 0–47 and pseudo-clock 48) with a single range check on `deviceSemaphores`, removing the need to treat the pseudo-clock as a special case.
- **Manual `copyState`**: the bare-metal toolchain (`-ffreestanding -nostdlib`) does not provide `memcpy`. Direct struct assignment of large types like `state_t` causes the compiler to emit a `memcpy` call, so the copy is done with a manual word-by-word loop instead.

---

Stay hungry, stay foolish
