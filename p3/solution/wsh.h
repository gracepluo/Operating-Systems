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
#include <dirent.h>
#include <sys/stat.h>

#ifdef DEBUG
    #define mydbg_print(...) printf(__VA_ARGS__)
#else
    #define mydbg_print(...)
#endif

#define MAX_COMMAND_LEN 1024
#define MAX_ARGS 64
#define MAX_HISTORY 10

typedef struct ShellVariable {
    char *name;
    char *value;
    struct ShellVariable *next;
} ShellVariable;

ShellVariable *shell_vars = NULL;

char *history[MAX_HISTORY];
int history_size = 0;
int history_index = 0;
int history_capacity = MAX_HISTORY; 

// Function declarations
int execute_command(char *cmd);
int parse_and_run(char *line);
int interactive_mode(void);
int batch_mode(FILE *file);
int handle_redirection(char **args);
void cleanup_memory(char **args);
char* trim_whitespace(char *str);


char * replace_variables(const char *command);
int execute_builtin(char **args, int arg_count);
void add_history(const char *command);
char* find_executable(const char *command);

#endif

