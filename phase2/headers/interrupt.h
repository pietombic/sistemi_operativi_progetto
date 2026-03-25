#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "../../headers/const.h"
#include "../../headers/types.h"
#include "initial.h"

void interruptHandler();
unsigned int getDeviceBitmap(int deviceLine);
int getDeviceNumber(unsigned int word);
int highestPriorityPendingLine();

#endif