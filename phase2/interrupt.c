#include "headers/types.h"
#include "headers/const.h"
#include "headers/listx.h"
#include "phase1/headers/pcb.h"
#include "phase1/headers/asl.h"
#include "initial.c"


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
        state_t currentState = currentProcess->p_s;
        scheduler();

    }
    else if (intlineNo == 2)
    {
        // Interval timer interrupt, gestisci l'interruzione del timer

    }
    else
    {
        int j = intlineNo - 3; // Calcola l'indice del dispositivo (linee 3-7)
        unsigned int word = getDeviceBitmap(j);
        int deviceNo = getDeviceNumber(word); 

        int devAddrBase = 0x10000054 + ((intlineNo - 3) * 0x80) + (deviceNo * 0x10);
        
        // Interruzioni di Device, utilizziamo la bitmap per identificare quale dispositivo ha generato l'interruzione
    }
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