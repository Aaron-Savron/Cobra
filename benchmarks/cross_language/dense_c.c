#include <assert.h>
#include <immintrin.h>
#include <stddef.h>

enum { K = 64, N = 128, ITERATIONS = 1000 };

static void dense(const float *input, const float *weights, const float *bias, float *output) {
    for (size_t j = 0; j < N; j += 8) {
        __m256 acc = _mm256_loadu_ps(bias + j);
        for (size_t k = 0; k < K; k++) {
            __m256 x = _mm256_set1_ps(input[k]);
            __m256 w = _mm256_loadu_ps(weights + k * N + j);
            acc = _mm256_fmadd_ps(x, w, acc);
        }
        _mm256_storeu_ps(output + j, acc);
    }
}

int main(void) {
    float input[K], weights[K * N], bias[N], output[N];
    for (size_t i = 0; i < K; i++) input[i] = 1.0f;
    for (size_t i = 0; i < K * N; i++) weights[i] = 0.5f;
    for (size_t i = 0; i < N; i++) bias[i] = 1.0f;

    for (int i = 0; i < ITERATIONS; i++) dense(input, weights, bias, output);

    float total = 0.0f;
    for (size_t i = 0; i < N; i++) total += output[i];
    assert(total == 4224.0f);
    return 0;
}
