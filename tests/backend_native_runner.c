#include <stdint.h>
#include <stdio.h>

extern int64_t buffer_main(void);
extern int64_t u8_main(void);
extern int64_t owned_main(void);
extern int64_t region_value(void);
extern int64_t string_main(void);
extern int64_t call_owned(void);
extern int64_t return_owned(void);
extern int64_t roundtrip_owned(void);
extern int64_t option_call(void);
extern int64_t owning_struct(void);
extern int64_t result_owned(void);
extern int64_t struct_return(void);

int main(void) {
    int64_t buffer = buffer_main();
    int64_t u8 = u8_main();
    int64_t owned = owned_main();
    int64_t region = region_value();
    int64_t string = string_main();
    int64_t called = call_owned();
    int64_t returned = return_owned();
    int64_t roundtrip = roundtrip_owned();
    int64_t option = option_call();
    int64_t aggregate = owning_struct();
    int64_t result = result_owned();
    int64_t returned_struct = struct_return();
    printf("native owned values: %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n",
           (long long)buffer, (long long)u8, (long long)owned, (long long)region, (long long)string,
           (long long)called, (long long)returned, (long long)roundtrip,
           (long long)option, (long long)aggregate, (long long)result,
           (long long)returned_struct);
    return buffer == 42 && u8 == 42 && owned == 42 && region == 42 && string == 5 &&
           called == 42 && returned == 42 && roundtrip == 42 &&
           option == 42 && aggregate == 42 && result == 42 &&
           returned_struct == 42 ? 0 : 1;
}
