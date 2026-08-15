#include <stdint.h>
#include <stdio.h>

extern int64_t object_main2(void);

int main(void) {
    int64_t result = object_main2();
    printf("native object emitter 2: %lld\n", (long long)result);
    return result == 135 ? 0 : 1;
}
