use std::arch::x86_64::*;

const K: usize = 64;
const N: usize = 128;
const ITERATIONS: usize = 1000;

#[target_feature(enable = "avx2,fma")]
unsafe fn dense(input: &[f32; K], weights: &[f32; K * N], bias: &[f32; N], output: &mut [f32; N]) {
    for j in (0..N).step_by(8) {
        let mut acc = _mm256_loadu_ps(bias.as_ptr().add(j));
        for k in 0..K {
            let x = _mm256_set1_ps(*input.get_unchecked(k));
            let w = _mm256_loadu_ps(weights.as_ptr().add(k * N + j));
            acc = _mm256_fmadd_ps(x, w, acc);
        }
        _mm256_storeu_ps(output.as_mut_ptr().add(j), acc);
    }
}

fn main() {
    assert!(is_x86_feature_detected!("avx2"));
    assert!(is_x86_feature_detected!("fma"));
    let input = [1.0_f32; K];
    let weights = [0.5_f32; K * N];
    let bias = [1.0_f32; N];
    let mut output = [0.0_f32; N];

    for _ in 0..ITERATIONS {
        unsafe { dense(&input, &weights, &bias, &mut output) };
    }

    let total: f32 = output.iter().sum();
    assert_eq!(total, 4224.0);
}
