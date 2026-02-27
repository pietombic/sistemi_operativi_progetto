#include "headers/types.h"

#include "exception.c"
#include "phase1/asl.c"
#include "phase1/pcb.c"

int processCount;
int softBlockCount;
struct list_head readyQueue;
pcb_t *currentProcess;

/* Device semaphore */
int deviceSemaphores[SEMDEVLEN];


typedef struct passupvector_t {
    memaddr tlb_refll_handler;
    memaddr exception_handler;
    memaddr tlb_refll_stackPtr;
    memaddr exception_stackPtr;
} passupvector_t;

passupvector_t* passupvector;

void uTLB_RefillHandler()
{
    int prid = getPRID();
    setENTRYHI(0x80000000);
    setENTRYLO(0x00000000);
    TLBWR();
    LDST((state_t *)BIOSDATAPAGE);
}


int main() {
    if (sizeof(pcb_t) > PAGESIZE) {
        PANIC();
    }
    passupvector->tlb_refll_handler = (memaddr)uTLB_RefillHandler;
    passupvector->exception_handler = (memaddr)exceptionHandler;
    passupvector->tlb_refll_stackPtr = (memaddr)KERNELSTACK;
    passupvector->exception_stackPtr = (memaddr)KERNELSTACK;

    initPcbs();
    initASL();

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
    initProcess->p_s.pc_epc = test;  // Set PC to test address
    
    // Place PCB in Ready Queue and increment Process Count
    list_add(&initProcess->p_list, &readyQueue);
    processCount++;

    scheduler();

    

}

extern void test();