/* Pieces of the command prompt shared between its source files. */
#ifndef SSHOS_SHELL_H
#define SSHOS_SHELL_H

#include "types.h"

int  shell_streq(const char *a, const char *b);
int  shell_parse_num(const char *s, u32 *out);  /* decimal, or hex after 0x or $ */
void shell_print_ip(u32 ip);
void shell_fail(int net_err);                   /* "Failed: <reason>." */
int  shell_need_network(void);                  /* start the network if needed; 0 = ready */
int  shell_network_start(void);                 /* the same, without printing anything */
int  shell_network_by_dhcp(void);              /* is the address to come from the network? */
void shell_read_line(char *line);               /* a typed line, at most 79 characters */

/* term.c */
void cmd_term(int argc, char **argv);


/* cmd_tcp.c */
void cmd_tcp(int argc, char **argv);
void cmd_tcptest(int argc, char **argv);

/* ssh.c */
int  kbd_ask_line(char *line, int max, int echo);   /* a typed line; echo 0 hides it; -1 = Esc */
void cmd_ssh(int argc, char **argv);

/* desk.c: the home screen */
void cmd_desk(void);

/* bench.c */
void cmd_bench(void);
void cmd_bench_term(void);

#endif
