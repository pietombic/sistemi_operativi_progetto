/*
 * functions.c — Funzioni di utilità condivise del Nucleo (Phase 2)
 *
 * Questo file raccoglie le funzioni di supporto usate da più moduli del Nucleo.
 * Non implementa direttamente syscall o handler, ma fornisce le operazioni
 * primitive su cui si appoggiano exception.c, interrupt.c e scheduler.c.
 *
 * Funzioni esportate:
 *   copyState              — copia di uno stato CPU (state_t)
 *   isSoftBlockSemaphore   — test: è un semaforo di dispositivo?
 *   blockCurrentProcess    — blocca il processo corrente su un semaforo
 *   findInTree             — DFS nell'albero dei processi per PID
 *   recursiveTerminateProcess — termina un processo e tutta la sua progenie
 *   updateCPUTime          — aggiorna il contatore di tempo CPU di un processo
 *   passUpOrDie            — passa un'eccezione a Phase 3 o termina il processo
 */

#include "../headers/const.h"
#include "../headers/types.h"
#include "../phase1/headers/pcb.h"
#include "../phase1/headers/asl.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include <uriscv/liburiscv.h>

/*
 * copyState — Copia di uno stato CPU da src a dst
 *
 * Ruolo nel sistema:
 *   In μRISC-V, lo stato di un processo (state_t) include tutti i registri
 *   general-purpose, il program counter (pc_epc), lo status, il cause e i
 *   registri CP0. Questa funzione copia uno stato intero word per word.
 *
 *   Viene usata in tutto il Nucleo ogni volta che occorre:
 *     - Salvare lo stato di un processo nel suo PCB prima di sospenderlo
 *     - Ripristinare uno stato salvato per riprendere un processo
 *     - Inizializzare lo stato di un nuovo processo (createProcess)
 *     - Propagare lo stato di eccezione al Support Level (passUpOrDie)
 *
 *   NOTA SULLE CONVENZIONI: copyState(dst, src) — il PRIMO argomento è la
 *   destinazione (come memcpy standard). Questa convenzione è critica: invertire
 *   dst e src causa bug sottilissimi dove si sovrascrive lo stato sbagliato.
 *
 *   Parametri:
 *     dst → puntatore alla struttura destinazione
 *     src → puntatore alla struttura sorgente
 */
void copyState(state_t *dst, state_t *src) {
    unsigned int *d = (unsigned int *) dst;
    unsigned int *s = (unsigned int *) src;
    /* Copiamo word per word per la dimensione esatta di state_t.
       STATE_T_SIZE_IN_BYTES / WORDLEN = numero di parole da 4 byte. */
    for (int i = 0; i < STATE_T_SIZE_IN_BYTES / WORDLEN; i++)
        d[i] = s[i];
}

/*
 * isSoftBlockSemaphore — Controlla se un semaforo è un semaforo di dispositivo
 *
 * Ruolo nel sistema:
 *   Nel Nucleo esistono due categorie di semafori:
 *     1. Semafori "di dispositivo" (deviceSemaphores[0..48]): usati per
 *        sincronizzare processi con I/O hardware e con il pseudo-clock.
 *        I processi bloccati su questi semafori incrementano softBlockCount.
 *     2. Semafori "generici" creati dai processi utente per sincronizzazione
 *        tra processi (mutex, barriere, ecc.).
 *
 *   Questa distinzione è necessaria per mantenere corretto softBlockCount,
 *   che lo scheduler usa per decidere se il sistema è in deadlock.
 *   Se softBlockCount > 0 e la ready queue è vuota, c'è attività I/O in corso
 *   (non deadlock); se entrambi sono zero e ci sono processi, è deadlock.
 *
 *   La verifica è semplice: controlla se l'indirizzo del semaforo cade
 *   nell'intervallo dell'array deviceSemaphores.
 *
 *   Parametri:
 *     semAdd → puntatore al semaforo da verificare
 *   Ritorna:
 *     1 (vero) se è un semaforo di dispositivo, 0 (falso) altrimenti
 */
int isSoftBlockSemaphore(int *semAdd) {
    return semAdd >= &deviceSemaphores[0] && semAdd < &deviceSemaphores[SEMDEVLEN];
}

/*
 * blockCurrentProcess — Blocca il processo corrente su un semaforo
 *
 * Ruolo nel sistema:
 *   Implementa il blocco di un processo su un semaforo nella sua forma
 *   completa: aggiorna i contatori, inserisce il processo nella lista dei
 *   bloccati del semaforo e invoca lo scheduler per eseguire un altro processo.
 *
 *   È usata da:
 *     - passeren() in exception.c (blocco su semaforo generico)
 *     - doIO() in exception.c (attesa completamento I/O)
 *     - waitForClock() in exception.c (attesa pseudo-clock)
 *
 *   Invarianti mantenuti:
 *     - currentProcess viene azzerato prima di chiamare lo scheduler
 *     - Il tempo CPU è aggiornato prima del blocco
 *     - softBlockCount è incrementato solo per semafori di dispositivo
 *
 *   ATTENZIONE: questa funzione non ritorna mai al chiamante direttamente.
 *   Lo scheduler assume il controllo e la funzione "ritorna" solo quando
 *   il processo viene successivamente sbloccato (da verhogen o interrupt)
 *   e lo scheduler lo eleggera' nuovamente. A quel punto l'esecuzione
 *   riprende da LDST nel chiamante originale.
 *
 *   Parametri:
 *     sem → puntatore al semaforo su cui bloccare il processo
 */
void blockCurrentProcess(int *sem) {
    /* Sicurezza: non dovremmo mai bloccare se non c'è un processo corrente.
       PANIC() arresta il sistema — indica un bug serio nel kernel. */
    if (!currentProcess) PANIC();

    /* Aggiornamento del tempo CPU: contabilizziamo il tempo trascorso
       dall'inizio del quanto corrente fino a questo momento.
       STCK legge il TOD (Time-Of-Day) clock ad alta risoluzione.
       È fondamentale farlo qui e non dopo il blocco, perché una volta
       sospeso il processo non può più aggiornare il proprio contatore. */
    cpu_t current_tod;
    STCK(current_tod);
    currentProcess->p_time += current_tod - start_time_current_quantum;
    /* Aggiorniamo il riferimento temporale per il prossimo processo che
       verrà eseguito dallo scheduler. */
    start_time_current_quantum = current_tod;

    /* Registriamo su quale semaforo il processo è bloccato.
       Questo campo è usato da recursiveTerminateProcess per rimuovere
       il processo dalla lista del semaforo in caso di terminazione forzata. */
    currentProcess->p_semAdd = sem;

    /* Se è un semaforo di dispositivo, incrementiamo softBlockCount.
       Lo scheduler controlla questo contatore per distinguere il caso
       "nessun processo pronto ma I/O in corso" (attesa legittima) dal
       deadlock (nessun processo pronto e nessuna attività I/O). */
    if (isSoftBlockSemaphore(sem)) {
        softBlockCount++;
    }

    /* Inseriamo il processo nella lista dei bloccati del semaforo (ASL).
       insertBlocked gestisce la lista FIFO dei processi bloccati. */
    insertBlocked(sem, currentProcess);

    /* Azzeriamo currentProcess e invochiamo lo scheduler.
       currentProcess = NULL segnala allo scheduler che non c'è un processo
       da interrompere e che deve sceglierne uno dalla ready queue. */
    currentProcess = NULL;
    scheduler();
    /* Non si ritorna mai qui */
}

/*
 * findInTree — Ricerca DFS di un processo nell'albero dei processi
 *
 * Ruolo nel sistema:
 *   Implementa una visita in profondità (DFS, Depth-First Search) dell'albero
 *   dei processi del Nucleo per trovare un processo dato il suo PID.
 *
 *   È usata da terminateProcess() in exception.c per localizzare il processo
 *   target quando la syscall specifica un PID diverso da 0 (diverso da sé).
 *
 *   Struttura dell'albero:
 *     - Ogni PCB ha una lista di figli (p_child) e un puntatore al padre (p_parent)
 *     - La radice è rootProcess (il processo iniziale, creato da initial.c)
 *     - list_for_each + container_of itera sulla lista dei figli RISC-V style
 *
 *   Complessità: O(n) nel numero di processi. Accettabile dato che UPROCMAX=8
 *   e il numero totale di processi nel sistema è piccolo.
 *
 *   Parametri:
 *     node → nodo corrente dell'albero (chiamata iniziale: rootProcess)
 *     pid  → PID del processo cercato
 *   Ritorna:
 *     puntatore al PCB trovato, oppure NULL se non esiste
 */
pcb_t *findInTree(pcb_t *node, int pid) {
    /* Caso base: nodo nullo o processo già terminato (PID negativo = marcato
       come terminato da recursiveTerminateProcess, ma non ancora liberato) */
    if (node == NULL || node->p_pid < 0) return NULL;

    /* Trovato il processo cercato */
    if (node->p_pid == pid) return node;

    /* Visita ricorsiva di tutti i figli.
       list_for_each itera su p_child (lista dei figli diretti).
       container_of recupera il puntatore al PCB completo dalla lista. */
    struct list_head *pos;
    list_for_each(pos, &node->p_child) {
        pcb_t *child = container_of(pos, pcb_t, p_sib);
        pcb_t *found = findInTree(child, pid);
        if (found) return found;
    }
    return NULL;
}

/*
 * recursiveTerminateProcess — Termina un processo e tutta la sua progenie
 *
 * Ruolo nel sistema:
 *   Implementa la terminazione "a cascata" di un sotto-albero dell'albero
 *   dei processi. Quando un processo viene terminato, tutti i suoi discendenti
 *   (figli, nipoti, ecc.) vengono terminati con lui.
 *
 *   Per ogni processo terminato, la funzione deve:
 *     1. Rimuoverlo dalla lista dei bloccati del semaforo (se bloccato)
 *     2. Aggiornare il semaforo e i contatori (softBlockCount, processCount)
 *     3. Rimuoverlo dalla ready queue (se era in attesa di esecuzione)
 *     4. Staccarlo dall'albero dei processi (outChild)
 *     5. Liberare il PCB (freePcb)
 *
 *   NOTA sulla gestione del semaforo:
 *     Se il processo è bloccato su un semaforo di DISPOSITIVO: decrementiamo
 *       solo softBlockCount (l'interrupt handler farà la V quando il
 *       dispositivo completerà, ma non ci sarà più nessuno da sbloccare).
 *     Se è bloccato su un semaforo GENERICO: incrementiamo il semaforo (*semAdd++)
 *       perché il processo che aspettava la risorsa non la userà più, e
 *       dobbiamo lasciare il semaforo in uno stato coerente.
 *
 *   Parametri:
 *     target → puntatore al PCB radice del sotto-albero da terminare
 */
void recursiveTerminateProcess(pcb_t *target) {
    if (!target) return;
    /* Processo già marcato come terminato: saltiamo per evitare doppia liberazione */
    if (target->p_pid < 0) return;

    /* Marchiamo il processo come "in corso di terminazione" con PID negativo.
       Questo evita che findInTree lo trovi di nuovo durante la ricorsione
       e che vengano fatte operazioni su un PCB parzialmente smontato. */
    target->p_pid = -1;

    /* Terminazione ricorsiva dei figli: removeChild stacca il primo figlio
       dalla lista e lo restituisce; ripetiamo finché non ci sono più figli. */
    pcb_t *child;
    while ((child = removeChild(target)) != NULL) {
        recursiveTerminateProcess(child);
    }

    /* Gestione del semaforo su cui il processo potrebbe essere bloccato */
    if (target->p_semAdd != NULL) {
        int *semAdd = target->p_semAdd;

        /* outBlocked rimuove il processo dalla lista dei bloccati del semaforo.
           Restituisce il puntatore al processo rimosso, o NULL se non era lì. */
        pcb_t *removed = outBlocked(target);
        if (removed) {
            if (isSoftBlockSemaphore(semAdd)) {
                /* Semaforo di dispositivo: decrementiamo solo il contatore.
                   Non incrementiamo il semaforo perché l'interrupt handler
                   lo farà quando il dispositivo completerà; a quel punto non
                   ci sarà nessuno da sbloccare ma il semaforo tornerà a 0. */
                softBlockCount--;
            } else {
                /* Semaforo generico: il processo non utilizzerà la risorsa,
                   quindi "restituiamo" il token al semaforo per non bloccare
                   altri processi che attendono quella risorsa. */
                (*semAdd)++;
            }
        }
    }

    /* Rimuoviamo il processo dalla ready queue nel caso fosse in attesa
       di esecuzione (outProcQ è no-op se il processo non è nella coda). */
    outProcQ(&readyQueue, target);

    /* Se stiamo terminando il processo corrente, azzeriamo il puntatore.
       Il chiamante (terminateProcess) controllerà questo e invocherà
       lo scheduler di conseguenza. */
    if (target == currentProcess) {
        currentProcess = NULL;
    }

    /* Stacchiamo il processo dalla lista dei figli del genitore.
       Senza questo, il genitore avrebbe un puntatore dangling al PCB liberato. */
    outChild(target);

    /* Liberiamo il PCB e decrementiamo il contatore dei processi attivi */
    freePcb(target);
    processCount--;
}

/*
 * updateCPUTime — Aggiorna il contatore di tempo CPU del processo
 *
 * Ruolo nel sistema:
 *   Contabilizza il tempo di esecuzione di un processo aggiornando p_time
 *   con il tempo trascorso dall'inizio del quanto corrente.
 *
 *   Il Nucleo usa un sistema "lazy" per il conteggio del tempo:
 *   invece di usare un timer per ogni processo, si salva il TOD all'inizio
 *   di ogni quanto (start_time_current_quantum) e si calcola la differenza
 *   ogni volta che è necessario aggiornare il tempo (syscall, blocco, ecc.).
 *
 *   Viene chiamata prima di qualsiasi operazione che potrebbe:
 *     - Sospendere il processo (blocco su semaforo)
 *     - Restituire il tempo al processo stesso (getCPUTime)
 *     - Cambiare il processo in esecuzione (terminazione, yield)
 *
 *   Aggiorna anche start_time_current_quantum al valore corrente, così
 *   il prossimo delta sarà calcolato correttamente dal nuovo "inizio".
 *
 *   Parametri:
 *     p → puntatore al PCB del processo da aggiornare (può essere NULL, no-op)
 */
void updateCPUTime(pcb_t *p) {
    if (p) {
        cpu_t current_tod;
        STCK(current_tod);   /* Legge il TOD clock ad alta risoluzione */
        p->p_time += (current_tod - start_time_current_quantum);
        /* Resettare il riferimento per evitare di contabilizzare due volte
           lo stesso intervallo di tempo in chiamate consecutive. */
        start_time_current_quantum = current_tod;
    }
}

/*
 * passUpOrDie — Passa un'eccezione al Support Level o termina il processo
 *
 * Ruolo nel sistema:
 *   È il meccanismo di escalation delle eccezioni verso Phase 3.
 *   Quando il Nucleo incontra un'eccezione che non sa gestire direttamente
 *   (TLB fault, page fault, syscall positive di Phase 3, ecc.), ha due opzioni:
 *
 *   1. "Pass Up": se il processo ha una support structure (p_supportStruct != NULL),
 *      salviamo lo stato dell'eccezione nel campo appropriato e carichiamo il
 *      contesto del gestore di Phase 3. Phase 3 gestirà l'eccezione e poi
 *      restituirà il controllo al processo tramite un'altra syscall.
 *
 *   2. "Die": se il processo NON ha una support structure, non può gestire
 *      l'eccezione. Lo terminiamo insieme a tutta la sua progenie.
 *
 *   I due tipi di eccezione e i loro gestori:
 *     PGFAULTEXCEPT (0): eccezioni TLB/page fault → sup_exceptContext[0]
 *     GENERALEXCEPT (1): eccezioni generali       → sup_exceptContext[1]
 *
 *   Parametri:
 *     exceptionType  → PGFAULTEXCEPT o GENERALEXCEPT
 *     exceptionState → stato CPU al momento dell'eccezione (dalla BIOS Data Page)
 */
void passUpOrDie(int exceptionType, state_t *exceptionState) {
    if (currentProcess->p_supportStruct == NULL) {
        /* "Die": il processo non ha un gestore per questa eccezione.
           Aggiorniamo il tempo CPU prima di terminare per accuratezza,
           poi terminiamo ricorsivamente il processo e tutta la sua progenie. */
        updateCPUTime(currentProcess);
        recursiveTerminateProcess(currentProcess);
        currentProcess = NULL;
        scheduler();
        /* Non si ritorna qui */
    } else {
        /* "Pass Up": deleghiamo l'eccezione al Support Level (Phase 3).

           1. Copiamo lo stato dell'eccezione nella struttura support del processo.
              Phase 3 lo userà per analizzare cosa è successo e per riprendere
              l'esecuzione del processo dopo la gestione.

           2. Carichiamo il contesto del gestore Phase 3: stackPtr, status e pc
              sono stati impostati da initProc() di Phase 3 quando ha creato
              il processo. LDCXT è equivalente a LDST ma accetta i tre campi
              separatamente invece di una state_t completa. */
        support_t *sup = currentProcess->p_supportStruct;

        /* Salviamo lo stato dell'eccezione nell'array indicizzato dal tipo:
           sup_exceptState[0] per TLB fault, sup_exceptState[1] per eccezioni generali */
        copyState(&sup->sup_exceptState[exceptionType], exceptionState);

        /* Carichiamo il contesto del gestore Phase 3 corrispondente al tipo
           di eccezione. Il controllo passa a Phase 3 senza possibilità di
           ritorno diretto (LDCXT non ritorna). */
        context_t *newContext = &sup->sup_exceptContext[exceptionType];
        LDCXT(newContext->stackPtr, newContext->status, newContext->pc);
        /* Non si ritorna qui */
    }
}
