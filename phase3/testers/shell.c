/*
    Shell U-proc (ASID 1).
    Reads commands from terminal, maps name to ASID, executes via SYS6.
    ASID assignment must match the flash device order in config_machine.json:
      flash0 = shell (ASID 1), flash1 = echo (ASID 2), flash2 = calc (ASID 3),
      flash3 = fibEight (ASID 4), flash4 = fibEleven (ASID 5),
      flash5 = uname (ASID 6), flash6 = date (ASID 7), flash7 = sl (ASID 8).
*/

#include <uriscv/liburiscv.h>
#include "h/tconst.h"
#include "h/print.h"

#define BUFSIZE 128

typedef struct {
    char name[16];
    int  asid;
} prog_entry_t;

/* ASID i → flash device i-1 (matches config_machine.json):
   flash0=shell(1), flash1=fibEight(2), flash2=echo(3), flash3=fibEleven(4),
   flash4=uname(5), flash5=date(6), flash6=sl(7) */
static prog_entry_t programs[] = {
    {"fibEight",  2},
    {"echo",      3},
    {"fibEleven", 4},
    {"uname",     5},
    {"date",      6},
    {"sl",        7},
    {"calc",      8},
    {"",          0}   /* sentinel */
};

static int sh_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == EOS && *b == EOS);
}

void main() {
    char buf[BUFSIZE + 1];
    int  len, i;

    print(WRITETERMINAL, "PandOS shell - type 'help' for commands\n");

    while (1) {
        print(WRITETERMINAL, "$ ");

        len = SYSCALL(READTERMINAL, (int)buf, 0, 0);
        if (len <= 0) {
            /* terminal error: terminate */
            SYSCALL(TERMINATE, 0, 0, 0);
        }

        /* strip trailing newline */
        if (len > 0 && buf[len - 1] == '\n') len--;
        buf[len] = EOS;

        /* skip empty input */
        if (len == 0) continue;

        /* built-in: exit */
        if (sh_streq(buf, "exit")) {
            print(WRITETERMINAL, "Bye!\n");
            SYSCALL(TERMINATE, 0, 0, 0);
        }

        /* built-in: help */
        if (sh_streq(buf, "help")) {
            print(WRITETERMINAL, "Commands: echo calc fibEight fibEleven uname date sl exit\n");
            continue;
        }

        /* look up program by name */
        int found = 0;
        for (i = 0; programs[i].asid != 0; i++) {
            if (sh_streq(buf, programs[i].name)) {
                SYSCALL(EXECUTE, programs[i].asid, 0, 0);
                found = 1;
                break;
            }
        }

        if (!found) {
            print(WRITETERMINAL, buf);
            print(WRITETERMINAL, ": command not found\n");
        }
    }
}
