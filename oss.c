#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#define SIZE 20
#define SHM_KEY 1234

struct PCB {
    int occupied; // either true or false
    pid_t pid; // process id of this child
    int startSeconds; // time when it was created
    int startNano; // time when it was created
    int serviceTimeSeconds; // total seconds it has been "scheduled"
    int serviceTimeNano; // total nanoseconds it has been "scheduled"
    int eventWaitSec; // when does its event happen?
    int eventWaitNano; // when does its event happen?
    int blocked; // is this process waiting on event?
};
struct PCB processTable[SIZE];

static void print_help(void) {
    printf("Usage: oss [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i fractionOfSecondToLaunchChildren] [-f logfile]\n");
    printf("  -h        Show help\n");
    printf("  -n Total number of workers to launch\n");
    printf("  -s Maximum number of workers in the system at once\n");
    printf("  -t the upper bound on how long the process should be scheduled before terminating\n");
    printf("  -i Simulated time between launches in seconds\n");
    printf("  -f print the output to file \n");
}

int main(int argc, char *argv[]) { 
    char opt;
    const char optstring[] = "hn:s:t:i:";

    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
            case 'h':
                print_help();
                return 0;
            case 'n':
                totalChildren = atoi(optarg);
                break;
            case 's':
                maxSimul = atoi(optarg);
                break;
            case 't':
                timeLimit = atof(optarg);
                break;
            case 'i':
                interval = atof(optarg);
                break;
            default:
                fprintf(stderr, "Invalid option\n");
                print_help();
                return 1;
        }
    }
}