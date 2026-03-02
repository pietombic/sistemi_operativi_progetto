#include "../../headers/types.h"

 int processCount;
 int softBlockCount;
 struct list_head readyQueue;
 pcb_t *currentProcess;
 cpu_t start_time_current_quantum; // Variabile globale per memorizzare quanto tempo un processo rimane nella CPU
/* Device semaphore */
 int deviceSemaphores[SEMDEVLEN];

void updateCPUTime(pcb_t *p);
pcb_t* findProcessByPID(int pid);
void utlb_RefillHandler();
