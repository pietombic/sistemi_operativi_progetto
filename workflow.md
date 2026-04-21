# Phase 3 - Workflow di Implementazione

---

## PUNTO DI RIPRESA (prossima sessione)

**Stato attuale:** codice compila, sistema va in halt al runtime. Debug in corso.

### Cosa fare subito

1. Aprire uRISCV con `phase3_config_machine.json` dalla root del progetto:
   ```bash
   cd /home/belbu/Desktop/sistemi_operativi_progetto
   uriscv phase3_config_machine.json
   ```

2. Avviare la macchina (Power On → Run).

3. Aprire il klog: menu `View → Log` (o equivalente in uRISCV GUI).

4. Leggere i messaggi klog e comunicarli nella prossima sessione.

### Messaggi klog attesi e loro significato

```
CIAO                     → test() avviato ✓
SHELL LAUNCHED           → shell creata con CREATEPROCESS ✓
pager excCode=X asid=1   → pager chiamata per la shell
pager: flash read asid=1 page=Y  → pager tenta lettura flash
pager: flash read OK     → flash read riuscita ✓
pager: flash read FAIL status=Z  → PROBLEMA: flash restituisce errore Z
pager: TLB-Mod fault     → PROBLEMA: write su pagina non dirty
SHELL TERMINATED         → shell terminata (SYS2 o fatalFault)
```

### Diagnosi in base a cosa si vede nel klog

**Caso A: `pager excCode=X` con X diverso da 27/28**
- X=25 o X=26 → TLB fault variante kernel-mode. Fix già applicato (ora accetta tutti tranne 24). OK.
- X=24 → TLB-Mod: la shell scrive su una pagina con D=0. Bug in initPageTable (tutte le pagine dovrebbero avere D=1 → DIRTYON già impostato). Strano.
- X altro → eccezzione inattesa. Comunicare il valore.

**Caso B: `pager: flash read FAIL status=Z`**
- Z=3 (ILLEGAL_OP_ERROR) → numero di blocco o comando sbagliato.
  - Controllare che `pageNo` non superi i blocchi del flash file.
  - Verificare la formula `(pageNo << 8) | rw` nel comando DOIO.
- Z=4 (FLASH_ERROR) → blocco corrotto o indirizzo DMA non allineato.
  - `framePhysAddr` deve essere allineato a 4096. Verificare SWAPPOOLSTART.
- Z=0 (NOT_INSTALLED) → flash device non trovato all'indirizzo calcolato.
  - Bug nel calcolo dell'indirizzo registro flash.
- Z=2 (BUSY) → timeout o deadlock sul device.

**Caso C: klog si ferma dopo `SHELL LAUNCHED` senza `pager excCode=`**
- La pager non viene mai chiamata → il TLB refill handler crasha prima.
- Controllare `uTLB_RefillHandler` in `exception.c`:
  - `currentProcess->p_supportStruct` è NULL? (non dovrebbe esserlo)
  - `pageIndex` fuori range? (con il fix `vpn` invece di `vpn-0x80000` dovrebbe essere OK)
- Aggiungere klog in `uTLB_RefillHandler` per debug.

**Caso D: klog mostra tutto OK ma sistema va in halt dopo**
- Il pager funziona ma la shell crasha dopo l'avvio.
- Probabile: READTERMINAL fallisce → shell chiama TERMINATE.
- Verificare il terminal device: `term0.uriscv` deve esistere nella root.
- Verificare il registro RX del terminale: `TERM0_BASE + 0x04`.

---

## Bug già risolti in questa sessione

### Bug 1: VPN extraction errata (HALT IMMEDIATO)
**File:** `phase2/exception.c` (uTLB_RefillHandler) e `phase3/vmSupport.c` (pager)

**Problema:** In uRISCV, `EntryHi` bits 29:12 contengono la VPN *relativa al segmento*, NON la VPN assoluta.
- Per VA `0x800000B0` (page 0 della shell): bits 29:12 = **0** (non `0x80000`)
- Per VA `0xBFFFF000` (stack): bits 29:12 = **0x3FFFF** (non `0xBFFFF`)

**Vecchio codice (sbagliato):**
```c
if (vpn == 0xBFFFF) { pageIndex = 31; }
else { pageIndex = vpn - 0x80000; }  // → pageIndex = -0x80000 per page 0!
```

**Fix applicato:**
```c
if (vpn == 0x3FFFF) { pageIndex = MAXPAGES - 1; }
else { pageIndex = (int)vpn; }
```

### Bug 2: excCode check troppo restrittivo nella pager
**File:** `phase3/vmSupport.c`

**Problema:** La pager controllava solo `EXC_UTLBL=27` e `EXC_UTLBS=28`, ma uRISCV
può generare `EXC_TLBL=25`, `EXC_TLBS=26`, o altri codici a seconda del contesto.

**Fix applicato:** La pager ora accetta qualsiasi eccezione che arriva via PGFAULTEXCEPT,
rigettando solo `EXC_MOD=24` (TLB-Modification = errore reale).

---

## Costanti chiave da uriscv/cpu.h

```c
CAUSE_EXCCODE_MASK = 0x7FFFFFFF   // full 31-bit mask (NON 0x7C)
EXC_MOD   = 24    // TLB-Modification (write to read-only) → fatalFault
EXC_TLBL  = 25    // TLB load fault (kernel mode)
EXC_TLBS  = 26    // TLB store fault (kernel mode)
EXC_UTLBL = 27    // user TLB load fault → page not present (normale)
EXC_UTLBS = 28    // user TLB store fault → page not present (normale)
EXC_ECU   = 8     // U-mode ECALL (syscall da U-proc)
EXC_ECM   = 11    // M-mode ECALL (normalizzato da exception.c)
STATE_GPR_LEN = 32
ENTRYHI_VPN_MASK = 0x3FFFF000    // bits 29:12 = VPN segmento-relativa
PRESENTFLAG = 0x80000000          // Index register: P bit = 1 → non trovato in TLB
```

## ASID → flash mapping (config FISSO: non modificare phase3_config_machine.json)

| ASID | Flash  | Programma  |
|------|--------|------------|
| 1    | flash0 | shell      |
| 2    | flash1 | fibEight   |
| 3    | flash2 | echo       |
| 4    | flash3 | fibEleven  |
| 5    | flash4 | uname      |
| 6    | flash5 | date       |
| 7    | flash6 | sl         |
| 8    | flash7 | calc (DISABILITATO nel config) |

## Build

```bash
# Kernel
cd /home/belbu/Desktop/sistemi_operativi_progetto/build
cmake .. && make

# Testers
cd /home/belbu/Desktop/sistemi_operativi_progetto/phase3/testers
make
```

## Struttura Support Level

```
test() [initProc.c]
  → initSwapStructs()
  → launchUproc(1)          ← crea shell ASID=1
  → PASSEREN(masterSemaphore)  ← aspetta terminazione shell

Shell ASID=1 esegue a UPROCSTARTADDR=0x800000B0
  → TLB miss → uTLB_RefillHandler (exception.c) → installa PTE (V=0)
  → TLB-Invalid → exceptionHandler → passUpOrDie(PGFAULTEXCEPT) → pager()
  → pager: PASSEREN(swapPoolSem) → sceglie frame → flashIO(READ) → aggiorna PTE+TLB
  → LDST → shell riprende l'esecuzione

Shell chiama print() → SYS4 (WRITETERMINAL) → sys4_writeTerminal → DOIO su terminal TX
Shell chiama SYSCALL(READTERMINAL) → SYS5 → sys5_readTerminal → DOIO su terminal RX
Shell chiama SYSCALL(EXECUTE, asid) → SYS6 → sys6_execute → CREATEPROCESS + PASSEREN(shellSemaphore)
Programma termina → SYS2 → VERHOGEN(shellSemaphore) → shell si sblocca

Shell chiama SYSCALL(TERMINATE) → SYS2 → VERHOGEN(masterSemaphore) → test() si sblocca → HALT
```
