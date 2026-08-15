#include <stdint.h>
#include <stdio.h>

extern int64_t struct_array_main(void);
extern int64_t call_points(void);
extern int64_t return_points(void);
extern int64_t board_main(void);

int main(void) {
    int64_t direct = struct_array_main();
    int64_t called = call_points();
    int64_t returned = return_points();
    int64_t board = board_main();
    printf("native struct arrays: %lld %lld %lld %lld\n",
           (long long)direct, (long long)called, (long long)returned, (long long)board);
    return direct == 12 && called == 7 && returned == 14 && board == 23 ? 0 : 1;
}
