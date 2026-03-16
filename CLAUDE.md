# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

**Build:**
```bash
cmake -B build
cmake --build build
```
This cross-compiles for RISC-V using `riscv64-unknown-elf-gcc` and produces `build/MultiPandOS.core.uriscv` and `build/MultiPandOS.stab.uriscv`.

**Run:**
```bash
uriscv
```
Load `config_machine.json` in the emulator GUI, press Play, then open Window → Terminal 0.

There are no automated tests; testing is done by running the emulator and observing output from `phase2/p2test.c` (the `test()` function is the initial process entry point).

## Architecture

This is **MultiPandOS**, a two-phase OS kernel for the uRISCV emulator (RISC-V bare-metal, no stdlib).

### Phase 1 — Data Structures (`phase1/`)
- `pcb.c` — PCB allocation from a static pool (`pcbFree_table[MAXPROC]`), priority-ordered process queues, and process tree (parent/child/sibling via embedded `list_head`). PIDs are assigned by a monotonically increasing counter.
- `asl.c` — Active Semaphore List: a sorted list of `semd_t` descriptors, each holding a queue of PCBs blocked on that semaphore's address (`s_key`). Descriptors are pooled (`semd_table[MAXPROC]`).
- `headers/listx.h` — Linux kernel-style intrusive doubly-linked list (`list_head`, `container_of`, `list_for_each`). All queues in the project use this.
- `headers/types.h` — Core types: `pcb_t`, `semd_t`, `support_t`, `swap_t`, `context_t`.
- `headers/const.h` — All hardware addresses, syscall codes, timing constants, and bitmasks.

### Phase 2 — Kernel (`phase2/`)
- `main.c` — Entry point; calls `initKernel()`.
- `initial.c` — Kernel initialization and **global state**:
  - `processCount` — total live processes
  - `softBlockCount` — processes blocked on I/O devices or pseudo-clock
  - `readyQueue` — priority-ordered queue of ready PCBs
  - `currentProcess` — the running PCB (NULL when none)
  - `start_time_current_quantum` — TOD clock snapshot for CPU time accounting
  - `deviceSemaphores[49]` — one semaphore per device subdevice + pseudo-clock (index 48)
  - `activeProcesses[MAXPROC]` — flat array of all live process pointers (ready, running, or blocked)
- `scheduler.c` — Round-robin scheduler with priority. Handles: dispatch, HALT (no procs), WAIT (softblock only), PANIC (deadlock).
- `exception.c` — Unified exception entry (`exceptionHandler`). Routes to:
  - `interruptHandler()` for interrupts
  - `systemCallHandler()` for SYSCALL (excCode 8/11); dispatches 10 syscalls (codes -1 to -10)
  - `passUpOrDie()` for TLB faults and general exceptions
- `interrupt.c` — Interrupt handler:
  - **excCode 7** = PLT (timeslice expired) → save state, round-robin
  - **excCode 3** = interval timer (100 ms pseudo-clock) → unblock all waiters on `deviceSemaphores[48]`
  - **excCode 17–21** = device lines 3–7 → ACK device, unblock waiting process, store status in `reg_a0`
- `klog.c` (root) — Circular debug log buffer (`klog_buffer[64][42]`). Use `klog_print()`, `klog_print_dec()`, `klog_print_hex()` for kernel debug output visible in the uRISCV memory/symbol tracer.

### Device Semaphore Layout (`deviceSemaphores[49]`)
| Index range | Device |
|---|---|
| 0–7 | Disk (line 3) |
| 8–15 | Flash (line 4) |
| 16–23 | Ethernet (line 5) |
| 24–31 | Printer (line 6) |
| 32–39 | Terminal TX (line 7) |
| 40–47 | Terminal RX (line 7) |
| 48 | Pseudo-clock |

### Syscall Codes (passed in `reg_a0`, negative values)
`CREATEPROCESS=-1`, `TERMPROCESS=-2`, `PASSEREN=-3`, `VERHOGEN=-4`, `DOIO=-5`, `GETTIME=-6`, `CLOCKWAIT=-7`, `GETSUPPORTPTR=-8`, `GETPROCESSID=-9`, `YIELD=-10`

### Key Patterns
- Exception state is always at `BIOSDATAPAGE` (0x0FFFF000) on entry.
- CPU time is tracked by snapshotting `STCK` at quantum start (`start_time_current_quantum`) and accumulating deltas into `p_time` at each preemption/block.
- `passUpOrDie`: if the current process has no `p_supportStruct`, terminate it; otherwise load the support-level context via `LDCXT`.
- `p_pid = -1` is used as a tombstone during recursive process termination to prevent double-free cycles.
