#include "../headers/types.h"
#include "../headers/const.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include "headers/klog.h"
#include <uriscv/liburiscv.h>


void scheduler() {
    if (!emptyProcQ(&readyQueue)) {
        currentProcess = removeProcQ(&readyQueue);

        setTIMER(TIMESLICE);
        STCK(start_time_current_quantum);

        LDST(&(currentProcess->p_s));   // non ritorna
    } else {
        if (processCount == 0) {
            klog_print("scheduler halt (process count == 0)\n");
            HALT();
        } else if (softBlockCount > 0) {
            // niente ready, ma qualcuno è bloccato: idle
            setTIMER(NEVER);                            // disabilita PLT
            setSTATUS(getSTATUS() | MSTATUS_MIE_MASK);  // abilita interrupt globali
            WAIT();
            scheduler();                                // quando torna, riprova
        } else {
            PANIC(); // deadlock
        }
    }
}