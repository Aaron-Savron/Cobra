#include <stdint.h>
#include <stdio.h>

extern int64_t match_some(void);
extern int64_t match_none(void);
extern int64_t match_err(void);
extern int64_t match_owning(void);
extern int64_t match_else(void);

int main(void) {
    int64_t a = match_some();
    int64_t b = match_none();
    int64_t c = match_err();
    int64_t d = match_owning();
    int64_t e = match_else();
    printf("native sum match: %lld %lld %lld %lld %lld\n",
           (long long)a, (long long)b, (long long)c, (long long)d, (long long)e);
    return a == 8 && b == 42 && c == 50 && d == 45 && e == 3 ? 0 : 1;
}
