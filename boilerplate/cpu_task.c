//cpu_task.c
#include <stdio.h>

int main() {
    volatile long long j = 0;
    // Massive loop to keep the CPU busy for a few seconds
    for (long long i = 0; i < 2500000000ULL; i++) {
        j += i;
    }
    printf("Task completed.\n");
    return 0;
}