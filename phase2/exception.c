#include "../headers/types.h"
#include "../headers/const.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include "headers/exception.h"
#include "headers/interrupt.h"
#include <uriscv/liburiscv.h>
#include <uriscv/cpu.h>
#include <stddef.h>
#include "headers/klog.h"
#include "headers/functions.h"

void exceptionHandler() {
    state_t *exceptionState = (state_t *) BIOSDATAPAGE;
    unsigned int cause   = exceptionState->cause;
    unsigned int excCode = cause & CAUSE_EXCCODE_MASK;

    /* interrupt */
    if (CAUSE_IS_INT(cause)) {
        interruptHandler();
        return;
    }

    /* ECALL (U-mode=8, M-mode=11): route to syscall handler.
       Normalize cause to 11 so support handlers always see M-mode ECALL code. */
    if (excCode == 8 || excCode == 11) {
        exceptionState->pc_epc += WORDLEN;
        exceptionState->cause = 11;
        systemCallHandler(exceptionState);
        return;
    }

    /* All other exceptions (TLB, access faults, illegal instr, etc.):
       route based on BADVADDR — avoids depending on exact uRISCV TLB exception codes */
    int exType = (getBADVADDR() >= KUSEG) ? PGFAULTEXCEPT : GENERALEXCEPT;
    passUpOrDie(exType, exceptionState);
}

void systemCallHandler(state_t *exceptionState) {
    int service_code = (int) exceptionState->reg_a0;
    
    /* syscall number >= 1: non valido, passa up */
    if (service_code >= 1) {
        passUpOrDie(GENERALEXCEPT, exceptionState);
        return;
    }

    /* syscall negativa invocata in user-mode: privilegio insufficiente */
    if ((exceptionState->status & MSTATUS_MPP_MASK) == MSTATUS_MPP_U) {
        exceptionState->cause = (exceptionState->cause & ~GETEXECCODE) | (PRIVINSTR << CAUSESHIFT);
        passUpOrDie(GENERALEXCEPT, exceptionState);
        return;
    }

    //switching on the different services

    switch (service_code) {
        case CREATEPROCESS: // -1
            createProcess(exceptionState);
            break;
        case TERMPROCESS:   // -2
            terminateProcess(exceptionState);
            break;
        case PASSEREN:      // -3
            passeren(exceptionState);
            break;
        case VERHOGEN:      // -4
            verhogen(exceptionState);
            break;
        case DOIO:           // -5
            doIO(exceptionState);
            break;
        case GETTIME:     // -6
            getCPUTime(exceptionState);
            break;
        case CLOCKWAIT:    // -7
            waitForClock(exceptionState);
            break;
        case GETSUPPORTPTR:   // -8
            getSupportData(exceptionState);
            break;
        case GETPROCESSID:     // -9
            getProcessID(exceptionState);
            break;
        case YIELD :           // -10
            yield(exceptionState);
            break;
        
        default:
            passUpOrDie(GENERALEXCEPT, exceptionState);
            break;
    }
}

/*
    allocates a new PCB, initializes it with the state/priority/support
    passed by the caller, inserts it as a child of currentProcess and
    puts it in the ready queue. Returns the new PID in reg_a0, or -1
    if no free PCB is available.
*/
void createProcess(state_t *state) {
    pcb_t *newPcb = allocPcb();

    //no free PCB
    if (newPcb == NULL) {
        state->reg_a0 = (unsigned int) -1;
        copyState(&currentProcess->p_s, state);
        LDST(&currentProcess->p_s);
        return;
    }

    copyState(&newPcb->p_s, (state_t *)state->reg_a1);

    //initializing the new process
    newPcb->p_supportStruct = (support_t *)state->reg_a3;
    newPcb->p_prio = (int) state->reg_a2;
    newPcb->p_time = 0;
    newPcb->p_semAdd = NULL;

    insertChild(currentProcess, newPcb);
    insertProcQ(&readyQueue, newPcb);
    processCount++;

    //returning the pid of the new process to the caller
    state->reg_a0 = (unsigned int) newPcb->p_pid;
    copyState(&currentProcess->p_s, state);
    LDST(state);
}

/*
    terminates the process identified by reg_a1 (0 = self) and its
    entire progeny. if the terminated process was currentProcess,
    the scheduler is invoked to pick the next one.
*/
void terminateProcess(state_t *state) {
    int pidToTerminate = (int) state->reg_a1;
    updateCPUTime(currentProcess);

    pcb_t *targetProcess;
    if (pidToTerminate == 0) {
        targetProcess = currentProcess;
    } else {
        targetProcess = findInTree(rootProcess, pidToTerminate);
    }

    //pid not found: return to caller
    if (targetProcess == NULL) {
        LDST(state);
        return;
    }

    int isCurrent = (targetProcess == currentProcess);
    recursiveTerminateProcess(targetProcess);

    if (isCurrent || currentProcess == NULL) {
        currentProcess = NULL;
        scheduler();
    } else {
        copyState(&currentProcess->p_s, state);
        LDST(&currentProcess->p_s);
    }
}

/*
    P (wait) on the semaphore at reg_a1. decrements the counter;
    if it goes negative the process blocks on the semaphore.
*/
void passeren(state_t *state) {
    int *semAdd = (int *) state->reg_a1;
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);
    (*semAdd)--;
    if ((*semAdd) < 0) {
        blockCurrentProcess(semAdd);
    }
    LDST(&currentProcess->p_s);
}

/*
    V (signal) on the semaphore at reg_a1. increments the counter;
    if there are blocked processes, the first one is unblocked and
    moved to the ready queue.
*/
void verhogen(state_t *state) {
    int *semAdd = (int *) state->reg_a1;
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);
    (*semAdd)++;

    if ((*semAdd) <= 0) {
        pcb_t *releasedPcb = removeBlocked(semAdd);
        if (releasedPcb != NULL) {
            releasedPcb->p_semAdd = NULL;
            insertProcQ(&readyQueue, releasedPcb);
        }
    }

    LDST(&currentProcess->p_s);
}

/*
    writes commandValue to the device register at commandAddr, then
    blocks the process on the corresponding device semaphore waiting
    for the interrupt. for terminals, TX and RX have separate semaphores.
*/
void doIO(state_t *state) {
    unsigned int commandAddr  = (unsigned int)state->reg_a1;
    unsigned int commandValue = (unsigned int)state->reg_a2;

    //validate the device register address
    if (commandAddr < START_DEVREG || commandAddr > 0x100002D4) {
        state->reg_a0 = -1;
        LDST(state);
        return;
    }

    //compute semaphore index from the device register address
    unsigned int offset = commandAddr - START_DEVREG;
    int line = (offset / 0x80) + 3;
    int dev  = (offset % 0x80) / 0x10;

    int semIndex;
    if (line == 7) {
        //terminals have two subdevices: TX (offset >= 8) and RX
        int isWrite = ((offset % 0x10) >= 0x8);
        semIndex = isWrite ? (40 + dev) : (32 + dev);
    } else {
        semIndex = (line - 3) * 8 + dev;
    }

    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);

    deviceSemaphores[semIndex]--;

    //volatile write to avoid a PLT interrupt before the device sees the command
    *((volatile unsigned int *)commandAddr) = commandValue;

    blockCurrentProcess(&deviceSemaphores[semIndex]);
}

/*
    returns the accumulated CPU time of currentProcess in reg_a0.
*/
void getCPUTime(state_t *state) {
    updateCPUTime(currentProcess);
    state->reg_a0 = (unsigned int) currentProcess->p_time;
    LDST(state);
}

/*
    blocks the process on the pseudo-clock semaphore until the next
    100ms interval timer tick.
*/
void waitForClock(state_t *state) {
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);
    deviceSemaphores[48]--;
    blockCurrentProcess(&deviceSemaphores[48]);
}

/*
    returns the support structure pointer of currentProcess in reg_a0,
    or NULL if none was set at creation time.
*/
void getSupportData(state_t *state) {
    state->reg_a0 = (unsigned int) currentProcess->p_supportStruct;
    LDST(state);
}

/*
    returns the PID of currentProcess (reg_a1 == 0) or of its parent
    (reg_a1 != 0) in reg_a0. returns 0 if the parent does not exist.
*/
void getProcessID(state_t *state) {
    int supIndex = (int) state->reg_a1;
    if (supIndex == 0) {
        state->reg_a0 = (unsigned int) currentProcess->p_pid;
    } else {
        state->reg_a0 = (currentProcess->p_parent != NULL)
                        ? (unsigned int) currentProcess->p_parent->p_pid
                        : 0;
    }
    LDST(state);
}

/*
    voluntarily relinquishes the CPU. currentProcess is put back at the
    tail of its priority band and the scheduler picks the next process.
*/
void yield(state_t *state) {
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);
    insertProcQ(&readyQueue, currentProcess);
    currentProcess = NULL;
    scheduler();
}

