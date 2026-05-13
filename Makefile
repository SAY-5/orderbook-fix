.PHONY: build test bench fmt fmt-check clean asan tsan

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j

test: build
	ctest --test-dir build --output-on-failure

bench: build
	./build/replay_bench --n 100000 --out bench/results/bench_local.json
	cat bench/results/bench_local.json

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
