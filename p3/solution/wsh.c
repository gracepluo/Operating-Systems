#include "wsh.h"
#include <strings.h>
#include <dirent.h>

int alphasort( const struct dirent **d1, const struct dirent **d2);
int errTrack = 0;


// Function to trim leading and trailing whitespaces
char* trim_whitespace(char *str) {
    mydbg_print("Trimming whitespace for input: '%s'\n", str);
   
    while (*str == ' ') str++;  // Trim leading spaces
    if (*str == 0) return str;  // Empty string


    char *end = str + strlen(str) - 1;
    while (end > str && *end == ' ') end--;  // Trim trailing spaces
    *(end + 1) = 0;


    mydbg_print("Trimmed result: '%s'\n", str);
    return str;
}


// Function to handle redirection
int handle_redirection(char **args) {
    mydbg_print("Handling redirection...\n");
   
    for (int i = 0; args[i] != NULL; i++) {
        mydbg_print("Processing argument: '%s'\n", args[i]);
       
        if (strstr(args[i], "<") != NULL) {
            mydbg_print("Input redirection found: '%s'\n", args[i]);
            int fd = open(args[i] + 1, O_RDONLY);
            if (fd < 0) {
                perror("wsh: input redirection error");
                return -1;
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
            args[i] = NULL;  // Remove redirection from args


        }else if (strstr(args[i], "&>>") != NULL) {  // New case for &>> (Append stdout and stderr)
            mydbg_print("Append stdout and stderr redirection found: '%s'\n", args[i]);
            int fd = open(args[i] + 3, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) {
                perror("wsh: append stdout and stderr redirection error");
                return -1;
            }
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            args[i] = NULL;
        }else if (strstr(args[i], ">>") != NULL) {
            mydbg_print("Append output redirection found: '%s'\n", args[i]);
            int fd = open(args[i] + 2, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) {
                perror("wsh: append output redirection error");
                return -1;
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            args[i] = NULL;
        } else if (strstr(args[i], "&>") != NULL) {
            mydbg_print("Redirect stdout and stderr found: '%s'\n", args[i]);
            int fd = open(args[i] + 2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("wsh: redirect stdout and stderr error");
                return -1;
            }
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            args[i] = NULL;
        } else if (strstr(args[i], "2>") != NULL) {  // New case for 2> (stderr redirection)
            mydbg_print("Stderr redirection found: '%s'\n", args[i]);
            printf("wsh> ");
            fflush(stdout);
            int fd = open(args[i] + 2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("wsh: stderr redirection error");
                return -1;
            }
            dup2(fd, STDERR_FILENO);  // Redirect stderr to the file
            close(fd);
            args[i] = NULL;
        }else if (strstr(args[i], ">") != NULL) {
            mydbg_print("Output redirection found: '%s'\n", args[i]);
            int fd = open(args[i] + 1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("wsh: output redirection error");
                return -1;
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            args[i] = NULL;
        }
    }


    mydbg_print("Redirection handling done.\n");
    return 0;  
}




// Execute a command
int execute_command(char *input_cmd) {
    //already checked that input_cmd is not NULL in the caller function, no need to check input_cmd NULL in this callee function
    mydbg_print("Executing command: '%s'\n", input_cmd);
    char * replaced_val_cmd = replace_variables(input_cmd);

    char *args[MAX_ARGS];
    int i = 0;
    char cmd[ MAX_COMMAND_LEN]; //local copy to avoid destruction of the input, variable is in the stack so don't need to free
    strcpy (cmd, replaced_val_cmd);


    char *token = strtok(cmd, " ");
    while (token != NULL && i < MAX_ARGS - 1) {
        mydbg_print("Token: '%s'\n", token);
        args[i++] = token; // TODO!!! Do I need to do strdup(token) here
        token = strtok(NULL, " ");
    }
    args[i] = NULL;  // Null-terminate the arguments array


    if (args[0] == NULL) {
        mydbg_print("Empty command, returning.\n");
        free(replaced_val_cmd);
        return 0;  // Empty command
    }

    if(strcmp(args[0], "history") != 0){
        add_history(replaced_val_cmd); // Adding command to history.
    }
    

    // Handle built-in commands
    if (strcmp(args[0], "exit") == 0 || strcmp(args[0], "cd") == 0 || strcmp(args[0], "export") == 0  || strcmp(args[0], "local") == 0  || strcmp(args[0], "vars") == 0  ||
    strcmp(args[0], "history") == 0  || strcmp(args[0], "ls") == 0) {
        int builtin_status =  execute_builtin(args, i);
        free(replaced_val_cmd);
        return builtin_status; //TODO 0 or 1
    }else{
        char *executable_path = find_executable(args[0]);
        if (executable_path == NULL) {
            fprintf(stderr, "Command not found: %s\n", args[0]);
            free(replaced_val_cmd);
            free(executable_path);
            return 1;  // Command not found
        }
        

    pid_t pid = fork();


    if (pid < 0) {
        perror("wsh: fork error");
        free(replaced_val_cmd);
        free(executable_path);
        return 1;
    } else if (pid == 0) {  // Child process
        // Handle redirection
        mydbg_print("In child process (pid: %d)\n", getpid());
       
        if (handle_redirection(args) < 0) {
            mydbg_print("Redirection failed.\n");
            free(replaced_val_cmd);
            free(executable_path);
            return 1;
        }

        // Execute the command
        mydbg_print("Executing command with execv: '%s'\n", args[0]);
        
        // Execute the command with execv using the full path of the executable
        mydbg_print("Executing command with execv: '%s'\n", executable_path);
        execv(executable_path, args);
        //perror("wsh: command execution failed");
        free(replaced_val_cmd);
        free(executable_path);
        return 0;
    } else {  // Parent process
        mydbg_print("Waiting for child process (pid: %d)\n", pid);
        int status = 0;
        waitpid(pid, &status, 0);   // TODO: use waitpid() or wait() ??? wait(NULL);  // Wait for the child process to finish
        //waitpid(0, 0, 0);        
        mydbg_print("Child process finished.\n");

        // Check if the child terminated normally
        if (WIFEXITED(status)) {
            mydbg_print("Child exited with status %d\n", WEXITSTATUS(status));
            free(replaced_val_cmd);
            free(executable_path);
            return WEXITSTATUS(status);
        }
        // Check if the child was terminated by a signal
        else if (WIFSIGNALED(status)) {
            mydbg_print("Child was terminated by signal %d\n", WTERMSIG(status));
        }
        free(replaced_val_cmd );
        free(executable_path);
        return 1;//TODO check
    }

    }
    free(replaced_val_cmd );
    return 0;

}


// Parse a line and run the command
int parse_and_run(char *line) {
    mydbg_print("Parsing and running line: '%s'\n", line);
   
    line = trim_whitespace(line);


    if (line ==NULL || line[0] == '#' || line[0] == '\0' || line[0] == '\n') {
        mydbg_print("Comment or empty line. Skipping...\n");
        return 0;  // Ignore comments and empty lines
    }
    // Remove trailing newline character, if present
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
    }
    int status = execute_command(line);
    mydbg_print("Last command returned: %d\n", status);
    return status;
}


// Interactive mode
int interactive_mode() {
    mydbg_print("Entering interactive mode...\n");
   
    char line[MAX_COMMAND_LEN];
    int res = 0;
    while (res == 0) {
        printf("wsh> ");
        fflush(stdout);  
        //sleep(1);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (feof(stdin)) {
                mydbg_print("EOF reached. Exiting...\n");
                break;  // EOF reached, exit shell
            }
            perror("wsh: read error");
            continue;
        }


        mydbg_print("Received input: '%s'\n", line);
        res = parse_and_run(line);
    }
    //printf("wsh> ");
    return res;
}


// Batch mode
int batch_mode(FILE *file) {
    mydbg_print("Entering batch mode...\n");
   
    char line[MAX_COMMAND_LEN];
    int res = 0;
    while (fgets(line, sizeof(line), file) != NULL && res == 0) {
        //printf("wsh> ");
        fflush(stdout);
        mydbg_print("Batch line: '%s'\n", line);
        res = parse_and_run(line);
    }
    return res;
}




// Function to create or update shell variable
void set_shell_variable(const char *name, const char *value) {
    ShellVariable *current = shell_vars;
    mydbg_print("in function set_shell_variable \n");
if (name != NULL && value != NULL){
    mydbg_print("Setting shell variable: %s=%s\n", name, value);
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            free(current->value);
            current->value = strdup(value);
            mydbg_print("Updated shell variable: %s=%s\n", name, value);
            return;
        }
        current = current->next;
    }
    
    // New variable
    ShellVariable *new_var = malloc(sizeof(ShellVariable));
    if (new_var == NULL) {
        mydbg_print("Cannot allocate memory for shell variable. \n");
        return;
    }


    new_var->name = strdup(name);
    new_var->value = strdup(value);
    new_var->next = shell_vars;
    shell_vars = new_var;
    mydbg_print("Created new shell variable: %s=%s\n", name, value);
    } else{
        mydbg_print("input val nul\n");
    }
}


// Function to get the value of a shell variable
const char* get_shell_variable(const char *name) {
    ShellVariable *current = shell_vars;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            return current->value;
        }
        current = current->next;
    }
    return NULL; // Return NULL if the variable does not exist
}


// Built-in function to print all shell variables (local variables)
void print_shell_variables(ShellVariable *current) {
    //ShellVariable *current = shell_vars;
    if (current == NULL) {
        // printf("%s=%s\n", current->name, current->value);
        // //fflush(stdout);
        // current = current->next;
        return;
    }
    print_shell_variables(current->next);
    printf("%s=%s\n", current->name, current->value);
    //fflush(stdout);
    
}


// Built-in function to export environment variables
int export_env_variable(const char *name, const char *value) {
    int res = 0;

    if(name == 0 || value == 0){
        mydbg_print("input is null\n");
        return res;
    }
    mydbg_print("Exporting environment variable: %s=%s\n", name, value);

      // If the environment variable is "PATH", check if "/bin" is included in the value
    if (strcmp(name, "PATH") == 0) {
        if (strstr(value, "/bin") == NULL) {
            mydbg_print("Error: The PATH value does not include '/bin' directory\n");
            printf("wsh> ");
            errTrack = 255;
            return 255;  // Do not set the environment variable if /bin is missing
            
        } else {
            mydbg_print("The PATH value includes '/bin' directory. Pass check. \n");
        }
    }

    if (setenv(name, value, 1) == 0) {
        mydbg_print("Environment variable %s set successfully\n", name);
    } else {
        perror("setenv");
        res = -1;
        mydbg_print("Exporting environment Error: %d\n", res);
    }
    return res;
}




// Function to replace variables (both environment and shell)
char * replace_variables(const char *command) {
    mydbg_print("Replacing variables in command: %s\n", command);
   
    char *result = malloc(strlen(command) + 1);
    strcpy(result, command);
    char *var_start = strchr(result, '$');
   
    while (var_start != NULL) {
        char *var_name = var_start + 1;
        char *var_end = strchr(var_name, ' ');
        if (var_end != NULL) {
            *var_end = '\0';
        }


        // Try to get the value
        char *value = getenv(var_name);
        if (value == NULL) {
            value = (char *)get_shell_variable(var_name);
        }
        if (value == NULL) {
            value = "";  // Replace with empty if not found
        }


        mydbg_print("Substituting variable: %s -> %s\n", var_name, value);


        // Perform substitution
        char *new_result = malloc(strlen(result) - strlen(var_name) + strlen(value) + 1);
        strncpy(new_result, result, var_start - result);
        strcat(new_result, value);
        if (var_end != NULL) {
            strcat(new_result, var_end + 1);
        }


        free(result);
        result = new_result;
        var_start = strchr(result, '$');
    }
    mydbg_print("Resulting command after substitution: %s\n", result);
    return result;
}


// Add a command to history
void add_history(const char *command) {
    mydbg_print("Adding command to history: %s\n", command);


    if (history_size < history_capacity) {
        history[history_size] = strdup(command);
        history_size++;
    } else {
        free(history[history_index]);
        history[history_index] = strdup(command);
    }
    history_index = (history_index + 1) % history_capacity;


    mydbg_print("History added at index %d\n", history_index);
}


// Print history
void print_history() {
    mydbg_print("Printing history\n");


    int i, index;
    for (i = 0; i < history_size; i++) {
        index = (history_index + i) % history_size;
        printf("%d) %s\n", i + 1, history[index]);
        fflush(stdout);
    }
}


// Execute command from history
void execute_history(int command_number) {
    //int res;
    mydbg_print("Executing history command number: %d\n", command_number);


    if (command_number > 0 && command_number <= history_size) {
        int index = (history_index + command_number - 1) % history_size;
        mydbg_print("Executing command from history: %s\n", history[index]);
        // res = parse_and_run(history[index]);
        parse_and_run(history[index]);
    } else {
        mydbg_print("No such command in history\n");
        printf("No such command in history.\n");
        fflush(stdout);
    }
}


// Function to search for an executable in the PATH
char* find_executable(const char *command) {
    mydbg_print("Searching for executable: %s\n", command);


    if (access(command, X_OK) == 0) {
        mydbg_print("Found executable at full path: %s\n", command);
        return strdup(command);
    }


    char *path = getenv("PATH");
    if (path == NULL) {
        path = "/bin";
    }


    char *path_dup = strdup(path);
    char *dir = strtok(path_dup, ":");


    while (dir != NULL) {
        char *full_path = malloc(strlen(dir) + strlen(command) + 2);
        sprintf(full_path, "%s/%s", dir, command);
        fflush(stdout);


        if (access(full_path, X_OK) == 0) {
            mydbg_print("Found executable: %s\n", full_path);
            free(path_dup);
            return full_path;
        }


        free(full_path);
        dir = strtok(NULL, ":");
    }

    if(path_dup != NULL){free(path_dup);}
    mydbg_print("Executable not found: %s\n", command);
    errTrack = -1;
    return NULL;
}


void builtin_ls(){
    struct dirent **listed;
    int line = scandir(".", &listed, NULL, alphasort);

    if(line < 0){
        fprintf(stderr, "Error, Scan Directory");
        return;
    }

    for(int i = 0; i<line; i++){
        if(listed[i]->d_name[0] != '.'){
            printf("%s\n", listed[i]->d_name);
        }
        free(listed[i]);
    }
    
    free(listed);
}






int execute_builtin(char **args, int arg_count) {
    mydbg_print("Executing built-in command: %s\n", args[0]);
    int res = 0;
    if (strcmp(args[0], "exit") == 0) {
        mydbg_print("Exiting shell\n");
        exit(errTrack);
    } else if (strcmp(args[0], "cd") == 0) {
        if (arg_count != 2) {
            mydbg_print("cd: wrong number of arguments\n");
            printf("Usage: cd <directory>\n");
        } else if (chdir(args[1]) != 0) {
            perror("cd");
            res = 1;
        } else {
            mydbg_print("Changed directory to: %s\n", args[1]);
        }
    } else if (strcmp(args[0], "export") == 0) {
        char *name = strtok(args[1], "=");
        char *value = strtok(NULL, "=");
        if (name && value) {
            mydbg_print("Exporting variable: %s=%s\n", name, value);
            res = export_env_variable(name, value);
            if(res == 255){
                printf("wsh> ");
            }
        }
    } else if (strcmp(args[0], "local") == 0) {
        char *name = strtok(args[1], "=");
        char *value = strtok(NULL, "=");
        if (name && value) {
            mydbg_print("Setting shell variable: %s=%s\n", name, value);
            set_shell_variable(name, value);
        }
    } else if (strcmp(args[0], "vars") == 0) {
        mydbg_print("Printing shell variables\n");
        ShellVariable *current = shell_vars;
        print_shell_variables(current);
    } else if (strcmp(args[0], "history") == 0) {
        if (arg_count == 2) {
            int command_number = atoi(args[1]);
            mydbg_print("Re-executing history command: %d\n", command_number);
            execute_history(command_number);
        } else {
            mydbg_print("Printing history\n");
            print_history();
        }
    } else if (strcmp(args[0], "ls") == 0) {
        mydbg_print("Executing built-in ls command\n");
        //system("ls -1");
        builtin_ls();
    }
    return res;
}


void free_shell_variables() {
    ShellVariable *current = shell_vars;
    ShellVariable *next;


    while (current != NULL) {
        next = current->next;
        free(current->name);
        free(current->value);
        free(current);
        current = next;
    }


    shell_vars = NULL;
    mydbg_print("Freed all shell variables.\n");
}


void free_history() {
    for (int i = 0; i < history_size; i++) {
        free(history[i]);
    }


    history_size = 0;
    history_index = 0;
    mydbg_print("Freed history.\n");
}


// Main function
int main(int argc, char *argv[]) {
    mydbg_print("Starting wsh shell...\n");
    int res;
    setenv("PATH","/bin",1); 
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [script file]\n", argv[0]);
        exit(1);
    }

    mydbg_print("argc = '%d'\n", argc);
    if (argc == 2) {  // Batch mode
        mydbg_print("Batch mode: Opening file '%s'\n", argv[1]);
        FILE *file = fopen(argv[1], "r");
        if (file == NULL) {
            perror("wsh: cannot open batch file");
            exit(1
            );
        }
        res = batch_mode(file);
        fclose(file);
    } else {  // Interactive mode
        mydbg_print("Interactive mode: No script file provided.\n");
        res = interactive_mode();
    }
    free_shell_variables();
    free_history();
    mydbg_print("res %d\n", res);
    if(res == -1){
        res = 0;
    }
    return errTrack;


}