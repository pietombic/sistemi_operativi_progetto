/*
    Calculator U-proc.
    Reads expressions of the form "a op b" (op in +, -, *, /) from terminal,
    prints the result, then terminates.
*/

#include <uriscv/liburiscv.h>
#include "h/tconst.h"
#include "h/print.h"

#define BUFSIZE 64

static int parse_int(const char *s, int *idx) {
    int neg = 0, val = 0;
    if (s[*idx] == '-') { neg = 1; (*idx)++; }
    while (s[*idx] >= '0' && s[*idx] <= '9') {
        val = val * 10 + (s[*idx] - '0');
        (*idx)++;
    }
    return neg ? -val : val;
}

static void int_to_str(int n, char *out) {
    char tmp[12];
    int i = 0, neg = 0;
    if (n == 0) { out[0] = '0'; out[1] = EOS; return; }
    if (n < 0)  { neg = 1; n = -n; }
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = EOS;
}

static void skip_spaces(const char *s, int *idx) {
    while (s[*idx] == ' ') (*idx)++;
}

void main() {
    char buf[BUFSIZE + 1];
    char result[16];
    int  len, idx;

    print(WRITETERMINAL, "calc: enter expression (e.g. 3 + 4), empty line to quit\n");

    while (1) {
        print(WRITETERMINAL, "> ");

        len = SYSCALL(READTERMINAL, (int)buf, 0, 0);
        if (len <= 0) break;
        if (len > 0 && buf[len - 1] == '\n') len--;
        buf[len] = EOS;
        if (len == 0) break;

        idx = 0;
        skip_spaces(buf, &idx);
        int a = parse_int(buf, &idx);
        skip_spaces(buf, &idx);
        char op = buf[idx++];
        skip_spaces(buf, &idx);
        int b = parse_int(buf, &idx);

        int res = 0, err = 0;
        if (op == '+') {
            res = a + b;
        } else if (op == '-') {
            res = a - b;
        } else if (op == '*') {
            res = a * b;
        } else if (op == '/') {
            if (b == 0) {
                print(WRITETERMINAL, "Error: division by zero\n");
                err = 1;
            } else {
                res = a / b;
            }
        } else {
            print(WRITETERMINAL, "Error: unknown operator (use + - * /)\n");
            err = 1;
        }

        if (!err) {
            int_to_str(res, result);
            print(WRITETERMINAL, result);
            print(WRITETERMINAL, "\n");
        }
    }

    SYSCALL(TERMINATE, 0, 0, 0);
}
