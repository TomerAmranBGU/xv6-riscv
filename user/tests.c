#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
int
main(int argc, char *argv[]){
    int j =0;
    for (int i =0; i<1000000000; i++){
        j++;
    }
    fprintf(2,"finished\n");
    exit(0);
}