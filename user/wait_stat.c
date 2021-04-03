
#include "user.h"

#define PROC_COUNT 10


struct perf {
    int ctime;
    int ttime;
    int stime;
    int retime;
    int rutime;
    int average_bursttime; //average of bursstimes in 100ths (so average*100)
};



void testOnlyCPU(){
    //This function make the cpu work hard, without any reason :)
    int i, start = uptime();
    while(uptime() - start < 30 )
            for(i = 0; i < 1000; i++);
}

void testSleeperProccess(){
    //This function make the proccess sleep alot
    for(int i = 0; i < 30; i++)
            sleep(1);
}

void CPUAndSleep(){
    //This function is a mix between the two above
    for(int i = 0; i < 5; i++){
        int start = uptime();
        while(uptime() - start < 7 )
            for(i = 0; i < 1000; i++);
                sleep(1);
    }
} 


int main(int argc, char *argv[])
{
    int i, pid = 1;
    for(i = 0; pid != 0 && i < PROC_COUNT; i++)
            pid = fork();

    if(pid == 0){
        if(i%3==0)
            testOnlyCPU();
        else if(i%3==1)
            testSleeperProccess();
        else 
            CPUAndSleep();
    }
    else{
            int totalwaitTime = 0;
            int totalRunTime = 0;
            int titalTurnaroundTime = 0;
            int s = 0;
            int pid;
            int *status = &s;
            struct perf pr;
            struct perf *prf = &pr;

            for(i = 0; i < PROC_COUNT; i++)
            {
                    pid = wait_stat(status, prf);
                    totalwaitTime += prf->stime;                    
                    totalRunTime += prf->rutime;                    
                    titalTurnaroundTime += (prf->ttime - prf->ctime);

                    fprintf(2, "pid= %d, - Creation time: %d\n", pid, prf->ctime);                    
                    fprintf(2, "pid= %d, - Termination time: %d\n", pid, prf->ttime);                    
                    fprintf(2, "pid= %d, - Waiting time: %d\n", pid, prf->stime);                    
                    fprintf(2, "pid= %d, - Running time: %d\n", pid, prf->runtime);                                    
                    fprintf(2, "pid= %d, - Turnaround: %d\n", pid, (prf->ttime - prf->ctime));
                    fprintf(2, "****************************\n");
            }

            fprintf(2, "average waiting: %d\n", totalwaitTime / PROC_COUNT);		
            fprintf(2, "average running: %d\n", totalRunTime / PROC_COUNT);		
            fprintf(2, "average turnaround: %d\n", titalTurnaroundTime / PROC_COUNT);
            
    }    
    return 0;
} 