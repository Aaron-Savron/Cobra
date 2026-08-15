#include <stdint.h>
#include <stdio.h>

extern int64_t circle_main(void);
extern int64_t rect_main(void);
extern int64_t tag_main(void);
extern int64_t call_tag(void);
extern int64_t slice_main(void);
extern int64_t struct_main(void);

int main(void) {
    int64_t a = circle_main();
    int64_t b = rect_main();
    int64_t c = tag_main();
    int64_t d = call_tag();
    int64_t e = slice_main();
    int64_t f = struct_main();
    printf("native owning enum: %lld %lld %lld %lld %lld %lld\n",
           (long long)a, (long long)b, (long long)c,
           (long long)d, (long long)e, (long long)f);
    return a == 100 && b == 7 && c == 4 && d == 2 && e == 60 && f == 8 ? 0 : 1;
}
