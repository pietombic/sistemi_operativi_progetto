#include "../headers/types.h"
#include "../headers/const.h"
#include "../headers/listx.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include "headers/exception.h"
#include "headers/interrupt.h"
#include <uriscv/liburiscv.h>
#include <uriscv/cpu.h>
#include <uriscv/arch.h>
#include "headers/klog.h"

extern void scheduler();

/* Helper per copiare lo stato in modo sicuro */
static void copyState(state_t *dst, state_t *src) {
    unsigned int *d = (unsigned int *) dst;
    unsigned int *s = (unsigned int *) src;
    for (int i = 0; i < STATE_T_SIZE_IN_BYTES / WORDLEN; i++) {
        d[i] = s[i];
    }
}

int highestPriorityPendingLine(){
    unsigned int cause = getCAUSE();
    for (int line = 1; line <= 7; line++) {
        if (CAUSE_IP_GET(cause, line)) {
            return line; 
        }
    }
    return -1; 
}
/*
void interruptHandler()
{
    cpu_t now;
    STCK(now);
    if (currentProcess != NULL) {
        currentProcess->p_time += (now - start_time_current_quantum);
    }
    start_time_current_quantum = now;

    int intlineNo = highestPriorityPendingLine();
    if (intlineNo == -1) return;

    state_t* savedState = GET_EXCEPTION_STATE_PTR(getPRID());

    if (intlineNo == 1) {
        setTIMER((cpu_t) NEVER); // Ferma il timer 
        
        if (currentProcess != NULL) {
            copyState(&currentProcess->p_s, savedState);
            currentProcess->p_s.status |= MSTATUS_MIE_MASK; // Assicura interrupt abilitati 
            insertProcQ(&readyQueue, currentProcess);
            currentProcess = NULL;
        }
        scheduler();
    } 
    else if (intlineNo == 2) {
        LDIT(PSECOND); // Reset 100ms 
        int* semAdd = &deviceSemaphores[SEMDEVLEN - 1];
        pcb_t* p;
        while ((p = removeBlocked(semAdd)) != NULL) {
            p->p_s.reg_a0 = 0;
            insertProcQ(&readyQueue, p);
            softBlockCount--; // Decrementa per ogni processo sbloccato 
        }
        if (currentProcess != NULL) LDST(savedState);
        else scheduler();
    } 
    else {
        int j = intlineNo - 3;
        unsigned int word = *((unsigned int *)(0x10000040 + (j * 0x04))); // Bitmap 
        
        int deviceNo = -1;
        for (int i = 0; i < 8; i++) {
            if (word & (1u << i)) { deviceNo = i; break; }
        }
        if (deviceNo == -1) {
            if (currentProcess != NULL) LDST(savedState);
            else scheduler();
            return;
        }

        unsigned int devAddrBase = 0x10000054 + (j * 0x80) + (deviceNo * 0x10);
        unsigned int* devRegPtr = (unsigned int*) devAddrBase;

        if (intlineNo == 7) {
            unsigned int txStatus = devRegPtr[2] & 0xFFu; 
            unsigned int rxStatus = devRegPtr[0] & 0xFFu; // RECV_STATUS 

            if (txStatus != 1 && txStatus != 3) { // Non READY o BUSY
                unsigned int savedStatus = devRegPtr[2];
                devRegPtr[3] = ACK; 
                int semIdx = 32 + deviceNo + 8; // Offset per TX Semaphores
                
                pcb_t* unblocked = removeBlocked(&deviceSemaphores[semIdx]);
                if (unblocked != NULL) {
                    unblocked->p_s.reg_a0 = savedStatus;
                    insertProcQ(&readyQueue, unblocked);
                    softBlockCount--;
                }
            }
            else if (rxStatus != 1 && rxStatus != 3) {
                unsigned int savedStatus = devRegPtr[0];
                devRegPtr[1] = ACK; // RECV_COMMAND 
                int semIdx = 32 + deviceNo; // Offset per RX Semaphores
                
                pcb_t* unblocked = removeBlocked(&deviceSemaphores[semIdx]);
                if (unblocked != NULL) {
                    unblocked->p_s.reg_a0 = (savedStatus >> 8) & 0xFFu; // Ritorna il carattere 
                    insertProcQ(&readyQueue, unblocked);
                    softBlockCount--;
                }
            }
        } else {
            unsigned int savedStatus = devRegPtr[0];
            devRegPtr[1] = ACK; // COMMAND register 
            int* semaphoreAddr = &deviceSemaphores[j * 8 + deviceNo];
            pcb_t* unblocked = removeBlocked(semaphoreAddr);
            if (unblocked != NULL) {
                unblocked->p_s.reg_a0 = savedStatus;
                insertProcQ(&readyQueue, unblocked);
                softBlockCount--;
            }
        }

        if (currentProcess != NULL) LDST(savedState);
        else scheduler();
    }
}
*/

#define INT_BITMAP_BASE  0x10000040
#define INT_BITMAP(line) (*((unsigned int *)(INT_BITMAP_BASE + ((line) - 3) * 0x4)))

/* Device register base address */
#define DEV_REG_BASE(line, dev) \
    ((unsigned int *)(START_DEVREG + ((line) - 3) * 0x80 + (dev) * 0x10))

#define DEV_STATUS(base)          ((base)[0])
#define DEV_COMMAND(base)         ((base)[1])

/* Terminal subdevices */
#define TERM_RECV_STATUS(base)    ((base)[0])
#define TERM_RECV_COMMAND(base)   ((base)[1])
#define TERM_TRANSM_STATUS(base)  ((base)[2])
#define TERM_TRANSM_COMMAND(base) ((base)[3])


#define DEV_SEM_BASE(line, dev) (((line) - 3) * 8 + (dev))
#define TERM_TX_SEM(dev)  (32 + (dev))
#define TERM_RX_SEM(dev)  (40 + (dev))

static int getHighestPriorityDevice(unsigned int bitmap) {
    for (int i = 0; i < 8; i++) {
        if (bitmap & (1u << i)) return i;
    }
    return -1;
}


void interruptHandler(void) {

    state_t *savedState = (state_t *) BIOSDATAPAGE;

    unsigned int cause   = savedState->cause;
    unsigned int excCode = cause & CAUSE_EXCCODE_MASK;

    /* CPU time accounting */
    cpu_t now;
    STCK(now);

    if (currentProcess != NULL) {
        currentProcess->p_time += (now - start_time_current_quantum);
    }
    start_time_current_quantum = now;


    /* ================================================================ */
    /* PLT interrupt (excCode == 7)                                     */
    /* ================================================================ */
    if (excCode == 7u) {

        /* Ack/disarm PLT */
        setTIMER((cpu_t) NEVER);

        if (currentProcess != NULL) {

            /* Salvo lo stato del processo al momento dell'interrupt */
            copyState(&currentProcess->p_s, savedState);

            /* Assicuro che, quando riparte, abbia gli interrupt abilitati */
            currentProcess->p_s.status |= MSTATUS_MIE_MASK;

            /* Round-robin: rimetto in ready queue */
            
            insertProcQ(&readyQueue, currentProcess);
            currentProcess = NULL;
        }

        scheduler();
        return;
    }

    /* ================================================================ */
    /* Interval timer (pseudo-clock) (excCode == 3)                     */
    /* ================================================================ */
    if (excCode == 3u) {

        /* Ack interval timer */
        LDIT(PSECOND);

        pcb_t *p;
        while ((p = removeBlocked(&deviceSemaphores[48])) != NULL) {
            p->p_semAdd   = NULL;
            p->p_s.reg_a0 = 0;
            insertProcQ(&readyQueue, p);
            softBlockCount--;
        }

        deviceSemaphores[48] = 0;

        if (currentProcess != NULL) {
            /* Nessun cambio di processo: riprende quello interrotto */
            LDST(savedState);
        } else {
            /* Nessun processo corrente: schedula qualcun altro */
            scheduler();
        }
        return;
    }

    /* ================================================================ */
    /* Device interrupts: excCode 17..21 => lines 3..7                  */
    /* ================================================================ */
    if (excCode >= 17u && excCode <= 21u) {

        int intLineNo = (int)(excCode - 14u); /* 17->3 ... 21->7 */
        unsigned int bitmap = INT_BITMAP(intLineNo);
        int devNo = getHighestPriorityDevice(bitmap);

        if (devNo < 0) {
            /* Spurious: nothing in bitmap */
            if (currentProcess != NULL) LDST(savedState);
            else scheduler();
            return;
        }

        /* Terminal line is 7 */
        if (intLineNo == 7) {

            unsigned int *termBase = DEV_REG_BASE(intLineNo, devNo);
            unsigned int txStatus = TERM_TRANSM_STATUS(termBase) & 0xFFu;
            unsigned int rxStatus = TERM_RECV_STATUS(termBase) & 0xFFu;

            /* TX ha priorità su RX */
            if (txStatus != READY && txStatus != BUSY) {

                unsigned int savedStatus = TERM_TRANSM_STATUS(termBase);
                TERM_TRANSM_COMMAND(termBase) = ACK;

                int semIdx = TERM_TX_SEM(devNo);

                if (deviceSemaphores[semIdx] < 0) {
                    deviceSemaphores[semIdx]++;
                    pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
                    if (unblocked != NULL) {
                        
                        unblocked->p_s.reg_a0 = savedStatus;
                        unblocked->p_semAdd   = NULL;
                        
                        insertProcQ(&readyQueue, unblocked);
                        softBlockCount--;
                    }
                }
            }

            if (rxStatus != READY && rxStatus != BUSY) {

                unsigned int savedStatus = TERM_RECV_STATUS(termBase);
                TERM_RECV_COMMAND(termBase) = ACK;

                int semIdx = TERM_RX_SEM(devNo);

                if (deviceSemaphores[semIdx] < 0) {
                    deviceSemaphores[semIdx]++;
                    pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
                    if (unblocked != NULL) {
                        unblocked->p_s.reg_a0 = savedStatus;
                        unblocked->p_semAdd   = NULL;
                        insertProcQ(&readyQueue, unblocked);
                        softBlockCount--;
                    }
                }
            }

        } else {

            /* Other device lines */
            unsigned int *devBase = DEV_REG_BASE(intLineNo, devNo);
            unsigned int savedStatus = DEV_STATUS(devBase);

            /* Ack */
            DEV_COMMAND(devBase) = ACK;

            int semIdx = DEV_SEM_BASE(intLineNo, devNo);

            if (deviceSemaphores[semIdx] < 0) {
                deviceSemaphores[semIdx]++;
                pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
                if (unblocked != NULL) {
                    unblocked->p_s.reg_a0 = savedStatus;
                    unblocked->p_semAdd   = NULL;
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

    /* ================================================================ */
    /* Unknown interrupt code: just resume/schedule                      */
    /* ================================================================ */
    if (currentProcess != NULL) LDST(savedState);
    else scheduler();
}
