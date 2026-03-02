#include "../headers/types.h"
#include "../headers/const.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include <uriscv/liburiscv.h>

void scheduler() {
    // 1. Tenta di rimuovere il prossimo PCB dalla Ready Queue
    currentProcess = removeProcQ(&readyQueue); 

    if (currentProcess != NULL) {
        //Carica 5 millisecondi sul PLT per il time slice (quanto di tempo)
        setTIMER(TIMESLICE); 

        //Salva il momento esatto in cui il processo inizia a correre
        STCK(start_time_current_quantum); 

        //Carica lo stato del processo e lancia l'esecuzione
        LDST(&(currentProcess->p_s)); 
    } 
    else {
        // Gestione quando la Ready Queue è vuota 
        
        if (processCount == 0) {
            // Caso 1: Tutti i processi terminati con successo
            HALT(); 
        } 
        else if (processCount > 0 && softBlockCount > 0) {
            // Caso 2: Processi esistenti ma tutti bloccati in attesa di I/O o timer
            
            // Abilita gli interrupt e disabilita il PLT prima del WAIT 
            setMIE(getMIE() & ~MIE_MTIE_MASK);
            unsigned int status = getSTATUS(); 
            status |= MSTATUS_MIE_MASK; 
            setSTATUS(status); 
            
            // Entra in Wait State (attesa di un interrupt di dispositivo)
            WAIT(); 
        } 
        else if (processCount > 0 && softBlockCount == 0) {
            // Caso 3: Deadlock (processi esistenti ma nessuno può essere sbloccato)
            PANIC(); 
        }
    }
}