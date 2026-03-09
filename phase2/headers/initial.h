#include "../../headers/types.h"

extern int processCount;
extern int softBlockCount;
extern struct list_head readyQueue;
extern pcb_t *currentProcess;
extern cpu_t start_time_current_quantum; // Variabile globale per memorizzare quanto tempo un processo rimane nella CPU
/* Device semaphore */
extern int deviceSemaphores[SEMDEVLEN];
//array di puntatori ai processi (che siano running, ready o blocked) 
extern pcb_t *activeProcesses[MAXPROC];

void updateCPUTime(pcb_t *p);
pcb_t* findProcessByPID(int pid);
void uTLB_RefillHandler();
void initKernel();
