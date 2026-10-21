/* builtins.h - commands implemented inside the shell process,
 * plus the "internal programs" gedit and man that run in a child. */
#ifndef BUILTINS_H
#define BUILTINS_H

int is_builtin(const char *name);
int run_builtin(int argc, char **argv);

int is_internal_program(const char *name);
_Noreturn void run_internal_program(int argc, char **argv);

#endif
