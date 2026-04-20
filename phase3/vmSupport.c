/*
    Pager (TLB exception handler) for the Support Level.
    Implements demand paging: on TLB-Invalid fault, finds the victim frame
    (FIFO), writes it back to flash if dirty, reads the new page from flash,
    updates the page table and TLB, then returns to the faulting instruction.
*/

#include "../headers/const.h"
#include "../headers/types.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "../phase2/headers/initial.h"
#include "headers/vmSupport.h"
#include "headers/initProc.h"
#include "headers/sysSupport.h"
#include <uriscv/liburiscv.h>
#include <uriscv/cpu.h>

static swap_t swapPool[POOLSIZE];
static int swapPoolSem;
static int swapFrameClock;

void initSwapStructs(void) {
    int i;
    swapPoolSem    = 1;
    swapFrameClock = 0;
    for (i = 0; i < POOLSIZE; i++) {
        swapPool[i].sw_asid   = -1;
        swapPool[i].sw_pageNo = -1;
        swapPool[i].sw_pte    = NULL;
    }
}

/* Atomically invalidates/updates a TLB entry after modifying a PTE.
   Disables interrupts, uses TLBP+TLBWI when found, falls back to TLBCLR. */
static void updateTLBEntry(pteEntry_t *pte) {
    unsigned int savedStatus = getSTATUS();
    setSTATUS(savedStatus & ~MSTATUS_MIE_MASK);

    setENTRYHI(pte->pte_entryHI);
    TLBP();
    if (!(getINDEX() & PRESENTFLAG)) {
        /* Entry found in TLB: update in place */
        setENTRYLO(pte->pte_entryLO);
        TLBWI();
    }
    /* If not found, TLB will reload from page table on next access */

    setSTATUS(savedStatus);
}

/* Flash I/O helper.
   asid: U-proc ASID (flash device = asid-1).
   pageNo: logical page number (block index in flash).
   frameAddr: physical address of RAM frame.
   rw: FLASHREAD (2) or FLASHWRITE (3).
   Returns device status (FLASHREADY=1 on success). */
static int flashIO(int asid, int pageNo, unsigned int frameAddr, int rw) {
    int devNo = asid - 1;

    /* Flash line is 1 offset from disk base (IL_FLASH - IL_DISK = 1) */
    dtpreg_t *flashReg = (dtpreg_t *)(START_DEVREG +
                          (IL_FLASH - IL_DISK) * 0x80 +
                          devNo * 0x10);

    SYSCALL(PASSEREN, (int)&flashMutex[devNo], 0, 0);

    flashReg->data0 = frameAddr;  /* must be set before issuing command */
    int status = SYSCALL(DOIO, (int)&flashReg->command, (pageNo << 8) | rw, 0);

    SYSCALL(VERHOGEN, (int)&flashMutex[devNo], 0, 0);

    return status;
}

/* Terminates a U-proc after a fatal fault, signaling the appropriate waiter. */
static void fatalFault(support_t *sup) {
    if (sup->sup_asid == 1)
        SYSCALL(VERHOGEN, (int)&masterSemaphore, 0, 0);
    else
        SYSCALL(VERHOGEN, (int)&shellSemaphore, 0, 0);
    SYSCALL(TERMPROCESS, 0, 0, 0);
}

/* TLB exception handler (Pager).
   Handles TLB-Invalid faults (page not in RAM) for U-procs.
   TLB-Modification and other faults are treated as program errors. */
void pager(void) {
    support_t *sup = (support_t *) SYSCALL(GETSUPPORTPTR, 0, 0, 0);

    unsigned int cause   = sup->sup_exceptState[PGFAULTEXCEPT].cause;
    unsigned int excCode = cause & CAUSE_EXCCODE_MASK;

    /* EXC_MOD=24: TLB-Modification (write to read-only page) — program error.
       Only EXC_UTLBL=27 and EXC_UTLBS=28 are valid TLB-Invalid faults we handle. */
    if (excCode != EXC_UTLBL && excCode != EXC_UTLBS) {
        fatalFault(sup);
        return;
    }

    SYSCALL(PASSEREN, (int)&swapPoolSem, 0, 0);

    /* EntryHi bits 29:12 = segment-relative VPN.
       Stack page (VA 0xBFFFF000) → bits 29:12 = 0x3FFFF.
       Text/data pages 0-30 → bits 29:12 = 0..30. */
    unsigned int entryHi = sup->sup_exceptState[PGFAULTEXCEPT].entry_hi;
    unsigned int vpn     = (entryHi & ENTRYHI_VPN_MASK) >> VPNSHIFT;
    int pageIndex = (vpn == 0x3FFFF) ? MAXPAGES - 1 : (int)vpn;

    /* Select victim frame (FIFO round-robin) */
    int frameIndex   = swapFrameClock;
    swapFrameClock   = (swapFrameClock + 1) % POOLSIZE;
    unsigned int framePhysAddr = SWAPPOOLSTART + frameIndex * PAGESIZE;

    /* If frame is occupied, evict the old page */
    if (swapPool[frameIndex].sw_asid != -1) {
        int oldAsid    = swapPool[frameIndex].sw_asid;
        int oldPageNo  = swapPool[frameIndex].sw_pageNo;
        pteEntry_t *oldPte = swapPool[frameIndex].sw_pte;

        /* Invalidate old PTE and flush from TLB atomically */
        oldPte->pte_entryLO &= ~VALIDON;
        updateTLBEntry(oldPte);

        /* Write evicted frame back to flash (backing store) */
        if (flashIO(oldAsid, oldPageNo, framePhysAddr, FLASHWRITE) != FLASHREADY) {
            SYSCALL(VERHOGEN, (int)&swapPoolSem, 0, 0);
            fatalFault(sup);
            return;
        }
    }

    /* Load requested page from flash into the frame */
    int currentAsid = sup->sup_asid;
    if (flashIO(currentAsid, pageIndex, framePhysAddr, FLASHREAD) != FLASHREADY) {
        SYSCALL(VERHOGEN, (int)&swapPoolSem, 0, 0);
        fatalFault(sup);
        return;
    }

    /* Update swap pool metadata */
    swapPool[frameIndex].sw_asid   = currentAsid;
    swapPool[frameIndex].sw_pageNo = pageIndex;
    swapPool[frameIndex].sw_pte    = &sup->sup_privatePgTbl[pageIndex];

    /* Update PTE: Valid=1, Dirty=1, PFN=frame physical address */
    pteEntry_t *currentPte = &sup->sup_privatePgTbl[pageIndex];

    currentPte->pte_entryLO = framePhysAddr | DIRTYON | VALIDON;
    /* Update TLB: if entry is cached with V=0, fix it in place; otherwise
       uTLB_RefillHandler will reload from page table on next TLB miss */
    updateTLBEntry(currentPte);

    SYSCALL(VERHOGEN, (int)&swapPoolSem, 0, 0);

    /* Retry the faulting instruction */
    LDST(&sup->sup_exceptState[PGFAULTEXCEPT]);
}
