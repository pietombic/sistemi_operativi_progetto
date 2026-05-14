/*
 * sysSupport.c — Gestore eccezioni generali del Support Level
 *
 * Questo file implementa l'handler invocato dal Nucleo (passUpOrDie) quando
 * un U-proc genera un'eccezione diversa da TLB (ovvero: syscall positive,
 * program trap, accessi illegali).
 *
 * SYSCALL GESTITE (numeri positivi, invocate da U-proc in user-mode):
 *   SYS2  — Terminate:     il U-proc vuole terminare normalmente
 *   SYS4  — WriteTerminal: scrive una stringa sul terminale 0
 *   SYS5  — ReadTerminal:  legge una riga dal terminale 0
 *   SYS6  — Execute:       la shell lancia un programma figlio (solo ASID 1)
 *
 * PROGRAM TRAP:
 *   Qualsiasi altra eccezione (accesso illegale, istruzione invalida, ecc.)
 *   è considerata un errore fatale del U-proc: il processo viene terminato.
 *
 * NOTA SUL PC:
 *   exception.c nel Nucleo incrementa pc_epc di 4 PRIMA di chiamare
 *   passUpOrDie per le syscall. Quindi qui NON si deve incrementare pc_epc
 *   di nuovo. Il LDST finale riprende l'esecuzione dall'istruzione DOPO
 *   l'ecall che ha generato la syscall.
 *
 * TERMINALE 0 — LAYOUT REGISTRI (offset da TERM0_BASE):
 *   +0x00  recv_status    (sola lettura)
 *   +0x04  recv_command   (scrittura: RECEIVECHAR per ricevere un carattere)
 *   +0x08  transm_status  (sola lettura)
 *   +0x0C  transm_command (scrittura: TRANSMITCHAR | (char << 8))
 */

#include "../headers/const.h"
#include "../headers/types.h"
#include "../phase1/headers/pcb.h"
#include "../phase2/headers/initial.h"
#include "headers/sysSupport.h"
#include "headers/initProc.h"
#include "headers/vmSupport.h"
#include <uriscv/liburiscv.h>
#include <uriscv/cpu.h>

/* Indirizzo base dei registri del terminale 0.
 * START_DEVREG è la base di tutti i device register.
 * IL_TERMINAL=21, IL_DISK=17: offset linea terminale = (21-17)*0x80 = 0x200 byte.
 * Ogni device occupa 0x10 byte; terminale 0 è il device 0 della sua linea. */
#define TERM0_BASE (START_DEVREG + (IL_TERMINAL - IL_DISK) * 0x80)

/* -----------------------------------------------------------------------
 * Funzione di terminazione per program trap / errore fatale
 * ----------------------------------------------------------------------- */

/*
 * programTrapHandler — termina il U-proc corrente dopo un errore irrecuperabile.
 *
 * Prima di chiamare TERMPROCESS, sblocca il processo che attende la terminazione:
 *   - ASID 1 (shell) → V(masterSemaphore): sblocca test()
 *   - ASID 2..8 (programma) → V(shellSemaphore): sblocca la shell
 *
 * Chiamata anche per syscall con argomenti non validi (es. puntatore < KUSEG).
 */
static void programTrapHandler(support_t *sup) {
    if (sup->sup_asid == 1)
        SYSCALL(VERHOGEN, (int)&masterSemaphore, 0, 0);
    else
        SYSCALL(VERHOGEN, (int)&shellSemaphore, 0, 0);
    SYSCALL(TERMPROCESS, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * SYS2 — Terminate
 * ----------------------------------------------------------------------- */

/*
 * sys2_terminate — gestisce la richiesta di terminazione normale di un U-proc.
 *
 * Un U-proc chiama SYS2 (TERMINATE, a0=2) per terminare in modo pulito.
 * Logica identica a programTrapHandler: segnala al processo padre che
 * l'attende e poi si autodistrugge.
 *
 * Questa funzione non ritorna mai.
 */
static void sys2_terminate(support_t *sup) {
    /* Segnala al padre: V sul semaforo corretto in base all'ASID */
    if (sup->sup_asid == 1)
        SYSCALL(VERHOGEN, (int)&masterSemaphore, 0, 0);
    else
        SYSCALL(VERHOGEN, (int)&shellSemaphore, 0, 0);
    SYSCALL(TERMPROCESS, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * SYS4 — WriteTerminal
 * ----------------------------------------------------------------------- */

/*
 * sys4_writeTerminal — scrive una stringa sul terminale 0 (SYS4).
 *
 * Argomenti (passati via registri salvati in sup_exceptState[GENERALEXCEPT]):
 *   reg_a1 = indirizzo virtuale della stringa (deve essere in KUSEG: ≥ 0x80000000)
 *   reg_a2 = lunghezza della stringa (0..MAXSTRINGLEN=128)
 *
 * Valore di ritorno (in reg_a0 dello stato salvato):
 *   ≥ 0 → numero di caratteri effettivamente scritti
 *   < 0 → negativo dello status del device in caso di errore
 *
 * Validazione:
 *   - len < 0 o len > MAXSTRINGLEN: errore fatale (program trap)
 *   - str < KUSEG: puntatore in spazio kernel → non permesso (program trap)
 *
 * Mutua esclusione:
 *   termMutexTX protegge il canale TX del terminale 0.
 *   Necessario perché più U-proc potrebbero scrivere contemporaneamente.
 *
 * Protocollo di trasmissione (per ogni carattere):
 *   1. Scrive in transm_command: bits [15:8] = carattere, bits [7:0] = TRANSMITCHAR
 *   2. DOIO si blocca finché il device genera un interrupt
 *   3. Controlla status & 0xFF: deve essere OKCHARTRANS (5) per successo
 *   4. Se errore, ritorna il negativo dello status e libera il mutex
 */
static void sys4_writeTerminal(support_t *sup) {
    state_t *saved = &sup->sup_exceptState[GENERALEXCEPT];  /* stato salvato al fault */
    char *str = (char *) saved->reg_a1;   /* indirizzo della stringa in user space */
    int   len = (int)   saved->reg_a2;    /* numero di caratteri da scrivere */

    /* Validazione argomenti: lunghezza fuori range o puntatore in kernel space */
    if (len < 0 || len > MAXSTRINGLEN || (unsigned int)str < KUSEG) {
        programTrapHandler(sup);
        return;
    }

    /* Acquisisce il mutex TX: accesso esclusivo al canale di trasmissione */
    SYSCALL(PASSEREN, (int)&termMutexTX, 0, 0);

    /* Indirizzo del registro transm_command (offset 0x0C dalla base del device) */
    unsigned int txCmdAddr = TERM0_BASE + 0x0C;
    int written = 0, i;

    for (i = 0; i < len; i++) {
        /* Formato comando: parte alta = carattere ASCII, parte bassa = TRANSMITCHAR */
        unsigned int cmd = ((unsigned int)(unsigned char)str[i] << 8) | TRANSMITCHAR;

        /* DOIO si blocca finché il terminale completa la trasmissione del carattere */
        int status = SYSCALL(DOIO, (int)txCmdAddr, (int)cmd, 0);

        if ((status & 0xFF) != OKCHARTRANS) {
            /* Errore: restituisce il negativo dello status e torna al U-proc */
            saved->reg_a0 = (unsigned int)(-(int)status);
            SYSCALL(VERHOGEN, (int)&termMutexTX, 0, 0);
            LDST(saved);
            return;
        }
        written++;  /* carattere trasmesso con successo */
    }

    /* Tutti i caratteri scritti: rilascia mutex e ritorna al U-proc */
    SYSCALL(VERHOGEN, (int)&termMutexTX, 0, 0);
    saved->reg_a0 = (unsigned int) written;  /* valore di ritorno: caratteri scritti */
    LDST(saved);    /* ripristina stato U-proc: continua dopo l'ecall */
}

/* -----------------------------------------------------------------------
 * SYS5 — ReadTerminal
 * ----------------------------------------------------------------------- */

/*
 * sys5_readTerminal — legge una riga dal terminale 0 nel buffer utente (SYS5).
 *
 * Argomenti:
 *   reg_a1 = indirizzo virtuale del buffer di destinazione (deve essere ≥ KUSEG)
 *
 * Valore di ritorno (in reg_a0):
 *   ≥ 0 → numero di caratteri letti (incluso il newline se presente)
 *   < 0 → negativo dello status del device in caso di errore
 *
 * Terminazione della lettura:
 *   - Newline ('\n'): la riga è completa, si smette di leggere.
 *   - MAXSTRINGLEN caratteri raggiunti: buffer pieno, si smette.
 *
 * Mutua esclusione:
 *   termMutexRX protegge il canale RX. Separato da termMutexTX:
 *   lettura e scrittura possono avvenire contemporaneamente senza conflitti.
 *
 * Protocollo di ricezione (per ogni carattere):
 *   1. Scrive RECEIVECHAR in recv_command
 *   2. DOIO si blocca finché il device segnala un carattere disponibile
 *   3. Controlla status & 0xFF: deve essere RECVD (5) per successo
 *   4. Il carattere ricevuto è in bits [15:8] dello status
 */
static void sys5_readTerminal(support_t *sup) {
    state_t *saved = &sup->sup_exceptState[GENERALEXCEPT];
    char *buf = (char *) saved->reg_a1;   /* buffer utente di destinazione */

    /* Validazione: il buffer deve essere in spazio utente (KUSEG = 0x80000000) */
    if ((unsigned int)buf < KUSEG) {
        programTrapHandler(sup);
        return;
    }

    /* Acquisisce il mutex RX: accesso esclusivo al canale di ricezione */
    SYSCALL(PASSEREN, (int)&termMutexRX, 0, 0);

    /* Indirizzo del registro recv_command (offset 0x04 dalla base del device) */
    unsigned int rxCmdAddr = TERM0_BASE + 0x04;
    int read = 0;

    while (read < MAXSTRINGLEN) {
        /* Avvia la ricezione di un carattere: DOIO si blocca fino all'interrupt */
        int status = SYSCALL(DOIO, (int)rxCmdAddr, RECEIVECHAR, 0);

        if ((status & 0xFF) != RECVD) {
            /* Errore di ricezione: restituisce negativo dello status */
            saved->reg_a0 = (unsigned int)(-(int)status);
            SYSCALL(VERHOGEN, (int)&termMutexRX, 0, 0);
            LDST(saved);
            return;
        }

        /* Il carattere ricevuto è nei bit [15:8] dello status */
        char c = (char)((status >> 8) & 0xFF);
        buf[read++] = c;

        /* Fine riga: il newline è incluso nel buffer (compatibile con fgets) */
        if (c == '\n') break;
    }

    /* Rilascia mutex e torna al U-proc con il numero di caratteri letti */
    SYSCALL(VERHOGEN, (int)&termMutexRX, 0, 0);
    saved->reg_a0 = (unsigned int) read;
    LDST(saved);
}

/* -----------------------------------------------------------------------
 * SYS6 — Execute
 * ----------------------------------------------------------------------- */

/*
 * sys6_execute — lancia un programma figlio a nome della shell (SYS6).
 *
 * SOLO la shell (ASID 1) può chiamare Execute. Chiamate da ASID 2..8
 * sono errori di programma → programTrapHandler.
 *
 * Argomenti:
 *   reg_a1 = ASID del processo da lanciare (deve essere 2..UPROCMAX)
 *
 * Valore di ritorno (in reg_a0):
 *   0 → esecuzione completata con successo (il figlio è terminato)
 *  -1 → ASID fuori range
 *
 * Funzionamento:
 *   1. Valida l'ASID del figlio (2..UPROCMAX).
 *   2. Inizializza la support_t del figlio (page table + handler contexts).
 *   3. Prepara lo stato iniziale del figlio (identico a launchUproc in initProc.c).
 *   4. Crea il processo figlio via SYS1 (CREATEPROCESS).
 *   5. La shell esegue P(shellSemaphore) e si blocca.
 *   6. Quando il figlio termina (sys2_terminate o programTrapHandler),
 *      fa V(shellSemaphore) prima di TERMPROCESS → sblocca la shell.
 *   7. La shell riprende e ritorna il controllo alla SYS6 (LDST).
 *
 * Nota: il figlio usa la propria support_t con ASID diverso,
 * quindi ha un address space separato (TLB lo distingue tramite ASID).
 */
static void sys6_execute(support_t *sup) {
    state_t *saved = &sup->sup_exceptState[GENERALEXCEPT];

    /* Solo la shell (ASID 1) può eseguire programmi figli */
    if (sup->sup_asid != 1) {
        programTrapHandler(sup);
        return;
    }

    int asid = (int) saved->reg_a1;

    /* Validazione: ASID del figlio deve essere in range 2..UPROCMAX */
    if (asid < 2 || asid > UPROCMAX) {
        saved->reg_a0 = (unsigned int) -1;  /* ASID non valido: ritorna errore */
        LDST(saved);
        return;
    }

    /* Inizializza la support_t del nuovo U-proc:
     * - ASID impostato
     * - page table: 32 entry tutte invalide (V=0), pronte per demand paging
     * - contesti handler: pager() e supportGeneralExHandler() */
    support_t *newSup = &supportStructures[asid - 1];
    initSupportStruct(newSup, asid);

    /* Prepara lo stato iniziale del processo figlio */
    state_t newState = {0};   /* azzera tutti i campi */
    newState.pc_epc   = UPROCSTARTADDR;     /* entry point del programma */
    newState.reg_sp   = USERSTACKTOP;       /* stack pointer iniziale */
    /* User-mode (MPP=00), interrupt abilitati */
    newState.status   = MSTATUS_MPIE_MASK | MSTATUS_MIE_MASK;
    newState.mie      = MIE_ALL;                     /* tutti gli interrupt */
    newState.entry_hi = (asid << ASIDSHIFT);         /* ASID nel TLB EntryHi */

    /* Crea il processo figlio: il Nucleo lo aggiunge alla readyQueue */
    SYSCALL(CREATEPROCESS, (int)&newState, 0, (int)newSup);

    /* La shell si blocca qui finché il figlio non termina.
     * Il figlio farà V(shellSemaphore) in sys2_terminate() o programTrapHandler(). */
    SYSCALL(PASSEREN, (int)&shellSemaphore, 0, 0);

    /* Il figlio è terminato: ritorna al U-proc shell (reg_a0 = 0 implicitamente) */
    LDST(saved);
}

/* -----------------------------------------------------------------------
 * Dispatcher delle syscall
 * ----------------------------------------------------------------------- */

/*
 * syscallHandler — smista la syscall al gestore corretto in base al numero.
 *
 * Il numero della syscall è in reg_a0 dello stato salvato.
 * I numeri positivi sono gestiti qui (Support Level).
 * Numeri non riconosciuti → program trap (comportamento indefinito per U-proc).
 */
static void syscallHandler(support_t *sup) {
    state_t *saved  = &sup->sup_exceptState[GENERALEXCEPT];
    int syscallNum  = (int) saved->reg_a0;  /* numero syscall passato in a0 */

    switch (syscallNum) {
        case TERMINATE:     sys2_terminate(sup);     break;  /* SYS2: terminazione */
        case WRITETERMINAL: sys4_writeTerminal(sup); break;  /* SYS4: output terminale */
        case READTERMINAL:  sys5_readTerminal(sup);  break;  /* SYS5: input terminale */
        case EXECUTE:       sys6_execute(sup);       break;  /* SYS6: lancia programma */
        default:            programTrapHandler(sup); break;  /* syscall sconosciuta */
    }
}

/* -----------------------------------------------------------------------
 * Entry point del general exception handler
 * ----------------------------------------------------------------------- */

/*
 * supportGeneralExHandler — handler per le general exception dei U-proc.
 *
 * Invocato dal Nucleo (passUpOrDie con GENERALEXCEPT) quando un U-proc genera:
 *   - EXC_ECU (8): ECALL da U-mode (syscall normale dal codice utente)
 *   - EXC_ECM (11): ECALL da M-mode (normalizzato da exception.c del Nucleo)
 *   - Qualsiasi altra eccezione (trap): program trap → terminazione
 *
 * Sequenza:
 *   1. Recupera la support_t del processo corrente (SYS8).
 *   2. Legge il codice eccezione dal cause register salvato.
 *   3. Se è una syscall (ECALL): la smista a syscallHandler().
 *   4. Altrimenti: program trap → programTrapHandler().
 *
 * NOTA: exception.c ha già incrementato pc_epc di 4 prima di passUpOrDie,
 * quindi LDST in syscallHandler tornerà all'istruzione SUCCESSIVA all'ecall.
 * NON bisogna incrementare pc_epc di nuovo qui.
 */
void supportGeneralExHandler(void) {
    /* Recupera la struttura di supporto del processo corrente */
    support_t *sup  = (support_t *) SYSCALL(GETSUPPORTPTR, 0, 0, 0);
    state_t *saved  = &sup->sup_exceptState[GENERALEXCEPT];

    /* Estrae il codice eccezione dal cause register (bits EXCCODE) */
    unsigned int excCode = saved->cause & CAUSE_EXCCODE_MASK;

    /* EXC_ECU=8 (ECALL in U-mode) o EXC_ECM=11 (ECALL in M-mode, normalizzato).
     * Entrambi indicano che il U-proc ha eseguito una syscall. */
    if (excCode == EXC_ECU || excCode == EXC_ECM) {
        syscallHandler(sup);    /* smista alla syscall corretta */
    } else {
        /* Program trap: istruzione illegale, accesso allineamento, ecc. */
        programTrapHandler(sup);
    }
}
