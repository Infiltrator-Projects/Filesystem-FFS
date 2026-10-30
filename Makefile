CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
FFS := native/filesystems/ffs
CORE := $(FFS)/core/ffs_core.c $(FFS)/core/ffs_primitives.c
MODULE := $(FFS)/module/ffs_module.c
INC := -I$(FFS)/core -I$(FFS)/module
OUT ?= build

.PHONY: all legacy test clean
# The installed FFS.so MUST expose read/write, defrag/recover and Test Media.
# The older freestanding V1/V2 reader is retained separately for ABI tests.
all:
	cmake -S . -B "$(OUT)/cmake-unified" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
	cmake --build "$(OUT)/cmake-unified" --target ffs-unified --parallel 2
	mkdir -p "$(OUT)"
	cp "$(OUT)/cmake-unified/FFS.so" "$(OUT)/FFS.so"

$(OUT):
	mkdir -p "$(OUT)"

legacy: $(OUT)/FFS-legacy.so
$(OUT)/FFS-legacy.so: $(MODULE) $(CORE) | $(OUT)
	$(CC) $(CFLAGS) -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fvisibility=hidden -nostdlib -shared -Wl,-z,defs -Wl,-z,relro -Wl,-z,now $(INC) $^ -o "$@"

$(OUT)/ffs-module-test: tests/ffs_dropin_module_test.c $(MODULE) $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

$(OUT)/ffs-core-test: tests/ffs_core_test.c $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

$(OUT)/ffs-primitives-test: tests/ffs_primitives_test.c $(FFS)/core/ffs_primitives.c | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

test: legacy $(OUT)/ffs-module-test $(OUT)/ffs-core-test $(OUT)/ffs-primitives-test
	$(MAKE) all
	cmake --build "$(OUT)/cmake-unified" --target ffs-fixture-test --parallel 2
	ctest --test-dir "$(OUT)/cmake-unified" --output-on-failure
	nm -D "$(OUT)/FFS.so" | grep -F 'infiltrator_filesystem_ffs_defrag_v1'
	nm -D "$(OUT)/FFS.so" | grep -F 'infiltrator_filesystem_ffs_fixture_v1'
	$(OUT)/ffs-module-test
	$(OUT)/ffs-core-test
	$(OUT)/ffs-primitives-test
	readelf -Ws $(OUT)/FFS-legacy.so | grep -F 'infiltrator_filesystem_module_v1'
	readelf -Ws $(OUT)/FFS-legacy.so | grep -F 'infiltrator_filesystem_module_v2'
	readelf -p .ifsmeta $(OUT)/FFS-legacy.so | grep -F 'IFS-MODULE:ffs:1'
	readelf -p .ifsmeta $(OUT)/FFS-legacy.so | grep -F 'IFS-MODULE:ffs:2'
	! readelf -d $(OUT)/FFS-legacy.so | grep -q '[(]NEEDED[)]'

clean:
	rm -rf -- "$(OUT)"
