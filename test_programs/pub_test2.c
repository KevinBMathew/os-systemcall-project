#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>

#define SYS_create_topic  548
#define SYS_publish       551
#define SYS_delete_topic  549

#define INTERVAL_FREQ  1    //seconds between publishes

static volatile int running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;  //signal the main loop to stop; cleanup happens after the loop
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "use program like this: %s <topic_id>\n", argv[0]);
        return 1;
    }

    int   topic_id = atoi(argv[1]);
    pid_t pid = getpid();
    int   n   = 1;
    char  msg[256];
    long  ret;

    //create the topic before publishing anything
    ret = syscall(SYS_create_topic, topic_id);
    if (ret < 0) {
        perror("create_topic");
        return 1;
    }
    printf("publisher: topic %d created (pid %d)\n", topic_id, pid);

    //Ctrl-C triggers deletion of topic from memory
    signal(SIGINT, sigint_handler);
    printf("publisher: press Ctrl-C to stop and delete the topic\n");

    while (running) {
        snprintf(msg, sizeof(msg), "hello #%d from PID %d", n, pid);

        ret = syscall(SYS_publish, topic_id, msg, (int)strlen(msg));
        if (ret < 0) {
            perror("publish");
            break;
        }
        printf("publisher: sent \"%s\"\n", msg);
        n++;

        sleep(INTERVAL_FREQ);
    }

    //delete the topic so sleeping subscribers wake with -ENOENT and can exit
    ret = syscall(SYS_delete_topic, topic_id);
    if (ret < 0)
        perror("delete_topic");  //log the error but still exit cleanly
    else
        printf("publisher: topic %d deleted\n", topic_id);

    return 0;
}