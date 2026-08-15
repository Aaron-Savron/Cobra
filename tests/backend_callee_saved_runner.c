#include <stdint.h>
#include <stdio.h>

extern int64_t stress_main(void);

int main(void) {
    int64_t stress = stress_main();
    printf("native callee-saved registers: %lld\n", (long long)stress);
    return stress == 35 ? 0 : 1;
}
