#include <stdint.h>
#include <stdio.h>

extern int64_t sum_struct(void);
extern int64_t sum_result(void);
extern int64_t sum_none(void);
extern int64_t sum_call(void);
extern int64_t sum_err(void);
extern int64_t sum_nested(void);

int main(void) {
    int64_t a = sum_struct();
    int64_t b = sum_result();
    int64_t c = sum_none();
    int64_t d = sum_call();
    int64_t e = sum_err();
    int64_t f = sum_nested();
    printf("native struct sums: %lld %lld %lld %lld %lld %lld\n",
           (long long)a, (long long)b, (long long)c, (long long)d,
           (long long)e, (long long)f);
    return a == 340 && b == 56 && c == 0 && d == 78 && e == 77 && f == 3
        ? 0 : 1;
}
