.PHONY: build test bench bench-regress cacheline fmt fmt-check clean asan tsan

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j

test: build
	ctest --test-dir build --output-on-failure

bench: build
	./build/replay_bench --n 100000 --out bench/results/bench_local.json
	cat bench/results/bench_local.json

# Regression gate: run a 10k smoke bench and diff against the committed
# baseline. 30% drift is allowed; anything worse fails CI. The baseline
# lives at bench/results/bench_smoke_baseline.json.
bench-regress: build
	./build/replay_bench --n 10000 --algo fifo --out /tmp/bench_smoke_current.json
	python3 bench/regress.py \
	  --baseline bench/results/bench_smoke_baseline.json \
	  --current /tmp/bench_smoke_current.json \
	  --threshold 0.30

cacheline: build
	./build/cacheline_study --out bench/results/cacheline_study.json

asan:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOBFIX_ASAN=ON
	cmake --build build-asan -j
	ctest --test-dir build-asan --output-on-failure

tsan:
	cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DOBFIX_TSAN=ON
	cmake --build build-tsan -j
	ctest --test-dir build-tsan --output-on-failure

fmt:
	find src tests bench -name '*.cpp' -o -name '*.h' | xargs clang-format -i

fmt-check:
	find src tests bench -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror

clean:
	rm -rf build build-asan build-tsan
