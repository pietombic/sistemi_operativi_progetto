void copyState(state_t *dst, state_t *src);
int isSoftBlockSemaphore(int *semAdd);
void blockCurrentProcess(int *sem);
pcb_t *findInTree(pcb_t *node, int pid);
void recursiveTerminateProcess(pcb_t *target);
void passUpOrDie(int exceptionType, state_t *exceptionState);
void updateCPUTime(pcb_t *p);