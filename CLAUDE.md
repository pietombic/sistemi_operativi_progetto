# PandOSsh - Contesto Progetto

## Cos'è
Sistema operativo didattico su architettura μRISC-V (uRISCV), sviluppato in C.
Emulatore: uRISCV. Compilatore: riscv64-unknown-elf-gcc.
Build: CMake → `cd build && cmake .. && make`.

## Struttura directory
```
phase1/       PCB e ASL (completato)
phase2/       Nucleo (completato + bug fixati)
phase3/       Support Level (DA IMPLEMENTARE)
testers/      Programmi U-proc (shell, calc, ...)
headers/      types.h, const.h globali
klog.c        debug logging
CMakeLists.txt build system
config_machine.json configurazione uRISCV
```

## Stato implementazione

### Phase 2 (Nucleo) - COMPLETATO
File chiave: `phase2/initial.c`, `phase2/scheduler.c`, `phase2/exception.c`,
`phase2/interrupt.c`, `phase2/functions.c`.

**Bug fixati in questa sessione:**
- `scheduler.c`: timer scalato 1000x → `setTIMER(TIMESLICE)` (senza moltiplicazione)
- `scheduler.c`: mancante `STCK(start_time_current_quantum)` prima di LDST
- `functions.c` `recursiveTerminateProcess`: `(*semAdd)++` ora solo per semafori non-dispositivo
- `exception.c` `systemCallHandler`: logica controllo syscall number + PRIVINSTR cause
- `interrupt.c`: check terminale `& 0xFF != 0` invece di `== OKCHARTRANS`

**Variabili globali phase2** (in `phase2/initial.c`, esportate da `phase2/headers/initial.h`):
```c
int processCount;           // processi attivi
int softBlockCount;         // processi bloccati su I/O
struct list_head readyQueue;
pcb_t *currentProcess;
cpu_t start_time_current_quantum;   // TOD inizio quanto corrente
int deviceSemaphores[SEMDEVLEN];    // semafori dispositivi nucleo (49 elementi)
pcb_t *rootProcess;
```

**Convenzioni phase2:**
- `copyState(dst, src)` → copia src IN dst (argomento dst PRIMA)
- Syscall nucleo: numeri NEGATIVI (-1 a -10), in kernel mode
- `passUpOrDie(exType, state)` → termina o passa al livello superiore

### Phase 3 (Support Level) - DA IMPLEMENTARE
Vedi `workflow.md` per il piano dettagliato.

## Costanti importanti (headers/const.h)
```c
PAGESIZE     = 4096
UPROCMAX     = 8          // max U-proc concorrenti
POOLSIZE     = 16         // swap pool frames (2 * UPROCMAX)
MAXPAGES     = 32         // pagine per U-proc
OSFRAMES     = 32         // frames per OS code
RAMSTART     = 0x20000000
KERNELSTACK  = 0x20001000
KUSEG        = 0x80000000
UPROCSTARTADDR = 0x800000B0  // entry point U-proc (.text)
USERSTACKTOP   = 0xC0000000  // SP iniziale U-proc
VPNSHIFT     = 12
ASIDSHIFT    = 6
DIRTYON      = 0x00000400   // bit D in EntryLo
VALIDON      = 0x00000200   // bit V in EntryLo
GLOBALON     = 0x00000100   // bit G in EntryLo
PGFAULTEXCEPT = 0           // indice TLB exceptions in sup_exceptState/Context
GENERALEXCEPT = 1           // indice general exceptions
TERMINATE    = 2            // SYS2 (user level)
WRITETERMINAL = 4           // SYS4
// READTERMINAL = 5         // SYS5 (DA AGGIUNGERE a const.h o phase3 header)
// EXECUTE = 6              // SYS6 (DA AGGIUNGERE)
IL_FLASH     = 18           // interrupt exception code flash
IL_TERMINAL  = 21           // interrupt exception code terminal
FLASHREAD    = 2
FLASHWRITE   = 3
START_DEVREG = 0x10000054   // base registri dispositivi
```

**Swap pool address:**
```c
#define SWAPPOOLSTART (RAMSTART + OSFRAMES * PAGESIZE)  // 0x20020000
```

## Configurazione macchina (config_machine.json)
- RAM: 64 frames (attenzione: potrebbe servire aumentarla per phase3 a 128+)
- TLB floor: 0x80000000
- TLB size: 16
- Terminal 0 abilitato
- Flash devices: da abilitare per i test

## Build
```bash
cd /home/belbu/Desktop/sistemi_operativi_progetto/build
cmake ..
make
# poi lanciare uriscv con config_machine.json
```

Per phase3 aggiornare `CMakeLists.txt`:
- Rimuovere `phase2/p2test.c`
- Aggiungere `phase3/initProc.c`, `phase3/vmSupport.c`, `phase3/sysSupport.c`

## Note architetturali importanti
- `uTLB_RefillHandler` deve restare in `phase2/exception.c` (spec §12.2)
- Support Level handlers girano in kernel-mode, interrupts abilitati
- U-proc girano in user-mode, interrupts abilitati
- Syscall positive (1+) → passate al Support Level
- Syscall negative → gestite dal Nucleo
- Ogni U-proc ha un proprio flash device (ASID i → flash device i-1)
- Shell = ASID 1, programmi = ASID 2-8
