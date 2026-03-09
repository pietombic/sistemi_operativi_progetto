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

//FIXME: da modificare
static void copyState(state_t *dst, state_t *src) {
    unsigned int *d = (unsigned int *) dst;
    unsigned int *s = (unsigned int *) src;
    for (int i = 0; i < STATE_T_SIZE_IN_BYTES / WORDLEN; i++)
        d[i] = s[i];
}

static int isDeviceSemaphore(int *semAdd) {
    int *base = &deviceSemaphores[0];
    int *top  = &deviceSemaphores[SEMDEVLEN];

    if (semAdd < base || semAdd >= top) return 0;

    int idx = (int)(semAdd - base);
    if (idx == 48) return 0;   /* pseudo‑clock NON è device */

    return 1;
}

static void blockCurrentProcess(int *sem) {
    if (!currentProcess) PANIC();

    cpu_t current_tod;
    STCK(current_tod);
    currentProcess->p_time += current_tod - start_time_current_quantum;
    start_time_current_quantum = current_tod;

    currentProcess->p_semAdd = sem;
    if (isDeviceSemaphore(sem)) {
        softBlockCount++;
    }

    insertBlocked(sem, currentProcess);
    currentProcess = NULL;
    klog_print("CALLING SCHEDULER FROM BLOCK\n");
    scheduler();
}

static void addActiveProcess(pcb_t *pcb){
    for (int i = 0; i < MAXPROC; i++) {
        if (activeProcesses[i] == NULL) {
            activeProcesses[i] = pcb;
            return;
        }
    }
    PANIC(); // Non ci sono slot disponibili per nuovi processi
}

static void removeActiveProcess(pcb_t *pcb){
    for (int i = 0; i < MAXPROC; i++) {
        if (activeProcesses[i] == pcb) {
            activeProcesses[i] = NULL;
            return;
        }
    }
    PANIC(); // Il processo da rimuovere non è stato trovato
}

static pcb_t *findActiveProcessbyID(pcb_t *pcb){
    for (int i = 0; i < MAXPROC; i++) {
        if (activeProcesses[i] && activeProcesses[i]->p_pid == pcb->p_pid) {
            return activeProcesses[i];
        }
    }
    PANIC(); // Il processo da rimuovere non è stato trovato
}

static void recursiveTerminateProcess(pcb_t *target){
    if (!target) return;
    if (target->p_pid < 0) return;

    target->p_pid = -1; // Segna il processo come terminato
    pcb_t *child;
    while ((child = removeChild(target)) != NULL) {
        recursiveTerminateProcess(child);
    }

    removeActiveProcess(target);

    if (target->p_semAdd != NULL){
        int *semAdd = target->p_semAdd;

        pcb_t *removed = outBlocked(target);
        if (removed) {
            if (isDeviceSemaphore(semAdd)) {
                softBlockCount--;
            }
        }
    }

    outProcQ(&readyQueue, target);
    if (target == currentProcess) {
        currentProcess = NULL;
    }
    freePcb(target);
    processCount--;
}


//FIXME: fino a qua



void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void exceptionHandler() {
    state_t *exceptionState = (state_t *) BIOSDATAPAGE;
    unsigned int cause = exceptionState->cause;
    unsigned int excCode = cause & CAUSE_EXCCODE_MASK;

    updateCPUTime(currentProcess); //

    klog_print("excCode = "); 
    klog_print_dec(excCode);
    klog_print("\n");

    if (CAUSE_IS_INT(cause)){
        // se è un interrupt
        interruptHandler();
    }else if (excCode == 8 || excCode == 11) {
        exceptionState->pc_epc += WORDLEN;
        systemCallHandler(exceptionState);
        return;
    }else if (excCode == 1 || excCode == 5 || excCode == 7){
        unsigned int mpp = (exceptionState->status & MSTATUS_MPP_MASK);
        if (mpp == MSTATUS_MPP_U) {
            exceptionState->cause = (exceptionState->cause & CLEAREXECCODE) | (PRIVINSTR << CAUSESHIFT);
            passUpOrDie(GENERALEXCEPT, exceptionState);
            return;
        }else {
            passUpOrDie(PGFAULTEXCEPT, exceptionState);
            return;
        }
    }else {
        passUpOrDie(GENERALEXCEPT, exceptionState);
        return;
    }
}

void systemCallHandler(state_t *exceptionState) {
    int service_code = (int) exceptionState->reg_a0;
    klog_print("system call code = ");
    klog_print_dec(service_code);
    klog_print("\n");

    //Controllo Kernel-mode per SYSCALL negative
    if (service_code < 0) {
        if ((exceptionState->status & MSTATUS_MPP_MASK) == MSTATUS_MPP_U) {
            //Simula Program Trap per istruzione privilegiata
            exceptionState->cause = 10; //? instruzione privata 
            passUpOrDie(GENERALEXCEPT, exceptionState); //programtrapHandler
            return;
        }
    }

    if (service_code > 1) {
        passUpOrDie(GENERALEXCEPT, exceptionState);
        return;
    }

    //Smistamento dei servizi
    switch (service_code) {
        case CREATEPROCESS: // -1
            createProcess(exceptionState);
            break;
        case TERMPROCESS:   // -2
            klog_print("terminate process syscall\n");
            terminateProcess(exceptionState);
            break;
        case PASSEREN:      // -3
            waitSemaphore(exceptionState);
            break;
        case VERHOGEN:      // -4
            signalSemaphore(exceptionState);
            break;
        case DOIO:           // -5
            klog_print("doIO syscall\n");
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
            // Se a0 >= 1 o è un codice non gestito, 
            // passa la gestione al Support Level (Pass Up or Die)
            passUpOrDie(GENERALEXCEPT, exceptionState);
            break;
    }
}

//FIXME: da rivedere la fine 
void createProcess(state_t *state) {
    pcb_t *newPcb = allocPcb(); 
    if (newPcb == NULL) {
        state->reg_a0 = (unsigned int) -1; 
        copyState(&currentProcess->p_s, state); 
        LDST(&currentProcess->p_s);
    }
    copyState(&newPcb->p_s, (state_t *)state->reg_a1);

    newPcb->p_supportStruct = (support_t *)state->reg_a3; 
    newPcb->p_prio = (int) state->reg_a2;
    newPcb->p_time = 0; 
    newPcb->p_semAdd = NULL; 

    addActiveProcess(newPcb);
    insertChild(currentProcess, newPcb); 
    insertProcQ(&readyQueue, newPcb); 

    processCount++; //incremento il contatore dei processi
    state->reg_a0 = (unsigned int) newPcb->p_pid; 
    copyState(&currentProcess->p_s, state);
    insertProcQ(&readyQueue, currentProcess);
    currentProcess = NULL;
    scheduler();

    klog_print("process created with PID = ");
    klog_print_dec(newPcb->p_pid);
    klog_print(" process count = ");
    klog_print_dec(processCount);
    klog_print("\n");
}
void terminateProcess(state_t *state) {
    int pidToTerminate = (int) state->reg_a1;
    updateCPUTime(currentProcess);
    
    pcb_t *targetProcess;
    if (pidToTerminate == 0) {
        targetProcess = currentProcess;
    } else {
        // cerca per valore PID, non per puntatore
        targetProcess = NULL;
        for (int i = 0; i < MAXPROC; i++) {
            if (activeProcesses[i] != NULL && 
                activeProcesses[i]->p_pid == pidToTerminate) {
                targetProcess = activeProcesses[i];
                break;
            }
        }
    }
    
    if (targetProcess == NULL) {
        LDST(state);
        return;
    }
    
    int isCurrent = (targetProcess == currentProcess);
    recursiveTerminateProcess(targetProcess);
    
    if (isCurrent) {
        currentProcess = NULL;
        scheduler();
    } else {
        copyState(&currentProcess->p_s, state);
        LDST(&currentProcess->p_s);
    }
}

void waitSemaphore(state_t *state) {
    int *semAdd = (int *) state->reg_a1; 
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);
    (*semAdd)--;

    if ((*semAdd) < 0) {        
        blockCurrentProcess(semAdd);
    } 
    LDST(&currentProcess->p_s);
}


void signalSemaphore(state_t *state) {
    int *semAdd = (int *) state->reg_a1;
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);
    (*semAdd)++;

    if ((*semAdd) <= 0) {
        pcb_t *releasedPcb = removeBlocked(semAdd);
        if (releasedPcb != NULL) {
            klog_print("[V] done on pid: ");
            klog_print_dec(releasedPcb->p_pid);
            klog_print("\n");
            releasedPcb->p_semAdd = NULL;
            insertProcQ(&readyQueue, releasedPcb);
        }
    }
    //
    
    LDST(&currentProcess->p_s);
}

void doIO(state_t *state) {
    int *commandAddr = (int *)state->reg_a1; 
    int commandValue = (int) state->reg_a2;

    unsigned int offset = (unsigned int)commandAddr - START_DEVREG;
    int line = (int)(offset / 0x80) + 3; // line 3-7
    int dev = (int)((offset % 0x80) / 0x10); // dev 0-7
    int subword = (int)((offset % 0x10) / WORDLEN); 
    
    int semIndex = (line == 7)
        ? ((subword == 3) ? 32+dev : 40+dev)
        : (((line - 3) * 8) + dev);
    
    // FIXME: problema qui, chiama un exception PERCHEEE
    //*commandAddr = commandValue; // scrivo il comando nel registro del dispositivo
    
    updateCPUTime(currentProcess); 
    copyState(&currentProcess->p_s, state);

    deviceSemaphores[semIndex]--;
    *commandAddr = commandValue;  // scrivi il comando QUI, dopo aver decrementato
    blockCurrentProcess(&deviceSemaphores[semIndex]);
    /*
    unsigned int *devBase = (unsigned int *) (START_DEVREG + (line - 3) * 0x80 + dev * 0x10);
    int ready = 0;

    if (line == 7) {
            if (subword == 3) {
                if (!(devBase[3] & 0x1)) {
                    deviceSemaphores[semIndex]--;
                    blockCurrentProcess(&deviceSemaphores[semIndex]);
                } else {
                    unsigned int status = devBase[2];
                    *commandAddr = commandValue;
                    devBase[3] = 1;
                    state->reg_a0 = status;
                    LDST(&currentProcess->p_s);
                }
            } else {
                if (!(devBase[1] & 0x1)) {
                    deviceSemaphores[semIndex]--;
                    blockCurrentProcess(&deviceSemaphores[semIndex]);
                } else {
                    unsigned int status = devBase[0];
                    devBase[1] = 1;
                    state->reg_a0 = status;
                    LDST(&currentProcess->p_s);
                }
            }
            } else {
                ready = devBase[1] & 0x1;
                if (!ready) {
                    deviceSemaphores[semIndex]--;
                    blockCurrentProcess(&deviceSemaphores[semIndex]);
                }
            }
    klog_print("doIO exiting...\n");
    */
}


void getCPUTime(state_t *state) {
    updateCPUTime(currentProcess);
    state->reg_a0 = (unsigned int) currentProcess->p_time;
    LDST(state);
}

void waitForClock(state_t *state) {
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);
    deviceSemaphores[48]--; // pseudo-clock semaphore
    softBlockCount++;
    blockCurrentProcess(&deviceSemaphores[48]);
}

void getSupportData(state_t *state) {
    state->reg_a0 = (unsigned int) currentProcess->p_supportStruct;
    LDST(state);
}

void getProcessID(state_t *state) {
    int supIndex = (int) state->reg_a1;
    if (supIndex == 0){
        state->reg_a0 = (unsigned int) currentProcess->p_pid;
    } else {
        if (currentProcess->p_supportStruct != NULL) {
            state->reg_a0 = (unsigned int) currentProcess->p_parent->p_pid;
        } else {
            state->reg_a0 = 0; 
        }
    }
}

void yield(state_t *state) {
    updateCPUTime(currentProcess); 
    copyState(&currentProcess->p_s, state);
    insertProcQ(&readyQueue, currentProcess);
    currentProcess = NULL;
    scheduler(); 
}

void passUpOrDie(int exceptionType, state_t *exceptionState) {
    if (currentProcess->p_supportStruct == NULL) {
        // DIE: termina il processo corrente (non una syscall terminate con parametri)
        updateCPUTime(currentProcess);
        recursiveTerminateProcess(currentProcess);
        currentProcess = NULL;
        scheduler();
    } else {
        support_t *sup = currentProcess->p_supportStruct;
        copyState(&sup->sup_exceptState[exceptionType], (state_t *) exceptionState);
        context_t *newContext = &sup->sup_exceptContext[exceptionType];
        LDCXT(newContext->stackPtr, newContext->status, newContext->pc);
    }
}