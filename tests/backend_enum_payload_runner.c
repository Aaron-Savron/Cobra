#include <stdint.h>
#include <stdio.h>

extern int64_t circle_case(void);
extern int64_t rect_case(void);
extern int64_t line_case(void);
extern int64_t call_param(void);
extern int64_t else_case(void);

int main(void) {
    int64_t a = circle_case();
    int64_t b = rect_case();
    int64_t c = line_case();
    int64_t d = call_param();
    int64_t e = else_case();
    printf("native enum payload: %lld %lld %lld %lld %lld\n",
           (long long)a, (long long)b, (long long)c, (long long)d, (long long)e);
    return a == 100 && b == 7 && c == 300 && d == 100 && e == 777 ? 0 : 1;
}
