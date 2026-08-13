K = 64
N = 128
ITERATIONS = 1000

input_values = [1.0] * K
weights = [0.5] * (K * N)
bias = [1.0] * N

for _ in range(ITERATIONS):
    output = [0.0] * N
    for j in range(N):
        total = bias[j]
        for k in range(K):
            total += input_values[k] * weights[k * N + j]
        output[j] = total

assert abs(sum(output) - 4224.0) < 1e-5
