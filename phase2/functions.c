#include "../headers/const.h"
#include "../headers/types.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include <uriscv/liburiscv.h>

void copyState(state_t *dst, state_t *src) {
    unsigned int *d = (unsigned int *) dst;
    unsigned int *s = (unsigned int *) src;
    for (int i = 0; i < STATE_T_SIZE_IN_BYTES / WORDLEN; i++)
        d[i] = s[i];
}

int isSoftBlockSemaphore(int *semAdd) {
    return semAdd >= &deviceSemaphores[0] && semAdd < &deviceSemaphores[SEMDEVLEN];
}

/*
    this function block the currentProcess on the
    given semaphore address. We will use this function
    whenever we will need to perform a blocking syscall.
*/

void blockCurrentProcess(int *sem) {
    if (!currentProcess) PANIC();

    //we save currentProcess time before blocking it 
    cpu_t current_tod;
    STCK(current_tod);
    currentProcess->p_time += current_tod - start_time_current_quantum;
    start_time_current_quantum = current_tod;

    currentProcess->p_semAdd = sem;
    if (isSoftBlockSemaphore(sem)) {
        softBlockCount++;
    }

    insertBlocked(sem, currentProcess);
    currentProcess = NULL;
    scheduler();
}

/*
    this function make a DFS on the process tree 
    to find a process given its PID.
*/

pcb_t *findInTree(pcb_t *node, int pid) {
    if (node == NULL || node->p_pid < 0) return NULL;
    if (node->p_pid == pid) return node;
    struct list_head *pos;
    list_for_each(pos, &node->p_child) {
        pcb_t *child = container_of(pos, pcb_t, p_sib);
        pcb_t *found = findInTree(child, pid);
        if (found) return found;
    }
    return NULL;
}

/*
    this function terminate a process given its pcb,
    moreover it terminate all the progeny of the target.
    we will use this function to perform the TERMPROCESS 
    syscall.
*/

void recursiveTerminateProcess(pcb_t *target){
    if (!target) return;
    if (target->p_pid < 0) return;

    //we mark the process as terminated
    target->p_pid = -1;
    pcb_t *child;

    //recursive call on the process childs
    while ((child = removeChild(target)) != NULL) {
        recursiveTerminateProcess(child);
    }

    //if the process to terminate was blocked on a semaphore
    //we remove the process from the semaphore (Outblocked)
    //if the semaphore was a device semaphore we decrease the 
    //softBlockCount

    if (target->p_semAdd != NULL){
        int *semAdd = target->p_semAdd;

        pcb_t *removed = outBlocked(target);
        if (removed) {

            if (isSoftBlockSemaphore(semAdd)) {
                softBlockCount--;
            }
            (*semAdd)++;
        }
    }

    outProcQ(&readyQueue, target);
    if (target == currentProcess) {
        currentProcess = NULL;
    }
    freePcb(target);
    processCount--;
}

void updateCPUTime(pcb_t *p) {
    if (p) {
        cpu_t current_tod;
        STCK(current_tod); 
        p->p_time += (current_tod - start_time_current_quantum);
        start_time_current_quantum = current_tod;
    }
}

void passUpOrDie(int exceptionType, state_t *exceptionState) {
    if (currentProcess->p_supportStruct == NULL) {
        // DIE: we terminate currentProcess
        updateCPUTime(currentProcess);
        recursiveTerminateProcess(currentProcess);
        currentProcess = NULL;
        scheduler();
    } else {
        //passing up to phase3
        support_t *sup = currentProcess->p_supportStruct;
        copyState(&sup->sup_exceptState[exceptionType], exceptionState);
        context_t *newContext = &sup->sup_exceptContext[exceptionType];
        LDCXT(newContext->stackPtr, newContext->status, newContext->pc);
    }
}