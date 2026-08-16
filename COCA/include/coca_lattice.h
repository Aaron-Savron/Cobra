/*
 * COCA Hyperdimensional Phase Space & AVX2 Lattice (coca_lattice.h)
 *
 * Implements:
 *   1. 2,048-D Complex Torus Phase Manifold (T^2048) for AST & Code Invariants
 *   2. AVX2 Vectorized Hermitian Inner Product & Cosine Similarity
 *   3. Element-wise Complex Phase Binding (u (*) v) & Conjugate Unbinding (u (*) v^\dagger)
 *   4. L2 Unitary Phase Projection & Normalization (No Magnitude Explosion)
 *
 * Strict 32-byte memory alignment, C11 native, zero external dependencies.
 */

#ifndef COCA_LATTICE_H
#define COCA_LATTICE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <immintrin.h>

#define COCA_HD_DIM 2048

typedef struct {
    float real[COCA_HD_DIM] __attribute__((aligned(32)));
    float imag[COCA_HD_DIM] __attribute__((aligned(32)));
} coca_phasor_t;

// Generates a deterministic unitary complex phasor on T^D from a hash seed
static inline void coca_phasor_from_seed(coca_phasor_t* p, uint64_t seed) {
    uint64_t state = seed != 0 ? seed : 0x853c49e6748fea9bULL;
    for (int i = 0; i < COCA_HD_DIM; i++) {
        // xorshift64*
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        uint64_t val = state * 0x2545F4914F6CDD1DULL;
        float theta = ((float)(val & 0xFFFFFFFF) / (float)0xFFFFFFFF) * 2.0f * (float)M_PI - (float)M_PI;
        p->real[i] = cosf(theta);
        p->imag[i] = sinf(theta);
    }
}

// AVX2 Complex L2 Normalization (Automatic Gain Control & Phase Projection)
static inline void coca_phasor_normalize(coca_phasor_t* p) {
    __m256 v_sum = _mm256_setzero_ps();
    for (int i = 0; i < COCA_HD_DIM; i += 8) {
        __m256 r = _mm256_loadu_ps(&p->real[i]);
        __m256 im = _mm256_loadu_ps(&p->imag[i]);
        v_sum = _mm256_fmadd_ps(r, r, _mm256_fmadd_ps(im, im, v_sum));
    }
    float tmp[8];
    _mm256_storeu_ps(tmp, v_sum);
    float total_sq = 0.0f;
    for (int k = 0; k < 8; k++) total_sq += tmp[k];

    if (total_sq < 1e-12f) return;
    float inv_mag = 1.0f / sqrtf(total_sq / (float)COCA_HD_DIM);
    __m256 v_inv = _mm256_set1_ps(inv_mag);

    for (int i = 0; i < COCA_HD_DIM; i += 8) {
        __m256 r = _mm256_loadu_ps(&p->real[i]);
        __m256 im = _mm256_loadu_ps(&p->imag[i]);
        _mm256_storeu_ps(&p->real[i], _mm256_mul_ps(r, v_inv));
        _mm256_storeu_ps(&p->imag[i], _mm256_mul_ps(im, v_inv));
    }
}

// AVX2 Vectorized Hermitian Similarity: Re<u, v> / D
static inline float coca_phasor_similarity(const coca_phasor_t* u, const coca_phasor_t* v) {
    __m256 v_sum = _mm256_setzero_ps();
    for (int i = 0; i < COCA_HD_DIM; i += 8) {
        __m256 ur = _mm256_loadu_ps(&u->real[i]);
        __m256 ui = _mm256_loadu_ps(&u->imag[i]);
        __m256 vr = _mm256_loadu_ps(&v->real[i]);
        __m256 vi = _mm256_loadu_ps(&v->imag[i]);

        __m256 real_prod = _mm256_mul_ps(ur, vr);
        __m256 imag_prod = _mm256_mul_ps(ui, vi);
        v_sum = _mm256_add_ps(v_sum, _mm256_add_ps(real_prod, imag_prod));
    }
    float tmp[8];
    _mm256_storeu_ps(tmp, v_sum);
    float total = 0.0f;
    for (int k = 0; k < 8; k++) total += tmp[k];
    return total / (float)COCA_HD_DIM;
}

// AVX2 Complex Element-wise Hadamard Binding: out = u (*) v
static inline void coca_phasor_bind(
    const coca_phasor_t* u,
    const coca_phasor_t* v,
    coca_phasor_t* out
) {
    for (int i = 0; i < COCA_HD_DIM; i += 8) {
        __m256 ur = _mm256_loadu_ps(&u->real[i]);
        __m256 ui = _mm256_loadu_ps(&u->imag[i]);
        __m256 vr = _mm256_loadu_ps(&v->real[i]);
        __m256 vi = _mm256_loadu_ps(&v->imag[i]);

        __m256 out_r = _mm256_fmsub_ps(ur, vr, _mm256_mul_ps(ui, vi));
        __m256 out_i = _mm256_fmadd_ps(ur, vi, _mm256_mul_ps(ui, vr));

        _mm256_storeu_ps(&out->real[i], out_r);
        _mm256_storeu_ps(&out->imag[i], out_i);
    }
}

// AVX2 Complex Conjugate Unbinding: out = u (*) v^\dagger
static inline void coca_phasor_unbind(
    const coca_phasor_t* u,
    const coca_phasor_t* v,
    coca_phasor_t* out
) {
    for (int i = 0; i < COCA_HD_DIM; i += 8) {
        __m256 ur = _mm256_loadu_ps(&u->real[i]);
        __m256 ui = _mm256_loadu_ps(&u->imag[i]);
        __m256 vr = _mm256_loadu_ps(&v->real[i]);
        __m256 vi = _mm256_loadu_ps(&v->imag[i]);

        __m256 out_r = _mm256_fmadd_ps(ur, vr, _mm256_mul_ps(ui, vi));
        __m256 out_i = _mm256_fmsub_ps(ui, vr, _mm256_mul_ps(ur, vi));

        _mm256_storeu_ps(&out->real[i], out_r);
        _mm256_storeu_ps(&out->imag[i], out_i);
    }
}

// Subword/Identifier FNV-1a Hash to 2,048-D Continuous Phase Vector
static inline void coca_encode_identifier(const char* ident, coca_phasor_t* out_p) {
    if (!ident || !out_p) return;
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; ident[i]; i++) {
        hash ^= (uint64_t)(unsigned char)ident[i];
        hash *= 1099511628211ULL;
    }
    coca_phasor_from_seed(out_p, hash);
}

#endif // COCA_LATTICE_H
