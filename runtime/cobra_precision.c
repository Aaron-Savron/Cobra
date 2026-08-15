/*
 * Mixed-precision storage: IEEE 754 binary16 (fp16) packing for f32
 * buffers. This is deliberately storage-only, not compute - GPU kernels
 * still run in fp32 (no GL_EXT_shader_16bit_storage / explicit-types
 * dependency, no driver-support risk). The value is halving memory/disk
 * footprint for trained weights: pack before gpu_upload_f32/
 * fs_write_file_bytes_checked, unpack after fs_read/gpu_download.
 *
 * Cobra has no bitwise operators (no <<, >>, &, |), so this can't be
 * written as ordinary Cobra source - it has to be a runtime primitive.
 */
#include <stdint.h>
#include <string.h>

static uint16_t cobra_f32_to_f16_bits(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;

    if (exp <= 0) {
        /* Underflow to zero (denormal fp16 range not supported - values this
           small are rare for trained weights and rounding to zero is a
           safe, well-defined fallback rather than a silent corruption). */
        return (uint16_t)sign;
    }
    if (exp >= 31) {
        /* Overflow to fp16 infinity, preserving sign. */
        return (uint16_t)(sign | 0x7C00u);
    }
    /* Round-to-nearest-even on the truncated mantissa bits. */
    uint32_t half_mant = mant >> 13;
    uint32_t round_bit = (mant >> 12) & 1u;
    uint32_t sticky = mant & 0xFFFu;
    if (round_bit && (sticky || (half_mant & 1u))) {
        half_mant++;
        if (half_mant == 0x400u) { half_mant = 0; exp++; if (exp >= 31) return (uint16_t)(sign | 0x7C00u); }
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | half_mant);
}

static float cobra_f16_bits_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign; /* +/-0 */
        } else {
            /* Denormal fp16 -> normal f32: normalize by shifting until the
               implicit leading bit appears. */
            int32_t e = -1;
            do { e++; mant <<= 1; } while (!(mant & 0x400u));
            mant &= 0x3FFu;
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13); /* inf/nan */
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* Packs `count` f32 values from `src` into `dst_bytes` as `count` fp16
   values (2 bytes each, little-endian). `dst_bytes` must hold at least
   count*2 bytes. Always succeeds (every f32 value maps to some fp16 value,
   including +/-inf and 0 for out-of-range magnitudes); returns count for a
   uniform "bytes written" convention with the write-side fs_* functions. */
int64_t cobra_pack_f16(const float *src, uint8_t *dst_bytes, int64_t count) {
    if (count < 0) return 0;
    for (int64_t i = 0; i < count; i++) {
        uint16_t h = cobra_f32_to_f16_bits(src[i]);
        dst_bytes[i * 2 + 0] = (uint8_t)(h & 0xFFu);
        dst_bytes[i * 2 + 1] = (uint8_t)((h >> 8) & 0xFFu);
    }
    return count;
}

/* Unpacks `count` fp16 values (2 bytes each, little-endian) from
   `src_bytes` into `dst` as f32. Returns count. */
int64_t cobra_unpack_f16(const uint8_t *src_bytes, float *dst, int64_t count) {
    if (count < 0) return 0;
    for (int64_t i = 0; i < count; i++) {
        uint16_t h = (uint16_t)(src_bytes[i * 2 + 0] | ((uint16_t)src_bytes[i * 2 + 1] << 8));
        dst[i] = cobra_f16_bits_to_f32(h);
    }
    return count;
}
