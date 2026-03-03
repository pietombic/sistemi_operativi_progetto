#include "../headers/types.h"
#include "../phase1/headers/asl.h"
#include "../phase1/headers/pcb.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include "headers/exception.h"
#include <uriscv/liburiscv.h>
#include "headers/klog.h"

extern void test();

/* Global variables definitions */
int processCount;
int softBlockCount;
struct list_head readyQueue;
pcb_t *currentProcess;
cpu_t start_time_current_quantum;
int deviceSemaphores[SEMDEVLEN];

void updateCPUTime(pcb_t *p) {
    cpu_t current_tod;
    STCK(current_tod); // Legge il valore attuale del TOD clock [cite: 418, 419]

    // Aggiunge la differenza al tempo accumulato nel PCB [cite: 414, 417]
    p->p_time += (current_tod - start_time_current_quantum);

    // Aggiorna il tempo di inizio per l'eventuale prossimo intervallo
    start_time_current_quantum = current_tod;
}

pcb_t* findProcessByPID(int pid) {
    // 1. Controlla il processo corrente
    if (currentProcess != NULL && currentProcess->p_pid == pid) {
        return currentProcess;
    }
    // 2. Cerca nella Ready Queue
    struct list_head *pos;
    list_for_each(pos, &readyQueue) {
        pcb_t *p = container_of(pos, pcb_t, p_list);
        if (p->p_pid == pid) return p;
    }
    // 3. Cerca nei processi bloccati (usando l'helper sopra)
    return NULL;
}

passupvector_t* passupvector;


void initKernel() {
    klog_print("init kernel");
    if (sizeof(pcb_t) > PAGESIZE) {
        klog_print("panic init kernel: pcb_t size exceeds page size\n");
        PANIC();
    }

    passupvector = (passupvector_t *) BIOSDATAPAGE;
    passupvector->tlb_refill_handler = (memaddr)uTLB_RefillHandler;
    passupvector->exception_handler = (memaddr)exceptionHandler;
    passupvector->tlb_refill_stackPtr = (memaddr)KERNELSTACK;
    passupvector->exception_stackPtr = (memaddr)KERNELSTACK;
    klog_print("passupvector initialized");
    initPcbs();
    initASL();
    klog_print("ASL initialized");

    processCount = 0;
    softBlockCount = 0;
    mkEmptyProcQ(&readyQueue);
    currentProcess = NULL;

    // Inizializza tutti i semafori dei dispositivi a 0 (liberi)
    int i;
    for (i = 0; i < SEMDEVLEN; i++) {
        deviceSemaphores[i] = 0;
    }

    // Inizializza il timer
    LDIT(PSECOND);

    // Inizializzazione del primo processo
    pcb_t *initProcess = allocPcb();
    
    // Initialize accumulated time to zero
    initProcess->p_time = 0;
    
    // Set all process tree fields to NULL
    initProcess->p_parent = NULL;
    mkEmptyProcQ(&(initProcess->p_child));
    mkEmptyProcQ(&(initProcess->p_sib));
    
    // Set blocking semaphore address to NULL
    initProcess->p_semAdd = NULL;
    
    // Set support structure pointer to NULL
    initProcess->p_supportStruct = NULL;
    
    // Configure processor state
    initProcess->p_s.mie = MIE_ALL;  // Enable interrupts
    initProcess->p_s.status = MSTATUS_MPIE_MASK | MSTATUS_MPP_M;  // Kernel mode on
    RAMTOP(initProcess->p_s.gpr[2]);
    initProcess->p_s.pc_epc = (memaddr)test;  // Set PC to test address
    
    // Place PCB in Ready Queue and increment Process Count
    list_add(&initProcess->p_list, &readyQueue);
    processCount++;

    scheduler();
}


void uTLB_RefillHandler()
{
    int prid = getPRID();
    setENTRYHI(0x80000000);
    setENTRYLO(0x00000000);
    TLBWR();
    LDST((state_t *)BIOSDATAPAGE);
}
