#include <stdint.h>
#include <stdio.h>

extern int64_t sum_owning(void);
extern int64_t sum_owning_param(void);
extern int64_t sum_result_owning(void);
extern int64_t sum_owning_none(void);
extern int64_t sum_nested_owning(void);

int main(void) {
    int64_t a = sum_owning();
    int64_t b = sum_owning_param();
    int64_t c = sum_result_owning();
    int64_t d = sum_owning_none();
    int64_t e = sum_nested_owning();
    printf("native owning sums: %lld %lld %lld %lld %lld\n",
           (long long)a, (long long)b, (long long)c, (long long)d,
           (long long)e);
    return a == 45 && b == 11 && c == 4 && d == 0 && e == 2 ? 0 : 1;
}
