#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>


#define SYS_subscribe  550

#define TOPIC_ID   42
#define BUF_SIZE  256

static volatile int running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0; 
}

int main(void)
{
    char          buf[BUF_SIZE]; 
    unsigned long seq = 0;   //cursor points to first message published
    long          ret;

    //register SIGINT so Ctrl-C breaks out of the subscribe call (in a blocking state otherwise)
    signal(SIGINT, sigint_handler);
    printf("subscriber: listening on topic %d (pid %d), press Ctrl-C to stop\n",
           TOPIC_ID, (int)getpid());

    while (running) {
        memset(buf, 0, sizeof(buf));

        ret = syscall(SYS_subscribe, TOPIC_ID, buf, BUF_SIZE - 1, &seq);

        if (ret < 0) {
            if (errno == EINTR) {
                printf("\nsubscriber: interrupted, exiting\n"); //for Ctrl+C
                break;
            }
            if (errno == ENOENT) {
                //publisher deleted the topic
                printf("subscriber: topic deleted by publisher, exiting\n");
                break;
            }
            //other errors
            perror("error!");
            break;
        }

        //ret is the number of bytes received; buf is already null-terminated
        printf("subscriber: [seq %lu] \"%.*s\"\n", seq, (int)ret, buf);
    }

    return 0;
}
