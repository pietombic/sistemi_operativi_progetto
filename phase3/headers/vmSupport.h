#ifndef VMSUPPORT_H
#define VMSUPPORT_H

#include "../../headers/types.h"
#include "../../headers/const.h"

/* Indirizzo fisico di inizio Swap Pool:
   RAMSTART + OSFRAMES * PAGESIZE = 0x20000000 + 32*4096 = 0x20020000 */
#define SWAPPOOLSTART (RAMSTART + OSFRAMES * PAGESIZE)

/* Status di successo per operazione flash */
#define FLASHREADY 1

/* Inizializza la tabella Swap Pool e il relativo semaforo mutex.
   Chiamata da test() all'avvio del Support Level. */
void initSwapStructs(void);

/* TLB exception handler (Pager): gestisce page fault passati up dal Nucleo.
   Entry point per sup_exceptContext[PGFAULTEXCEPT]. */
void pager(void);

#endif
