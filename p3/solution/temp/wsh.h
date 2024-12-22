#ifndef WSH_H
#define WSH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#ifdef DEBUG
    #define mydbg_print(...) printf(__VA_ARGS__)
#else
    #define mydbg_print(...)
#endif

#define MAX_COMMAND_LEN 1024
#define MAX_ARGS 64

// Function declarations
void execute_command(char *cmd);
void parse_and_run(char *line);
void interactive_mode(void);
void batch_mode(FILE *file);
int handle_redirection(char **args);
void cleanup_memory(char **args);
char* trim_whitespace(char *str);

#endif

