#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    unsigned int seconds;
    unsigned int nanoseconds;
} MyClock;

volatile sig_atomic_t terminateRequested = 0;

static void signal_handler(int sig) {
    terminateRequested = 1;
}

static int choose_cpu_use(unsigned int remainingNs, int quantumNs) {
    unsigned int quantum = (unsigned int)quantumNs;

    // If the job can finish in this dispatch, use exactly what is left
    if (remainingNs <= quantum) {
        return (int)remainingNs;
    }

    // Otherwise block 20% of the time and use part of the quantum
    if ((rand() % 100) < 20) {
        return (rand() % (quantumNs - 1)) + 1;
    }

    return quantumNs;
}

int main(int argc, char *argv[]) {
    int msgId = 0;
    key_t key;
    int localPid;
    unsigned int totalBurstNs;
    unsigned int nsUsedSoFar = 0U;
    MyClock *myClock;

    if (argc < 3) {
        fprintf(stderr, "user: missing arguments\n");
        return EXIT_FAILURE;
    }

    localPid = atoi(argv[1]);
    totalBurstNs = (unsigned int)strtoul(argv[2], NULL, 10);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int shmid = shmget(1234, sizeof(MyClock), 0666);
    if (shmid < 0) {
        fprintf(stderr,"Worker: shmget error\n");
        exit(1);
    }

    MyClock *myClock = (MyClock *)shmat(shmid, NULL, 0);
    if (myClock == (void *)-1) {
        fprintf(stderr,"Worker:... Error in shmat\n");
        return 1;
    }
    
    // get a key for our message queue
    if ((key = ftok("msgq.txt", 1)) == -1) {
        perror("ftok");
        exit(1);
    }

    // create our message queue
    if ((msqid = msgget(key, 0644)) == -1) {
        perror("msgget in child");
        shmdt(myClock);
        exit(1);
    }

    srand((unsigned int)(getpid() ^ (localPid << 16) ^ time(NULL)));

    while (!terminateRequested) {
        DispatchMessage dispatch;
        ReportMessage report;
        unsigned int remainingNs;
        int usedNs;

        // oss addresses each dispatch to one child by using the real pid as the message type
        if (msgrcv(msgId, &dispatch, sizeof(dispatch) - sizeof(long), (long)getpid(), 0) == -1) {
            if (errno == EINTR && terminateRequested) {
                break;
            }
            perror("user msgrcv");
            shmdt(myClock);
            return EXIT_FAILURE;
        }

        remainingNs = totalBurstNs - nsUsedSoFar;
        usedNs = choose_cpu_use(remainingNs, dispatch.quantumNs);
        nsUsedSoFar += (unsigned int)usedNs;

        report.mtype = 1;
        report.pid = getpid();

        // Negative means terminate after using that much CPU; positive means still alive
        if ((unsigned int)usedNs == remainingNs) {
            report.usedNs = -usedNs;
        } else {
            report.usedNs = usedNs;
        }

        if (msgsnd(msgId, &report, sizeof(report) - sizeof(long), 0) == -1) {
            perror("user msgsnd");
            shmdt(myClock);
            return EXIT_FAILURE;
        }

        if (report.usedNs < 0) {
            break;
        }
    }

    shmdt(myClock);
    return EXIT_SUCCESS;
}
