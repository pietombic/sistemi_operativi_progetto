# Phase 3 - Workflow di Implementazione

---

## STATO ATTUALE: IMPLEMENTAZIONE COMPLETA ✓

Kernel (MultiPandOS) compila. Tutti i testers compilano.

### Fix risolti in questa sessione:
- **CMakeLists.txt**: rimosso p2test.c, aggiunti phase3/initProc.c, vmSupport.c, sysSupport.c
- **vmSupport.c**: TLB-Mod check corretto (EXC_MOD=24, EXC_UTLBL=27, EXC_UTLBS=28 da uriscv/cpu.h)
- **vmSupport.c**: rimosso VERHOGEN errato su swapPoolSem prima dell'acquisizione
- **vmSupport.c**: updateTLBEntry usa TLBP+TLBWI (ottimizzato); installazione nuova entry usa updateTLBEntry
- **sysSupport.c**: rimosso doppio PC increment (già fatto da exception.c prima di passUpOrDie)
- **sysSupport.c**: sys6_execute usa initSupportStruct (no duplicazione); aggiunto check ASID==1
- **sysSupport.c**: usa EXC_ECU/EXC_ECM da uriscv/cpu.h invece di costanti hardcoded
- **initProc.c**: initPageTable, initSupportContexts ora pubbliche; aggiunto initSupportStruct
- **testers/Makefile**: corretto toolchain (riscv64-unknown-elf), lib path (/usr/lib/uriscv), emulation elf32lriscv
- **testers/shell.c**: implementazione completa (loop read→parse→execute, built-in help/exit)
- **testers/calc.c**: implementazione completa (parsing "a op b", risultato su terminale)

### ASID → flash mapping (da phase3_config_machine.json, NON modificabile):
| ASID | Flash | Programma |
|------|-------|-----------|
| 1    | flash0 | shell |
| 2    | flash1 | fibEight |
| 3    | flash2 | echo |
| 4    | flash3 | fibEleven |
| 5    | flash4 | uname |
| 6    | flash5 | date |
| 7    | flash6 | sl |
| 8    | flash7 | calc (disabilitato in config) |

---

## Come buildare e lanciare

```bash
# Kernel
cd /home/belbu/Desktop/sistemi_operativi_progetto/build
cmake .. && make

# Testers (nella dir testers)
cd /home/belbu/Desktop/sistemi_operativi_progetto/phase3/testers
make

# Lanciare emulatore
uriscv  # aprire phase3_config_machine.json
```

---

## Architettura costanti chiave (da uriscv/cpu.h)

```c
CAUSE_EXCCODE_MASK = 0x7FFFFFFF   // full 31-bit mask
EXC_ECU   = 8                     // U-mode ECALL
EXC_ECM   = 11                    // M-mode ECALL (normalizzato da exception.c)
EXC_MOD   = 24                    // TLB-Modification
EXC_UTLBL = 27                    // user TLB load fault (page not present)
EXC_UTLBS = 28                    // user TLB store fault (page not present)
STATE_GPR_LEN = 32                // da uriscv/types.h
ENTRYHI_VPN_MASK = 0x3FFFF000     // da uriscv/cpu.h
PRESENTFLAG = 0x80000000          // Index register P bit (not found = 1)
```

---

## Note debug comuni

- **Page fault loop infinito**: PTE non aggiornata → controllare che updateTLBEntry venga chiamata dopo pte_entryLO = ... | VALIDON
- **Swap pool corrotto**: FIFO clock non resettato → verificare initSwapStructs() chiamata da test()
- **Terminal deadlock**: mutex TX/RX non rilasciato su errore → sys4/sys5 fanno VERHOGEN prima di LDST in caso errore
- **Shell non parte**: verificare ASID=1 in entry_hi, MPP=U in status, e che flash0 sia abilitato in config
- **Processo figlio non termina**: shellSemaphore rimane 0 → sys2_terminate deve fare VERHOGEN(&shellSemaphore) per ASID≠1

---

## Dubbi rimasti

1. **TLB refill**: uTLB_RefillHandler (exception.c) carica PTE con V=0 → poi TLB-Invalid → pager. Questo è il flusso corretto.
2. **RECVD vs CHARRECV**: const.h ha entrambi = 5. Il check in sys5 usa RECVD. Corretto.
3. **flash7 disabilitato**: calc.uriscv esiste ma non è accessibile dalla shell (rimosso dalla tabella programmi).
