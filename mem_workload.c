#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main() {
    size_t chunk = 5 * 1024 * 1024;
    size_t total = 0;
    while (1) {
        void *p = malloc(chunk);
        if (!p) { printf("malloc failed at %zu MiB\n", total/1024/1024); break; }
        memset(p, 1, chunk);
        total += chunk;
        printf("allocated %zu MiB\n", total/1024/1024);
        fflush(stdout);
        sleep(1);
    }
    return 0;
}
