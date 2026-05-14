/*
 * initProc.c — Processo Instanziatore (Support Level)
 *
 * Questo file è il punto di ingresso del Support Level (Phase 3).
 * La funzione test() viene eseguita come processo root del livello utente:
 *   1. Inizializza le strutture dati condivise (swap pool, semafori, mutex).
 *   2. Lancia la shell come U-proc con ASID 1.
 *   3. Attende su masterSemaphore che la shell termini.
 *   4. Termina se stesso via SYS10 (TERMPROCESS).
 *
 * Ogni U-proc (User Process) gira in user-mode su architettura μRISC-V e
 * possiede:
 *   - Un ASID univoco (1..UPROCMAX) che identifica l'address space nel TLB.
 *   - Una page table privata (32 entry: 31 pagine dati/testo + 1 stack).
 *   - Una struttura support_t con i contesti per i due handler di eccezione.
 *
 * Dipendenze esportate (usate da vmSupport.c e sysSupport.c):
 *   supportStructures[], masterSemaphore, shellSemaphore,
 *   flashMutex[], termMutexTX, termMutexRX.
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

/* -----------------------------------------------------------------------
 * Strutture dati globali condivise tra i moduli del Support Level
 * ----------------------------------------------------------------------- */

/* Array di strutture support_t, una per ogni possibile U-proc.
 * supportStructures[i] corrisponde al processo con ASID = i+1.
 * Contiene: ASID, page table privata, stack degli handler, contesti eccezione. */
support_t supportStructures[UPROCMAX];

/* Semaforo su cui test() esegue P (PASSEREN) in attesa che la shell
 * (ASID 1) termini. La shell (o chi la termina) esegue una V (VERHOGEN)
 * prima di chiamare TERMPROCESS, sboccando test(). */
int masterSemaphore;

/* Semaforo su cui la shell esegue P dopo aver lanciato un programma figlio
 * (SYS6 Execute). Il programma figlio esegue una V prima di terminare,
 * sbloccando la shell così che possa accettare il prossimo comando. */
int shellSemaphore;

/* Mutex per i flash device: uno per device (ASID i usa flash i-1).
 * Valore iniziale 1 (libero). Garantisce accesso esclusivo al device
 * tra il pager (eviction/load) e qualsiasi altra operazione concorrente. */
int flashMutex[DEVPERINT];

/* Mutex per il terminale 0 in trasmissione (TX) e ricezione (RX).
 * TX e RX sono indipendenti, quindi due mutex separati permettono
 * lettura e scrittura contemporanee senza blocco inutile. */
int termMutexTX;
int termMutexRX;

/* -----------------------------------------------------------------------
 * Funzioni di inizializzazione delle strutture support_t
 * ----------------------------------------------------------------------- */

/*
 * initPageTable — inizializza la page table privata di un U-proc.
 *
 * La page table ha MAXPAGES (32) entry:
 *   - Entry 0..30: pagine testo/dati, VPN = 0x80000 + i
 *     (corrispondono all'indirizzo virtuale 0x80000000 + i*PAGESIZE in KUSEG)
 *   - Entry 31: pagina stack, VPN = 0xBFFFF
 *     (corrispondente allo stack top 0xBFFFF000; USERSTACKTOP = 0xC0000000)
 *
 * Formato di pte_entryHI (TLB EntryHi μRISC-V):
 *   bit 31:12 = VPN (Virtual Page Number)
 *   bit 11:6  = ASID (identifica l'address space, evita flush TLB al context switch)
 *
 * Tutte le entry sono inizializzate con:
 *   pte_entryLO = DIRTYON (bit D=1): la pagina è "sporca" fin dall'inizio
 *                 per permettere scritture su flash al momento dell'evizione.
 *                 bit V=0: pagina non valida, non ancora caricata in RAM.
 *
 * Parametri:
 *   sup  — struttura support_t da inizializzare
 *   asid — ASID del processo (1..UPROCMAX)
 */
void initPageTable(support_t *sup, int asid) {
    int i;

    /* Pagine 0..MAXPAGES-2: segmento testo e dati in KUSEG */
    for (i = 0; i < MAXPAGES - 1; i++) {
        /* VPN = 0x80000 + i: mappa sulla parte bassa di KUSEG */
        sup->sup_privatePgTbl[i].pte_entryHI =
            ((0x80000 + i) << VPNSHIFT) | (asid << ASIDSHIFT);
        /* Pagina non valida ma dirty: al primo accesso scatta page fault */
        sup->sup_privatePgTbl[i].pte_entryLO = DIRTYON;
    }

    /* Pagina 31: stack (VPN 0xBFFFF, indirizzo 0xBFFFF000)
     * USERSTACKTOP = 0xC0000000, quindi sp iniziale punta a fine pagina stack */
    sup->sup_privatePgTbl[MAXPAGES - 1].pte_entryHI =
        (0xBFFFF << VPNSHIFT) | (asid << ASIDSHIFT);
    sup->sup_privatePgTbl[MAXPAGES - 1].pte_entryLO = DIRTYON;
}

/*
 * initSupportContexts — inizializza i due contesti di eccezione del U-proc.
 *
 * Ogni U-proc ha due handler registrati nella sua support_t:
 *   sup_exceptContext[PGFAULTEXCEPT] (indice 0) → pager()
 *     Invocato dal Nucleo quando passUpOrDie() è chiamata con TLB exception.
 *   sup_exceptContext[GENERALEXCEPT] (indice 1) → supportGeneralExHandler()
 *     Invocato dal Nucleo per syscall (SYS2+) e program trap.
 *
 * Ogni contesto specifica: stack pointer, status register, program counter.
 *
 * Status per gli handler (kernel-mode, interrupt abilitati):
 *   MSTATUS_MPP_M  = bit MPP = 11 → ritorno in M-mode (kernel)
 *   MSTATUS_MIE_MASK = bit MIE = 1 → interrupt globali abilitati
 *   MSTATUS_MPIE_MASK = bit MPIE = 1 → salvataggio del MIE pre-trap
 *
 * Stack TLB e stack Gen sono array di 500 interi nella support_t.
 * Usiamo l'ultimo elemento (&stack[499]) come top dello stack
 * (lo stack cresce verso il basso su RISC-V).
 */
void initSupportContexts(support_t *sup) {
    /* Status: kernel-mode con interrupt abilitati */
    unsigned int handlerStatus = MSTATUS_MPIE_MASK | MSTATUS_MPP_M | MSTATUS_MIE_MASK;

    /* Contesto per TLB exception → pager() */
    sup->sup_exceptContext[PGFAULTEXCEPT].stackPtr = (unsigned int) &sup->sup_stackTLB[499];
    sup->sup_exceptContext[PGFAULTEXCEPT].status   = handlerStatus;
    sup->sup_exceptContext[PGFAULTEXCEPT].pc       = (unsigned int) pager;

    /* Contesto per general exception → supportGeneralExHandler() */
    sup->sup_exceptContext[GENERALEXCEPT].stackPtr = (unsigned int) &sup->sup_stackGen[499];
    sup->sup_exceptContext[GENERALEXCEPT].status   = handlerStatus;
    sup->sup_exceptContext[GENERALEXCEPT].pc       = (unsigned int) supportGeneralExHandler;
}

/*
 * initSupportStruct — inizializza completamente una support_t per un U-proc.
 *
 * Funzione pubblica chiamata da:
 *   - launchUproc() (qui sotto) per la shell al boot del Support Level
 *   - sys6_execute() in sysSupport.c per ogni programma lanciato dalla shell
 *
 * Ordine delle operazioni:
 *   1. Imposta sup->sup_asid (identificatore univoco dell'address space)
 *   2. Inizializza la page table privata (tutte le pagine invalide)
 *   3. Registra i contesti degli handler di eccezione
 */
void initSupportStruct(support_t *sup, int asid) {
    sup->sup_asid = asid;           /* ASID usato per EntryHi nel TLB */
    initPageTable(sup, asid);       /* page table: 32 entry, tutte V=0 */
    initSupportContexts(sup);       /* pager + generalHandler */
}

/* -----------------------------------------------------------------------
 * Creazione e avvio di un U-proc
 * ----------------------------------------------------------------------- */

/*
 * launchUproc — crea e avvia un U-proc con il dato ASID.
 *
 * Prepara lo stato iniziale del processo utente:
 *   pc_epc   = UPROCSTARTADDR (0x800000B0): entry point del .text U-proc
 *   reg_sp   = USERSTACKTOP  (0xC0000000): cima dello stack utente
 *   status   = user-mode + interrupt abilitati (MPP=00, MIE=1, MPIE=1)
 *   mie      = MIE_ALL: tutte le sorgenti di interrupt abilitate
 *   entry_hi = ASID << ASIDSHIFT: identifica l'address space nel TLB
 *
 * Invoca SYS1 (CREATEPROCESS) passando lo stato e il puntatore alla
 * support_t: il Nucleo li associa al nuovo PCB così che passUpOrDie()
 * possa trovare i contesti degli handler al momento di un'eccezione.
 */
static void launchUproc(int asid) {
    /* Prende la support_t pre-allocata per questo ASID */
    support_t *sup = &supportStructures[asid - 1];
    initSupportStruct(sup, asid);

    state_t uprocState = {0};   /* azzera tutti i registri */
    uprocState.pc_epc   = UPROCSTARTADDR;  /* entry point U-proc */
    uprocState.reg_sp   = USERSTACKTOP;    /* stack top utente */

    /* User-mode (MPP=00), interrupt abilitati (MIE=1, MPIE=1).
     * Nota: NON si imposta MPP_M → al primo MRET si torna in U-mode */
    uprocState.status   = MSTATUS_MPIE_MASK | MSTATUS_MIE_MASK;
    uprocState.mie      = MIE_ALL;                   /* accetta tutti gli interrupt */
    uprocState.entry_hi = (asid << ASIDSHIFT);       /* ASID nel TLB EntryHi */

    /* SYS1: crea il processo figlio con stato e struttura di supporto.
     * Il terzo argomento (0) significa: non specificare priorità aggiuntiva. */
    SYSCALL(CREATEPROCESS, (int)&uprocState, 0, (int)sup);
}

/* -----------------------------------------------------------------------
 * Funzione principale del Support Level
 * ----------------------------------------------------------------------- */

/*
 * test — processo instanziatore, entry point del Support Level.
 *
 * Eseguita come processo nucleo con la struttura support_t fornita
 * dall'istruttore tramite il meccanismo di test del Nucleo (p2test.c).
 *
 * Sequenza di avvio:
 *   1. initSwapStructs(): inizializza swap pool (16 frame) e semaforo mutex.
 *   2. Inizializza tutti i mutex a 1 (liberi):
 *      - flashMutex[i] per ogni flash device i (0..DEVPERINT-1)
 *      - termMutexTX (trasmissione terminale)
 *      - termMutexRX (ricezione terminale)
 *   3. Azzera i semafori di sincronizzazione:
 *      - masterSemaphore = 0: test() si bloccherà su PASSEREN
 *      - shellSemaphore  = 0: shell si bloccherà su PASSEREN dopo Execute
 *   4. Lancia la shell (ASID 1) con launchUproc(1).
 *   5. Attende su masterSemaphore (P): la shell farà V quando termina.
 *   6. Termina via SYS10 (TERMPROCESS).
 */
void test(void) {
    klog_print("CIAO \n");
    int i;

    /* Inizializza la swap pool (frame fisici per demand paging) e il suo mutex */
    initSwapStructs();

    /* Inizializza i mutex dei flash device (uno per device, DEVPERINT device totali).
     * flashMutex[i] protegge l'accesso esclusivo al flash device i.
     * Valore 1 = semaforo binario libero (convenzione: P decrementa, V incrementa) */
    for (i = 0; i < DEVPERINT; i++)
        flashMutex[i] = 1;

    /* Mutex per il terminale 0: TX (trasmissione) e RX (ricezione) separati */
    termMutexTX = 1;
    termMutexRX = 1;

    /* Semafori di sincronizzazione tra processi: inizializzati a 0 (bloccanti) */
    masterSemaphore = 0;    /* test() aspetta qui la fine della shell */
    shellSemaphore  = 0;    /* shell aspetta qui la fine di ogni figlio */

    /* Lancia la shell come ASID 1. La shell è il processo "init" del livello utente:
     * legge comandi dal terminale e li esegue tramite SYS6 (Execute). */
    launchUproc(1);
    klog_print("SHELL LAUNCHED \n");

    /* P(masterSemaphore): si blocca finché la shell non fa V(masterSemaphore).
     * La shell fa V in sys2_terminate() o in programTrapHandler() se ha ASID 1. */
    SYSCALL(PASSEREN, (int)&masterSemaphore, 0, 0);
    klog_print("SHELL TERMINATED \n");

    /* Tutto il Support Level è terminato: test() si autodistrugge */
    SYSCALL(TERMPROCESS, 0, 0, 0);
}
