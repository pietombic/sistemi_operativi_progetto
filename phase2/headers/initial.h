#include "../../headers/types.h"

extern int processCount;
extern int softBlockCount;
extern int globalLock;
extern struct list_head readyQueue;
extern pcb_t *currentProcess;
extern cpu_t
    start_time_current_quantum; // Variabile globale per memorizzare quanto
                                // tempo un processo rimane nella CPU
/* Device semaphore */
extern int deviceSemaphores[SEMDEVLEN];
extern pcb_t *rootProcess;

void updateCPUTime(pcb_t *p);
void uTLB_RefillHandler();
void initKernel();
