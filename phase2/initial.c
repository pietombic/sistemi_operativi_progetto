#include "../headers/types.h"
#include "../phase1/headers/asl.h"
#include "../phase1/headers/pcb.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include "headers/exception.h"
#include <uriscv/liburiscv.h>
#include "headers/klog.h"

extern void test();
extern void scheduler(); // 
extern void exceptionHandler(); // 

/* Global variables definitions */
int processCount;
int softBlockCount;
struct list_head readyQueue;
pcb_t *currentProcess;
cpu_t start_time_current_quantum;
int deviceSemaphores[SEMDEVLEN];
pcb_t *activeProcesses[MAXPROC];

void updateCPUTime(pcb_t *p) {
    if (p) {
        cpu_t current_tod;
        STCK(current_tod); // Legge il valore attuale del TOD clock [cite: 418, 419]

        // Aggiunge la differenza al tempo accumulato nel PCB [cite: 414, 417]
        p->p_time += (current_tod - start_time_current_quantum);

        // Aggiorna il tempo di inizio per l'eventuale prossimo intervallo
        start_time_current_quantum = current_tod;
    }
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
    if (sizeof(pcb_t) > PAGESIZE) {
        PANIC();
    }

    memaddr ramtop;
    RAMTOP(ramtop);

    passupvector = (passupvector_t *) (BIOSDATAPAGE + 0x900);
    passupvector->tlb_refill_handler = (memaddr)uTLB_RefillHandler;
    passupvector->exception_handler = (memaddr)exceptionHandler;
    //passupvector->tlb_refill_stackPtr = ramtop;
    passupvector->tlb_refill_stackPtr = KERNELSTACK;
    //passupvector->exception_stackPtr = ramtop - PAGESIZE;
    passupvector->exception_stackPtr = KERNELSTACK;

    initPcbs();
    initASL();

    processCount = 0;
    softBlockCount = 0;
    mkEmptyProcQ(&readyQueue);
    currentProcess = NULL;

    // Inizializza tutti i semafori dei dispositivi a 0 (liberi)
    for (int i = 0; i < SEMDEVLEN; i++) {
        deviceSemaphores[i] = 0;
    }
    //inizializzo tutti i puntatori ai processi a NULL
    for (int i = 0; i < MAXPROC; i++) {
        activeProcesses[i] = NULL;
    }

    // Inizializza il timer
    LDIT(PSECOND);

    // Inizializzazione del primo processo
    pcb_t *initProcess = allocPcb();
    
    initProcess->p_time = 0;
    initProcess->p_parent = NULL;
    initProcess->p_semAdd = NULL;
    initProcess->p_supportStruct = NULL;
    initProcess->p_s.mie = MIE_ALL;  // Enable interrupts
    initProcess->p_s.status = MSTATUS_MPIE_MASK | MSTATUS_MPP_M | MSTATUS_MIE_MASK;  
    initProcess->p_s.pc_epc = (memaddr) test;
    initProcess->p_prio = 0; // Priority 0 for the initial process
    //initProcess->p_s.reg_sp = ramtop - (2 * PAGESIZE); // Stack pointer for the initial process
    initProcess->p_s.reg_sp = ramtop;
    //RAMTOP(initProcess->p_s.gpr[2]);
    
    activeProcesses[0] = initProcess; // Salva il puntatore al processo iniziale nell'array dei processi attivi
    insertProcQ(&readyQueue, initProcess);
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
/*
void uTLB_RefillHandler() {
    unsigned int entryHI = getENTRYHI();  // contiene il VPN che ha causato il miss
    
    // Identity mapping: VPN → stessa PFN, valida e dirty
    setENTRYHI(entryHI);
    setENTRYLO((entryHI & 0xFFFFF000) | DIRTYON | VALIDON | GLOBALON);
    
    TLBWR();
    LDST((state_t *)BIOSDATAPAGE);
}
*/