/*
 * vmSupport.c — Pager (gestore TLB exception / demand paging)
 *
 * Implementa il meccanismo di demand paging per i processi utente (U-proc)
 * del Support Level (Phase 3).
 *
 * CONCETTO DI BASE — DEMAND PAGING:
 *   Un U-proc ha un address space virtuale di 32 pagine (128 KB), ma la RAM
 *   disponibile per i processi utente è limitata a POOLSIZE=16 frame fisici
 *   (la "swap pool"). Le pagine non in RAM sono memorizzate su flash disk.
 *   Quando un U-proc accede a una pagina non presente in RAM (V=0 nella PTE),
 *   l'hardware genera una TLB exception che il Nucleo passa a pager() tramite
 *   passUpOrDie(). Il pager:
 *     1. Sceglie un frame vittima (FIFO round-robin).
 *     2. Se occupato, scrive la pagina vittima su flash (eviction).
 *     3. Legge la nuova pagina da flash nel frame liberato.
 *     4. Aggiorna la page table del U-proc e il TLB.
 *     5. Ritorna all'istruzione che aveva causato il fault (LDST).
 *
 * LAYOUT FISICO DELLA SWAP POOL:
 *   Inizia a SWAPPOOLSTART = RAMSTART + OSFRAMES * PAGESIZE = 0x20020000.
 *   Frame i occupa indirizzi [SWAPPOOLSTART + i*PAGESIZE, ..+PAGESIZE).
 *
 * CONCORRENZA:
 *   swapPoolSem è un mutex binario (init=1). Ogni operazione sul swapPool
 *   (lettura/scrittura dei metadati, invalidazione PTE, I/O flash) avviene
 *   in sezione critica tra P(swapPoolSem) e V(swapPoolSem).
 *   I flash device sono protetti da flashMutex[] (uno per device).
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
#include "headers/klog.h"

/* -----------------------------------------------------------------------
 * Strutture dati private del Pager
 * ----------------------------------------------------------------------- */

/*
 * swapPool — array di descrittori per i POOLSIZE frame fisici della swap pool.
 * swap_t contiene:
 *   sw_asid   = ASID del U-proc che occupa il frame (-1 = frame libero)
 *   sw_pageNo = indice della pagina logica (0..MAXPAGES-1) memorizzata
 *   sw_pte    = puntatore alla PTE corrispondente nella page table del U-proc
 *               (serve per invalidare velocemente la PTE all'eviction)
 */
static swap_t swapPool[POOLSIZE];

/*
 * swapPoolSem — mutex binario che protegge l'intera sezione critica del pager.
 * Inizializzato a 1 (libero). Usato con PASSEREN/VERHOGEN (SYS3/SYS4 del Nucleo).
 */
static int swapPoolSem;

/*
 * swapFrameClock — indice del prossimo frame da usare come vittima (FIFO).
 * Avanza in modo circolare: 0 → 1 → ... → POOLSIZE-1 → 0 → ...
 * Non è protetto dal mutex perché viene letto/scritto solo dentro la sezione critica.
 */
static int swapFrameClock;

/* -----------------------------------------------------------------------
 * Inizializzazione
 * ----------------------------------------------------------------------- */

/*
 * initSwapStructs — inizializza la swap pool e il suo semaforo.
 *
 * Chiamata una sola volta da test() prima di lanciare qualsiasi U-proc.
 * Dopo questa chiamata tutti i frame risultano liberi (sw_asid = -1).
 */
void initSwapStructs(void) {
    int i;
    swapPoolSem    = 1;     /* mutex libero */
    swapFrameClock = 0;     /* si parte dal frame 0 */
    for (i = 0; i < POOLSIZE; i++) {
        swapPool[i].sw_asid   = -1;     /* frame non occupato */
        swapPool[i].sw_pageNo = -1;
        swapPool[i].sw_pte    = NULL;
    }
}

/* -----------------------------------------------------------------------
 * Gestione TLB
 * ----------------------------------------------------------------------- */

/*
 * updateTLBEntry — aggiorna atomicamente una entry nel TLB hardware.
 *
 * Problema: dopo aver modificato una PTE (es. V=0 per eviction, o V=1 dopo
 * il caricamento), la copia in cache del TLB può essere inconsistente.
 * Questa funzione sincronizza TLB e page table.
 *
 * Algoritmo:
 *   1. Disabilita gli interrupt (atomic section): nessun altro può modificare
 *      il TLB mentre operiamo su ENTRYHI/INDEX/ENTRYLO.
 *   2. Scrive il VPN+ASID della PTE in CP0.EntryHi.
 *   3. TLBP (TLB Probe): cerca nel TLB se c'è già una entry con quel VPN+ASID.
 *      Risultato in CP0.Index: se bit PRESENTFLAG=0 → trovata alla posizione Index.
 *   4. Se trovata: aggiorna EntryLo in place con TLBWI (TLB Write Indexed).
 *   5. Se non trovata: non fa nulla — la entry verrà caricata da uTLB_RefillHandler
 *      al prossimo accesso (leggerà la PTE aggiornata dalla page table).
 *   6. Ripristina lo status (riabilita interrupt).
 *
 * Nota: usare TLBCLR (flush totale) sarebbe corretto ma molto più costoso.
 *       Aggiornare in place è O(1) e sufficiente.
 *
 * Parametri:
 *   pte — puntatore alla PTE da sincronizzare nel TLB
 */
static void updateTLBEntry(pteEntry_t *pte) {
    /* Salva e disabilita interrupt: operazione atomica sul TLB */
    unsigned int savedStatus = getSTATUS();
    setSTATUS(savedStatus & ~MSTATUS_MIE_MASK);

    /* Carica il VPN+ASID da cercare nel TLB */
    setENTRYHI(pte->pte_entryHI);
    TLBP();     /* scrive il risultato della ricerca in CP0.Index */

    if (!(getINDEX() & PRESENTFLAG)) {
        /* Entry trovata nel TLB alla posizione Index: aggiorna EntryLo */
        setENTRYLO(pte->pte_entryLO);
        TLBWI();    /* sovrascrive la entry alla posizione Index */
    }
    /* Se non trovata: il TLB caricherà la PTE aggiornata al prossimo miss */

    /* Ripristina interrupt */
    setSTATUS(savedStatus);
}

/* -----------------------------------------------------------------------
 * I/O su Flash
 * ----------------------------------------------------------------------- */

/*
 * flashIO — esegue un'operazione di lettura o scrittura sul flash device.
 *
 * I flash device su μRISC-V sono disk-like: ogni "blocco" corrisponde a una
 * pagina (4 KB). Il blocco di numero pageNo contiene la pagina logica pageNo
 * del U-proc con ASID asid.
 *
 * Layout registri flash (dtpreg_t, vedere architettura uRISCV):
 *   +0x00  status    (sola lettura)
 *   +0x04  command   (scrittura per avviare operazione)
 *   +0x08  data0     (indirizzo RAM del buffer, da impostare prima del command)
 *   +0x0C  data1     (non usato per flash)
 *
 * Indirizzo base del device:
 *   START_DEVREG + (IL_FLASH - IL_DISK) * 0x80 + devNo * 0x10
 *   Il Nucleo usa (IL_FLASH - IL_DISK) come offset di linea rispetto ai disk.
 *
 * Formato del comando flash:
 *   bit 15:8 = numero del blocco (pageNo)
 *   bit  7:0 = operazione: FLASHREAD(2) o FLASHWRITE(3)
 *
 * Mutua esclusione:
 *   L'accesso al device è protetto da flashMutex[devNo].
 *   P prima, V dopo → accesso esclusivo garantito tra pager e SYS6.
 *
 * Parametri:
 *   asid      — ASID del U-proc (il suo flash device è asid-1)
 *   pageNo    — indice del blocco logico da leggere/scrivere (0..MAXPAGES-1)
 *   frameAddr — indirizzo fisico del frame RAM sorgente/destinazione
 *   rw        — FLASHREAD (2) per lettura, FLASHWRITE (3) per scrittura
 *
 * Ritorna: status del device (FLASHREADY=1 in caso di successo)
 */
static int flashIO(int asid, int pageNo, unsigned int frameAddr, int rw) {
    int devNo = asid - 1;   /* flash device 0 → ASID 1, device 1 → ASID 2, ... */

    /* Calcola l'indirizzo del registro del flash device.
     * IL_FLASH=18, IL_DISK=17 → offset linea = 1 → 1 * 0x80 = 0x80 byte dopo i disk */
    dtpreg_t *flashReg = (dtpreg_t *)(START_DEVREG +
                          (IL_FLASH - IL_DISK) * 0x80 +
                          devNo * 0x10);

    /* Acquisisce il mutex del device (accesso esclusivo) */
    SYSCALL(PASSEREN, (int)&flashMutex[devNo], 0, 0);

    /* data0 va impostato PRIMA di scrivere command: indica il buffer RAM */
    flashReg->data0 = frameAddr;

    /* Avvia l'operazione: bits [15:8] = numero blocco, bits [7:0] = rw */
    int status = SYSCALL(DOIO, (int)&flashReg->command, (pageNo << 8) | rw, 0);

    /* Rilascia il mutex del device */
    SYSCALL(VERHOGEN, (int)&flashMutex[devNo], 0, 0);

    return status;  /* FLASHREADY=1 se tutto ok, altro valore se errore */
}

/* -----------------------------------------------------------------------
 * Terminazione per errore fatale
 * ----------------------------------------------------------------------- */

/*
 * fatalFault — termina un U-proc a seguito di un errore irrecuperabile.
 *
 * Prima di chiamare TERMPROCESS, esegue V sul semaforo del "padre" in attesa:
 *   - Se ASID 1 (shell): V(masterSemaphore) → sblocca test()
 *   - Altrimenti:        V(shellSemaphore)  → sblocca la shell
 *
 * Questa funzione non ritorna mai (TERMPROCESS termina il processo).
 */
static void fatalFault(support_t *sup) {
    if (sup->sup_asid == 1)
        SYSCALL(VERHOGEN, (int)&masterSemaphore, 0, 0);
    else
        SYSCALL(VERHOGEN, (int)&shellSemaphore, 0, 0);
    SYSCALL(TERMPROCESS, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * Pager — gestore principale delle TLB exception
 * ----------------------------------------------------------------------- */

/*
 * pager — entry point per le TLB exception dei U-proc.
 *
 * Invocato dal Nucleo (tramite passUpOrDie) quando un U-proc causa
 * un'eccezione TLB. Gestisce il page fault: porta la pagina richiesta
 * da flash a RAM e aggiorna TLB + page table.
 *
 * TIPI DI ECCEZIONE TLB (excCode nel campo cause):
 *   EXC_TLBL (25) / EXC_TLBS (26) — TLB Miss: pagina non presente in TLB.
 *     Può essere: (a) PTE V=0 (non in RAM) → page fault da gestire,
 *                 (b) PTE non trovata → refill già fatto da uTLB_RefillHandler.
 *   EXC_UTLBL (27) / EXC_UTLBS (28) — TLB Invalid: PTE presente ma V=0.
 *     Causa più comune del page fault → dobbiamo caricare la pagina.
 *   EXC_MOD (24) — TLB Modification: tentativo di scrivere su pagina read-only.
 *     Errore del programma → terminiamo il U-proc.
 *
 * ALGORITMO:
 *   1. Ottieni support_t del processo corrente via SYS8 (GETSUPPORTPTR).
 *   2. Controlla il codice eccezione: EXC_MOD è fatale.
 *   3. Acquisisce swapPoolSem (sezione critica del pager).
 *   4. Ricava il page index dalla VPN faulting (da EntryHi salvato).
 *   5. Sceglie il frame vittima con FIFO (swapFrameClock).
 *   6. Se il frame è occupato:
 *      a. Invalida la PTE della pagina ospitata (V=0, flush TLB atomico).
 *      b. Scrive il frame su flash (eviction).
 *   7. Carica la nuova pagina dal flash nel frame.
 *   8. Aggiorna i metadati dello swap pool.
 *   9. Aggiorna la PTE: V=1, D=1, PFN=frameAddr.
 *  10. Aggiorna il TLB.
 *  11. Rilascia swapPoolSem.
 *  12. LDST → ritorna all'istruzione faulting (riprova l'accesso).
 */
void pager(void) {
    /* Recupera la struttura di supporto del processo corrente.
     * GETSUPPORTPTR (SYS8) ritorna il puntatore passato alla CREATEPROCESS. */
    support_t *sup = (support_t *) SYSCALL(GETSUPPORTPTR, 0, 0, 0);

    /* Legge il cause register salvato al momento del fault TLB */
    unsigned int cause   = sup->sup_exceptState[PGFAULTEXCEPT].cause;
    unsigned int excCode = cause & CAUSE_EXCCODE_MASK;  /* estrae i bit del codice */

    klog_print("pager excCode="); klog_print_dec(excCode);
    klog_print(" asid="); klog_print_dec(sup->sup_asid); klog_print("\n");

    /* TLB Modification: il programma tenta di scrivere su una pagina protetta.
     * Non è un page fault recuperabile: termina il processo. */
    if (excCode == EXC_MOD) {
        klog_print("pager: TLB-Mod fault -> fatalFault\n");
        fatalFault(sup);
        return; /* mai raggiunto, ma evita warning del compilatore */
    }

    /* Entra in sezione critica: nessun altro pager può modificare swapPool */
    SYSCALL(PASSEREN, (int)&swapPoolSem, 0, 0);

    /* ----------------------------------------------------------------
     * Calcolo del page index dalla VPN faulting
     * EntryHi salvato al momento del fault: bits 31:12 = VPN, bits 11:6 = ASID.
     * ENTRYHI_VPN_MASK estrae solo la parte VPN.
     * VPN 0x3FFFF → stack (pagina 31, l'ultima), altrimenti VPN = page index.
     * ---------------------------------------------------------------- */
    unsigned int entryHi = sup->sup_exceptState[PGFAULTEXCEPT].entry_hi;
    unsigned int vpn     = (entryHi & ENTRYHI_VPN_MASK) >> VPNSHIFT;
    int pageIndex = (vpn == 0x3FFFF) ? MAXPAGES - 1 : (int)vpn;

    /* ----------------------------------------------------------------
     * Selezione del frame vittima — algoritmo FIFO (clock)
     * swapFrameClock avanza circolarmente: garantisce equità tra i frame.
     * ---------------------------------------------------------------- */
    int frameIndex       = swapFrameClock;
    swapFrameClock       = (swapFrameClock + 1) % POOLSIZE;
    unsigned int framePhysAddr = SWAPPOOLSTART + frameIndex * PAGESIZE;

    /* ----------------------------------------------------------------
     * Eviction: se il frame è occupato, salva la vecchia pagina su flash
     * ---------------------------------------------------------------- */
    if (swapPool[frameIndex].sw_asid != -1) {
        int oldAsid        = swapPool[frameIndex].sw_asid;
        int oldPageNo      = swapPool[frameIndex].sw_pageNo;
        pteEntry_t *oldPte = swapPool[frameIndex].sw_pte;

        /* Passo 1: invalida la PTE della pagina espulsa (V=0).
         * Qualsiasi accesso futuro a quella pagina causerà un nuovo page fault. */
        oldPte->pte_entryLO &= ~VALIDON;

        /* Passo 2: rimuove/aggiorna atomicamente la entry dal TLB (se presente).
         * Disabilita interrupt per evitare che un altro processo usi la entry
         * sporca nel breve intervallo tra l'invalidazione della PTE e il flush TLB. */
        updateTLBEntry(oldPte);

        /* Passo 3: scrive il contenuto del frame su flash (backing store).
         * Se l'I/O fallisce, la situazione è irrecuperabile: termina il processo. */
        if (flashIO(oldAsid, oldPageNo, framePhysAddr, FLASHWRITE) != FLASHREADY) {
            SYSCALL(VERHOGEN, (int)&swapPoolSem, 0, 0);  /* rilascia mutex prima di terminare */
            fatalFault(sup);
            return;
        }
    }

    /* ----------------------------------------------------------------
     * Loading: legge la nuova pagina dal flash nel frame fisico liberato
     * ---------------------------------------------------------------- */
    int currentAsid = sup->sup_asid;
    klog_print("pager: flash read asid="); klog_print_dec(currentAsid);
    klog_print(" page="); klog_print_dec(pageIndex); klog_print("\n");

    int rstat = flashIO(currentAsid, pageIndex, framePhysAddr, FLASHREAD);
    if (rstat != FLASHREADY) {
        /* Lettura flash fallita: pagina non accessibile, processo terminato */
        klog_print("pager: flash read FAIL status="); klog_print_dec(rstat); klog_print("\n");
        SYSCALL(VERHOGEN, (int)&swapPoolSem, 0, 0);
        fatalFault(sup);
        return;
    }
    klog_print("pager: flash read OK\n");

    /* ----------------------------------------------------------------
     * Aggiornamento metadati swap pool
     * Registra chi occupa ora il frame: ASID, pageNo, puntatore alla PTE.
     * ---------------------------------------------------------------- */
    swapPool[frameIndex].sw_asid   = currentAsid;
    swapPool[frameIndex].sw_pageNo = pageIndex;
    swapPool[frameIndex].sw_pte    = &sup->sup_privatePgTbl[pageIndex];

    /* ----------------------------------------------------------------
     * Aggiornamento PTE: mappa la pagina logica nel frame fisico
     * pte_entryLO:
     *   bit PFN (bit 31:12) = framePhysAddr >> 12 (indirizzo fisico del frame)
     *   bit D = 1 (DIRTYON): la pagina può essere scritta su flash all'eviction
     *   bit V = 1 (VALIDON): la pagina è ora presente in RAM
     * Non si imposta GLOBALON: ogni U-proc ha il suo address space (ASID separati).
     * ---------------------------------------------------------------- */
    pteEntry_t *currentPte = &sup->sup_privatePgTbl[pageIndex];
    currentPte->pte_entryLO = framePhysAddr | DIRTYON | VALIDON;

    /* Aggiorna il TLB: se c'è una entry con V=0 per questa VPN, correggila.
     * In caso contrario, uTLB_RefillHandler la caricherà al prossimo TLB miss. */
    updateTLBEntry(currentPte);

    /* Esce dalla sezione critica */
    SYSCALL(VERHOGEN, (int)&swapPoolSem, 0, 0);

    /* ----------------------------------------------------------------
     * Ritorna all'istruzione faulting: ora la pagina è in RAM e il TLB
     * è aggiornato, quindi l'accesso avrà successo.
     * LDST ripristina lo stato salvato al momento del fault (EPC punta
     * all'istruzione che ha causato il fault, non alla successiva).
     * ---------------------------------------------------------------- */
    LDST(&sup->sup_exceptState[PGFAULTEXCEPT]);
}
