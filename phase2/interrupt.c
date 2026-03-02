#include "../headers/types.h"
#include "../headers/const.h"
#include "../headers/listx.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include "headers/excpetion.h"
#include "headers/interrupt.h"
#include <uriscv/liburiscv.h>


void interruptHandler()
{
    int cause = getCAUSE();
    int intlineNo = highestPriorityPendingLine();
    if (intlineNo == -1)
    {
        return; // Nessuna interruzione pendente, esci dall'handler
    }
    else if (intlineNo == 1)
    {
        // Program Local Timer interrupt
        setTIMER(TIMESLICE); // Reset del timer
        state_t* old = (state_t *) INT_OLD_AREA();
        currentProcess->p_s = *old;
        insertProcQ(&readyQueue, currentProcess);
        scheduler();

    }
    else if (intlineNo == 2)
    {
        LDIT(PSECOND); // Reset del timer per l'interruzione del timer di sistema
        // sblocca tutti i PCB bloccati che attendono un tick dello Pseudo-clock.
        int* semAdd = &deviceSemaphores[SEMDEVLEN - 1];
        pcb_t* unblocked;
        while ((unblocked = removeBlocked(semAdd)) != NULL) {
            insertProcQ(&readyQueue, unblocked);
        }
        // ritorna il controllo al processo corrente: fa LDST sullo stato salvato al momento dell'interruzione
        state_t* old = (state_t *) INT_OLD_AREA();
        currentProcess->p_s = *old;
        LDST(&(currentProcess->p_s));
        
        scheduler();
        

    }
    else
    {
        // Device interrupt
        int j = intlineNo - 3; // calcola l'indice del dispositivo
        unsigned int word = getDeviceBitmap(j);
        int deviceNo = getDeviceNumber(word);

        // calcola l'indirizzo del registro del dispositivo
        unsigned int devAddrBase = 0x10000054 + ((intlineNo - 3) * 0x80) + (deviceNo * 0x10);
        
        // salva lo status code del subdevice del dispositivo
        unsigned int* devRegPtr = (unsigned int*) devAddrBase;
        int statusCode = *devRegPtr;
        
        // ACK dell'interruzione scrivendo 1 nel registro del comando del dispositivo
        unsigned int* cmdRegPtr = (unsigned int*) (devAddrBase + 0x04);
        *cmdRegPtr = ACK;
        
        // operazione V sul semaforo del dispositivo
        int* semaphoreAddr = &deviceSemaphores[(intlineNo - 3) * 8 + deviceNo];
        pcb_t* unblocked = removeBlocked(semaphoreAddr);
        
        // controlla se ci sono dei PCB bloccati
        if (unblocked != NULL) {
            // inserisci il codice di stato nel registro a0 del PCB sbloccato
            unblocked->p_s.reg_a0 = statusCode;
            
            // inserisci il PCB sbloccato nella Ready Queue
            insertProcQ(&readyQueue, unblocked);
        }
        
        // ritorna il controllo al processo corrente o chiama lo scheduler se il processo corrente è NULL
        if (currentProcess != NULL) {
            // salva lo stato del processo corrente e inseriscilo nella Ready Queue
            state_t* old = (state_t *) INT_OLD_AREA();
            currentProcess->p_s = *old;
            insertProcQ(&readyQueue, currentProcess);
        }
        
        // chiama lo scheduler per eseguire il prossimo processo
        scheduler();
    }
}

unsigned int getDeviceBitmap(int deviceLine) {
    // Bitmap dei dispositivi per la word
    unsigned int irrAddr = 0x10000040 + (deviceLine * 0x04);
    unsigned int* irrRegPtr = (unsigned int*) irrAddr;
    return *irrRegPtr;
}

int getDeviceNumber(unsigned int word) {
    if(word & DEV0ON)
        return 0;
    else if(word & DEV1ON)
        return 1;
    else if(word & DEV2ON)
        return 2;
    else if(word & DEV3ON)
        return 3;
    else if(word & DEV4ON)
        return 4;
    else if(word & DEV5ON)
        return 5;
    else if(word & DEV6ON)
        return 6;
    else if(word & DEV7ON)
        return 7;
    else
        return -1; // Nessun dispositivo attivo

}

int highestPriorityPendingLine(){
    int cause = getCAUSE();
    for (int line = 1; line <= 7; line++) {
        if (CAUSE_IP_GET(cause, line)) {
            return line; // Restituisce la linea di interruzione più alta
        }
    }
    return -1; // Nessuna interruzione pendente
}