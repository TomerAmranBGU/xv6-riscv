#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main(int argc, char** argv){
    fprintf(2, "process: %d trace was called\n", getpid());
    trace(1,1);
    exit(0);
}