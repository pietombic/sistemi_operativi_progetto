#include <uriscv/liburiscv.h>

#include "h/tconst.h"
#include "h/print.h"

void main() {
	print(WRITETERMINAL, "PandOSsh\n");
	SYSCALL(TERMINATE, 0, 0, 0);
}
