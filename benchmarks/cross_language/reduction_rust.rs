const COUNT: usize = 16384;
const ITERATIONS: usize = 1500;

fn main() {
    let values = [2.0_f32; COUNT];
    let mut total = 0.0_f32;
    let mut average = 0.0_f32;
    let mut peak = 0.0_f32;
    for _ in 0..ITERATIONS {
        total = values.iter().sum();
        peak = values.iter().copied().fold(values[0], f32::max);
        average = total / COUNT as f32;
    }
    assert_eq!(total, 32768.0);
    assert_eq!(average, 2.0);
    assert_eq!(peak, 2.0);
}
