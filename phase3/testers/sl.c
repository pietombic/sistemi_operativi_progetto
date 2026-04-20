#include <uriscv/liburiscv.h>

#include "h/tconst.h"
#include "h/print.h"

#define LOGO1  "     ++      +------ "
#define LOGO2  "     ||      |+-+ |  "
#define LOGO3  "   /---------|| | |  "
#define LOGO4  "  + ========  +-+ |  "

#define LWHL11 " _|--O========O~\\-+ "
#define LWHL12 "//// \\_/      \\_/  "

void main() {
	print(WRITETERMINAL, LOGO1"\n");
	print(WRITETERMINAL, LOGO2"\n");
	print(WRITETERMINAL, LOGO3"\n");
	print(WRITETERMINAL, LOGO4"\n");
	print(WRITETERMINAL, LWHL11"\n");
	print(WRITETERMINAL, LWHL12"\n");
	SYSCALL(TERMINATE, 0, 0, 0);
}
