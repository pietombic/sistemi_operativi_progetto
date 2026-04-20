#include <uriscv/liburiscv.h>

#include "h/tconst.h"
#include "h/print.h"

void main() {
	print(WRITETERMINAL, "Sat  5 Nov 06:15:00 PST 1955\n");
	SYSCALL(TERMINATE, 0, 0, 0);
}
