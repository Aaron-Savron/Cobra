#include <assert.h>
#include <stddef.h>

enum { COUNT = 16384, ITERATIONS = 1500 };

int main(void) {
    float values[COUNT];
    for (size_t i = 0; i < COUNT; i++) values[i] = 2.0f;

    float total = 0.0f;
    float average = 0.0f;
    float peak = 0.0f;
    for (int iteration = 0; iteration < ITERATIONS; iteration++) {
        total = 0.0f;
        peak = values[0];
        for (size_t i = 0; i < COUNT; i++) {
            total += values[i];
            if (values[i] > peak) peak = values[i];
        }
        average = total / COUNT;
    }

    assert(total == 32768.0f);
    assert(average == 2.0f);
    assert(peak == 2.0f);
    return 0;
}
