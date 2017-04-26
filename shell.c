#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/shm.h>
#define PERM S_IRUSR|S_IWUSR

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_wait(struct tokens *tokens);
unused int cmd_fg(struct tokens *tokens);
unused int cmd_bg(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_pwd, "pwd", "print working directory"},
  {cmd_cd, "cd", "change directory"},
  {cmd_wait, "wait", "wait for all background jobs to terminate"}
  // {cmd_fg, "fg", "move a process in foreground"},
  // {cmd_bg, "bg", "resume a paused background process"}
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

/* Prints working directory */
int cmd_pwd(unused struct tokens *tokens) {
  char cwd[4028];
  if (getcwd(cwd, sizeof(cwd)) != NULL)
    fprintf(stdout, "%s\n", cwd);
  else
    fprintf(stderr, "Error.");
  return 1;
}

/* Changes to directory */
int cmd_cd(struct tokens *tokens) {
  char *filename;
  filename = tokens_get_token(tokens, 1);
  if (filename == NULL)
    fprintf(stderr, "Please specify a directory.\n");
  else if (chdir(filename) != 0)
    fprintf(stderr, "%s not found.\n", filename);
  return 1;
}

/* Waits on process to finish */
int cmd_wait(struct tokens *tokens) {
  int status;
  fprintf(stderr, "%s\n", "Stop. Wait a minute.");
  while(wait(&status) != -1)
    continue;
  return 1;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      size_t argc = tokens_get_length(tokens);
      pid_t pid;
      if (argc > 0) {
        /* Turn off interrupt handling */
        signal(SIGINT, SIG_IGN);
        signal(SIGSTOP, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);

        /* Fork */
        pid = fork();
        if (!pid) {
          setpgid(pid, getpid());
          tcsetpgrp(shell_terminal, getpgid(pid));
          /* Turn interrupts back on*/
          signal(SIGINT, SIG_DFL);
          signal(SIGSTOP, SIG_DFL);
          signal(SIGTTOU, SIG_DFL);

          /* argv processing */
          char **argv = (char **) malloc((argc + 1) * sizeof(char*)); // allocate enough space for argc number of pointers... do I need to free each of these char*s too?
          for (unsigned int arg = 0; arg < argc; arg++) {
            argv[arg] = tokens_get_token(tokens, arg); 

            /* I/O redirection */
            if (argv[arg][0] == '>') {
              if (arg + 1 >= argc)
                fprintf(stderr, "%s\n", "Syntax error.");
              int newfd;
              if ((newfd = open(tokens_get_token(tokens, arg+1), O_CREAT|O_TRUNC|O_WRONLY, 0644)) < 0) {
                fprintf(stderr, "%s\n", "Error opening file for writing.");
                exit(1);
              }
              /* replace stdout with argv 2*/
              dup2(newfd, 1);
              argv[arg] = NULL;
              break;
            } else if (argv[arg][0] == '<') {
              if (argc < 3)
                fprintf(stderr, "%s\n", "Syntax error.");
              int newfd;
              if ((newfd = open(tokens_get_token(tokens, arg+1), O_RDONLY)) < 0) {
                fprintf(stderr, "%s\n", "Error opening file for reading.");
                exit(1);
              }
              /* replace stdin with argv 2 */
              dup2(newfd, 0);
              argv[arg] = NULL;
              break;
            }
            /* Background processes */
            if (argv[arg][0] == '&') {
              tcsetpgrp(shell_terminal, shell_pgid);
              argv[arg] = NULL;
              break;
            }
          }
          argv[argc] = NULL; // NULL terminator

          if (strchr((const char *) argv[0], '/') == NULL) { 
            
            /* Path resolution */
            char *pathbuf;
            char *pathtok;
            size_t path_len;
            static char resolved[4096];
          
            /* confstr explained on man page - https://linux.die.net/man/3/confstr */
            /* strtok explained on tutorialspoint - https://www.tutorialspoint.com/c_standard_library/c_function_strtok.htm */
            path_len = confstr(_CS_PATH, NULL, (size_t) 0);
            pathbuf = malloc(path_len);
            if (pathbuf == NULL)
              abort(); 
            confstr(_CS_PATH, pathbuf, path_len);
            pathtok = strtok(pathbuf, ":");
            while (pathtok != NULL) {
              memset(resolved, '\0', path_len);
              strcpy(resolved, pathtok);
              strcat(resolved, "/");
              strcat(resolved, argv[0]);
              execv(resolved, (char * const*) argv);
              pathtok = strtok(NULL, ":");
            }
            free(pathbuf);
          } else {
            execv(argv[0], (char * const*) argv);
          }
          /* Command not found */
          // fprintf(stderr, "No, Patrick, %s is not a valid command.\n", argv[0]);
          fprintf(stderr, "%s: command not found\n", argv[0]);
          free(argv);
          exit(0);
        } else {
          /* Need help understanding next two lines */
          /* We can only access status once no other process is trying to access?*/
          int status;
          size_t tokens_length = tokens_get_length(tokens);
          bool background = tokens_length > 1 && tokens_get_token(tokens, tokens_length-1)[0] == '&';
          if (background) {
            waitpid(pid, &status, WNOHANG);
          } else {
            waitpid(pid, &status, 0);
          }
          tcsetpgrp(shell_terminal, shell_pgid);
          signal(SIGINT, SIG_DFL);
          signal(SIGSTOP, SIG_DFL);
          signal(SIGTTOU, SIG_DFL);
        }
      }
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
