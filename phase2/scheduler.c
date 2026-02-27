#include "../headers/types.h"
#include "../headers/const.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "initial.c"

 void scheduler() {
    currentProcess = removeProcQ(&readyQueue); 
    LDIT(TIMESLICE);
    if (currentProcess != NULL) {
        LDST(&currentProcess->p_s);     /* load the saved state */
    }else {
        if (processCount == 0) {
            HALT();
        } else if (processCount > 0 &&  softBlockCount > 0) {
            //copiato dalle spec
            setMIE(MIE_ALL & ~MIE_MTIE_MASK);
            unsigned int status = getSTATUS();
            status |= MSTATUS_MIE_MASK;
            setSTATUS(status);
            //
            WAIT();
        }else if (processCount > 0 && softBlockCount == 0) {
            //deadlock
            PANIC();
        }
    }
}