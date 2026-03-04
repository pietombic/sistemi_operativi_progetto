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



void interruptHandler()
{
    int intlineNo = highestPriorityPendingLine();
    if (intlineNo == -1)
    {
        return; // Nessuna interruzione pendente, esci dall'handler
    }
    else if (intlineNo == 1)
    {
        // Program Local Timer interrupt
        setTIMER(TIMESLICE); // Reset del timer
        int cpu = getPRID(); // Ottieni il numero della CPU (core) che ha generato l'interruzione
        state_t* old = GET_EXCEPTION_STATE_PTR(cpu);
        currentProcess->p_s = *old;
        insertProcQ(&readyQueue, currentProcess);
        currentProcess = NULL;
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
            softBlockCount--;

        }
        // ritorna il controllo al processo corrente: fa LDST sullo stato salvato al momento dell'interruzione
        state_t* old = GET_EXCEPTION_STATE_PTR(getPRID());

       if(currentProcess != NULL){
            LDST(old);
        }else{
            scheduler();
        }
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

        if (intlineNo == 7) {
            /* terminal line has two subdevices: 0=receive, 1=transmit.
             * statusCode reflects the type of event (TRANSMITCHAR, OKCHARTRANS,
             * RECEIVECHAR).  Log the interrupt and adjust the value for
             * receive events so that the unblocked process sees the character
             * itself in a0. */
            klog_print("[INT] terminal interrupt\n");
            klog_print("[INT] subdevice="); klog_print_dec(deviceNo);
            klog_print(" status="); klog_print_dec(statusCode); klog_print("\n");
            if (deviceNo == 0) {
                /* receive subdevice: statusCode low byte contains flag,
                 * high byte the character. */
                if ((statusCode & 0xFF) == RECEIVECHAR) {
                    statusCode = (statusCode >> 8) & 0xFF;
                }
            }
            /* transmit subdevice (deviceNo==1) requires no extra adjustment */
        }
        
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
            softBlockCount--;
        }
        
        // ritorna il controllo al processo corrente o chiama lo scheduler se il processo corrente è NULL
        // ritorna il controllo al processo corrente o chiama lo scheduler se il processo corrente è NULL
        state_t* old = GET_EXCEPTION_STATE_PTR(getPRID());

       if(currentProcess != NULL){
            LDST(old);
        }else{
            scheduler();
        }
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
    unsigned int cause = getCAUSE();
    for (int line = 1; line <= 7; line++) {
        if (CAUSE_IP_GET(cause, line)) {
            return line; // Restituisce la linea di interruzione più alta
        }
    }
    return -1; // Nessuna interruzione pendente
}