#include <stdio.h>
#include <time.h>
#include <stdlib.h>
int main(int argc, char *argv[]) {
    int dur = (argc > 1) ? atoi(argv[1]) : 30;
    time_t start = time(NULL);
    volatile long x = 0;
    while (time(NULL) - start < dur) x++;
    printf("done: %ld iters in %d seconds\n", x, dur);
    return 0;
}
