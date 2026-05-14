/*
 * initProc.h — Interfaccia pubblica del processo instanziatore (Phase 3)
 *
 * Esporta le variabili globali condivise tra i moduli del Support Level
 * (initProc.c, vmSupport.c, sysSupport.c) e i prototipi delle funzioni
 * di inizializzazione delle strutture support_t.
 */

#ifndef INITPROC_H
#define INITPROC_H

#include "../../headers/types.h"
#include "../../headers/const.h"

/* Array di strutture support_t: una per ogni U-proc (ASID 1..UPROCMAX).
 * supportStructures[i] corrisponde all'ASID = i+1. */
extern support_t supportStructures[UPROCMAX];

/* Semaforo su cui test() attende la terminazione della shell (ASID 1).
 * Inizializzato a 0 da test(); la shell fa V prima di terminare. */
extern int masterSemaphore;

/* Semaforo su cui la shell (ASID 1) attende la fine di ogni figlio lanciato
 * con SYS6 (Execute). Inizializzato a 0; il figlio fa V prima di terminare. */
extern int shellSemaphore;

/* Mutex per i flash device (DEVPERINT device totali, uno per ASID).
 * flashMutex[i] protegge il device i; il U-proc con ASID = i+1 usa il device i.
 * Inizializzato a 1 (libero). */
extern int flashMutex[DEVPERINT];

/* Mutex per la trasmissione (TX) del terminale 0.
 * Garantisce che un solo U-proc alla volta scriva sul terminale. */
extern int termMutexTX;

/* Mutex per la ricezione (RX) del terminale 0.
 * Separato da termMutexTX: lettura e scrittura possono coesistere. */
extern int termMutexRX;

/* Funzione di test/inizializzazione del Support Level: entry point di Phase 3. */
void test(void);

/* Inizializza la page table privata di un U-proc con ASID dato.
 * Tutte le 32 entry sono marcate non valide (V=0, D=1). */
void initPageTable(support_t *sup, int asid);

/* Inizializza i contesti degli handler TLB e general exception nella support_t.
 * PGFAULTEXCEPT → pager(), GENERALEXCEPT → supportGeneralExHandler(). */
void initSupportContexts(support_t *sup);

/* Inizializza completamente una support_t (ASID + page table + contesti).
 * Chiamata da launchUproc() e da sys6_execute(). */
void initSupportStruct(support_t *sup, int asid);

#endif
