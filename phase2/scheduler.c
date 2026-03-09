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
        setTIMER(TIMESLICE); //
        LDST(&(currentProcess->p_s));
    }else if (processCount == 0) {
        klog_print("Scheduler HALT, no processes left\n");
        HALT();
    }else if (softBlockCount > 0) {

        klog_print("WAIT: processCount=");
        klog_print_dec(processCount);
        klog_print(" softBlockCount=");
        klog_print_dec(softBlockCount);
        klog_print("\n");

        setMIE(MIE_ALL & ~MIE_MTIE_MASK);
        unsigned int status = getSTATUS();
        status |= MSTATUS_MIE_MASK;
        setSTATUS(status);

        WAIT();
    }else if (processCount > 0 && softBlockCount == 0 && emptyProcQ(&readyQueue)) {
        //caso di deadlock, cerchiamo forzatamente un processo attivo
        for (int i = 0; i < MAXPROC; i++){
            if (activeProcesses[i] != NULL) {
                klog_print("pid=");
            klog_print_dec(activeProcesses[i]->p_pid);
            klog_print(" sem=");
            klog_print_hex((unsigned int)activeProcesses[i]->p_semAdd);
            klog_print(" is48=");
            klog_print_dec(activeProcesses[i]->p_semAdd == &deviceSemaphores[48]);
            klog_print("\n");
                currentProcess = activeProcesses[i];
                LDST(&(currentProcess->p_s));
            }
        }
    }
    //vero deadlock (speriamo di non arrivarci)
    klog_print("DEADLOCK\n");
    PANIC();
}