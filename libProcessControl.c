#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<fcntl.h>
#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<sys/stat.h>

#include "libParseArgs.h"
#include "libProcessControl.h"

/**
 * parallelDo -n NUM -o OUTPUT_DIR COMMAND_TEMPLATE ::: [ ARGUMENT_LIST ...]
 * build and execute shell command lines in parallel
 */

/**
 * create and return a newly malloced command from commandTemplate and argument
 * the new command replaces each occurrance of {} in commandTemplate with argument
 */
char *createCommand(char *commandTemplate, char *argument) {
	char *command = NULL;
	char *substring = "{}";
	int argumentLen = strlen(argument);
	int substringLen = 2;
	int commandTemplateLen = strlen(commandTemplate);
	int substringNum = 0;
	char *string = commandTemplate;
	char *pointer;
	pointer = strstr(string, substring);
	if (pointer == NULL) {
		command = (char *) malloc(sizeof(char) * (commandTemplateLen+1));
		int i = 0;
		while(*(commandTemplate+i) != '\0') {
			*(command+i) = *(commandTemplate+i);
			i++;
		};
		*(command+i) = '\0';
		return command;
	};
	while(pointer != NULL) {
		substringNum++;
		string = pointer + substringLen;
		pointer = strstr(string, substring);
	}
	int commandLen;
	if (argumentLen < substringLen) {
		commandLen = commandTemplateLen + substringNum * argumentLen + 1;
	} else {
		commandLen = commandTemplateLen + substringNum * (argumentLen - substringLen) + 1;
	}
	if ((command = (char *) malloc(sizeof(char)*commandLen)) == NULL) return NULL;

	char *commandPointer = command;
	char *string2 = commandTemplate;
	char *pointer2;
	pointer2 = strstr(string2, substring);
	while (pointer2 != NULL) {
		int size = pointer2 - string2;
		strncpy(commandPointer, string2, size);
		commandPointer += size;
		strncpy(commandPointer, argument, argumentLen);
		commandPointer += argumentLen;
		string2 = pointer2 + substringLen;
		pointer2 = strstr(string2, substring);
	}
	strcpy(commandPointer, string2);
	return command;
}

typedef struct PROCESS_STRUCT {
	int pid;
	int ifExited;
	int exitStatus;
	int status;
	char *command;
} PROCESS_STRUCT;

typedef struct PROCESS_CONTROL {
	int numProcesses;
	int numRunning;
	int maxNumRunning;
	int numCompleted;
	PROCESS_STRUCT *process;
} PROCESS_CONTROL;

PROCESS_CONTROL processControl;

void printSummary(){
	printf("%d %d %d\n", processControl.numProcesses, processControl.numCompleted, processControl.numRunning);
}
void printSummaryFull(){
	printSummary();
	for(int i=0;i<processControl.numCompleted; i++){
		printf("%d %d %d %s\n",
				processControl.process[i].pid,
				processControl.process[i].ifExited,
				processControl.process[i].exitStatus,
				processControl.process[i].command);
	}
}
/**
 * find the record for pid and update it based on status
 * status has information encoded in it, you will have to extract it
 */
void updateStatus(int pid, int status){
	if (WIFEXITED(status) && !WIFSIGNALED(status) && !WIFSTOPPED(status) && !WIFCONTINUED(status)) {
		processControl.process[processControl.numCompleted].pid = pid;
		processControl.process[processControl.numCompleted].status = status;
		processControl.process[processControl.numCompleted].exitStatus = WEXITSTATUS(status);
		processControl.process[processControl.numCompleted].ifExited = 1;
	}
}

void handler(int signum){
	fprintf(stderr, "Signal %d caught\n", signum);
}

int runParallelHelper(int child_pid_index, pid_t *child_pid) {
	// Initialize variables for the Filenames
	char stdout[8] = ".stdout";
        char stderr[8] = ".stderr";
        int j = 0;
        int outputDirLen = 0;
        while (*(pparams.outputDir + j) != '\0') {
                j++;
                outputDirLen++;
        };
	// Create variables for forking
	int isParent = 1;
        pid_t c_pid=fork();
	pid_t pid = getpid();
        if(c_pid==0){
		// exec the command to the proper filename
		int isParent = 0;
		const size_t max_pid_len = 12;
                int pid = getpid();
                char * mypid = malloc(max_pid_len + 1);
                snprintf(mypid, max_pid_len, "%d", pid);
                int i = 0;
                int mypidLen = 0;
                while (*(mypid + i) != '\0') {
                        i++;
                        mypidLen++;
                };
                int filenameLen = mypidLen + outputDirLen + 9;
                char *stdoutFilename = (char *) malloc(sizeof(char) * filenameLen);
                char *stderrFilename = (char *) malloc(sizeof(char) * filenameLen);
                strncat(stdoutFilename, pparams.outputDir, outputDirLen);
                strncat(stdoutFilename, "/", 2);
                strncat(stdoutFilename, mypid, mypidLen);
                strncat(stdoutFilename, stdout, 8);

                strncat(stderrFilename, pparams.outputDir, outputDirLen);
                strncat(stderrFilename, "/", 2);
                strncat(stderrFilename, mypid, mypidLen);
                strncat(stderrFilename, stderr, 8);

                int fdout, fderr;
                if ((fdout=open(stdoutFilename, O_WRONLY|O_CREAT|O_TRUNC, 0666)) < 0) {
                        perror(stdoutFilename);
                        return(1);
                }
                if ((fderr =open(stderrFilename, O_WRONLY|O_CREAT|O_TRUNC, 0666)) < 0) {
                        perror(stderrFilename);
                        return(1);
                }
                dup2(fdout, 1);
                dup2(fderr, 2);
                close(fdout);
                close(fderr);
		free(stdoutFilename);
		free(stderrFilename);
                execl("/bin/bash", "/bin/bash", "-c", processControl.process[child_pid_index].command, (char *)NULL);
		return 0;
        } else if(c_pid>0) {
		// Increase the number of processes and document the child pid
		child_pid[child_pid_index] = c_pid;
                processControl.numRunning += 1;
		return 1;
        } else {
                printf("Fork error, no child.\n");
        }
}

/**
 * This function does the bulk of the work for parallelDo. This is called
 * after understanding the command line arguments. runParallel 
 * uses pparams to generate the commands (createCommand), 
 * forking, redirecting stdout and stderr, waiting for children, ...
 * Instead of passing around variables, we make use of globals pparams and
 * processControl.
 */
int runParallel(){
	signal(SIGUSR2, handler);
        signal(SIGUSR1, handler); // use SIG_DFL to set the hander back to the default
        signal(SIGHUP, SIG_IGN); // Ignore termal hangup, so keep running

        // Create the PROCESS_CONTROL and initialize it
	PROCESS_STRUCT tempProcessArray[pparams.argumentListLen];
        processControl.numProcesses = pparams.argumentListLen;
	processControl.numRunning = 0;
	processControl.numCompleted = 0;
	processControl.maxNumRunning = pparams.maxNumRunning;
	processControl.process = tempProcessArray;
	for(int i = 0; i<pparams.argumentListLen; i++) {
		PROCESS_STRUCT process;
		process.pid = 0;
		process.ifExited = 0;
		process.exitStatus = 0;
		process.status = 0;
		process.command = createCommand(pparams.commandTemplate, pparams.argumentList[i]);
		tempProcessArray[i] = process;
	};
	// create the Output Directory
	int create = mkdir(pparams.outputDir, 0777);
	// Create variables for Filename creation
	char stdout[8] = ".stdout";
        char stderr[8] = ".stderr";
	int j = 0;
	int outputDirLen = 0;
	while (*(pparams.outputDir + j) != '\0') {
		j++;
		outputDirLen++;
	};
	// Create variables for forking
	int child_pid_index = 0;
	int isParent = 1;
	pid_t *child_pid = (pid_t *) malloc(sizeof(pid_t) * pparams.argumentListLen);
        pid_t current_child_pid;
        pid_t init_child_pid;
	pid_t pid = getpid();

        for(int i=0;i<pparams.maxNumRunning;i++){
                init_child_pid=fork();
 
                if(init_child_pid==0) {
			// exec the command to the proper filename
			const size_t max_pid_len = 12;
			int pid = getpid();
			char * mypid = malloc(max_pid_len + 1);
			snprintf(mypid, max_pid_len, "%d", pid);
			int i = 0;
			int mypidLen = 0;
			while (*(mypid + i) != '\0') {
				i++;
				mypidLen++;
			};
			int filenameLen = mypidLen + outputDirLen + 9;
			char *stdoutFilename = (char *) malloc(sizeof(char) * filenameLen);
			char *stderrFilename = (char *) malloc(sizeof(char) * filenameLen);
			strncat(stdoutFilename, pparams.outputDir, outputDirLen);
			strncat(stdoutFilename, "/", 2);
			strncat(stdoutFilename, mypid, mypidLen);
			strncat(stdoutFilename, stdout, 8);

			strncat(stderrFilename, pparams.outputDir, outputDirLen);
                        strncat(stderrFilename, "/", 2);
                        strncat(stderrFilename, mypid, mypidLen);
                        strncat(stderrFilename, stderr, 8);

			int fdout, fderr;
        		if ((fdout=open(stdoutFilename, O_WRONLY|O_CREAT|O_TRUNC, 0666)) < 0) {
                		perror(stdoutFilename);
                		return(1);
        		}
        		if ((fderr =open(stderrFilename, O_WRONLY|O_CREAT|O_TRUNC, 0666)) < 0) {
                		perror(stderrFilename);
                		return(1);
        		}
			dup2(fdout, 1);
			dup2(fderr, 2);
			close(fdout);
			close(fderr);
			free(stdoutFilename);
			free(stderrFilename);
			isParent = 0;
			execl("/bin/bash", "/bin/bash", "-c", processControl.process[child_pid_index].command, (char *)NULL);
               	} else if(init_child_pid>0) {
			// increase the number of processes running and document the child_pid.
                       	child_pid[child_pid_index] = init_child_pid;
			processControl.numRunning += 1;
			child_pid_index++;
               	} else {
                       	printf("Fork error, no child.\n");
               	}
		if (!isParent) {
			break;
		}
        }
	int isParent2;
	if (isParent) {
		isParent2 = 1;
	} else {
		isParent2 = 0;
	}
	if(isParent) {
		int waitsCompleted = 0;
                while(waitsCompleted != pparams.argumentListLen) {
                        // Waiting on my child
			int child_status;
                        current_child_pid =waitpid(child_pid[waitsCompleted], &child_status, 0); // child_status now encodes information about the child
			updateStatus(child_pid[waitsCompleted], child_status);
                        if(WIFEXITED(child_status)){
				processControl.numCompleted += 1;
                               	processControl.numRunning -= 1;
				if((processControl.numCompleted + processControl.numRunning) != pparams.argumentListLen && processControl.numRunning != processControl.maxNumRunning) {
					// Create another child process once one child process is finished.
					isParent2 = runParallelHelper(child_pid_index, child_pid);
					if (!isParent2) {
						break;
					} else {
						child_pid_index++;
					};
                         	}
                       	} else {
                               	printf("Parent (pid=%d): child pid=%d state changed for some other reason\n", pid, current_child_pid);
                       	}
			waitsCompleted++;
		}
        }

        sleep(10);

        if(isParent2){
		printSummaryFull();
                exit(0);
        } else {
         	exit(7);
	}
}
