#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
//#include <stdlib.h>

struct perf {
  int ctime;
  int ttime;
  int stime;
  int retime;
  int rutime;
  int average_bursttime;
};

int main(int argc, char** argv){


    int pid = fork();
    struct perf performance = {0,0,0,0,0,0}; 
    int status;
    if (pid ==0){
        sleep(30);
    }
    else{
        int ans = wait_stat(&status,&performance);
        fprintf(2, "wait ans: %d\n", ans);
        fprintf(2,"%d %d %d %d %d ", performance.ctime, performance.ttime, performance.stime, performance.rutime, performance.retime);
        fprintf(2,"parent\n");
    }
    exit(0);
}