# SPDX-License-Identifier: GPL-3.0-or-later
# The only shipped filesystem implementation is the Common V2 FFS.so.
# Source-native and historical worker regressions run as test-only targets.
.PHONY: all test clean
all:
	cmake -S . -B build/cmake-unified -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
	cmake --build build/cmake-unified --parallel 2
test: all
	ctest --test-dir build/cmake-unified --output-on-failure
clean:
	rm -rf build/cmake-unified
