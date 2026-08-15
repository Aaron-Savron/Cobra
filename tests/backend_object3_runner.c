#include <stdint.h>
#include <stdio.h>

extern int64_t object_main3(void);

int main(void) {
    int64_t result = object_main3();
    printf("native object emitter 3: %lld\n", (long long)result);
    return result == 38 ? 0 : 1;
}
