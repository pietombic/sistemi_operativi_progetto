#ifndef KLOG_H
#define KLOG_H

void klog_print(char *str);
void klog_print_dec(unsigned int num);
void klog_print_hex(unsigned int num);

extern unsigned int klog_line_index;
extern unsigned int klog_char_index;
extern char klog_buffer[64][42];   // usa gli stessi numeri di klog.c

#endif