#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <string.h>
#include <signal.h>
#include <sys/msg.h>
#include <time.h>
#define MAX 18

int bitmap[1];
int activeChildren = 0;
int clockShmid;
int clockSem;
int pcbShmid;
int msgId;
FILE* logFile = NULL;

struct sembuf sem;

struct Frame
{
    int used;
    int dirtyBit;
    int secondChance;
    int process;
    int page;
    long time;
};

struct Page
{
    int location;
    int valid;
    int dirtyBit;
};

struct PCB
{
    int simPid;
    int processPid;
    struct Page pageTable[32];
};
struct PCB* sharedPcb;

struct Clock
{
    long seconds;
    long nanoSeconds;
};
struct Clock* sharedClock;

struct message
{
    long mtype;
    int simPid;
    int terminated;
    int request;
    int write;
    char message[1];
};
struct message messageSend;
struct message messageReceive;

struct queue
{
    struct node* first;
    struct node* last;
};

struct node
{
    int pid;
    struct node* next;
};

void ctrlc_handler(int signum)
{
    fprintf(stderr, "\n^C interrupt received.\n");
    shmctl(pcbShmid, IPC_RMID, NULL);
    shmctl(clockShmid, IPC_RMID, NULL);
    semctl(clockSem, 0, IPC_RMID);
    msgctl(msgId, IPC_RMID, NULL);
    kill(0, SIGKILL);
}

void timer_handler(int signum)
{
    fprintf(stderr, "Error: Program timed out.\n");
    shmctl(pcbShmid, IPC_RMID, NULL);
    shmctl(clockShmid, IPC_RMID, NULL);
    semctl(clockSem, 0, IPC_RMID);
    msgctl(msgId, IPC_RMID, NULL);
    kill(0, SIGKILL);
}

void createQueue(struct queue* que);
void enqueue(struct queue* que, int pid);
int dequeue(struct queue* que);
void semLock();
void semRelease();

int main(int argc, char *argv[0])
{
    int opt;
    int method = 0;
    bitmap[0] = 0;
    int totalProcesses = 0;
    long currentTime;
    long lastForkTime;
    int frameTableTime = 0;
    logFile = fopen("log.txt", "w");

    signal(SIGALRM, timer_handler);
    signal(SIGINT, ctrlc_handler);

    while ((opt = getopt(argc, argv, "hm:")) != -1)
    {
        switch (opt)
        {
            case 'h':
                printf("Usage: %s [-h][-m memory access method]\n", argv[0]);
                printf("        -m selects memory access method\n");
                exit(EXIT_FAILURE);
            case 'm':
                method = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s [-h]\n", argv[0]);
                exit(EXIT_FAILURE);
        }

    }
    if(method != 0 && method != 1)
    {
        printf("Please use either 0 or 1 for memory access method\n");
        exit(EXIT_FAILURE);
    }



    key_t msgKey = ftok("oss", 1);
    msgId = msgget(msgKey, IPC_CREAT | 0666);
    if (msgId == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    key_t semClockKey = ftok("oss", 2);                   //get a key for semaphore
    clockSem = semget(semClockKey, 1, IPC_CREAT | 0666);      //creates the shared semaphore
    if (clockSem == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    semctl(clockSem, 0, SETVAL, 1);

    key_t pcbKey = ftok("oss", 3);
    pcbShmid = shmget(pcbKey, sizeof(struct PCB) * 18, IPC_CREAT | 0666);
    if(pcbShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        semctl(clockSem, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    sharedPcb = shmat(pcbShmid, NULL, 0);
    if(sharedPcb == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    key_t clockKey = ftok("oss", 4);
    clockShmid = shmget(clockKey, sizeof(struct Clock), IPC_CREAT | 0666);
    if(clockShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    sharedClock = shmat(clockShmid, NULL, 0);
    if(sharedClock == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        shmctl(clockShmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    struct Frame frameTable[256];
    int i = 0;
    int spotFound = 0;
    int request;
    int pageRequest;
    int replaceFrame;
    int memoryAccess = 0;
    int pageFault = 0;
    long time = 0;
    struct queue* replacementQueue;
    sharedClock->seconds = 0;
    sharedClock->nanoSeconds = 0;
    replacementQueue = malloc(sizeof(struct queue));
    createQueue(replacementQueue);
    currentTime = 500;
    alarm(2);

    for(i = 0; i < 256; i++)
    {
        frameTable[i].used = 0;
        frameTable[i].dirtyBit = 0;
        frameTable[i].secondChance = 0;
        frameTable[i].time = 0;
    }

    while(totalProcesses < 50 || activeChildren > 0)
    {

        if(totalProcesses < 50 && (currentTime - lastForkTime) > (rand() % 500) + 1)
        {
            lastForkTime = currentTime;
            for(i = 0; i < MAX; i++)
            {
                waitpid(-1, NULL, WNOHANG);
                if(bitmap[0] & (1 << i))
                {
                    continue;
                }
                else
                {
                    bitmap[0] |= (1 << i);
                    sharedPcb[i].simPid = i;
                    sharedPcb[i].processPid = totalProcesses;
                    int j;
                    for(j = 0; j < 32; j++)
                    {
                        sharedPcb[i].pageTable[j].location = -1;
                        sharedPcb[i].pageTable[j].valid = 0;
                        sharedPcb[i].pageTable[j].dirtyBit = 0;
                    }
                    pid_t processPid = fork();
                    if(processPid == -1)
                    {
                        fprintf(stderr, "%s: Error: fork ", argv[0]);
                        perror("");
                        shmdt(sharedClock);
                        shmdt(sharedPcb);
                        shmctl(pcbShmid, IPC_RMID, NULL);
                        shmctl(clockShmid, IPC_RMID, NULL);
                        msgctl(msgId, IPC_RMID, NULL);
                        semctl(clockSem, 0, IPC_RMID);
                        exit(EXIT_FAILURE);
                    }

                    if(processPid == 0)
                    {
                        char processIndex[12];
                        char processMethod[12];
                        sprintf(processMethod, "%d", method);
                        sprintf(processIndex, "%d", i);
                        execl("./process", "process", processIndex, processMethod, NULL);
                        fprintf(stderr, "%s: Error: execl failed.", argv[0]);
                        perror("");
                        shmdt(sharedClock);
                        shmdt(sharedPcb);
                        shmctl(pcbShmid, IPC_RMID, NULL);
                        shmctl(clockShmid, IPC_RMID, NULL);
                        msgctl(msgId, IPC_RMID, NULL);
                        semctl(clockSem, 0, IPC_RMID);
                        exit(EXIT_FAILURE);
                    }
                    totalProcesses++;
                    activeChildren++;
                    break;
                }
            }
        }

        msgrcv(msgId, &messageReceive, sizeof(struct message), 19, 0);
        memoryAccess++;
        sharedPcb = shmat(pcbShmid, NULL, 0);
        if(messageReceive.terminated == 1)
        {
            fprintf(logFile, "OSS: Process %d terminated at %d:%d\n Clearing frames\n", messageReceive.simPid, sharedClock->seconds, sharedClock->nanoSeconds);
            for(i = 0; i < 32; i++)
            {
                if(sharedPcb[messageReceive.simPid].pageTable[i].valid == 1)
                {
                    if(sharedPcb[messageReceive.simPid].pageTable[i].dirtyBit == 1)
                    {
                        semLock();
                        sharedClock->nanoSeconds = sharedClock->nanoSeconds + 14000000;
                        if(sharedClock->nanoSeconds > 1000000000)
                        {
                            sharedClock->seconds = sharedClock->seconds + 1;
                            sharedClock->nanoSeconds = sharedClock->nanoSeconds - 1000000000;
                        }
                        semRelease();

                        fprintf(logFile, "OSS: Dirty bit was set on frame %d, adding time\n",
                                sharedPcb[messageReceive.simPid].pageTable[pageRequest].location);
                    }
                    frameTable[sharedPcb[messageReceive.simPid].pageTable[i].location].used = 0;
                    frameTable[sharedPcb[messageReceive.simPid].pageTable[i].location].dirtyBit = 0;
                }
            }
            bitmap[0] &= ~(1 << messageReceive.simPid);
            activeChildren--;
        }

        else
        {
            request = messageReceive.request;
            pageRequest = request / 1024;
            if(messageReceive.write == 1)
            {
                fprintf(logFile, "OSS: Process %d requesting write of address"
                                 " %d at %d:%d\n", sharedPcb[messageReceive.simPid].processPid, request,
                        sharedClock->seconds, sharedClock->nanoSeconds);
            }
            else
            {
                fprintf(logFile, "OSS: Process %d requesting read of address"
                                 " %d at %d:%d\n", sharedPcb[messageReceive.simPid].processPid,
                        request, sharedClock->seconds, sharedClock->nanoSeconds);

            }
            if(sharedPcb[messageReceive.simPid].pageTable[pageRequest].valid == 0)
            {
                for(i = 0; i < 256; i++)
                {
                    if(frameTable[i].used == 0)
                    {
                        frameTable[i].used = 1;
                        sharedPcb[messageReceive.simPid].pageTable[pageRequest].location = i;
                        sharedPcb[messageReceive.simPid].pageTable[pageRequest].valid = 1;
                        frameTable[i].page = pageRequest;
                        frameTable[i].process = messageReceive.simPid;
                        frameTable[i].secondChance = 1;
                        spotFound = 1;
                        enqueue(replacementQueue, i);
                        pageFault++;
                        if(messageReceive.write == 1)
                        {
                            frameTable[i].dirtyBit = 1;
                            sharedPcb[messageReceive.simPid].pageTable[pageRequest].dirtyBit = 1;
                            semLock();
                            sharedClock->nanoSeconds = sharedClock->nanoSeconds + 14000000;
                            if(sharedClock->nanoSeconds > 1000000000)
                            {
                                sharedClock->seconds = sharedClock->seconds + 1;
                                sharedClock->nanoSeconds = sharedClock->nanoSeconds - 1000000000;
                            }
                            semRelease();
                        }

                        semLock();
                        sharedClock->nanoSeconds = sharedClock->nanoSeconds + 14000000;
                        if(sharedClock->nanoSeconds > 1000000000)
                        {
                            sharedClock->seconds = sharedClock->seconds + 1;
                            sharedClock->nanoSeconds = sharedClock->nanoSeconds - 1000000000;
                        }
                        semRelease();
                        fprintf(logFile, "OSS: Address %d not in a frame, pagefault\n"
                                         "       Putting address %d in frame %d at %d:%d\n", request,
                                request, i, sharedClock->seconds, sharedClock->nanoSeconds);

                        break;
                    }
                }
                while(spotFound == 0)
                {
                    replaceFrame = dequeue(replacementQueue);
                    if(frameTable[replaceFrame].secondChance == 1)
                    {
                        frameTable[replaceFrame].secondChance = 0;
                        enqueue(replacementQueue, replaceFrame);
                    }
                    else
                    {
                        pageFault++;
                        sharedPcb[frameTable[replaceFrame].process].pageTable[frameTable[replaceFrame].page].valid = 0;
                        fprintf(logFile, "OSS: Address %d not in a frame, pagefault\n"
                                         "       Clearing frame %d and swapping in %d page %d\n",
                                request, replaceFrame, sharedPcb[messageReceive.simPid].processPid ,pageRequest);
                        if(sharedPcb[frameTable[replaceFrame].process].pageTable[frameTable[replaceFrame].page].dirtyBit == 1)
                        {
                            sharedPcb[frameTable[replaceFrame].process].pageTable[frameTable[replaceFrame].page].dirtyBit = 0;
                            frameTable[replaceFrame].dirtyBit = 0;
                            fprintf(logFile, "OSS: Dirty bit was set on frame %d, adding time\n", replaceFrame);

                            semLock();  //extra time for dirty bit
                            sharedClock->nanoSeconds = sharedClock->nanoSeconds + 14000000;
                            if(sharedClock->nanoSeconds > 1000000000)
                            {
                                sharedClock->seconds = sharedClock->seconds + 1;
                                sharedClock->nanoSeconds = sharedClock->nanoSeconds - 1000000000;
                            }
                            semRelease();
                        }
                        sharedPcb[messageReceive.simPid].pageTable[pageRequest].location = replaceFrame;
                        sharedPcb[messageReceive.simPid].pageTable[pageRequest].valid = 1;
                        frameTable[replaceFrame].page = pageRequest;
                        frameTable[replaceFrame].process = messageReceive.simPid;
                        frameTable[replaceFrame].secondChance = 1;
                        spotFound = 1;
                        enqueue(replacementQueue, replaceFrame);
                        semLock();
                        sharedClock->nanoSeconds = sharedClock->nanoSeconds + 14000000;
                        if(sharedClock->nanoSeconds > 1000000000)
                        {
                            sharedClock->seconds = sharedClock->seconds + 1;
                            sharedClock->nanoSeconds = sharedClock->nanoSeconds - 1000000000;
                        }
                        semRelease();
                        if(messageReceive.write == 1)
                        {
                            semLock();
                            sharedClock->nanoSeconds = sharedClock->nanoSeconds + 14000000;
                            if(sharedClock->nanoSeconds > 1000000000)
                            {
                                sharedClock->seconds = sharedClock->seconds + 1;
                                sharedClock->nanoSeconds = sharedClock->nanoSeconds - 1000000000;
                            }
                            semRelease();
                            frameTable[replaceFrame].dirtyBit = 1;
                            sharedPcb[messageReceive.simPid].pageTable[pageRequest].dirtyBit = 1;
                        }
                    }
                }
                spotFound = 0;
            }
            else
            {
                semLock();
                sharedClock->nanoSeconds = sharedClock->nanoSeconds + 10;
                semRelease();
                fprintf(logFile, "OSS: Address %d in frame %d, giving data at %d:%d\n", request,
                        sharedPcb[messageReceive.simPid].pageTable[pageRequest].location,
                        sharedClock->seconds, sharedClock->nanoSeconds);
            }
            messageSend.mtype = messageReceive.simPid + 1;
            msgsnd(msgId, &messageSend, sizeof(struct message), 0);
        }
        currentTime = (sharedClock->seconds * 1000000000) + sharedClock->nanoSeconds;
        if(sharedClock->seconds > frameTableTime)
        {
            frameTableTime = sharedClock->seconds;
            fprintf(logFile, "         Occupied    SecondChance   DirtyBit\n");
            for(i = 0; i < 256; i++)
            {
                fprintf(logFile, "Frame %d     %d           %d             %d\n", i, frameTable[i].used,
                        frameTable[i].secondChance, frameTable[i].dirtyBit);
            }
        }
    }
    wait();
    wait();
    wait();

    time = sharedClock->seconds * 1000000000 + sharedClock->nanoSeconds;
    printf("There were %d memory accesses\nThere were %d memory accesses per second\n", memoryAccess,
           memoryAccess/sharedClock->seconds);
    printf("There were %d page faults\n", pageFault);
    printf("The average memory access speed was %d nanoseconds\n", time/memoryAccess);


    shmctl(pcbShmid, IPC_RMID, NULL);
    shmctl(clockShmid, IPC_RMID, NULL);
    msgctl(msgId, IPC_RMID, NULL);
    semctl(clockSem, 0, IPC_RMID);
    fclose(logFile);


    return 0;
}

void createQueue(struct queue* que)
{
    que->first = NULL;
    que->last = NULL;
}

void enqueue(struct queue* que, int pid)
{
    struct node* temp;
    temp = malloc(sizeof(struct node));
    if(temp == NULL)
    {
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        shmctl(clockShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }
    temp->pid = pid;
    temp->next = NULL;

    if(que->first == NULL)
    {
        que->first = temp;
        que->last = temp;
    }

    else
    {
        que->last->next = temp;
        que->last = temp;
    }
}

int dequeue(struct queue* que)
{
    struct node* temp;
    int pid = que->first->pid;
    temp = que->first;
    que->first = que->first->next;
    free(temp);
    return(pid);
}

void semLock()
{
    sem.sem_num = 0;
    sem.sem_op = -1;
    sem.sem_flg = 0;
    semop(clockSem, &sem, 1);
}

void semRelease()
{
    sem.sem_num = 0;
    sem.sem_op = 1;
    sem.sem_flg = 0;
    semop(clockSem, &sem, 1);
}

