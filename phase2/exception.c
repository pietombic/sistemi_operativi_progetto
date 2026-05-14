/*
 * exception.c — Gestore delle eccezioni del Nucleo (Phase 2)
 *
 * Questo file è il punto di ingresso unico per TUTTE le eccezioni che si
 * verificano in μRISC-V. La CPU, quando incontra un'eccezione (interrupt,
 * syscall, fault di indirizzo, istruzione illegale, ecc.), salva lo stato
 * corrente nella BIOS Data Page e salta all'handler registrato.
 *
 * Responsabilità principali:
 *   1. uTLB_RefillHandler  — gestisce i TLB miss ricaricando la PTE corretta
 *   2. exceptionHandler    — smista ogni eccezione al sotto-handler appropriato
 *   3. systemCallHandler   — gestisce le 10 syscall del Nucleo (SYS1–SYS10)
 *   4. Le 10 funzioni di syscall (createProcess, terminateProcess, ecc.)
 *
 * Relazione con il resto del sistema:
 *   - Phase 1 (PCB/ASL): usate per gestire processi e semafori
 *   - interrupt.c: interruptHandler() viene chiamato da exceptionHandler()
 *   - functions.c: utility condivise (blockCurrentProcess, passUpOrDie, ecc.)
 *   - Phase 3: passUpOrDie() trasferisce eccezioni non gestite al Support Level
 */

#include "../headers/types.h"
#include "../headers/const.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include "headers/exception.h"
#include "headers/interrupt.h"
#include <uriscv/liburiscv.h>
#include <uriscv/cpu.h>
#include <stddef.h>
#include "headers/klog.h"
#include "headers/functions.h"

/*
 * uTLB_RefillHandler — Gestore del TLB miss (ricaricamento TLB)
 *
 * Ruolo nel sistema:
 *   Quando la CPU tenta di accedere a un indirizzo virtuale e non trova
 *   la corrispondente entry nel TLB (Translation Lookaside Buffer), genera
 *   un evento di "TLB Refill". Questo handler ha il compito di trovare la
 *   Page Table Entry (PTE) corretta nella page table privata del processo
 *   corrente e di scriverla nel TLB, così che la CPU possa ritentare
 *   l'accesso con successo.
 *
 *   Questo handler è distinto dal gestore di Page Fault: il TLB Refill
 *   gestisce solo i miss (la pagina è valida ma non è nel TLB), mentre
 *   il Page Fault (gestito da Phase 3) gestisce i casi in cui la pagina
 *   non è in memoria fisica.
 *
 */
void uTLB_RefillHandler(void) {
    /* Lo stato salvato dalla CPU si trova sempre all'inizio della BIOS Data Page.
       Contiene i registri del processo interrotto, incluso entry_hi con il VPN
       dell'indirizzo che ha causato il miss. */
    state_t *savedState = (state_t *) BIOSDATAPAGE;

    /* Estraiamo il Virtual Page Number (VPN) dall'EntryHi salvata.
       EntryHi contiene sia il VPN sia l'ASID del processo. La maschera
       ENTRYHI_VPN_MASK isola i bit del VPN, poi li shiftiamo a destra di
       VPNSHIFT (12) per ottenere il numero di pagina. */
    unsigned int entryHi = savedState->entry_hi;
    unsigned int vpn     = (entryHi & ENTRYHI_VPN_MASK) >> VPNSHIFT;

    /* Calcolo dell'indice nella page table del processo.
       In μRISC-V, i bit 29:12 dell'indirizzo virtuale formano il VPN
       relativo al segmento (non assoluto). Per KUSEG (0x80000000–0xBFFFFFFF):
         - Pagine 0..30: VPN relativo = 0..30  → indice diretto
         - Pagina di stack (31): VA = 0xBFFFF000 → bit 29:12 = 0x3FFFF
           (caso speciale perché il bit 30 è settato in quell'indirizzo)
       Senza questo controllo useremmo un indice fuori bounds nell'array. */
    int pageIndex;
    if (vpn == 0x3FFFF) {
        /* Ultima pagina (stack): mappata sempre all'indice 31 (MAXPAGES-1) */
        pageIndex = MAXPAGES - 1;
    } else {
        pageIndex = (int)vpn;
    }

    /* Leggiamo la PTE dalla page table privata del processo corrente.
       sup_privatePgTbl è un array di MAXPAGES=32 entry, una per pagina virtuale.
       Ogni entry contiene EntryHi (VPN+ASID) ed EntryLo (PFN+flags). */
    pteEntry_t *pte = &currentProcess->p_supportStruct->sup_privatePgTbl[pageIndex];

    /* Carichiamo la PTE nel TLB tramite le istruzioni privilegiate:
       - setENTRYHI / setENTRYLO: scrivono i registri CP0 TLBHI/TLBLO
       - TLBWR: scrive l'entry nel TLB (sovrascrive un'entry casuale con TLBWR) */
    setENTRYHI(pte->pte_entryHI);
    setENTRYLO(pte->pte_entryLO);
    TLBWR();

    /* Ritorniamo al processo: la CPU rieseguirà l'istruzione che aveva causato
       il miss, questa volta trovando l'entry nel TLB. */
    LDST(savedState);
}

/*
 * exceptionHandler — Dispatcher centrale di tutte le eccezioni
 *
 * Ruolo nel sistema:
 *   È il punto di ingresso unico registrato come handler delle eccezioni
 *   generali in μRISC-V. Ogni volta che si verifica un'eccezione (qualsiasi
 *   tipo), la CPU salva lo stato e chiama questa funzione.
 *
 *   Il compito di exceptionHandler è esaminare il codice dell'eccezione
 *   (campo cause del registro CAUSE) e smistare al gestore corretto:
 *     - Interrupt       → interruptHandler()   (interrupt.c)
 *     - Syscall ECALL   → systemCallHandler()  (questo file)
 *     - Tutto il resto  → passUpOrDie()        (TLB fault, access fault, ecc.)
 *
 *   Questa struttura "dispatcher" mantiene il codice modulare e permette
 *   a Phase 3 di intercettare le eccezioni non gestite dal Nucleo.
 */
void exceptionHandler() {
    /* Lo stato salvato si trova nella BIOS Data Page: è lo snapshot completo
       dei registri della CPU al momento dell'eccezione. */
    state_t *exceptionState = (state_t *) BIOSDATAPAGE;
    unsigned int cause   = exceptionState->cause;
    /* excCode è nei bit 6:2 del registro cause (standard RISC-V) */
    unsigned int excCode = cause & CAUSE_EXCCODE_MASK;

    /* Caso 1: Interrupt (excCode indica interrupt pending nel registro MIP).
       La macro CAUSE_IS_INT controlla se l'eccezione è un interrupt. */
    if (CAUSE_IS_INT(cause)) {
        interruptHandler();
        return;
    }

    /* Caso 2: ECALL (Environment Call) — la syscall di RISC-V.
       excCode == 8  → ECALL da user-mode (U-mode)
       excCode == 11 → ECALL da machine-mode (M-mode, usato dal kernel)
       Normalizziamo il cause code a 11 (M-mode) prima di passarlo al
       systemCallHandler, così i gestori di Phase 3 vedono sempre 11. */
    if (excCode == 8 || excCode == 11) {
        /* Avanziamo il PC di 4 byte (WORDLEN) per saltare l'istruzione ECALL:
           senza questo, al ritorno la CPU rieseguirebbe la syscall all'infinito. */
        exceptionState->pc_epc += WORDLEN;
        exceptionState->cause = 11;
        systemCallHandler(exceptionState);
        return;
    }

    /* Caso 3: Tutte le altre eccezioni (TLB fault, page fault, istruzione
       illegale, accesso non allineato, ecc.).
       Invece di dipendere dai codici esatti di μRISC-V per i TLB fault,
       usiamo BADVADDR: se l'indirizzo incriminato appartiene a KUSEG
       (>= 0x80000000), è un page fault; altrimenti è un'eccezione generale. */
    int exType = (getBADVADDR() >= KUSEG) ? PGFAULTEXCEPT : GENERALEXCEPT;
    passUpOrDie(exType, exceptionState);
}

/*
 * systemCallHandler — Gestore delle syscall del Nucleo
 *
 * Ruolo nel sistema:
 *   Gestisce le 10 syscall primitive del Nucleo (SYS1–SYS10), identificate
 *   da numeri NEGATIVI (-1 a -10) nel registro a0. Queste syscall sono
 *   disponibili solo ai processi in kernel-mode (M-mode).
 *
 *   Regole di accesso:
 *     - service_code >= 1: codice non valido per il Nucleo → passa a Phase 3
 *     - service_code negativo in user-mode: violazione di privilegio → GENERALEXCEPT
 *     - service_code negativo in kernel-mode: eseguito direttamente
 *
 *   Le syscall di Phase 3 (positive, SYS2 TERMINATE, SYS4 WRITETERMINAL, ecc.)
 *   vengono gestite dalla funzione passUpOrDie, che le trasferisce al
 *   Support Level (Phase 3).
 */
void systemCallHandler(state_t *exceptionState) {
    /* Il codice del servizio richiesto è in reg_a0 (primo argomento RISC-V) */
    int service_code = (int) exceptionState->reg_a0;

    /* Codici >= 1: non sono syscall del Nucleo (che usa numeri negativi).
       Possono essere syscall di Phase 3 (positive) o codici non validi.
       In entrambi i casi deleghiamo a passUpOrDie come eccezione generale. */
    if (service_code >= 1) {
        passUpOrDie(GENERALEXCEPT, exceptionState);
        return;
    }

    /* Syscall negativa richiesta da user-mode: violazione di privilegio.
       MSTATUS_MPP_MASK isola i bit MPP (Machine Previous Privilege) dal
       registro status, che indicano il livello di privilegio prima dell'eccezione.
       MSTATUS_MPP_U == 0 → il processo era in U-mode (user).
       Impostiamo il cause code a PRIVINSTR (istruzione privilegiata non autorizzata)
       e solleviamo una GENERALEXCEPT verso il Support Level o terminiamo. */
    if ((exceptionState->status & MSTATUS_MPP_MASK) == MSTATUS_MPP_U) {
        /* Sostituiamo il cause code mantenendo tutti i bit tranne excCode,
           che sovrascriviamo con PRIVINSTR (Privileged Instruction). */
        exceptionState->cause = (exceptionState->cause & ~GETEXECCODE) | (PRIVINSTR << CAUSESHIFT);
        passUpOrDie(GENERALEXCEPT, exceptionState);
        return;
    }

    /* Dispatch sulla syscall richiesta: ogni caso chiama la funzione dedicata.
       I nomi delle costanti corrispondono alle macro in const.h (CREATEPROCESS=-1, ecc.) */
    switch (service_code) {
        case CREATEPROCESS:   /* SYS1 (-1): crea un nuovo processo figlio */
            createProcess(exceptionState);
            break;
        case TERMPROCESS:     /* SYS2 (-2): termina un processo e la sua progenie */
            terminateProcess(exceptionState);
            break;
        case PASSEREN:        /* SYS3 (-3): operazione P (wait) su semaforo */
            passeren(exceptionState);
            break;
        case VERHOGEN:        /* SYS4 (-4): operazione V (signal) su semaforo */
            verhogen(exceptionState);
            break;
        case DOIO:            /* SYS5 (-5): avvia un'operazione I/O su dispositivo */
            doIO(exceptionState);
            break;
        case GETTIME:         /* SYS6 (-6): ottieni il tempo CPU accumulato */
            getCPUTime(exceptionState);
            break;
        case CLOCKWAIT:       /* SYS7 (-7): attendi il prossimo tick del pseudo-clock */
            waitForClock(exceptionState);
            break;
        case GETSUPPORTPTR:   /* SYS8 (-8): ottieni il puntatore alla support structure */
            getSupportData(exceptionState);
            break;
        case GETPROCESSID:    /* SYS9 (-9): ottieni il PID del processo (o del padre) */
            getProcessID(exceptionState);
            break;
        case YIELD:           /* SYS10 (-10): cedi volontariamente la CPU */
            yield(exceptionState);
            break;

        default:
            /* Codice non riconosciuto: trattato come eccezione generale */
            passUpOrDie(GENERALEXCEPT, exceptionState);
            break;
    }
}

/*
 * createProcess — SYS1: Creazione di un nuovo processo
 *
 * Ruolo nel sistema:
 *   Implementa la creazione dinamica di processi nel Nucleo. Il nuovo processo
 *   viene inserito come figlio di currentProcess (albero dei processi) e messo
 *   nella ready queue per essere schedulato.
 *
 *   Parametri (passati in registro dal chiamante):
 *     reg_a1 → puntatore allo stato iniziale del nuovo processo (state_t*)
 *     reg_a2 → priorità del nuovo processo
 *     reg_a3 → puntatore alla support_t (struttura di Phase 3), o NULL
 *
 *   Valore di ritorno:
 *     reg_a0 → PID del nuovo processo, oppure -1 se non ci sono PCB liberi
 */
void createProcess(state_t *state) {
    /* Allochiamo un PCB dalla lista libera (Phase 1).
       Se la lista è esaurita, allocPcb() restituisce NULL. */
    pcb_t *newPcb = allocPcb();

    /* Nessun PCB disponibile: segnaliamo l'errore al chiamante e riprendiamo */
    if (newPcb == NULL) {
        state->reg_a0 = (unsigned int) -1;
        /* Aggiorniamo lo stato salvato di currentProcess con il valore di ritorno
           e ripristiniamo l'esecuzione del processo chiamante. */
        copyState(&currentProcess->p_s, state);
        LDST(&currentProcess->p_s);
        return;
    }

    /* Copiamo lo stato iniziale fornito dal chiamante nel PCB del nuovo processo.
       reg_a1 punta a una struttura state_t che descrive i registri iniziali
       (program counter, stack pointer, ecc.) del nuovo processo. */
    copyState(&newPcb->p_s, (state_t *)state->reg_a1);

    /* Inizializzazione del PCB:
       - p_supportStruct: struttura Phase 3 (può essere NULL per processi kernel)
       - p_prio: priorità usata dallo scheduler per l'accodamento
       - p_time: tempo CPU accumulato, parte da zero
       - p_semAdd: nessun semaforo bloccante all'avvio */
    newPcb->p_supportStruct = (support_t *)state->reg_a3;
    newPcb->p_prio = (int) state->reg_a2;
    newPcb->p_time = 0;
    newPcb->p_semAdd = NULL;

    /* Inseriamo il nuovo processo nell'albero come figlio di currentProcess
       e nella ready queue per essere schedulato. Incrementiamo il contatore
       globale dei processi attivi. */
    insertChild(currentProcess, newPcb);
    insertProcQ(&readyQueue, newPcb);
    processCount++;

    /* Restituiamo il PID del nuovo processo al chiamante e riprendiamo */
    state->reg_a0 = (unsigned int) newPcb->p_pid;
    copyState(&currentProcess->p_s, state);
    LDST(state);
}

/*
 * terminateProcess — SYS2: Terminazione di un processo e della sua progenie
 *
 * Ruolo nel sistema:
 *   Permette a un processo di terminare se stesso (reg_a1 == 0) o un altro
 *   processo identificato dal suo PID. La terminazione è ricorsiva: tutti i
 *   figli (e i figli dei figli, ecc.) vengono terminati insieme al target.
 *
 *   Questo meccanismo garantisce che non rimangano processi "orfani" nel sistema
 *   dopo la terminazione del padre, mantenendo la coerenza dell'albero dei processi.
 *
 *   Parametri:
 *     reg_a1 → PID del processo da terminare (0 = se stesso)
 */
void terminateProcess(state_t *state) {
    int pidToTerminate = (int) state->reg_a1;

    /* Aggiorniamo il tempo CPU prima di tutto, per contabilizzare il tempo
       speso fino a questo momento dal processo chiamante. */
    updateCPUTime(currentProcess);

    /* Determiniamo il processo target:
       - PID 0 → il processo chiamante (currentProcess)
       - altro → cerchiamo nell'albero dei processi a partire dalla radice */
    pcb_t *targetProcess;
    if (pidToTerminate == 0) {
        targetProcess = currentProcess;
    } else {
        /* findInTree effettua una DFS sull'albero per trovare il processo con
           quel PID; restituisce NULL se non esiste. */
        targetProcess = findInTree(rootProcess, pidToTerminate);
    }

    /* PID non trovato nell'albero: restituiamo il controllo al chiamante
       senza fare nulla (la specifica non richiede un codice di errore qui). */
    if (targetProcess == NULL) {
        LDST(state);
        return;
    }

    /* Memorizziamo se stiamo terminando il processo corrente, perché dopo
       recursiveTerminateProcess il PCB potrebbe essere già liberato. */
    int isCurrent = (targetProcess == currentProcess);
    recursiveTerminateProcess(targetProcess);

    /* Se abbiamo terminato il processo corrente (o se currentProcess è diventato
       NULL per qualche motivo), invochiamo lo scheduler per eleggere il prossimo. */
    if (isCurrent || currentProcess == NULL) {
        currentProcess = NULL;
        scheduler();
    } else {
        /* Il processo chiamante è ancora attivo: aggiorniamo il suo stato salvato
           e riprendiamo la sua esecuzione. */
        copyState(&currentProcess->p_s, state);
        LDST(&currentProcess->p_s);
    }
}

/*
 * passeren — SYS3: Operazione P (wait/down) su semaforo
 *
 * Ruolo nel sistema:
 *   Implementa la primitiva di sincronizzazione P (Passeren, "passare" in
 *   olandese). Decrementa il valore del semaforo: se il risultato è negativo,
 *   il processo corrente si blocca finché un'altra entità non esegue una V
 *   sullo stesso semaforo.
 *
 *   I semafori sono usati nel sistema per:
 *     - Sincronizzazione tra processi (mutex, attesa di eventi)
 *     - Attesa di completamento I/O (deviceSemaphores)
 *     - Attesa del pseudo-clock (deviceSemaphores[48])
 *
 *   Parametri:
 *     reg_a1 → indirizzo del semaforo (int*)
 */
void passeren(state_t *state) {
    int *semAdd = (int *) state->reg_a1;

    /* Aggiorniamo il tempo CPU e salviamo lo stato del processo prima di
       potenzialmente bloccarlo: se blocchiamo, lo stato deve essere già
       coerente per quando il processo verrà risvegliato. */
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);

    /* Decremento atomico del semaforo */
    (*semAdd)--;

    if ((*semAdd) < 0) {
        /* Semaforo negativo: il processo deve attendere.
           blockCurrentProcess si occupa di inserire il processo nella lista
           dei bloccati del semaforo e di chiamare lo scheduler. */
        blockCurrentProcess(semAdd);
        /* Non si arriva mai qui: blockCurrentProcess() non ritorna */
    }

    /* Semaforo non negativo: la risorsa era disponibile, continuiamo */
    LDST(&currentProcess->p_s);
}

/*
 * verhogen — SYS4: Operazione V (signal/up) su semaforo
 *
 * Ruolo nel sistema:
 *   Implementa la primitiva di sincronizzazione V (Verhogen, "aumentare" in
 *   olandese). Incrementa il valore del semaforo e, se ci sono processi in
 *   attesa (valore ancora <= 0 dopo l'incremento), ne sblocca uno.
 *
 *   Il processo sbloccato viene inserito nella ready queue; sarà lo scheduler
 *   a decidere quando effettivamente eseguirlo.
 *
 *   Parametri:
 *     reg_a1 → indirizzo del semaforo (int*)
 */
void verhogen(state_t *state) {
    int *semAdd = (int *) state->reg_a1;

    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);

    /* Incremento del semaforo */
    (*semAdd)++;

    /* Se il valore è ancora <= 0, c'era almeno un processo in attesa.
       (Invariante: se sem < 0, |sem| == numero di processi bloccati)
       Sblocchiamo il primo processo in coda al semaforo. */
    if ((*semAdd) <= 0) {
        pcb_t *releasedPcb = removeBlocked(semAdd);
        if (releasedPcb != NULL) {
            /* Puliamo il riferimento al semaforo e inseriamo nella ready queue */
            releasedPcb->p_semAdd = NULL;
            insertProcQ(&readyQueue, releasedPcb);
            /* Nota: softBlockCount non viene decrementato qui perché verhogen
               può essere chiamata su qualsiasi semaforo, non solo su quelli di
               dispositivo. I semafori di dispositivo vengono V-ati dall'interrupt
               handler, che gestisce softBlockCount autonomamente. */
        }
    }

    LDST(&currentProcess->p_s);
}

/*
 * doIO — SYS5: Avvio di un'operazione I/O su dispositivo
 *
 * Ruolo nel sistema:
 *   Permette a un processo di interagire con i dispositivi di I/O (dischi,
 *   flash, stampanti, terminali). Il processo scrive un comando nel registro
 *   del dispositivo e poi si blocca sul semaforo corrispondente in attesa
 *   che l'interrupt handler segnali il completamento dell'operazione.
 *
 *   Questo meccanismo disaccoppia il processo dal dispositivo: il processo
 *   si sospende e lo scheduler può eseguire altri processi mentre il
 *   dispositivo lavora in hardware.
 *
 *   Mappa dei semafori (deviceSemaphores[]):
 *     [0..7]   → disk 0..7        (linea 3)
 *     [8..15]  → flash 0..7       (linea 4)
 *     [16..23] → ethernet 0..7    (linea 5)
 *     [24..31] → printer 0..7     (linea 6)
 *     [32..39] → terminal RX 0..7 (linea 7, ricezione)
 *     [40..47] → terminal TX 0..7 (linea 7, trasmissione)
 *     [48]     → pseudo-clock
 *
 *   Parametri:
 *     reg_a1 → indirizzo del registro comando del dispositivo
 *     reg_a2 → valore da scrivere nel registro comando
 */
void doIO(state_t *state) {
    unsigned int commandAddr  = (unsigned int)state->reg_a1;
    unsigned int commandValue = (unsigned int)state->reg_a2;

    /* Validazione: l'indirizzo deve cadere nell'intervallo dei registri dispositivo.
       START_DEVREG = 0x10000054, il limite superiore copre tutti i 49 dispositivi. */
    if (commandAddr < START_DEVREG || commandAddr > 0x100002D4) {
        state->reg_a0 = -1;
        LDST(state);
        return;
    }

    /* Calcolo dell'indice del semaforo dall'indirizzo del registro.
       La formula inverte la macro DEV_REG_BASE(line, dev):
         offset = commandAddr - START_DEVREG
         line   = (offset / 0x80) + 3   (ogni linea occupa 0x80 byte = 8 dev × 16 byte)
         dev    = (offset % 0x80) / 0x10 (ogni dispositivo occupa 0x10 byte = 16 byte) */
    unsigned int offset = commandAddr - START_DEVREG;
    int line = (offset / 0x80) + 3;
    int dev  = (offset % 0x80) / 0x10;

    int semIndex;
    if (line == 7) {
        /* I terminali (linea 7) hanno due sotto-dispositivi con semafori separati:
           TX (trasmissione, offset >= 8 nel blocco da 16 byte) e RX (ricezione).
           Il registro di comando TX è al byte offset 8 nel blocco del terminale. */
        int isWrite = ((offset % 0x10) >= 0x8);
        semIndex = isWrite ? (40 + dev) : (32 + dev);
    } else {
        /* Dispositivi non-terminale: indice lineare nella tabella */
        semIndex = (line - 3) * 8 + dev;
    }

    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);

    /* Decrementiamo il semaforo PRIMA di inviare il comando al dispositivo.
       Questo garantisce che il processo sia già "in attesa" quando arriva
       l'interrupt, evitando la race condition in cui l'interrupt arriva
       prima che il processo si blocchi. */
    deviceSemaphores[semIndex]--;

    /* Scrittura volatile: impedisce al compilatore di riordinare o eliminare
       questa scrittura. Il dispositivo hardware vede il comando solo quando
       la scrittura arriva effettivamente al bus di memoria. */
    *((volatile unsigned int *)commandAddr) = commandValue;

    /* Blocchiamo il processo in attesa del completamento dell'I/O.
       L'interrupt handler (interrupt.c) eseguirà la V corrispondente
       quando il dispositivo avrà completato l'operazione. */
    blockCurrentProcess(&deviceSemaphores[semIndex]);
    /* Non si ritorna mai qui */
}

/*
 * getCPUTime — SYS6: Lettura del tempo CPU accumulato
 *
 * Ruolo nel sistema:
 *   Permette a un processo di conoscere il proprio tempo di esecuzione
 *   totale (CPU time) dalla sua creazione. Utile per profiling e per
 *   implementare politiche di scheduling a livello utente.
 *
 *   Il valore restituito include il tempo del quanto corrente (aggiornato
 *   da updateCPUTime), quindi è sempre preciso al momento della chiamata.
 *
 *   Valore di ritorno:
 *     reg_a0 → tempo CPU accumulato in microsecondi (o tick di TOD)
 */
void getCPUTime(state_t *state) {
    /* Aggiorniamo prima il contatore per includere il tempo trascorso
       dall'inizio del quanto corrente fino a questo momento. */
    updateCPUTime(currentProcess);
    state->reg_a0 = (unsigned int) currentProcess->p_time;
    LDST(state);
}

/*
 * waitForClock — SYS7: Attesa del prossimo tick del pseudo-clock
 *
 * Ruolo nel sistema:
 *   Blocca il processo fino al prossimo tick dell'interval timer (ogni 100ms,
 *   PSECOND). Permette ai processi di sincronizzarsi con il tempo reale senza
 *   busy-waiting.
 *
 *   Il pseudo-clock è implementato tramite il semaforo deviceSemaphores[48]:
 *   ogni 100ms, l'interrupt handler esegue una V su tutti i processi bloccati
 *   su questo semaforo e lo azzera (non è un semaforo accumulante).
 *
 *   Nota: il semaforo viene sempre decrementato a prescindere dal suo valore
 *   corrente, potenzialmente diventando molto negativo se molti processi
 *   attendono. L'interrupt handler gestisce questo caso sbloccando tutti.
 */
void waitForClock(state_t *state) {
    updateCPUTime(currentProcess);
    copyState(&currentProcess->p_s, state);

    /* Semaforo 48 = pseudo-clock (ultimo dei 49 semafori di dispositivo) */
    deviceSemaphores[48]--;
    blockCurrentProcess(&deviceSemaphores[48]);
    /* Non si ritorna qui */
}

/*
 * getSupportData — SYS8: Ottenere il puntatore alla support structure
 *
 * Ruolo nel sistema:
 *   Restituisce il puntatore alla struttura support_t del processo corrente.
 *   Questa struttura è fondamentale per Phase 3: contiene la page table privata
 *   del processo, i contesti di eccezione del Support Level e l'ASID.
 *
 *   Un U-proc (processo utente di Phase 3) usa questa syscall per ottenere
 *   il proprio support_t e accedere alle strutture dati che lo riguardano.
 *   Se il processo non ha una support structure (è un processo puro del Nucleo),
 *   restituisce NULL.
 *
 *   Valore di ritorno:
 *     reg_a0 → puntatore a support_t, oppure NULL
 */
void getSupportData(state_t *state) {
    state->reg_a0 = (unsigned int) currentProcess->p_supportStruct;
    LDST(state);
}

/*
 * getProcessID — SYS9: Ottenere il PID del processo corrente o del padre
 *
 * Ruolo nel sistema:
 *   Permette a un processo di conoscere la propria identità o quella del padre
 *   all'interno dell'albero dei processi. Utile per Phase 3 dove ogni U-proc
 *   ha bisogno di identificarsi (es. per associarsi all'ASID corretto).
 *
 *   Parametri:
 *     reg_a1 → 0: restituisce il PID del processo corrente
 *               altro: restituisce il PID del padre (0 se non esiste)
 *
 *   Valore di ritorno:
 *     reg_a0 → PID richiesto, oppure 0 se non esiste il padre
 */
void getProcessID(state_t *state) {
    int supIndex = (int) state->reg_a1;
    if (supIndex == 0) {
        /* PID del processo corrente */
        state->reg_a0 = (unsigned int) currentProcess->p_pid;
    } else {
        /* PID del padre: restituiamo 0 se il processo non ha padre
           (es. il processo iniziale / root process) */
        state->reg_a0 = (currentProcess->p_parent != NULL)
                        ? (unsigned int) currentProcess->p_parent->p_pid
                        : 0;
    }
    LDST(state);
}

/*
 * yield — SYS10: Cessione volontaria della CPU
 *
 * Ruolo nel sistema:
 *   Permette a un processo di rinunciare al proprio quanto di tempo residuo
 *   e di tornare in coda. Lo scheduler eleggerà il prossimo processo da
 *   eseguire secondo la politica a priorità.
 *
 *   È diverso dal blocking: il processo rimane nella ready queue e può
 *   essere rischedulato immediatamente se è il più prioritario.
 *   Tipicamente usato quando un processo sa di non avere lavoro utile
 *   da fare in questo momento ma non vuole bloccarsi su un semaforo.
 */
void yield(state_t *state) {
    /* Contabilizziamo il tempo CPU del quanto corrente */
    updateCPUTime(currentProcess);
    /* Salviamo lo stato aggiornato nel PCB prima di reinserirlo in coda */
    copyState(&currentProcess->p_s, state);
    /* Rimettiamo il processo in ready queue (alla posizione corretta per priorità) */
    insertProcQ(&readyQueue, currentProcess);
    /* Liberiamo currentProcess così lo scheduler sa che non c'è un processo
       in esecuzione e ne deve scegliere uno dalla coda. */
    currentProcess = NULL;
    scheduler();
    /* Non si ritorna qui */
}
