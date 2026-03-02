#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "initial.h"
#include "../../headers/types.h"
#include "../../headers/const.h"

void interruptHandler();
unsigned int getDeviceBitmap(int deviceLine); 
int getDeviceNumber(unsigned int word);      
int highestPriorityPendingLine();            

#endif