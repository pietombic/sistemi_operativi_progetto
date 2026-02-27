#include "headers/types.h"
#include "headers/const.h"
#include "headers/listx.h"
#include "phase1/headers/pcb.h"
#include "phase1/headers/asl.h"

void uTLB_RefillHandler()
{
    int prid = getPRID();
    setENTRYHI(0x80000000);
    setENTRYLO(0x00000000);
    TLBWR();
    LDST((state_t *)BIOSDATAPAGE);
}
void interruptHandler()
{
    int cause = getCAUSE();
    int int_exc_code = cause & CAUSE_EXCCODE_MASK();

    if (int_exc_code == IL_CPUTIMER)
    {
        // Program Local Timer interrupt

    }
    else if (int_exc_code == IL_TIMER)
    {
        // Interval timer interrupt, gestisci l'interruzione del timer

    }
    else
    {
        int pendingLine = highestPriorityPendingLine();
        // Interruzioni di Device, utilizziamo la bitmap per identificare quale dispositivo ha generato l'interruzione
    }
}
int highestPriorityPendingLine(){
    int cause = getCAUSE();
    int pending = cause & CAUSE_IP_MASK(); // Ottieni i bit delle interruzioni pendenti
    for (int i = 7; i >= 0; i--) {
        if (pending & (1 << i)) {
            return i; // Restituisce la linea di interruzione più alta
        }
    }
    return -1; // Nessuna interruzione pendente
}