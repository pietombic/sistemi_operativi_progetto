/*
    InstantiatorProcess (test): initializes Support Level and launches the shell.
    Exports global semaphores and support structures used by vmSupport/sysSupport.
*/
#include "../headers/const.h"
#include "../headers/types.h"
#include "../phase1/headers/pcb.h"
#include "../phase2/headers/initial.h"
#include "headers/initProc.h"
#include "headers/vmSupport.h"
#include "headers/sysSupport.h"
#include <uriscv/liburiscv.h>
#include "headers/klog.h"

support_t supportStructures[UPROCMAX];

int masterSemaphore;   /* test() waits here for shell to terminate */
int shellSemaphore;    /* shell waits here for child program to terminate */

int flashMutex[DEVPERINT];  /* one mutex per flash device (ASID i uses flash i-1) */
int termMutexTX;
int termMutexRX;

/* Initializes page table for U-proc with given ASID.
   Pages 0-30: VPN 0x80000+i (text/data). Page 31: VPN 0xBFFFF (stack).
   All entries: D=1, V=0 (not yet in RAM). */
void initPageTable(support_t *sup, int asid) {
    int i;
    for (i = 0; i < MAXPAGES - 1; i++) {
        sup->sup_privatePgTbl[i].pte_entryHI =
            ((0x80000 + i) << VPNSHIFT) | (asid << ASIDSHIFT);
        sup->sup_privatePgTbl[i].pte_entryLO = DIRTYON;
    }
    sup->sup_privatePgTbl[MAXPAGES - 1].pte_entryHI =
        (0xBFFFF << VPNSHIFT) | (asid << ASIDSHIFT);
    sup->sup_privatePgTbl[MAXPAGES - 1].pte_entryLO = DIRTYON;
}

/* Initializes support exception contexts for a U-proc.
   PGFAULTEXCEPT → pager(), GENERALEXCEPT → supportGeneralExHandler().
   Both run kernel-mode with interrupts enabled. */
void initSupportContexts(support_t *sup) {
    /* kernel-mode: MPP=11 (M-mode), MIE=1 (interrupts on), MPIE=1 */
    unsigned int handlerStatus = MSTATUS_MPIE_MASK | MSTATUS_MPP_M | MSTATUS_MIE_MASK;

    sup->sup_exceptContext[PGFAULTEXCEPT].stackPtr = (unsigned int) &sup->sup_stackTLB[499];
    sup->sup_exceptContext[PGFAULTEXCEPT].status   = handlerStatus;
    sup->sup_exceptContext[PGFAULTEXCEPT].pc       = (unsigned int) pager;

    sup->sup_exceptContext[GENERALEXCEPT].stackPtr = (unsigned int) &sup->sup_stackGen[499];
    sup->sup_exceptContext[GENERALEXCEPT].status   = handlerStatus;
    sup->sup_exceptContext[GENERALEXCEPT].pc       = (unsigned int) supportGeneralExHandler;
}

/* Public helper: fully initializes a support structure for U-proc with given ASID.
   Called by launchUproc (here) and sys6_execute (sysSupport.c). */
void initSupportStruct(support_t *sup, int asid) {
    sup->sup_asid = asid;
    initPageTable(sup, asid);
    initSupportContexts(sup);
}

static void launchUproc(int asid) {
    support_t *sup = &supportStructures[asid - 1];
    initSupportStruct(sup, asid);

    state_t uprocState = {0};
    uprocState.pc_epc   = UPROCSTARTADDR;
    uprocState.reg_sp   = USERSTACKTOP;
    /* user-mode: MPP=00 (U), MIE=1 (interrupts on), MPIE=1 */
    uprocState.status   = MSTATUS_MPIE_MASK | MSTATUS_MIE_MASK;
    uprocState.mie      = MIE_ALL;
    uprocState.entry_hi = (asid << ASIDSHIFT);

    SYSCALL(CREATEPROCESS, (int)&uprocState, 0, (int)sup);
}

void test(void) {
    klog_print("CIAO \n");
    int i;

    initSwapStructs();

    for (i = 0; i < DEVPERINT; i++)
        flashMutex[i] = 1;
    termMutexTX = 1;
    termMutexRX = 1;

    masterSemaphore = 0;
    shellSemaphore  = 0;

    launchUproc(1);  /* launch shell (ASID 1) */
    klog_print("SHELL LAUNCHED \n");

    SYSCALL(PASSEREN, (int)&masterSemaphore, 0, 0);  /* wait for shell to finish */
    klog_print("SHELL TERMINATED \n");
    //ci arriva
    SYSCALL(TERMPROCESS, 0, 0, 0);
}
