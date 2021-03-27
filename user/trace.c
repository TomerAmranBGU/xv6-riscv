#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main(int argc, char** argv){
    fprintf(2, "process: %d trace was called\n", getpid());
    int pid = fork();
    if ( pid > 0){ //parent
        fprintf(2,"forked new process pid: %d\n", pid);
        wait(&pid);
    }
    else { // child
        trace(1 << 13 | 1<<6, getpid());
        kill(-1);
        kill(-2);
        sleep(3);
        exit(0);
    }
    exit(0);
}