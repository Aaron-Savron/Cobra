#include <stdint.h>
#include <stdio.h>

extern int64_t struct_buffer_main(void);
extern int64_t struct_buffer_append(void);
extern int64_t struct_buffer_write(void);
extern int64_t struct_buffer_call(void);
extern int64_t struct_buffer_roundtrip(void);

int main(void) {
    int64_t direct = struct_buffer_main();
    int64_t appended = struct_buffer_append();
    int64_t written = struct_buffer_write();
    int64_t called = struct_buffer_call();
    int64_t roundtrip = struct_buffer_roundtrip();
    printf("native struct buffers: %lld %lld %lld %lld %lld\n",
           (long long)direct, (long long)appended, (long long)written,
           (long long)called, (long long)roundtrip);
    return direct == 7 && appended == 15 && written == 19 && called == 23 &&
           roundtrip == 42 ? 0 : 1;
}
