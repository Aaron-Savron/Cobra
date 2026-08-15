#include <stdint.h>
#include <stdio.h>

extern int64_t object_main4(void);

int main(void) {
    int64_t result = object_main4();
    printf("native object emitter 4: %lld\n", (long long)result);
    return result == 63 ? 0 : 1;
}
