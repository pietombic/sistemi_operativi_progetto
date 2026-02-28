#include "../headers/types.h"
#include "../headers/const.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "initial.c"



void exceptionHandler() {
    int cause = getCAUSE();
    if (CAUSE_IS_INT()){
        interruptHandler();
    }else{
        if(cause >= 24 && cause <= 28){
            // TLB exception
        }
        else if(cause == 8 || cause == 11){
            systemCallHandler();
        }
        else if(cause >= 0 && cause <= 7 || cause == 10 || cause >= 12 && cause <= 23){
            // Program trap
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