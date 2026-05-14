/*
 * interrupt.c — Gestore degli interrupt hardware del Nucleo (Phase 2)
 *
 * In μRISC-V, gli interrupt hardware sono eccezioni speciali che interrompono
 * il flusso normale di esecuzione per segnalare eventi asincroni:
 *   - PLT (Process Local Timer): scadenza del quanto di tempo del processo
 *   - Interval Timer: tick del pseudo-clock ogni 100ms
 *   - Dispositivi I/O: disco, flash, ethernet, stampante, terminale
 *
 * Questo file implementa interruptHandler(), chiamato da exceptionHandler()
 * (exception.c) ogni volta che causa indica un interrupt.
 *
 * Struttura degli interrupt in μRISC-V:
 *   excCode 3  → Interval Timer interrupt
 *   excCode 7  → PLT (Process Local Timer) interrupt
 *   excCode 17 → Disk interrupt       (linea 3)
 *   excCode 18 → Flash interrupt      (linea 4)
 *   excCode 19 → Ethernet interrupt   (linea 5)
 *   excCode 20 → Printer interrupt    (linea 6)
 *   excCode 21 → Terminal interrupt   (linea 7)
 *
 * Per ogni interrupt da dispositivo:
 *   1. Si legge il bitmap degli interrupt pendenti sulla linea
 *   2. Si seleziona il dispositivo con priorità più alta (indice più basso)
 *   3. Si salva lo status, si invia ACK al dispositivo
 *   4. Si sblocca il processo che attendeva su quel dispositivo
 */

#include "headers/interrupt.h"
#include "../headers/const.h"
#include "../headers/types.h"
#include "../phase1/headers/asl.h"
#include "../phase1/headers/pcb.h"
#include "headers/exception.h"
#include "headers/functions.h"
#include "headers/initial.h"
#include "headers/scheduler.h"
#include <uriscv/arch.h>
#include <uriscv/cpu.h>
#include <uriscv/liburiscv.h>

extern void scheduler();

/*
 * Macro di accesso ai registri hardware dei dispositivi.
 *
 * INT_BITMAP_BASE: indirizzo base del registro bitmap degli interrupt pendenti.
 * INT_BITMAP(line): legge il bitmap della linea interrupt specificata.
 *   Ogni bit i del bitmap indica se il dispositivo i su quella linea ha un
 *   interrupt pendente. Le linee vanno da 3 (disk) a 7 (terminal).
 *   Formula: offset = (line - 3) * 4 byte
 */
#define INT_BITMAP_BASE 0x10000040
#define INT_BITMAP(line) \
    (*((unsigned int *)(INT_BITMAP_BASE + ((line) - 3) * 0x4)))

/*
 * DEV_REG_BASE(line, dev): calcola il puntatore al blocco di registri del
 * dispositivo 'dev' sulla linea 'line'. Ogni linea occupa 0x80 byte (8 dev × 16 byte),
 * ogni dispositivo occupa 0x10 byte (16 byte = 4 registri da 4 byte).
 * Corrisponde all'inverso della formula usata in doIO() per trovare il semaforo.
 */
#define DEV_REG_BASE(line, dev) \
    ((unsigned int *)(START_DEVREG + ((line) - 3) * 0x80 + (dev) * 0x10))

/* Accessori ai registri di status e comando per dispositivi generici.
   Il registro status è in posizione [0], il comando in [1]. */
#define DEV_STATUS(base)  ((base)[0])
#define DEV_COMMAND(base) ((base)[1])

/* Accessori per i registri del terminale.
   Il terminale ha 4 registri da 4 byte nel suo blocco:
     [0] = RECV_STATUS  (stato del sotto-dispositivo di ricezione)
     [1] = RECV_COMMAND (comando del sotto-dispositivo di ricezione)
     [2] = TRANSM_STATUS  (stato del sotto-dispositivo di trasmissione)
     [3] = TRANSM_COMMAND (comando del sotto-dispositivo di trasmissione) */
#define TERM_RECV_STATUS(base)    ((base)[0])
#define TERM_RECV_COMMAND(base)   ((base)[1])
#define TERM_TRANSM_STATUS(base)  ((base)[2])
#define TERM_TRANSM_COMMAND(base) ((base)[3])

/* Calcolo degli indici nel vettore deviceSemaphores[] per linea e dispositivo.
   Stessa logica di doIO() in exception.c — manteniamo la coerenza. */
#define DEV_SEM_BASE(line, dev) (((line) - 3) * 8 + (dev))
#define TERM_TX_SEM(dev) (40 + (dev))   /* TX: sotto-dispositivo di trasmissione */
#define TERM_RX_SEM(dev) (32 + (dev))   /* RX: sotto-dispositivo di ricezione */

/*
 * getHighestPriorityDevice — Trova il dispositivo con priorità più alta nel bitmap
 *
 * Ruolo nel sistema:
 *   Quando più dispositivi sulla stessa linea interrupt hanno interrupt pendenti
 *   contemporaneamente, dobbiamo sceglierne uno da servire. La specifica μRISC-V
 *   assegna priorità crescente ai dispositivi con numero più basso (0 = massima
 *   priorità, 7 = minima priorità).
 *
 *   Questa funzione effettua una scansione lineare del bitmap da bit 0 a bit 7
 *   e restituisce l'indice del primo bit settato, che corrisponde al dispositivo
 *   con la priorità più alta.
 *
 *   Parametri:
 *     bitmap → registro bitmap degli interrupt pendenti per una linea
 *   Ritorna:
 *     indice del dispositivo (0..7), oppure -1 se nessun bit è settato
 */
static int getHighestPriorityDevice(unsigned int bitmap) {
    for (int i = 0; i < 8; i++) {
        if (bitmap & (1u << i))
            return i;
    }
    return -1;  /* Nessun interrupt pendente (non dovrebbe accadere in pratica) */
}

/*
 * interruptHandler — Handler principale di tutti gli interrupt hardware
 *
 * Ruolo nel sistema:
 *   È il gestore centralizzato per tutti gli interrupt hardware del Nucleo.
 *   Viene chiamato da exceptionHandler() (exception.c) quando il campo cause
 *   del registro CAUSE indica un interrupt (CAUSE_IS_INT).
 *
 *   Gestisce tre categorie di interrupt:
 *
 *   1. PLT interrupt (excCode 7):
 *      Il quanto di tempo del processo corrente è scaduto. Il processo viene
 *      rimesso nella ready queue (round-robin) e si invoca lo scheduler.
 *
 *   2. Interval Timer interrupt (excCode 3):
 *      Tick del pseudo-clock ogni 100ms. Tutti i processi bloccati su
 *      deviceSemaphores[48] vengono sbloccati e spostati nella ready queue.
 *
 *   3. Device interrupt (excCode 17..21):
 *      Un dispositivo I/O ha completato un'operazione. Si invia ACK al
 *      dispositivo, si sblocca il processo che attendeva su quel dispositivo
 *      e gli si restituisce il codice di stato dell'operazione in reg_a0.
 *
 *   Al termine di ogni caso, se c'era un processo in esecuzione al momento
 *   dell'interrupt, si riprende la sua esecuzione (LDST); altrimenti si
 *   invoca lo scheduler per eleggere il prossimo processo.
 */
void interruptHandler(void) {
    /* Recuperiamo lo stato salvato dalla BIOS Data Page: è lo snapshot dei
       registri del processo (o del kernel) che era in esecuzione prima
       dell'interrupt. Ne avremo bisogno per riprendere l'esecuzione. */
    state_t *savedState = (state_t *)BIOSDATAPAGE;

    unsigned int cause   = savedState->cause;
    unsigned int excCode = cause & CAUSE_EXCCODE_MASK;

    /* ------------------------------------------------------------------ */
    /* Aggiornamento del tempo CPU del processo corrente.                  */
    /* Ogni interrupt è una buona occasione per contabilizzare il tempo    */
    /* speso finora: leggiamo il TOD (Time-Of-Day clock) e aggiungiamo la  */
    /* differenza rispetto all'inizio del quanto corrente.                 */
    /* Aggiorniamo anche start_time_current_quantum per il prossimo delta. */
    /* ------------------------------------------------------------------ */
    cpu_t now;
    STCK(now);  /* Legge il valore corrente del TOD clock in 'now' */

    if (currentProcess != NULL) {
        currentProcess->p_time += (now - start_time_current_quantum);
    }
    /* Resettare il riferimento temporale indipendentemente da currentProcess,
       altrimenti il delta del prossimo interrupt includerebbe anche il tempo
       in cui non c'era nessun processo in esecuzione. */
    start_time_current_quantum = now;

    /* ================================================================== */
    /* CASO 1: PLT interrupt — scadenza del quanto di tempo (excCode == 7) */
    /* ================================================================== */
    if (excCode == 7u) {
        /* Disabilitiamo il PLT per evitare interrupt spurii durante la
           gestione. Lo scheduler lo riarmerà per il prossimo processo.
           NEVER è un valore molto grande che equivale a "mai scadere". */
        setTIMER((cpu_t)NEVER);

        if (currentProcess != NULL) {
            /* Salviamo lo stato corrente del processo nel suo PCB.
               Lo stato contiene il punto esatto in cui il processo era
               quando il quanto è scaduto, così può riprendere da lì. */
            copyState(&currentProcess->p_s, savedState);

            /* Riabilitiamo gli interrupt nel flag di stato salvato del processo.
               Gli interrupt potrebbero essere stati disabilitati durante la
               transizione kernel↔user; li riabilitiamo per il prossimo quanto. */
            currentProcess->p_s.status |= MSTATUS_MIE_MASK;

            /* Round-robin: rimettiamo il processo in coda alla sua banda di
               priorità. Non è una promozione né una penalità: il processo ha
               esaurito il suo quanto, ora tocca agli altri. */
            insertProcQ(&readyQueue, currentProcess);
            currentProcess = NULL;
        }

        /* Invochiamo lo scheduler per eleggere il prossimo processo */
        scheduler();
        return;
    }

    /* ================================================================== */
    /* CASO 2: Interval Timer — pseudo-clock tick (excCode == 3)          */
    /* ================================================================== */
    if (excCode == 3u) {
        /* ACK all'interval timer: ricarica il timer per il prossimo tick.
           PSECOND = 100ms in microsecondi. Senza questo, il timer non
           genererebbe altri interrupt periodici. */
        LDIT(PSECOND);

        /* Sblocchiamo TUTTI i processi in attesa sul pseudo-clock.
           Nota: removeBlocked restituisce i processi uno alla volta
           finché la lista è vuota. Questo è il caso in cui la V "broadcast"
           su un semaforo è necessaria (tutti i dormienti si svegliano). */
        pcb_t *p;
        while ((p = removeBlocked(&deviceSemaphores[48])) != NULL) {
            p->p_semAdd = NULL;
            /* Il processo riprende con valore di ritorno 0 (nessun errore):
               la syscall waitForClock() è completata con successo. */
            p->p_s.reg_a0 = 0;
            insertProcQ(&readyQueue, p);
            softBlockCount--;
        }

        /* Azzeriamo il semaforo del pseudo-clock: il suo valore può essere
           molto negativo (quanti processi erano bloccati), ma al prossimo
           tick vogliamo ripartire da 0, non da un valore accumulato. */
        deviceSemaphores[48] = 0;

        if (currentProcess != NULL) {
            /* Il processo interrotto può riprendere immediatamente:
               l'interval timer non causa preemption */
            LDST(savedState);
        } else {
            scheduler();
        }
        return;
    }

    /* ================================================================== */
    /* CASO 3: Device interrupt — completamento I/O (excCode 17..21)      */
    /* ================================================================== */
    if (excCode >= 17u && excCode <= 21u) {
        /* Convertiamo excCode nel numero di linea interrupt (3..7).
           In μRISC-V: excCode = linea + 14 → linea = excCode - 14 */
        int intLineNo = (int)(excCode - 14u);

        /* Leggiamo il bitmap degli interrupt pendenti su questa linea.
           Ogni bit i settato indica che il dispositivo i ha completato
           (o ha un errore) e vuole essere servito. */
        unsigned int bitmap = INT_BITMAP(intLineNo);

        /* Scegliamo il dispositivo con priorità più alta (bit meno significativo
           settato nel bitmap = numero dispositivo più basso). */
        int devNo = getHighestPriorityDevice(bitmap);

        /* Nessun dispositivo pendente (raro, ma possibile per race condition).
           Riprendiamo normalmente. */
        if (devNo < 0) {
            if (currentProcess != NULL)
                LDST(savedState);
            else
                scheduler();
            return;
        }

        /* -------------------------------------------------------------- */
        /* Gestione terminale (linea 7): due sotto-dispositivi indipendenti */
        /* -------------------------------------------------------------- */
        if (intLineNo == 7) {
            unsigned int *termBase = DEV_REG_BASE(intLineNo, devNo);

            /* Leggiamo lo stato di entrambi i sotto-dispositivi.
               Il byte meno significativo (& 0xFF) contiene il codice di stato:
               0 = idle/not started, 1 = busy, 5 = completato con successo (OKCHARTRANS).
               Valori != 0 e != 1 indicano completamento (con o senza errori). */
            unsigned int txStatus = TERM_TRANSM_STATUS(termBase) & 0xFFu;
            unsigned int rxStatus = TERM_RECV_STATUS(termBase) & 0xFFu;

            /* ---------------------------------------------------------- */
            /* Sotto-dispositivo TX (trasmissione / output):               */
            /* Ha completato l'invio di un carattere o ha avuto un errore  */
            /* ---------------------------------------------------------- */
            if ((txStatus & 0xFF) != 0) {
                /* Salviamo lo status COMPLETO (tutti i 32 bit) prima dell'ACK:
                   dopo l'ACK il registro potrebbe cambiare e perderemmo
                   il codice di stato da restituire al processo. */
                unsigned int savedStatus = TERM_TRANSM_STATUS(termBase);

                /* Inviamo ACK al dispositivo: lo notifichiamo che abbiamo
                   preso nota del completamento e che può accettare il prossimo
                   comando. Senza ACK il dispositivo resterebbe bloccato. */
                TERM_TRANSM_COMMAND(termBase) = ACK;

                int semIdx = TERM_TX_SEM(devNo);

                /* Sblocchiamo il processo solo se c'era effettivamente qualcuno
                   in attesa (semaforo negativo = almeno un processo bloccato). */
                if (deviceSemaphores[semIdx] < 0) {
                    deviceSemaphores[semIdx]++;
                    pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
                    if (unblocked != NULL) {
                        /* Restituiamo lo status dell'operazione in reg_a0:
                           il processo che ha chiamato doIO() troverà qui
                           il risultato dell'operazione di trasmissione. */
                        unblocked->p_s.reg_a0 = savedStatus;
                        unblocked->p_semAdd   = NULL;
                        insertProcQ(&readyQueue, unblocked);
                        softBlockCount--;
                    }
                }
            }

            /* ---------------------------------------------------------- */
            /* Sotto-dispositivo RX (ricezione / input):                   */
            /* Ha ricevuto un carattere o ha avuto un errore               */
            /* ---------------------------------------------------------- */
            if ((rxStatus & 0xFF) != 0) {
                /* Come per TX: salviamo lo status prima dell'ACK */
                unsigned int savedStatus = TERM_RECV_STATUS(termBase);
                TERM_RECV_COMMAND(termBase) = ACK;

                int semIdx = TERM_RX_SEM(devNo);

                if (deviceSemaphores[semIdx] < 0) {
                    deviceSemaphores[semIdx]++;
                    pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
                    if (unblocked != NULL) {
                        unblocked->p_s.reg_a0 = savedStatus;
                        unblocked->p_semAdd   = NULL;
                        insertProcQ(&readyQueue, unblocked);
                        softBlockCount--;
                    }
                }
            }

        } else {
            /* -------------------------------------------------------------- */
            /* Gestione dispositivi non-terminale (linee 3..6):               */
            /* disco, flash, ethernet, stampante — un solo sotto-dispositivo  */
            /* -------------------------------------------------------------- */
            unsigned int *devBase    = DEV_REG_BASE(intLineNo, devNo);

            /* Salviamo lo status prima di inviare ACK (stessa ragione dei terminali) */
            unsigned int savedStatus = DEV_STATUS(devBase);

            /* ACK al dispositivo: segnaliamo che abbiamo ricevuto l'interrupt */
            DEV_COMMAND(devBase) = ACK;

            /* Calcoliamo l'indice del semaforo corrispondente */
            int semIdx = DEV_SEM_BASE(intLineNo, devNo);

            /* Sblocchiamo il processo in attesa, se presente */
            if (deviceSemaphores[semIdx] < 0) {
                deviceSemaphores[semIdx]++;
                pcb_t *unblocked = removeBlocked(&deviceSemaphores[semIdx]);
                if (unblocked != NULL) {
                    /* Il codice di stato dell'operazione I/O va in reg_a0 */
                    unblocked->p_s.reg_a0 = savedStatus;
                    unblocked->p_semAdd   = NULL;
                    insertProcQ(&readyQueue, unblocked);
                    softBlockCount--;
                }
            }
        }

        /* Dopo aver servito il dispositivo, decidiamo come continuare:
           - se c'era un processo in esecuzione prima dell'interrupt,
             lo riprendiamo (l'interrupt non causa preemption per i device)
           - altrimenti invochiamo lo scheduler */
        if (currentProcess != NULL) {
            LDST(savedState);
        } else {
            scheduler();
        }
        return;
    }

    /* ================================================================== */
    /* Fallback: interrupt non riconosciuto (non dovrebbe accadere)        */
    /* ================================================================== */
    if (currentProcess != NULL)
        LDST(savedState);
    else
        scheduler();
}
