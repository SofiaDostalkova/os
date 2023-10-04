#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]){
  int n;
  n = atoi(argv[1]);
  int result = square(n);
  printf("The square of %d is %d\n", n, result);
  exit(0);
}
