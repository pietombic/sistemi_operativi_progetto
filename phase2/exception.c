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
    // 1. Recupero parametri dai registri
    // a1: indirizzo del campo comando del dispositivo
    // a2: valore del comando da scrivere
    klog_print("do IO \n");
    int *commandAddr = (int *)state->reg_a1;
    int commandValue = state->reg_a2;


    // 2. Calcolo dell'indice del semaforo di dispositivo
    // Il Nucleus mantiene un array di semafori per i dispositivi (Device Semaphores)
    // Devi mappare l'indirizzo del registro al semaforo corrispondente
    int *semAdd = getDeviceSemaphore(commandAddr); 

    // Scrittura del comando nel registro del dispositivo
    *commandAddr = commandValue; 

    //incremento il PC di 4 per puntare all'istruzione successiva al risveglio
    state->pc_epc += 4; 

    //copio lo stato attuale nel PCB del processo corrente
    currentProcess->p_s = *state; 

    updateCPUTime(currentProcess); 
    klog_print("do IO: updated CPU time\n");

    //poiché i semafori dei dispositivi sono inizializzati a 0, questa operazione bloccherà sempre il processo.
    if (insertBlocked(semAdd, currentProcess)) {
        klog_print("panic do IO \n");
        PANIC(); // Errore critico se non ci sono descrittori di semaforo liberi
    }
    //il processo è ora in stato "waiting" per I/O
    softBlockCount++; 
    klog_print("do IO: process blocked, softBlockCount incremented\n");
    // Il processo corrente non è più "running"
    currentProcess = NULL; 
    klog_print("do IO: current process set to NULL\n");
    scheduler(); 
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
    // Map device command register address to semaphore index
    // Device registers are at 0x10000054 + ((line - 3) * 0x80) + (deviceNo * 0x10)
    // Semaphores are at deviceSemaphores[(line - 3) * 8 + deviceNo]
    
    unsigned int addr = (unsigned int)commandAddr;
    
    // Calculate device line and device number from address
    // This is a simplified mapping - adjust based on your actual device layout
    if (addr >= 0x10000054) {
        unsigned int offset = addr - 0x10000054;
        int line = (offset / 0x80) + 3;
        int device = (offset % 0x80) / 0x10;
        
        if (line >= 3 && line <= 9 && device >= 0 && device <= 7) {
            return &deviceSemaphores[(line - 3) * 8 + device];
        }
    }
    
    // Default: return NULL if mapping fails
    return NULL;
}

