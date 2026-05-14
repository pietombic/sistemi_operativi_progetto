/*
 * vmSupport.h — Interfaccia pubblica del Pager (demand paging, Phase 3)
 *
 * Definisce le costanti e i prototipi relativi alla gestione della memoria
 * virtuale per i U-proc: swap pool, flash I/O, TLB management.
 */

#ifndef VMSUPPORT_H
#define VMSUPPORT_H

#include "../../headers/types.h"
#include "../../headers/const.h"

/*
 * SWAPPOOLSTART — indirizzo fisico di inizio della swap pool.
 *
 * Layout della RAM (RAMSTART = 0x20000000):
 *   [RAMSTART, RAMSTART + OSFRAMES*PAGESIZE)  → OS code/data (32 frame = 128 KB)
 *   [SWAPPOOLSTART, ...]                      → swap pool (16 frame = 64 KB)
 *
 * SWAPPOOLSTART = 0x20000000 + 32 * 4096 = 0x20020000
 */
#define SWAPPOOLSTART (RAMSTART + OSFRAMES * PAGESIZE)

/*
 * FLASHREADY — status code di successo per un'operazione flash.
 * Il device ritorna 1 in caso di lettura/scrittura completata correttamente.
 */
#define FLASHREADY 1

/*
 * initSwapStructs — inizializza la swap pool e il semaforo mutex.
 * Deve essere chiamata una sola volta da test() prima di avviare U-proc.
 * Dopo questa chiamata tutti i POOLSIZE frame risultano liberi.
 */
void initSwapStructs(void);

/*
 * pager — TLB exception handler (gestore page fault).
 *
 * Entry point registrato in sup_exceptContext[PGFAULTEXCEPT] per ogni U-proc.
 * Implementa il demand paging: su TLB-Invalid fault, carica la pagina
 * richiesta dal flash al frame vittima (FIFO), aggiorna PTE e TLB,
 * poi ritorna all'istruzione faulting via LDST.
 */
void pager(void);

#endif
