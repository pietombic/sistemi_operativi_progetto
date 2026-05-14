/*
 * sysSupport.h — Interfaccia pubblica del general exception handler (Phase 3)
 *
 * Definisce le costanti per i numeri di syscall gestiti dal Support Level
 * e il prototipo dell'handler.
 *
 * NOTA: TERMINATE(2) e WRITETERMINAL(4) sono già definiti in headers/const.h.
 *       READTERMINAL e EXECUTE sono aggiuntivi, specifici del Support Level.
 */

#ifndef SYSSUPPORT_H
#define SYSSUPPORT_H

#include "../../headers/types.h"
#include "../../headers/const.h"

/* Numeri delle syscall positive gestite dal Support Level.
 * Vengono passati in reg_a0 dall'U-proc tramite ECALL. */
#define READTERMINAL  5     /* SYS5: legge una riga dal terminale 0 */
#define EXECUTE       6     /* SYS6: lancia un programma figlio (solo shell) */

/* Lunghezza massima di una stringa leggibile/scrivibile via terminale.
 * Applicata sia in sys4_writeTerminal che in sys5_readTerminal. */
#define MAXSTRINGLEN  128

/*
 * supportGeneralExHandler — entry point per general exception dei U-proc.
 *
 * Registrato in sup_exceptContext[GENERALEXCEPT] per ogni U-proc.
 * Gestisce: syscall positive (SYS2, SYS4, SYS5, SYS6) e program trap.
 */
void supportGeneralExHandler(void);

#endif
