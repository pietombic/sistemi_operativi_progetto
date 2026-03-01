#include "../headers/types.h"
#include "../headers/const.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "initial.c"



void exceptionHandler() {
    // 1. Ottieni lo stato salvato al momento dell'eccezione
    state_t *exceptionState = GET_EXCEPTION_STATE_PTR(0); 

    // 2. Estrai il codice dell'eccezione usando la maschera
    unsigned int cause = exceptionState->cause; [cite: 128]
    unsigned int excCode = (cause & GETEXECCODE) >> CAUSESHIFT;

    if (CAUSE_IS_INT(cause)){
        interruptHandler();
    }else{
        if(cause >= 24 && cause <= 28){
            tlbExceptionHandler();
        }
        else if(cause == 8 || cause == 11){
            systemCallHandler();
        }
        else if(cause >= 0 && cause <= 7 || cause == 10 || cause >= 12 && cause <= 23){
            programTrapHandler();
        }
        else{
            // Unrecognized exception code
            PANIC();
        }

    }
}

void systemCallHandler() {
    //Ottengo lo stato salvato 
    state_t *state = GET_EXCEPTION_STATE_PTR(0); 

    //Leggo il codice del servizio da a0
    int service_code = state->reg_a0;

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
            doIO(state);
            break;
        case GETTIME:     // -6
            getCPUTime(state);
            break;
        case CLOCKWAIT:    // -7
            waitForClock(state);
            break;
        case GETSUPPORTPTR:   // -8
            getSupportPtr(state);
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
        newPcb->p_s = *((state_t *)state->reg_a1); //copio lo stato passato da a1 al nuovo PCB
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
    int pid = state->reg_a0; //leggo il PID del processo da terminare da a0
    pcb_t *target;
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
void recursiveTerminate(pcb_t *p){
    while (!emptyChild(p)) {
        recursiveTerminate(removeChild(p));
    }
    //rimuovo dalle code (Ready o ASL)
    if (p->p_semAdd != NULL) {
        // Se è bloccato su un semaforo, rimuovilo e aggiorna soft-block
        outBlocked(p);
        softBlockCount--;
    } else if (p != currentProcess) {
        // Se è nella Ready Queue
        outProcQ(&readyQueue, p);
    }
    // rimuovo dal genitore (se non è già stato fatto)
    outChild(p);
    //aggiorno contatore globale e libera PCB
    processCount--;
    freePcb(p);
}

void waitSemaphore(state_t *state) {
    int semIndex = (int *)state->reg_a0; //leggo l'indice del semaforo da a0
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

    //poiché i semafori dei dispositivi sono inizializzati a 0, questa operazione bloccherà sempre il processo.
    if (insertBlocked(semAdd, currentProcess)) {
        PANIC(); // Errore critico se non ci sono descrittori di semaforo liberi
    }
    //il processo è ora in stato "waiting" per I/O
    softBlockCount++; 
    // Il processo corrente non è più "running"
    currentProcess = NULL; 
    scheduler(); 
}

void getCpuTime(state_t *state) {
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

void programTrapHandler() {

}

void tlbExceptionHandler() { 
    state_t *oldState = GET_EXCEPTION_STATE_PTR(0);

    /* DIE: se non c'è processo corrente o non ha supportStruct */
    if (currentProcess == NULL || currentProcess->p_supportStruct == NULL) {
        /* terminateProcess legge il PID=0 del processo corrente */
        oldState->reg_a0 = 0;
        terminateProcess(oldState);

        /* Se terminateProcess ritorna comunque, schedula */
        scheduler();
        return;
    }

    /* PASS UP: salva lo stato dell'eccezione e salta al support handler */
    support_t *sup = currentProcess->p_supportStruct;
    sup->sup_exceptState[PGFAULTEXCEPT] = *oldState;
    LDST(&sup->sup_exceptContext[PGFAULTEXCEPT]);

    /* Non si ritorna mai da LDST */
}
