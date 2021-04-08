#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
enum procpriority {TESTHIGH, HIGH, NORMAL, LOW, TESTLOW};

int
main(int argc, char *argv[]){
    int pid = fork();
    if (pid ==0){    
        // fprintf(2,"parent set priority and: %d\n",set_priority(LOW));
        int j =0;
        for (int i =0; i<10000; i++){
            j++;
            if (j%100 ==0){
                fprintf(2,"child arrived to %d\n",j);
            }
        }
    }
    else {
        fprintf(2,"child set priority and: %d\n",set_priority(HIGH));  
        int j =0;
        for (int i =0; i<10000; i++){
            j++;
            if (j%100 ==0){
                fprintf(2,"parent arrived to %d\n",j);
            }
        }
    }
    fprintf(2,"should be -1: %d\n",set_priority(90));
    exit(0);
}