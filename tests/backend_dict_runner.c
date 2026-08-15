#include <stdint.h>
#include <stdio.h>

extern int64_t dict_ops(void);
extern int64_t dict_index(void);
extern int64_t dict_call(void);
extern int64_t dict_rehash(void);

int main(void) {
    int64_t ops = dict_ops();
    int64_t index = dict_index();
    int64_t call = dict_call();
    int64_t rehash = dict_rehash();
    printf("native dicts: %lld %lld %lld %lld\n",
           (long long)ops, (long long)index, (long long)call,
           (long long)rehash);
    return ops == 171233 && index == 1330 && call == 103 && rehash == 36
        ? 0 : 1;
}
