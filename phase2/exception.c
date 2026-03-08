#include "../headers/types.h"
#include "../headers/const.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include "headers/exception.h"
#include "headers/interrupt.h"
#include <uriscv/liburiscv.h>
#include <uriscv/cpu.h>
#include <stddef.h>
#include "headers/klog.h"

static void copyState(state_t *dst, state_t *src) {
    unsigned int *d = (unsigned int *) dst;
    unsigned int *s = (unsigned int *) src;
    for (int i = 0; i < STATE_T_SIZE_IN_BYTES / WORDLEN; i++)
        d[i] = s[i];
}

static int isDeviceSemaphore(int *semAdd) {
    int *base = &deviceSemaphores[0];
    int *top  = &deviceSemaphores[SEMDEVLEN];

    if (semAdd < base || semAdd >= top) return 0;

    int idx = (int)(semAdd - base);
    if (idx == 48) return 0;   /* pseudo‑clock NON è device */

    return 1;
}

static void blockCurrentProcess(int *sem) {
    if (!currentProcess) PANIC();

    cpu_t current_tod;
    STCK(current_tod);
    currentProcess->p_time += current_tod - start_time_current_quantum;
    start_time_current_quantum = current_tod;

    currentProcess->p_semAdd = sem;
    if (isDeviceSemaphore(sem)) {
        softBlockCount++;
    }

    insertBlocked(sem, currentProcess);
    currentProcess = NULL;
    scheduler();
}



void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void exceptionHandler() {
    // 1. Ottieni lo stato salvato al momento dell'eccezione
    int cpu = getPRID(); // Ottieni il numero della CPU (core) che ha generato l'eccezione
    state_t *exceptionState = GET_EXCEPTION_STATE_PTR(cpu); 
    klog_print("exception handler called\n"); 

    // 2. Estrai il codice dell'eccezione usando la maschera
    unsigned int cause = getCAUSE(); 
    unsigned int excCode = cause & CAUSE_EXCCODE_MASK;
    klog_print("excCode = "); 
    klog_print_dec(excCode);
    klog_print("\n");

    if (CAUSE_IS_INT(cause)){
        // se è un interrupt
        interruptHandler();
    }else{
        // altrimenti gestiscila come eccezione
        if (excCode >= 24 && excCode <= 28) {
            klog_print("first exc\n");
            passUpOrDie(PGFAULTEXCEPT, exceptionState);
        } else if (excCode == SYSEXCEPTION || excCode == 11 || excCode == 8) {
            klog_print("second exc\n");
            systemCallHandler();
        } else {
            // Gestisce tutti i codici 0-7, 9, 10, 12-23 come Program Trap
            klog_print("third exc\n");
            passUpOrDie(GENERALEXCEPT, exceptionState);
        }
    }
}

void systemCallHandler() {
    // FIXIT: controllare perché non passa il giusto service code
    klog_print("system call handler\n");
    //Ottengo lo stato salvato 
    // FIXME: rimuovere il commento in int cpu
    int cpu = getPRID();
    state_t *state = GET_EXCEPTION_STATE_PTR(cpu);
     

    //Leggo il codice del servizio da a0
    int service_code = state->reg_a0;

    klog_print("service code = ");
    klog_print_dec(service_code);
    klog_print("\n");

    //Controllo Kernel-mode per SYSCALL negative
    if (service_code < 0) {
        unsigned int status = state->status;
        //verfica di essere in user mode, se sì simulo un program trap per istruzione privilegiata
        if ((status & MSTATUS_MPP_MASK) == MSTATUS_MPP_U) {
            //Simula Program Trap per istruzione privilegiata
            state->cause = (state->cause & CLEAREXECCODE) | (PRIVINSTR << CAUSESHIFT);
            programTrapHandler(); 
            return;
        }
    }
    //Smistamento dei servizi
    switch (service_code) {
        case CREATEPROCESS: // -1
            createProcess(state);
            break;
        case TERMPROCESS:   // -2
            terminateProcess(state);
            break;
        case PASSEREN:      // -3
            waitSemaphore(state);
            break;
        case VERHOGEN:      // -4
            signalSemaphore(state);
            break;
        case DOIO:           // -5
            klog_print("doIO syscall\n");
            doIO(state);
            break;
        case GETTIME:     // -6
            getCPUTime(state);
            break;
        case CLOCKWAIT:    // -7
            waitForClock(state);
            break;
        case GETSUPPORTPTR:   // -8
            getSupportData(state);
            break;
        case GETPROCESSID:     // -9
            getProcessID(state);
            break;
        case YIELD :           // -10
            yield(state);
            break;
        
        default:
            // Se a0 >= 1 o è un codice non gestito, 
            // passa la gestione al Support Level (Pass Up or Die)
            passUpOrDie(GENERALEXCEPT, state);
            break;
    }
}

void createProcess(state_t *state) {
    pcb_t *newPcb = allocPcb(); 
    if (newPcb == NULL) {
        state->reg_a0 = -1; //non ci sono PCB liberi
    }else {
        if(state->reg_a1 == NULL){
            state->reg_a0 = -1;
        }else{
            newPcb->p_s = *((state_t *)state->reg_a1); //copio lo stato passato da a1 al nuovo PCB
        }
        newPcb->p_supportStruct = (support_t *)state->reg_a3; //collego il support struct passato da a3 al nuovo PCB
        newPcb->p_prio = state->reg_a2; //imposto la priorità del nuovo processo a quella passata da a2
        newPcb->p_time = 0; //inizializzo il tempo di CPU usato a 0
        newPcb->p_semAdd = NULL; //inizializzo il puntatore al semaforo a NULL

        insertChild(currentProcess, newPcb); //inserisco il nuovo PCB come figlio del processo corrente
        insertProcQ(&readyQueue, newPcb); //inserisco il nuovo PCB nella ready queue

        processCount++; //incremento il contatore dei processi
        state->reg_a0 = newPcb->p_pid; //ritorno il PID del nuovo processo in a0

    }
    state->pc_epc += 4; //incremento il PC per evitare di rieseguire la system call

    LDST(state); //carico lo stato aggiornato
}

void terminateProcess(state_t *state) {
    int pid = state->reg_a1; //leggo il PID del processo da terminare da a0
    pcb_t *target;
    // Prima di terminare, se il bersaglio siamo noi, aggiorniamo il nostro tempo finale
    if (pid == 0 || (currentProcess != NULL && currentProcess->p_pid == pid)) {
        updateCPUTime(currentProcess);
    }

    if (pid == 0) {
        target = currentProcess; //se il PID è 0, termina il processo corrente
    } else {
        target = findProcessByPID(pid); //altrimenti, cerco il processo con il PID specificato
    }
    if (target != NULL) {
        recursiveTerminate(target); //se il processo esiste, lo termino ricorsivamente
    }
    if (currentProcess == NULL || target == currentProcess) {
        scheduler(); //se il processo corrente è stato terminato, invoco lo scheduler per scegliere un nuovo processo da eseguire
    } else { //se abbiamo terminato qualcun altro ritorno al processo chiamante
        state->pc_epc += 4; //altrimenti, incremento il PC per evitare di rieseguire la system call
        LDST(state); //carico lo stato aggiornato
    }
}

void recursiveTerminate(pcb_t *p) {
    while (!emptyChild(p)) {
        recursiveTerminate(removeChild(p));
    }

    if (p->p_semAdd != NULL) {
        int isDeviceSem = (p->p_semAdd >= &deviceSemaphores[0] &&
                           p->p_semAdd <  &deviceSemaphores[SEMDEVLEN]);
        outBlocked(p);
        if (isDeviceSem) softBlockCount--;
    } else if (p != currentProcess) {
        outProcQ(&readyQueue, p);
    }

    if (p->p_parent != NULL) {
        outChild(p);
    }

    processCount--;
    freePcb(p);
}

void waitSemaphore(state_t *state) {
    int *semAdd = (int *)state->reg_a1; //leggo l'indice del semaforo da a0
    (*semAdd)--;

    if ((*semAdd) < 0) {        
        //Incrementa il PC di 4 per evitare loop infiniti al risveglio
        state->pc_epc += 4;
        //Copia lo stato salvato nel PCB del processo corrente 
        currentProcess->p_s = *state;
        //Aggiorna il tempo di CPU accumulato per il processo corrente
        updateCPUTime(currentProcess);
        //Inserisce il PCB nell'ASL e lo blocca sul semaforo, se insertBlocked ritorna TRUE, i semafori sono finiti e si va in PANIC
        if (insertBlocked(semAdd, currentProcess)) {
            klog_print("wait semaphore \n");
            PANIC(); 
        }
        // 5. Chiama lo Scheduler per far partire un altro processo
        currentProcess = NULL;
        scheduler();
    } else {
        //risorsa disponibile: incrementa PC e restituisci il controllo al processo chiamante
        state->pc_epc += 4;
        LDST(state);
    }
}

// FIXME: non aumenta il valore della variabile del semaforo, ma solo quella del processo chiamante, da capire se è un problema o no
void signalSemaphore(state_t *state) {
    int *semIndex = (int *)state->reg_a1;
    (*semIndex)++;
    if ((*semIndex) <= 0) {
        // c era qualcuno bloccato, lo sblocchiamo e lo mettiamo nella Ready Queue
        pcb_t *releasedPcb = removeBlocked(semIndex);
        if (releasedPcb != NULL) {
            // Inserisce il processo sbloccato nella Ready Queue
            insertProcQ(&readyQueue, releasedPcb);
        }
    }
    // L'operazione V non blocca mai il chiamante, quindi incrementa il PC e restituisce il controllo al processo chiamante
    state->pc_epc += 4;
    LDST(state);
}

void doIO(state_t *state) {
    klog_print("doIO entered\n");
    int *commandAddr = (int *)state->reg_a1; 
    int commandValue = (int) state->reg_a2;

    unsigned int offset = (unsigned int)commandAddr - START_DEVREG;
    int line = (int)(offset / 0x80) + 3; // line 3-7
    int dev = (int)((offset % 0x80) / 0x10); // dev 0-7
    int subword = (int)((offset % 0x10) / WORDLEN); 
    
    int semIndex = (line == 7)
        ? ((subword == 3) ? 32+dev : 40+dev)
        : (((line - 3) * 8) + dev);
    klog_print("CHECKPOINT 1 \n");
    // FIXME: problema qui, chiama un exception PERCHEEE
    *commandAddr = commandValue; // scrivo il comando nel registro del dispositivo
    klog_print("CHECKPOINT 2 \n");
    updateCPUTime(currentProcess); 
    copyState(&currentProcess->p_s, state);

    unsigned int *devBase = (unsigned int *) (START_DEVREG + (line - 3) * 0x80 + dev * 0x10);
    int ready = 0;

    if (line == 7) {
                if (subword == 3) {
                    if (!(devBase[3] & 0x1)) {
                        deviceSemaphores[semIndex]--;
                        blockCurrentProcess(&deviceSemaphores[semIndex]);
                    } else {
                        *commandAddr = commandValue;
                        devBase[3] = 1;
                    }
                } else {
                    if (!(devBase[1] & 0x1)) {
                        deviceSemaphores[semIndex]--;
                        blockCurrentProcess(&deviceSemaphores[semIndex]);
                    } else {
                        devBase[1] = 1;
                    }
                }
            } else {
                ready = devBase[1] & 0x1;
                if (!ready) {
                    deviceSemaphores[semIndex]--;
                    blockCurrentProcess(&deviceSemaphores[semIndex]);
                }
            }
    klog_print("doIO exiting\n");
}


void getCPUTime(state_t *state) {
    cpu_t current_tod;
    //Leggo il valore attuale del clock TOD 
    STCK(current_tod);
    //Calcolo il tempo totale: tempo_accumulato + (ora_attuale - ora_inizio_esecuzione)
    cpu_t total_time = currentProcess->p_time + (current_tod - start_time_current_quantum);

    state->reg_a0 = total_time;
    state->pc_epc += 4;
    LDST(state);
}

//DA CAPIRE !!! 
void waitForClock(state_t *state) {
    //prendo il semaforo dello pseudo-clock
    int *semAdd = &deviceSemaphores[SEMDEVLEN - 1];

    state->pc_epc += 4;
    currentProcess->p_s = *state;
    updateCPUTime(currentProcess);
    //se un processo tenta di bloccarsi su un semaforo nuovo ma la lista semdFree_h è vuota,
    //insertBlocked restituisce TRUE  per segnalare che non è stato possibile completare l'operazione.
    if (insertBlocked(semAdd, currentProcess)) {
        klog_print("wait for clock: semdFree_h is empty\n");
        PANIC(); // Errore se non ci sono descrittori di semaforo liberi [cite: 112]
    }
    softBlockCount++;
    currentProcess = NULL;
    scheduler();
}

void getSupportData(state_t *state) {
    state->reg_a0 = (unsigned int)currentProcess->p_supportStruct;
    state->pc_epc += 4;
    LDST(state);
}

void getProcessID(state_t *state) {
    int requestedParent = state->reg_a1;
    
    //devo restituire il PID del processo chiamante
    if (requestedParent == 0) {
        state->reg_a0 = currentProcess->p_pid;
    } else {
        //restituisco il pid del genitore se esiste
        if (currentProcess->p_parent != NULL) {
            state->reg_a0 = currentProcess->p_parent->p_pid; 
        } else {
            state->reg_a0 = 0; 
        }
    }
    state->pc_epc += 4;

    LDST(state); 
}

void yield(state_t *state) {
    state->pc_epc += 4; 
    currentProcess->p_s = *state;
    updateCPUTime(currentProcess); 
    //inserisco il processo corrente nella Ready Queue (da ready diventa running)
    insertProcQ(&readyQueue, currentProcess);
    currentProcess = NULL;
    scheduler(); 
}

void passUpOrDie(int exceptionType, state_t *exceptionState) {
    if (currentProcess == NULL) {
        PANIC(); // non c'è nessun processo a cui "pass up" o "die"
    }

    if (currentProcess->p_supportStruct == NULL) {
        // DIE: termina il processo corrente (non una syscall terminate con parametri)
        pcb_t *victim = currentProcess;
        currentProcess = NULL;
        recursiveTerminate(victim);
        scheduler(); // non ritorna
    } else {
        updateCPUTime(currentProcess);
        currentProcess->p_supportStruct->sup_exceptState[exceptionType] = *exceptionState;
        context_t ctx = currentProcess->p_supportStruct->sup_exceptContext[exceptionType];
        LDCXT(ctx.stackPtr, ctx.status, ctx.pc); // non ritorna
    }
}
void programTrapHandler() {
    // Handle program trap exception
    // Get the current exception state
    int cpu = getPRID(); // Ottieni il numero della CPU (core) che ha generato l'eccezione
    state_t *exceptionState = GET_EXCEPTION_STATE_PTR(cpu);
    
    // Pass up to support level or terminate
    passUpOrDie(GENERALEXCEPT, exceptionState);
}

int *getDeviceSemaphore(int *commandAddr) {
    unsigned int addr = (unsigned int)commandAddr;
    klog_print("getDeviceSemaphore: addr = ");
    klog_print_hex(addr);
    klog_print("\n");

    // Workaround: se l'indirizzo è 0xB, lo interpretiamo come terminale 0 trasmissione
    if (addr == 0xB) {
        klog_print("  -> workaround: using terminal 0 transmit semaphore (index 40)\n");
        return &deviceSemaphores[40];
    }

    if (addr < 0x10000054 || addr > 0x100002D4) {
        klog_print("  -> out of range\n");
        return NULL;
    }

    unsigned int offset = addr - 0x10000054;
    int intLineNo = (offset / 0x80) + 3;
    int devNo = (offset % 0x80) / 0x10;
    int index;

    if (intLineNo == 7) {
        unsigned int devOffset = offset % 0x10;
        if (devOffset < 0x8)
            index = 32 + devNo;
        else
            index = 40 + devNo;
    } else {
        index = (intLineNo - 3) * 8 + devNo;
    }

    klog_print("  -> line="); klog_print_dec(intLineNo);
    klog_print(" dev="); klog_print_dec(devNo);
    klog_print(" index="); klog_print_dec(index);
    klog_print("\n");

    return &deviceSemaphores[index];
}