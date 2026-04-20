#ifndef SYSSUPPORT_H
#define SYSSUPPORT_H

#include "../../headers/types.h"
#include "../../headers/const.h"

/* Positive syscall numbers handled by the Support Level */
/* TERMINATE=2 and WRITETERMINAL=4 already defined in const.h */
#define READTERMINAL  5
#define EXECUTE       6

#define MAXSTRINGLEN  128

void supportGeneralExHandler(void);

#endif
