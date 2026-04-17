#include "headers/interrupt.h"
#include "../headers/const.h"
#include "../headers/types.h"
#include "../phase1/headers/asl.h"
#include "../phase1/headers/pcb.h"
#include "headers/exception.h"
#include "headers/functions.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include <uriscv/arch.h>
#include <uriscv/cpu.h>
#include <uriscv/liburiscv.h>

extern void scheduler();

// Indirizzi di base per i registri dei dispositivi
#define INT_BITMAP_BASE 0x10000040
#define INT_BITMAP(line)                                                       \
  (*((unsigned int *)(INT_BITMAP_BASE + ((line) - 3) * 0x4)))

#define DEV_REG_BASE(line, dev)                                                \
  ((unsigned int *)(START_DEVREG + ((line) - 3) * 0x80 + (dev) * 0x10))

#define DEV_STATUS(base) ((base)[0])
#define DEV_COMMAND(base) ((base)[1])

// Terminali
#define TERM_RECV_STATUS(base) ((base)[0])
#define TERM_RECV_COMMAND(base) ((base)[1])
#define TERM_TRANSM_STATUS(base) ((base)[2])
#define TERM_TRANSM_COMMAND(base) ((base)[3])

#define DEV_SEM_BASE(line, dev) (((line) - 3) * 8 + (dev))
#define TERM_TX_SEM(dev) (40 + (dev))
#define TERM_RX_SEM(dev) (32 + (dev))

// Funzione che restituisce il dispositivo con priorità più alta
static int getHighestPriorityDevice(unsigned int bitmap) {
  for (int i = 0; i < 8; i++) {
    if (bitmap & (1u << i))
      return i;
  }
  return -1;
}

void interruptHandler(void) {

  // Salvo lo stato del processo al momento dell'interrupt
  state_t *savedState = (state_t *)BIOSDATAPAGE;

  unsigned int cause = savedState->cause;
  unsigned int excCode = cause & CAUSE_EXCCODE_MASK;

  // Contabilizzo il tempo CPU del processo corrente
  cpu_t now;
  STCK(now);

  if (currentProcess != NULL) {
    currentProcess->p_time += (now - start_time_current_quantum);
  }
  start_time_current_quantum = now;

  // PLT interrupt
  if (excCode == 7u) {

    // Disabilito il timer
    setTIMER((cpu_t)NEVER);

    if (currentProcess != NULL) {

      // Salvo lo stato del processo
      copyState(&currentProcess->p_s, savedState);

      // Abilito gli interrupt
      currentProcess->p_s.status |= MSTATUS_MIE_MASK;

      // Round-robin: rimetto in ready queue
      insertProcQ(&readyQueue, currentProcess);
      currentProcess = NULL;
    }

    scheduler();
    return;
  }

  // Interval timer (pseudo-clock)
  if (excCode == 3u) {

    // Ack interval timer
    LDIT(PSECOND);

    pcb_t *p;
    while ((p = removeBlocked(&deviceSemaphores[48])) != NULL) {
      p->p_semAdd = NULL;
      p->p_s.reg_a0 = 0;
      insertProcQ(&readyQueue, p);
      softBlockCount--;
    }

    deviceSemaphores[48] = 0;

    if (currentProcess != NULL) {
      // riprende il processo interrotto
      LDST(savedState);
    } else {
      scheduler();
    }
    return;
  }

  // Device interrupts
  if (excCode >= 17u && excCode <= 21u) {

    int intLineNo = (int)(excCode - 14u);
    unsigned int bitmap = INT_BITMAP(intLineNo);
    // Prendo il dispositivo con priorità più alta
    int devNo = getHighestPriorityDevice(bitmap);

    if (devNo < 0) {
      if (currentProcess != NULL)
        LDST(savedState);
      else
        scheduler();
      return;
    }

    // Se il dispositivo è un terminale
    if (intLineNo == 7) {

      unsigned int *termBase = DEV_REG_BASE(intLineNo, devNo);
      unsigned int txStatus = TERM_TRANSM_STATUS(termBase) & 0xFFu;
      unsigned int rxStatus = TERM_RECV_STATUS(termBase) & 0xFFu;

      // Gestione del sotto-dispositivo di trasmissione: invia ACK, sblocca il
      // processo in attesa dell'operazione di output e gli restituisce lo
      // stato del terminale tramite il registro a0.
      // TX -> dispositivo di trasmissione
      if ((txStatus & 0xFF) != 0) {

        unsigned int savedStatus = TERM_TRANSM_STATUS(termBase);
        TERM_TRANSM_COMMAND(termBase) = ACK;

        int semIdx = TERM_TX_SEM(devNo);

        if (deviceSemaphores[semIdx] < 0) {
          deviceSemaphores[semIdx]++;
          pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
          if (unblocked != NULL) {

            unblocked->p_s.reg_a0 = savedStatus;
            unblocked->p_semAdd = NULL;

            insertProcQ(&readyQueue, unblocked);
            softBlockCount--;
          }
        }
      }

      // Gestione del sotto-dispositivo di ricezione: invia ACK, sblocca il
      // processo in attesa dell'operazione di input e gli restituisce lo
      // stato del terminale tramite il registro a0.
      // RX -> dispositivo di ricezione
      if ((rxStatus & 0xFF) != 0) {

        unsigned int savedStatus = TERM_RECV_STATUS(termBase);
        TERM_RECV_COMMAND(termBase) = ACK;

        int semIdx = TERM_RX_SEM(devNo);

        if (deviceSemaphores[semIdx] < 0) {
          deviceSemaphores[semIdx]++;
          pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
          if (unblocked != NULL) {
            unblocked->p_s.reg_a0 = savedStatus;
            unblocked->p_semAdd = NULL;
            insertProcQ(&readyQueue, unblocked);
            softBlockCount--;
          }
        }
      }

    } else {

      // Gestione degli altri dispositivi
      unsigned int *devBase = DEV_REG_BASE(intLineNo, devNo);
      unsigned int savedStatus = DEV_STATUS(devBase);

      // Invio ACK
      DEV_COMMAND(devBase) = ACK;

      int semIdx = DEV_SEM_BASE(intLineNo, devNo);

      // Sblocco il processo in attesa dell'operazione
      if (deviceSemaphores[semIdx] < 0) {
        deviceSemaphores[semIdx]++;
        pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
        if (unblocked != NULL) {
          unblocked->p_s.reg_a0 = savedStatus;
          unblocked->p_semAdd = NULL;
          insertProcQ(&readyQueue, unblocked);
          softBlockCount--;
        }
      }
    }

    if (currentProcess != NULL) {
      LDST(savedState);
    } else {
      scheduler();
    }
    return;
  }

  if (currentProcess != NULL)
    LDST(savedState);
  else
    scheduler();
}
