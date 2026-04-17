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
        //dispatching the ready process
        currentProcess = removeProcQ(&readyQueue);
        
        setTIMER(TIMESLICE);
        STCK(start_time_current_quantum);
        LDST(&(currentProcess->p_s));
    }else if (processCount == 0) {
        klog_print("Scheduler HALT, no processes left\n");
        HALT();
    }else if (processCount > 0 && softBlockCount > 0 ) {
        // pasted from p2 specs
        setMIE(MIE_ALL & ~MIE_MTIE_MASK);
        unsigned int status = getSTATUS();
        status |= MSTATUS_MIE_MASK;
        setSTATUS(status);
        
        //waiting for a blocked process to be ready
        WAIT();
    }else if (processCount > 0 && softBlockCount == 0 && emptyProcQ(&readyQueue)) {
        klog_print("DEADLOCK ! \n");
        PANIC();
    }
}