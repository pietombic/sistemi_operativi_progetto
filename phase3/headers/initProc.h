#ifndef INITPROC_H
#define INITPROC_H

#include "../../headers/types.h"
#include "../../headers/const.h"

extern support_t supportStructures[UPROCMAX];

extern int masterSemaphore;
extern int shellSemaphore;

extern int flashMutex[DEVPERINT];
extern int termMutexTX;
extern int termMutexRX;

void test(void);
void initPageTable(support_t *sup, int asid);
void initSupportContexts(support_t *sup);
void initSupportStruct(support_t *sup, int asid);

#endif
