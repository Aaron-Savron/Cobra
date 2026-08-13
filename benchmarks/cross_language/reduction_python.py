COUNT = 16384
ITERATIONS = 1500
values = [2.0] * COUNT

for _ in range(ITERATIONS):
    total = sum(values)
    average = total / COUNT
    peak = max(values)

assert total == 32768.0
assert average == 2.0
assert peak == 2.0
