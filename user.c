#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SHM_KEY 1234
#define MSG_KEY 2345
#define OSS_MESSAGE_TYPE 1

typedef struct {
    unsigned int seconds;
    unsigned int nanoseconds;
} MyClock;

typedef struct {
    long mtype;
    int quantumNs;
} DispatchMessage;

typedef struct {
    long mtype;
    pid_t pid;
    int usedNs;
} ReportMessage;

static volatile sig_atomic_t terminateRequested = 0;

static void signal_handler(int sig) {
    (void)sig;
    terminateRequested = 1;
}

static int choose_cpu_use(unsigned int remainingNs, int quantumNs) {
    unsigned int quantum = (unsigned int)quantumNs;

    if (remainingNs <= quantum) {
        return (int)remainingNs;
    }

    if ((rand() % 100) < 20) {
        return (rand() % (quantumNs - 1)) + 1;
    }

    return quantumNs;
}

int main(int argc, char *argv[]) {
    int shmId;
    int msgId;
    int localPid;
    unsigned int totalBurstNs;
    unsigned int usedSoFarNs = 0U;
    MyClock *myClock;

    if (argc < 3) {
        fprintf(stderr, "user: missing arguments\n");
        return EXIT_FAILURE;
    }

    localPid = atoi(argv[1]);
    totalBurstNs = (unsigned int)strtoul(argv[2], NULL, 10);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    shmId = shmget(SHM_KEY, sizeof(MyClock), 0666);
    if (shmId == -1) {
        perror("user shmget");
        return EXIT_FAILURE;
    }

    myClock = (MyClock *)shmat(shmId, NULL, 0);
    if (myClock == (void *)-1) {
        perror("user shmat");
        return EXIT_FAILURE;
    }

    msgId = msgget(MSG_KEY, 0666);
    if (msgId == -1) {
        perror("msgget in child");
        shmdt(myClock);
        return EXIT_FAILURE;
    }

    srand((unsigned int)(getpid() ^ (localPid << 16) ^ time(NULL)));

    while (!terminateRequested) {
        DispatchMessage dispatch;
        ReportMessage report;
        unsigned int remainingNs;
        int usedNs;

        if (msgrcv(msgId, &dispatch, sizeof(dispatch) - sizeof(long), (long)getpid(), 0) == -1) {
            if (errno == EINTR && terminateRequested) {
                break;
            }
            perror("user msgrcv");
            shmdt(myClock);
            return EXIT_FAILURE;
        }

        remainingNs = totalBurstNs - usedSoFarNs;
        usedNs = choose_cpu_use(remainingNs, dispatch.quantumNs);
        usedSoFarNs += (unsigned int)usedNs;

        report.mtype = OSS_MESSAGE_TYPE;
        report.pid = getpid();
        report.usedNs = ((unsigned int)usedNs == remainingNs) ? -usedNs : usedNs;

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
