#include "headers/initial.h"
#include "../headers/types.h"
#include "../phase1/headers/asl.h"
#include "../phase1/headers/pcb.h"
#include "headers/exception.h"
#include "headers/klog.h"
#include "headers/scheduler.h"
#include <uriscv/liburiscv.h>

extern void test();
extern void scheduler();
extern void exceptionHandler();

/* Global variables definitions */
int processCount;
int softBlockCount;
int globalLock;
struct list_head readyQueue;
pcb_t *currentProcess;
cpu_t start_time_current_quantum;
int deviceSemaphores[SEMDEVLEN];
pcb_t *rootProcess;
passupvector_t *passupvector;

void initKernel() {
  if (sizeof(pcb_t) > PAGESIZE) {
    PANIC();
  }

  memaddr ramtop;
  RAMTOP(ramtop);

  // Inizializzazione del passupvector
  passupvector = (passupvector_t *)(BIOSDATAPAGE + 0x900);
  passupvector->tlb_refill_handler = (memaddr)uTLB_RefillHandler;
  passupvector->exception_handler = (memaddr)exceptionHandler;
  // passupvector->tlb_refill_stackPtr = ramtop;
  passupvector->tlb_refill_stackPtr = KERNELSTACK;
  // passupvector->exception_stackPtr = ramtop - PAGESIZE;
  passupvector->exception_stackPtr = KERNELSTACK;

  initPcbs();
  initASL();

  globalLock = 0;
  processCount = 0;
  softBlockCount = 0;
  mkEmptyProcQ(&readyQueue);
  currentProcess = NULL;

  // Inizializza tutti i semafori dei dispositivi a 0 (liberi)
  for (int i = 0; i < SEMDEVLEN; i++) {
    deviceSemaphores[i] = 0;
  }
  // Inizializza il timer
  LDIT(PSECOND);

  // Inizializzazione del primo processo
  pcb_t *initProcess = allocPcb();

  initProcess->p_time = 0;
  initProcess->p_parent = NULL;
  initProcess->p_semAdd = NULL;
  initProcess->p_supportStruct = NULL;
  initProcess->p_s.mie = MIE_ALL; // Enable interrupts
  initProcess->p_s.status =
      MSTATUS_MPIE_MASK | MSTATUS_MPP_M | MSTATUS_MIE_MASK;
  initProcess->p_s.pc_epc = (memaddr)test;
  initProcess->p_prio = 0;
  // initProcess->p_s.reg_sp = ramtop - (2 * PAGESIZE); // Stack pointer for the
  // initial process
  initProcess->p_s.reg_sp = ramtop;
  // RAMTOP(initProcess->p_s.gpr[2]);

  rootProcess = initProcess;
  // Inserisco il processo root nella coda dei processi pronti
  insertProcQ(&readyQueue, initProcess);
  processCount++;

  scheduler();
}