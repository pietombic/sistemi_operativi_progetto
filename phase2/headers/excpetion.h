#include "initial.h"
#include "../../headers/types.h"
#include "../../headers/const.h"

void exceptionHandler();
void systemCallHandler();
void createProcess(state_t *state);
void terminateProcess(state_t *state);
void recursiveTerminate(pcb_t *p);
void waitSemaphore(state_t *state);
void signalSemaphore(state_t *state);
void doIO(state_t *state);
void getCPUTime(state_t *state);
void waitForClock(state_t *state);
void getSupportData(state_t *state);
void getProcessID(state_t *state);
void yield(state_t *state);
void passUpOrDie(int exceptionType, state_t *exceptionState);
void programTrapHandler();

/* Funzione per gli interrupt (definita in interrupt.c) */
void interruptHandler();
int *getDeviceSemaphore(int *commandAddr);