#include <stdint.h>
#include <stdio.h>

extern int64_t grid_main(void);

int main(void) {
    int64_t grid = grid_main();
    printf("native grid array fields: %lld\n", (long long)grid);
    return grid == 20 ? 0 : 1;
}
