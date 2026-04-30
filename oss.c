#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHM_KEY 1234
#define MSG_KEY 2345
#define OSS_MESSAGE_TYPE 1
#define MAX_TOTAL_PROCESSES 20
#define MAX_PCBS 18
#define BLOCK_DURATION_NS 100000000
#define DISPATCH_OVERHEAD_NS 1000
#define UNBLOCK_OVERHEAD_NS 10000
#define IDLE_TICK_NS 1000

typedef struct {
    int occupied;
    int blocked;
    int localPid;
    pid_t pid;
    unsigned int startSeconds;
    unsigned int startNanoseconds;
    unsigned int serviceSeconds;
    unsigned int serviceNanoseconds;
    unsigned int lastReadySeconds;
    unsigned int lastReadyNanoseconds;
    unsigned int totalWaitSeconds;
    unsigned int totalWaitNanoseconds;
    unsigned int eventWaitSec;
    unsigned int eventWaitNano;
    unsigned int totalBurstNs;
} PCB;

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

typedef struct {
    int data[MAX_PCBS];
    int front;
    int rear;
    int size;
} ReadyQueue;

MyClock *myClock;
static PCB processTable[MAX_PCBS];
static ReadyQueue readyQueue;
static FILE *logFile = NULL;
volatile sig_atomic_t shutdownRequested = 0;
static int shmId = -1;
static int msgId = -1;

static int totalToLaunch = MAX_TOTAL_PROCESSES;
static int maxSimultaneous = 5;
static double timeLimitSeconds = 2.0;
static double launchIntervalSeconds = 0.0;
static const char *logFileName = "oss.log";

static int launchedCount = 0;
static int finishedCount = 0;
static int activeCount = 0;
static int nextLocalPid = 1;
static unsigned long long processCpuTimeNs = 0ULL;
static unsigned long long ossOverheadNs = 0ULL;
static unsigned long long nextTableDumpNs = 500000000ULL;
static unsigned long long nextLaunchTimeNs = 0ULL;

static void add_ns_to_values(unsigned int *seconds, unsigned int *nanoseconds, unsigned long long addNs) {
    // Store time in split sec/ns form while still doing arithmetic in one 64-bit value
    unsigned long long total = (unsigned long long)(*seconds) * 1000000000ULL + *nanoseconds + addNs;
    *seconds = (unsigned int)(total / 1000000000ULL);
    *nanoseconds = (unsigned int)(total % 1000000000ULL);
}

static unsigned long long clock_to_ns(unsigned int seconds, unsigned int nanoseconds) {
    return (unsigned long long)seconds * 1000000000ULL + nanoseconds;
}

static unsigned long long sim_time_ns(void) {
    return clock_to_ns(myClock->seconds, myClock->nanoseconds);
}

static void advance_clock(unsigned long long ns, bool processWork) {
    // Track whether the time advance came from user work or from oss overhead/idle time
    add_ns_to_values(&myClock->seconds, &myClock->nanoseconds, ns);
    if (processWork) {
        processCpuTimeNs += ns;
    } else {
        ossOverheadNs += ns;
    }
}

static int compare_time(int leftSec, int leftNano, int rightSec, int rightNano) {
    if (leftSec < rightSec) {
        return -1;
    }
    if (leftSec > rightSec) {
        return 1;
    }
    if (leftNano < rightNano) {
        return -1;
    }
    if (leftNano > rightNano) {
        return 1;
    }
    return 0;
}

static void log_both(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (logFile != NULL) {
        va_start(args, fmt);
        vfprintf(logFile, fmt, args);
        va_end(args);
        fflush(logFile);
    }
}

static bool queue_push(ReadyQueue *queue, int value) {
    if (queue->size >= MAX_PCBS) {
        return false;
    }
    // Circular queue keeps enqueue/dequeue O(1) for round-robin scheduling
    queue->data[queue->rear] = value;
    queue->rear = (queue->rear + 1) % MAX_PCBS;
    queue->size++;
    return true;
}

static int queue_pop(ReadyQueue *queue) {
    int value = -1;

    if (queue->size == 0) {
        return -1;
    }

    value = queue->data[queue->front];
    queue->front = (queue->front + 1) % MAX_PCBS;
    queue->size--;
    return value;
}

static void maybe_dump_state(void) {
    // Table output every half second of simulated time
    while (sim_time_ns() >= nextTableDumpNs) {
        log_both("OSS: Process table at time %u:%u\n", myClock->seconds, myClock->nanoseconds);
    log_both("OSS: Entry Occupied LocalPid SysPid Blocked CPU(s:ns) Wait(s:ns) Event(s:ns)\n");
    for (int i = 0; i < MAX_PCBS; i++) {
        PCB *pcb = &processTable[i];
        if (!pcb->occupied) {
            continue;
        }
        log_both("OSS: %5d %8d %8d %6d %7d %5u:%09u %5u:%09u %5u:%09u\n",
                 i,
                 pcb->occupied,
                 pcb->localPid,
                 pcb->pid,
                 pcb->blocked,
                 pcb->serviceSeconds,
                 pcb->serviceNanoseconds,
                 pcb->totalWaitSeconds,
                 pcb->totalWaitNanoseconds,
                 pcb->eventWaitSec,
                 pcb->eventWaitNano);
    }

    log_both("OSS: Blocked processes [");
    for (int i = 0; i < MAX_PCBS; i++) {
        if (processTable[i].occupied && processTable[i].blocked) {
            log_both(" P%d", processTable[i].localPid);
        }
    }
    log_both(" ]\n");
        nextTableDumpNs += 500000000ULL;
    }
}

void signal_handler(int sig) {
    printf("\nReceived SIGALRM (3 second timeout). Cleaning up...\n");
    
    // Send kill signal to all children based on their PIDs in process table
    for (int i = 0; i < MAX_PCBS; i++) {
        if (processTable[i].occupied && processTable[i].pid > 0) {
            printf("Killing child process %d\n", processTable[i].pid);
            kill(processTable[i].pid, SIGTERM);
        }
    }
    printf("OSS terminated due to 3 second timeout\n");
    exit(1);
}

static unsigned int parse_time_limit_ns(double seconds) {
    if (seconds < 0.0) {
        return 0U;
    }
    return (unsigned int)(seconds * 1000000000.0);
}

static unsigned long long random_interval_ns(void) {
    unsigned long long maxNs = (unsigned long long)(launchIntervalSeconds * 1000000000.0);

    if (maxNs == 0ULL) {
        return 0ULL;
    }

    return (unsigned long long)(rand() % (int)(maxNs + 1ULL));
}

static void schedule_next_launch(void) {
    // Each new launch time is relative to the current simulated clock
    nextLaunchTimeNs = sim_time_ns() + random_interval_ns();
}

static int find_open_pcb_slot(void) {
    for (int i = 0; i < MAX_PCBS; i++) {
        if (!processTable[i].occupied) {
            return i;
        }
    }
    return -1;
}

static void clear_pcb(int index) {
    memset(&processTable[index], 0, sizeof(processTable[index]));
}

static int launch_child(void) {
    int pcbIndex;
    PCB *pcb;
    char slotArg[16];
    char burstArg[32];

    pcbIndex = find_open_pcb_slot();
    if (pcbIndex == -1) {
        return -1;
    }

    pcb = &processTable[pcbIndex];
    clear_pcb(pcbIndex);

    pcb->occupied = 1;
    pcb->blocked = 0;
    pcb->localPid = nextLocalPid++;
    pcb->startSeconds = myClock->seconds;
    pcb->startNanoseconds = myClock->nanoseconds;
    pcb->lastReadySeconds = myClock->seconds;
    pcb->lastReadyNanoseconds = myClock->nanoseconds;

    // Give each child a total CPU burst in the range (0, -t]
    pcb->totalBurstNs = (rand() % (parse_time_limit_ns(timeLimitSeconds) + 1U));
    if (pcb->totalBurstNs == 0U) {
        pcb->totalBurstNs = 1U;
    }

    snprintf(slotArg, sizeof(slotArg), "%d", pcb->localPid);
    snprintf(burstArg, sizeof(burstArg), "%u", pcb->totalBurstNs);

    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "Fork failed\n");
        clear_pcb(pcbIndex);
        return 0;
    }
    if (pid == 0) {
        // Pass the logical pid and total burst so the child can simulate its own lifetime
        execl("./user", "./user", slotArg, burstArg, (char *)NULL);
        perror("execl");
        exit(1);
    }

    pcb->pid = pid;
    queue_push(&readyQueue, pcbIndex);
    launchedCount++;
    activeCount++;
    log_both("OSS: Generating process with PID %d and putting it in ready queue at time %u:%u\n", pcb->localPid, myClock->seconds, myClock->nanoseconds);
    schedule_next_launch();
    return pcbIndex;
}

static unsigned long long next_unblock_time_ns(void) {
    unsigned long long result = 0ULL;
    bool found = false;

    // When idle, oss jumps the clock to the earliest blocked-process wakeup
    for (int i = 0; i < MAX_PCBS; i++) {
        PCB *pcb = &processTable[i];
        unsigned long long wakeNs;

        if (!pcb->occupied || !pcb->blocked) {
            continue;
        }

        wakeNs = clock_to_ns(pcb->eventWaitSec, pcb->eventWaitNano);
        if (!found || wakeNs < result) {
            result = wakeNs;
            found = true;
        }
    }

    return found ? result : 0ULL;
}

static void finish_process(int pcbIndex) {
    PCB *pcb = &processTable[pcbIndex];
    int status;

    // Remove the child immediately
    waitpid(pcb->pid, &status, 0);
    log_both("OSS: Process PID %d terminated and was removed from the system at time %u:%u\n", pcb->localPid, myClock->seconds, myClock->nanoseconds);
    clear_pcb(pcbIndex);
    finishedCount++;
    activeCount--;
}

static void dispatch_process(int pcbIndex) {
    PCB *pcb = &processTable[pcbIndex];
    DispatchMessage dispatch;
    ReportMessage report;
    ssize_t received;

    log_both("OSS: Ready queue [");
    for (int index = 0; index < readyQueue.size; index++) {
        int pcbIndex = readyQueue.data[(readyQueue.front + index) % MAX_PCBS];
        log_both(" P%d", processTable[pcbIndex].localPid);
    }
    log_both(" ]\n");

    unsigned long long nowNs = sim_time_ns();
    unsigned long long readyNs = clock_to_ns(pcb->lastReadySeconds, pcb->lastReadyNanoseconds);
    unsigned long long deltaNs = nowNs - readyNs;

    // Waiting time is the time spent sitting in the ready queue since last becoming ready
    add_ns_to_values(&pcb->totalWaitSeconds, &pcb->totalWaitNanoseconds, deltaNs);

    log_both("OSS: Dispatching process with PID %d from ready queue at time %u:%u\n", pcb->localPid, myClock->seconds, myClock->nanoseconds);

    // The message type is the real child pid so only that one process receives the dispatch
    dispatch.mtype = (long)pcb->pid;
    dispatch.quantumNs = 25000000;
    if (msgsnd(msgId, &dispatch, sizeof(dispatch) - sizeof(long), 0) == -1) {
        perror("msgsnd");
        shutdownRequested = 1;
        return;
    }

    advance_clock(DISPATCH_OVERHEAD_NS, false);
    log_both("OSS: total time this dispatch was %d nanoseconds\n", DISPATCH_OVERHEAD_NS);
    maybe_dump_state();

    received = msgrcv(msgId, &report, sizeof(report) - sizeof(long), OSS_MESSAGE_TYPE, 0);
    if (received == -1) {
        if (errno != EINTR) {
            perror("msgrcv");
        }
        shutdownRequested = 1;
        return;
    }

    if (report.usedNs < 0) {
        unsigned int actualNs = (unsigned int)(-report.usedNs);
        // A negative reply means the child consumed this much time and then finished
        add_ns_to_values(&pcb->serviceSeconds, &pcb->serviceNanoseconds, actualNs);
        advance_clock(actualNs, true);
        log_both("OSS: Receiving that process with PID %d ran for %u nanoseconds and terminated\n", pcb->localPid, actualNs);
        maybe_dump_state();
        finish_process(pcbIndex);
        return;
    }

    add_ns_to_values(&pcb->serviceSeconds, &pcb->serviceNanoseconds, (unsigned int)report.usedNs);
    advance_clock((unsigned int)report.usedNs, true);
    log_both("OSS: Receiving that process with PID %d ran for %d nanoseconds\n", pcb->localPid, report.usedNs);

    if (report.usedNs < 25000000) {
        // A partial positive usage means the child blocked for I/O after some CPU time
        pcb->eventWaitSec = myClock->seconds;
        pcb->eventWaitNano = myClock->nanoseconds;
        add_ns_to_values(&pcb->eventWaitSec, &pcb->eventWaitNano, BLOCK_DURATION_NS);
        pcb->blocked = 1;
        log_both("OSS: Process PID %d did not use its entire time quantum and was blocked until %u:%u\n", pcb->localPid, pcb->eventWaitSec, pcb->eventWaitNano);
    } else {
        // A full-quantum reply stays runnable, so it goes back to the tail of the queue
        pcb->lastReadySeconds = myClock->seconds;
        pcb->lastReadyNanoseconds = myClock->nanoseconds;
        queue_push(&readyQueue, pcbIndex);
        log_both("OSS: Putting process with PID %d into ready queue\n", pcb->localPid);
    }
    maybe_dump_state();
}

static void print_help() {
    printf("Usage: oss [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i fractionOfSecondToLaunchChildren] [-f logfile]\n");
    printf("  -h        Show help\n");
    printf("  -n proc   Total number of child processes to launch (max 20)\n");
    printf("  -s simul  Maximum children allowed in system simultaneously (max 18)\n");
    printf("  -t value  Upper bound in seconds for each child's total CPU burst\n");
    printf("  -i value  Maximum simulated seconds between child launches\n");
    printf("  -f file   Log file name\n");
}

int main(int argc, char *argv[]) {
    char opt;
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                print_help();
                return 0;
            case 'n':
                totalToLaunch = atoi(optarg);
                break;
            case 's':
                maxSimultaneous = atoi(optarg);
                break;
            case 't':
                timeLimitSeconds = atof(optarg);
                break;
            case 'i':
                launchIntervalSeconds = atof(optarg);
                break;
            case 'f':
                logFileName = optarg;
                break;
            default:
                print_help();
                return 1;
        }
    }

    if (totalToLaunch < 1) {
        totalToLaunch = 1;
    }
    if (totalToLaunch > MAX_TOTAL_PROCESSES) {
        totalToLaunch = MAX_TOTAL_PROCESSES;
    }
    if (maxSimultaneous < 1) {
        maxSimultaneous = 1;
    }
    if (maxSimultaneous > MAX_PCBS) {
        maxSimultaneous = MAX_PCBS;
    }
    if (timeLimitSeconds <= 0.0) {
        timeLimitSeconds = 1.0;
    }
    if (launchIntervalSeconds < 0.0) {
        launchIntervalSeconds = 0.0;
    }

    logFile = fopen(logFileName, "w");
    if (logFile == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    memset(processTable, 0, sizeof(processTable));

    readyQueue.front = 0;
    readyQueue.rear = 0;
    readyQueue.size = 0;

    srand((unsigned int)(time(NULL) ^ getpid()));
    signal(SIGALRM, signal_handler);

    shmId = shmget(SHM_KEY, sizeof(MyClock), IPC_CREAT | 0666);
    if (shmId < 0) {
        fprintf(stderr,"Parent:... Error in shmget\n");
        exit(1);
    }

    myClock = (MyClock *)shmat(shmId, NULL, 0);
    if (myClock == (void *)-1) {
        perror("oss shmat");
        exit(1);
    }


    // Create one shared message queue before any child is launched.
    if ((msgId = msgget(MSG_KEY, IPC_CREAT | 0666)) == -1) {
        perror("msgget in oss");
        shmdt(myClock);
        exit(1);
    }

    myClock->seconds = 0;
    myClock->nanoseconds = 0;

    nextLaunchTimeNs = 0ULL;

    alarm(3);

    while (!shutdownRequested && (finishedCount < totalToLaunch || activeCount > 0)) {
        maybe_dump_state();

        // Only launch when both the simulated launch time and concurrency limit allow it
        if (launchedCount < totalToLaunch && activeCount < maxSimultaneous && sim_time_ns() >= nextLaunchTimeNs) {
            if (launch_child() == -1) {
                shutdownRequested = 1;
                break;
            }
        }

        // Blocked processes are simple timed events, so scanning the table is enough here
        for (int i = 0; i < MAX_PCBS; i++) {
            PCB *pcb = &processTable[i];
            if (!pcb->occupied || !pcb->blocked) {
                continue;
            }

            if (compare_time(pcb->eventWaitSec, pcb->eventWaitNano, myClock->seconds, myClock->nanoseconds) <= 0) {
                // Once the event time arrives, the process is eligible for the ready queue again
                pcb->blocked = 0;
                pcb->lastReadySeconds = myClock->seconds;
                pcb->lastReadyNanoseconds = myClock->nanoseconds;
                queue_push(&readyQueue, i);
                log_both("OSS: Process PID %d completed I/O and moved to ready queue at time %u:%u\n", pcb->localPid, myClock->seconds, myClock->nanoseconds);
                advance_clock(UNBLOCK_OVERHEAD_NS, false);
                maybe_dump_state();
            }
        }

        if (readyQueue.size != 0) {
            int pcbIndex = queue_pop(&readyQueue);
            dispatch_process(pcbIndex);
        } else {
            // If nothing is ready, advance simulated time to the next interesting event
            unsigned long long nowNs = sim_time_ns();
            unsigned long long wakeNs = next_unblock_time_ns();
            unsigned long long targetNs = 0ULL;

            // With no ready work, only a future launch or unblock can change the system state
            if (launchedCount < totalToLaunch && activeCount < maxSimultaneous) {
                targetNs = nextLaunchTimeNs;
            }
            if (wakeNs != 0ULL && (targetNs == 0ULL || wakeNs < targetNs)) {
                targetNs = wakeNs;
            }
            if (targetNs == 0ULL || targetNs <= nowNs) {
                advance_clock(IDLE_TICK_NS, false);
            } else {
                advance_clock(targetNs - nowNs, false);
            }
            maybe_dump_state();
        }
    }

    if (shutdownRequested) {
        log_both("OSS: Shutdown requested before normal completion at time %u:%u\n", myClock->seconds, myClock->nanoseconds);
    }

    unsigned long long totalTimeNs = sim_time_ns();
    double utilization = 0.0;

    if (totalTimeNs > 0ULL) {
        utilization = ((double)processCpuTimeNs / (double)totalTimeNs) * 100.0;
    }

    log_both("OSS: Final report\n");
    log_both("OSS: Total simulated time: %u:%u\n", myClock->seconds, myClock->nanoseconds);
    log_both("OSS: Process CPU time: %llu ns\n", processCpuTimeNs);
    log_both("OSS: OSS overhead/idle time: %llu ns\n", ossOverheadNs);
    log_both("OSS: Average CPU utilization: %.2f%%\n", utilization);
    log_both("OSS: Processes launched: %d, finished: %d\n", launchedCount, finishedCount);

    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }

    msgctl(msgId, IPC_RMID, NULL);
    shmdt(myClock);
    myClock = 0;
    shmctl(shmId, IPC_RMID, NULL);

    if (logFile != NULL && logFile != stdout) {
        fclose(logFile);
        logFile = NULL;
    }

    return shutdownRequested ? EXIT_FAILURE : EXIT_SUCCESS;
}
