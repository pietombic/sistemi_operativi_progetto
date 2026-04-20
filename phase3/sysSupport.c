/*
    Support Level general exception handler.
    Handles positive syscalls (SYS2-SYS6) and program traps from U-procs.
    Note: PC is already advanced by exception.c before passUpOrDie, so we do NOT
    increment pc_epc here again.
*/

#include "../headers/const.h"
#include "../headers/types.h"
#include "../phase1/headers/pcb.h"
#include "../phase2/headers/initial.h"
#include "headers/sysSupport.h"
#include "headers/initProc.h"
#include "headers/vmSupport.h"
#include <uriscv/liburiscv.h>
#include <uriscv/cpu.h>

/* Terminal 0 register base address */
#define TERM0_BASE (START_DEVREG + (IL_TERMINAL - IL_DISK) * 0x80)

/* Terminates a U-proc, signaling the appropriate waiter. */
static void programTrapHandler(support_t *sup) {
    if (sup->sup_asid == 1)
        SYSCALL(VERHOGEN, (int)&masterSemaphore, 0, 0);
    else
        SYSCALL(VERHOGEN, (int)&shellSemaphore, 0, 0);
    SYSCALL(TERMPROCESS, 0, 0, 0);
}

/* SYS2 - Terminate: U-proc wants to exit normally. */
static void sys2_terminate(support_t *sup) {
    if (sup->sup_asid == 1)
        SYSCALL(VERHOGEN, (int)&masterSemaphore, 0, 0);
    else
        SYSCALL(VERHOGEN, (int)&shellSemaphore, 0, 0);
    SYSCALL(TERMPROCESS, 0, 0, 0);
}

/* SYS4 - WriteTerminal: write a string to terminal 0.
   a1 = virtual address of string, a2 = length (0..128).
   Returns number of chars written in a0, or negative on error. */
static void sys4_writeTerminal(support_t *sup) {
    state_t *saved = &sup->sup_exceptState[GENERALEXCEPT];
    char *str = (char *) saved->reg_a1;
    int   len = (int)   saved->reg_a2;

    if (len < 0 || len > MAXSTRINGLEN || (unsigned int)str < KUSEG) {
        programTrapHandler(sup);
        return;
    }

    SYSCALL(PASSEREN, (int)&termMutexTX, 0, 0);

    unsigned int txCmdAddr = TERM0_BASE + 0x0C;  /* transm_command offset */
    int written = 0, i;
    for (i = 0; i < len; i++) {
        unsigned int cmd = ((unsigned int)(unsigned char)str[i] << 8) | TRANSMITCHAR;
        int status = SYSCALL(DOIO, (int)txCmdAddr, (int)cmd, 0);
        if ((status & 0xFF) != OKCHARTRANS) {
            saved->reg_a0 = (unsigned int)(-(int)status);
            SYSCALL(VERHOGEN, (int)&termMutexTX, 0, 0);
            LDST(saved);
            return;
        }
        written++;
    }

    SYSCALL(VERHOGEN, (int)&termMutexTX, 0, 0);
    saved->reg_a0 = (unsigned int) written;
    LDST(saved);
}

/* SYS5 - ReadTerminal: read a line from terminal 0 into buffer at a1.
   Reads until newline or MAXSTRINGLEN chars. Returns chars read in a0. */
static void sys5_readTerminal(support_t *sup) {
    state_t *saved = &sup->sup_exceptState[GENERALEXCEPT];
    char *buf = (char *) saved->reg_a1;

    if ((unsigned int)buf < KUSEG) {
        programTrapHandler(sup);
        return;
    }

    SYSCALL(PASSEREN, (int)&termMutexRX, 0, 0);

    unsigned int rxCmdAddr = TERM0_BASE + 0x04;  /* recv_command offset */
    int read = 0;
    while (read < MAXSTRINGLEN) {
        int status = SYSCALL(DOIO, (int)rxCmdAddr, RECEIVECHAR, 0);
        if ((status & 0xFF) != RECVD) {
            saved->reg_a0 = (unsigned int)(-(int)status);
            SYSCALL(VERHOGEN, (int)&termMutexRX, 0, 0);
            LDST(saved);
            return;
        }
        char c = (char)((status >> 8) & 0xFF);
        buf[read++] = c;
        if (c == '\n') break;
    }

    SYSCALL(VERHOGEN, (int)&termMutexRX, 0, 0);
    saved->reg_a0 = (unsigned int) read;
    LDST(saved);
}

/* SYS6 - Execute: launch a U-proc with the given ASID.
   Only callable by the shell (ASID 1). a1 = target ASID (2..UPROCMAX).
   Shell blocks on shellSemaphore until the child terminates. */
static void sys6_execute(support_t *sup) {
    state_t *saved = &sup->sup_exceptState[GENERALEXCEPT];

    /* Only the shell (ASID 1) may call Execute */
    if (sup->sup_asid != 1) {
        programTrapHandler(sup);
        return;
    }

    int asid = (int) saved->reg_a1;
    if (asid < 2 || asid > UPROCMAX) {
        saved->reg_a0 = (unsigned int) -1;
        LDST(saved);
        return;
    }

    /* Initialize support structure for the new U-proc */
    support_t *newSup = &supportStructures[asid - 1];
    initSupportStruct(newSup, asid);

    state_t newState = {0};
    newState.pc_epc   = UPROCSTARTADDR;
    newState.reg_sp   = USERSTACKTOP;
    newState.status   = MSTATUS_MPIE_MASK | MSTATUS_MIE_MASK;
    newState.mie      = MIE_ALL;
    newState.entry_hi = (asid << ASIDSHIFT);

    SYSCALL(CREATEPROCESS, (int)&newState, 0, (int)newSup);

    /* Shell waits here until child signals shellSemaphore on termination */
    SYSCALL(PASSEREN, (int)&shellSemaphore, 0, 0);

    LDST(saved);
}

static void syscallHandler(support_t *sup) {
    state_t *saved  = &sup->sup_exceptState[GENERALEXCEPT];
    int syscallNum  = (int) saved->reg_a0;

    switch (syscallNum) {
        case TERMINATE:     sys2_terminate(sup);    break;
        case WRITETERMINAL: sys4_writeTerminal(sup); break;
        case READTERMINAL:  sys5_readTerminal(sup);  break;
        case EXECUTE:       sys6_execute(sup);       break;
        default:            programTrapHandler(sup); break;
    }
}

/* General exception handler entry point for the Support Level.
   exception.c already incremented PC before passUpOrDie, so we do NOT
   increment pc_epc again here. */
void supportGeneralExHandler(void) {
    support_t *sup  = (support_t *) SYSCALL(GETSUPPORTPTR, 0, 0, 0);
    state_t *saved  = &sup->sup_exceptState[GENERALEXCEPT];
    unsigned int excCode = saved->cause & CAUSE_EXCCODE_MASK;

    /* EXC_ECU=8 (U-mode ECALL) or EXC_ECM=11 (normalized by exception.c) */
    if (excCode == EXC_ECU || excCode == EXC_ECM) {
        syscallHandler(sup);
    } else {
        programTrapHandler(sup);
    }
}
