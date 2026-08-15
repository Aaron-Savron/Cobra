#include <stdint.h>
#include <stdio.h>

extern int64_t nested_main(void);
extern int64_t matrix_main(void);

int main(void) {
    int64_t nested = nested_main();
    int64_t matrix = matrix_main();
    printf("native nested arrays: %lld %lld\n", (long long)nested, (long long)matrix);
    return nested == 6 && matrix == 13 ? 0 : 1;
}
