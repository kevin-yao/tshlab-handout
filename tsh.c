/* 
 * tsh - A tiny shell program with job control
 * 
 * <Name: Kangping Yao  Andrew id: kyao>
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */


/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens {
    int argc;               /* Number of arguments */
    char *argv[MAXARGS];    /* The arguments list */
    char *infile;           /* The input file */
    char *outfile;          /* The output file */
    enum builtins_t {       /* Indicates if argv[0] is a builtin command */
        BUILTIN_NONE,
        BUILTIN_QUIT,
        BUILTIN_JOBS,
        BUILTIN_BG,
        BUILTIN_FG} builtins;
};

/* End global variables */


/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list); 
int addjob(struct job_t *job_list, pid_t pid, int state, 
          char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid); 
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*Here are some self-define wrapper functions */
pid_t Fork(void);
pid_t Waitpid(pid_t pid, int *status, int options);
void Kill(pid_t pid, int signal);
void Execve(const char *filename, char *const argv[], 
	 char *const envp[]);
void Setpgid(pid_t pid, pid_t pgid);
void Sigemptyset(sigset_t *set);
void Sigfillset(sigset_t *set);
void Sigaddset(sigset_t *set, int signum);
void Sigdelset(sigset_t *set, int signum);
void Sigprocmask(int option, const sigset_t *set, 
                sigset_t *oldset);
void Sigsuspend(const sigset_t *mask);
int Open(char *filename, int flags);
void Close(int fd);
void Dup2(int oldfd, int newfd);
/*
 * main - The shell's main routine 
 */
int 
main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];    /* cmdline for fgets */
    int emit_prompt = 1; /* emit prompt (default) */

    /*
     * Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) 
     */
    Dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(job_list);


    /* Execute the shell's read/eval loop */
    while (1) {
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { 
            /* End of file (ctrl-d) */
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }
        
        /* Remove the trailing newline */
        cmdline[strlen(cmdline)-1] = '\0';
        
        /* Evaluate the command line */
        eval(cmdline);
        
        fflush(stdout);
        fflush(stdout);
    } 
    
    exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void 
eval(char *cmdline) 
{
    int bg;      /* Should the job run in bg or fg? */
    struct cmdline_tokens tok;
    pid_t pid=0, jid=0;
    int fdin=0, fdout=0;
    struct job_t * job= NULL;
    sigset_t mask, stopmask;
    /*Mask is used to block signal*/
    Sigemptyset(&mask);
    /*
	 * Block some signal which will cause shell 
 	 * deleting job before adding job.
	 */
    Sigaddset(&mask, SIGCHLD);
    Sigaddset(&mask, SIGINT);
    Sigaddset(&mask, SIGTSTP);
    /*
	 * Stopmask is used to suspend shell 
	 * when foreground job is running.
	 */
    Sigfillset(&stopmask);
    /*Delte some signals we don't want to mask*/
    Sigdelset(&stopmask, SIGCHLD);
    Sigdelset(&stopmask, SIGINT);
    Sigdelset(&stopmask, SIGTSTP);
    /* Parse command line */
    bg = parseline(cmdline, &tok); 

    if (bg == -1) /* Parsing error */
        return;
    if (tok.argv[0] == NULL) /* Ignore empty lines */
        return;
    if(tok.builtins != BUILTIN_NONE){
        switch(tok.builtins){
        case BUILTIN_QUIT:
            /*Send quit signal to current process, then quit shell*/
            pid = getpid();
            Kill(pid, SIGQUIT);
            break;
        case BUILTIN_JOBS:
            /*
			 * If there is a redirection, 
			 * output running job to that file.
			 */
            if(tok.outfile != NULL){
                fdout = Open(tok.outfile, O_WRONLY);
                listjobs(job_list, fdout);
            }else{ /*Print jobs to stdout*/
                listjobs(job_list, STDOUT_FILENO);
            }
            break;
        case BUILTIN_BG:
            if(*tok.argv[1]=='%'){   /* Using jid as index */
                jid = atoi(tok.argv[1]+1);
                job = getjobjid(job_list, jid);
            }else{   /*Using pid as index*/
                pid = atoi(tok.argv[1]);
                job = getjobpid(job_list, pid);
            }
            if(job!=NULL){
                jid = job->jid;
                pid = job->pid;
                if(job->state == ST){
                    /*
					 * Send SIGCONT signal to stopped process
					 * and its children.
					 */
                    Kill(-pid, SIGCONT);
                    /*Change the job state to BG*/
                    job->state = BG;
                    printf("[%d] (%d) %s\n", jid, pid, job->cmdline);                   
                }
            }        
            break;
        case BUILTIN_FG:
            if(*tok.argv[1]=='%'){    /*Using jid as index*/
                jid = atoi(tok.argv[1]+1);
                job = getjobjid(job_list, jid);
            }else{    /*Using pid as index*/
                pid = atoi(tok.argv[1]);
                job = getjobpid(job_list, pid);
            }
            if(job!=NULL){
                pid = job->pid;
                jid = job->jid;
                if(job->state == ST){ /*Running stopped job*/
                    Kill(-pid, SIGCONT);
                    job->state = FG; /*Change state to FG*/
                }
                /*
                 * Change the state of BG job to FG.
                 * Since shell will wait untill foreground job finish
				 * before next prompt, so we don't need to concern 
				 * whether there already has a foreground job running.
                 */
                else if(job->state == BG){ 
                    job->state = FG;            
                }
                /*
                 * If there is a FG job running, then wait until 
				 * it finished.Since signal maybe from background job, 
				 * so here we need to verify that foreground 
                 * job has done.
                 */
                while(fgpid(job_list)>0)
                    Sigsuspend(&stopmask);
            } 
            break;
        default: 
            (void) fprintf(stderr, "Error: wrong built-in command\n");
            return;
        }
    }else{
        /*
         * Block SIGCHLD, SIGINT, SIGTSTP here, since these 
		 * signals may cause sigchild handler deleting job 
	     * even before shell adding this job.
         */
        Sigprocmask(SIG_BLOCK, &mask, NULL);
        if((pid=Fork())==0){
            Setpgid(0,0);
            /*If there is a input redirection, redirect stdin to fdin*/
            if(tok.infile != NULL){
                fdin = Open(tok.infile, O_RDONLY);
                Dup2(fdin, STDIN_FILENO);       
            }
            /*
			 * If there is a output redirection, 
			 * redirect stdout to fdout.
			 */
            if(tok.outfile != NULL){
                fdout = Open(tok.outfile, O_WRONLY);
                Dup2(fdout, STDOUT_FILENO);
            }   
            Sigprocmask(SIG_UNBLOCK, &mask, NULL);
            /*Execute job here*/
            Execve(tok.argv[0], tok.argv, environ);
            
            if(tok.infile != NULL){
                Close(fdin);
            }
            if(tok.outfile != NULL){
                Close(fdout);
            }           
        }
        if(bg == 0){/*Foreground job*/
            assert(addjob(job_list, pid, FG, cmdline)!=0);
            /* Unblock signal after shell add this job*/
            Sigprocmask(SIG_UNBLOCK, &mask, NULL);
            /* 
             * Wait until foreground job finished, since signal 
			 * may come from background job so here we need to 
			 * verify whether foreground job has finished.
             */
            while(fgpid(job_list)>0)
                Sigsuspend(&stopmask);              
        }else if(bg == 1){/*Background job*/
            assert(addjob(job_list, pid, BG, cmdline)!=0);
            printf("[%d] (%d) %s\n", pid2jid(pid), pid, cmdline);
            /*Unblock signal after system call printf*/
            Sigprocmask(SIG_UNBLOCK, &mask, NULL);
        }else{
            printf("wrong command\n");
        }
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. 
 *             Characters enclosed in single or double quotes are 
 *             treated as a single argument. 
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job  
 *  -1:        if cmdline is incorrectly formatted
 * 
 * Note:       The string elements of tok (e.g., argv[], infile, outfile) 
 *             are statically allocated inside parseline() and will be 
 *             overwritten the next time this function is invoked.
 */
int 
parseline(const char *cmdline, struct cmdline_tokens *tok) 
{

    static char array[MAXLINE];       /* holds local copy of command line*/
    const char delims[10] = " \t\r\n";/* argument delimiters(white-space)*/
    char *buf = array;                /* ptr that traverses command line */
    char *next;                       /* ptr to the end of the current arg*/
    char *endbuf;                     /* ptr to end of cmdline string */
    int is_bg;                        /* background job? */

    int parsing_state;                /* indicates if the next token is the
                                         input or output file */

    if (cmdline == NULL) {
        (void) fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void) strncpy(buf, cmdline, MAXLINE);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf) {
        /* Skip the white-spaces */
        buf += strspn (buf, delims);
        if (buf >= endbuf) break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<') {
            if (tok->infile) {
                (void) fprintf(stderr, 
							  "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>') {
            if (tok->outfile) {
                (void) fprintf(stderr, 
							  "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf ++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"') {
            /* Detect quoted tokens */
            buf++;
            next = strchr (buf, *(buf-1));
        } else {
            /* Find next delimiter */
            next = buf + strcspn (buf, delims);
        }
        
        if (next == NULL) {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* 
		 * Record the token as either the next 
		 * argument or the i/o file 
		 */
        switch (parsing_state) {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS-1) break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL) {
        (void) fprintf(stderr,
                       "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0)  /* ignore blank line */
        return 1;
    if (!strcmp(tok->argv[0], "quit")) {             /* quit command */
        tok->builtins = BUILTIN_QUIT;
    } else if (!strcmp(tok->argv[0], "jobs")) {      /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    } else if (!strcmp(tok->argv[0], "bg")) {        /* bg command */
        tok->builtins = BUILTIN_BG;
    } else if (!strcmp(tok->argv[0], "fg")) {        /* fg command */
        tok->builtins = BUILTIN_FG;
    } else {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The 
 *     handler reaps all available zombie children, but doesn't wait 
 *     for any other currently running children to terminate.  
 */
void 
sigchld_handler(int sig) 
{
    pid_t pid;
    pid_t jid;
    int status;
    /*
     * Reap all terminated children process here and also 
	 * return when a child stop. Report process status and 
     * do all job deletion here.
     */  
    while((pid = waitpid(-1, &status, WNOHANG|WUNTRACED))>0){
        jid = pid2jid(pid);
        /* 
		 * If a child process terminate normally, just 
		 * delete the job here. 
		 */
        if(WIFEXITED(status)){
            assert(deletejob(job_list, pid)!=0);
        }
        /* 
         * If a child process terminated by receiving signal, 
		 * delete the job here and then print signal information.
         */
        else if(WIFSIGNALED(status)){
            assert(deletejob(job_list, pid)!=0);
            printf("Job [%d] (%d) terminated by signal %d\n", jid, pid, 
            WTERMSIG(status));
        }
        /* 
         * If a child process stopped by receiving signal, change the job 
         * state here and then print signal information.
         */
        else if(WIFSTOPPED(status)){
            getjobpid(job_list, pid)->state = ST;
            printf("Job [%d] (%d) stopped by signal %d\n", jid, pid, 
            WSTOPSIG(status));
        }
    }
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 * user types ctrl-c at the keyboard.  Catch it and send it along
 * to the foreground job.  
 */
void 
sigint_handler(int sig) 
{
    pid_t pid=0;
    /*
     * If there is a foreground job running, get its pid first,
     * then forward SIGINT signal to the foreground job process group.
     * When this job terminated, job process will send SIGCHLD back to
     * shell, then delete job and print information in sigchild handler.
     */
    if((pid=fgpid(job_list))){
        Kill(-pid, SIGINT);
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 * the user types ctrl-z at the keyboard. Catch it and suspend the
 * foreground job by sending it a SIGTSTP.  
 */
void 
sigtstp_handler(int sig) 
{
    pid_t pid; 
    /*
     * If there is a foreground job running, get its pid first,
     * then forward SIGTSTP signal to the foreground job process group.
     * When this job stopped, job process will send SIGCHLD back to
     * shell, then change the job state and print information in sigchild 
     * handler.
     */
    if((pid=fgpid(job_list))){
        Kill(-pid, SIGTSTP);        
    }
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 :q
* Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void 
clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void 
initjobs(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int 
maxjid(struct job_t *job_list) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int 
addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(job_list[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n",
                       job_list[i].jid,
                       job_list[i].pid,
                       job_list[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int 
deletejob(struct job_t *job_list, pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t 
fgpid(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t 
*getjobpid(struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int 
pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void 
listjobs(struct job_t *job_list, int output_fd) 
{
    int i;
    char buf[MAXLINE];

    for (i = 0; i < MAXJOBS; i++) {
        memset(buf, '\0', MAXLINE);
        if (job_list[i].pid != 0) {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            switch (job_list[i].state) {
            case BG:
                sprintf(buf, "Running    ");
                break;
            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void 
usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void 
unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void 
app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t 
*Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /*block sigs of type being handled */
    action.sa_flags = SA_RESTART; /*restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 * child shell by sending it a SIGQUIT signal.
 */
void 
sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

/***********************************
 * Self-define wrapper function
 **********************************/
 
/*
 * wrapper of fork in order to detect exception.
 */
pid_t 
Fork(void)
{
    pid_t pid;
    if((pid=fork()) < 0)
        unix_error("Fork error");
    return pid;
}
/*
 * wrapper of waitpid in order to detect exception.
 */
pid_t 
Waitpid(pid_t pid, int *status, int options)
{
    pid_t returnPid;
    if((returnPid=waitpid(pid, status, options)) < 0)
        unix_error("Waitpid error");
    return returnPid;
}
/*
 * wrapper of kill in order to detect exception.
 */
void
Kill(pid_t pid, int signal)
{
    if(kill(pid, signal)<0)
        unix_error("Kill error");
    return;
}
/*
 * wrapper of execve in order to detect exception.
 */
void 
Execve(const char *filename, char *const argv[], char *const envp[])
{
    if(execve(filename, argv, envp)<0)
        unix_error("Execve error");
    return;
}
/*
 * wrapper of setpgid in order to detect exception.
 */
void
Setpgid(pid_t pid, pid_t pgid)
{
    if(setpgid(pid, pgid)<0)
        unix_error("Setpgid error");
    return;
}

/*
 * wrapper of sigemptyset in order to detect exception.
 */
void 
Sigemptyset(sigset_t *set)
{
    if(sigemptyset(set)<0)
        unix_error("Sigemptyset error");
    return;
}
/*
 * wrapper of sigfillset in order to detect exception.
 */
void 
Sigfillset(sigset_t *set)
{
    if(sigfillset(set)<0)
        unix_error("Sigfillset error");
    return;
}
/*
 * wrapper of sigaddset in order to detect exception.
 */
void 
Sigaddset(sigset_t *set, int signum)
{
    if(sigaddset(set, signum)<0)
        unix_error("Sigaddset error");
    return;
}
/*
 * wrapper of sigdelset in order to detect exception.
 */
void 
Sigdelset(sigset_t *set, int signum)
{
    if(sigdelset(set, signum)<0)
        unix_error("Sigdelset error");
    return;
}
/*
 * wrapper of sigprocmask in order to detect exception.
 */
void 
Sigprocmask(int option, const sigset_t *set, sigset_t *oldset)
{
    if(sigprocmask(option, set, oldset)<0)
        unix_error("Sigprocmask error");
    return;
}
/*
 * wrapper of sigprocmask in order to detect exception.
 */
void 
Sigsuspend(const sigset_t *mask)
{
    if(sigsuspend(mask)!=-1)
        unix_error("Sigsuspend error");
    return;
}
/*
 * wrapper of open in order to detect exception.
 */
int 
Open(char *filename, int flags)
{
    int fd;
    if((fd=open(filename, flags))<0)
        unix_error("Open error");
    return fd;
}
/*
 * wrapper of close in order to detect exception.
 */
void 
Close(int fd)
{
    if(close(fd)<0)
        unix_error("Close error");
    return;
}
/*
 * wrapper of dup2 in order to detect exception.
 */
void 
Dup2(int oldfd, int newfd)
{
    if(dup2(oldfd, newfd)<0)
        unix_error("Dup2 error");
    return;
}
/****************************************
 * End of self-define wrapper function
 ***************************************/
