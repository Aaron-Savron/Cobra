#include <stdint.h>
#include <stdio.h>

extern int64_t nested_struct(void);
extern int64_t nested_sum_struct(void);
extern int64_t nested_field_store(void);
extern int64_t struct_return(void);

int main(void) {
    int64_t a = nested_struct();
    int64_t b = nested_sum_struct();
    int64_t c = nested_field_store();
    int64_t d = struct_return();
    printf("native nested structs: %lld %lld %lld %lld\n",
           (long long)a, (long long)b, (long long)c, (long long)d);
    return a == 10 && b == 8 && c == 3 && d == 3 ? 0 : 1;
}
