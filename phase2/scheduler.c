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
        klog_print("SONO QUI! \n");
        currentProcess = removeProcQ(&readyQueue);
        setTIMER(TIMESLICE); 
        LDST(&(currentProcess->p_s));
    }else if (processCount == 0) {
        klog_print("Scheduler HALT, no processes left\n");
        HALT();
    }else if (processCount > 0 && softBlockCount > 0 ) {
        
        setMIE(MIE_ALL & ~MIE_MTIE_MASK);
        unsigned int status = getSTATUS();
        status |= MSTATUS_MIE_MASK;
        setSTATUS(status);
        klog_print("STO ANDANDO IN WAIT \n");
        WAIT();
    }else if (processCount > 0 && softBlockCount == 0 && emptyProcQ(&readyQueue)) {
        //caso di deadlock, cerchiamo forzatamente un processo attivo
        
        for (int i = 0; i < MAXPROC; i++){
            if (activeProcesses[i] != NULL) {
                currentProcess = activeProcesses[i];
                LDST(&(currentProcess->p_s));
            }
        }
    }
    //vero deadlock (speriamo di non arrivarci)
    klog_print("DEADLOCK\n");
    PANIC();
}