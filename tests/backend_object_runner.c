#include <stdint.h>
#include <stdio.h>

extern int64_t object_main(void);

int main(void) {
    int64_t result = object_main();
    printf("native object emitter: %lld\n", (long long)result);
    return result == 56 ? 0 : 1;
}
