CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
FFS := native/filesystems/ffs
CORE := $(FFS)/core/ffs_core.c $(FFS)/core/ffs_primitives.c
MODULE := $(FFS)/module/ffs_module.c
INC := -I$(FFS)/core -I$(FFS)/module
OUT ?= build

.PHONY: all test clean
all: $(OUT)/FFS.so
$(OUT):
	mkdir -p "$(OUT)"

$(OUT)/FFS.so: $(MODULE) $(CORE) | $(OUT)
	$(CC) $(CFLAGS) -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fvisibility=hidden -nostdlib -shared -Wl,-z,defs -Wl,-z,relro -Wl,-z,now $(INC) $^ -o "$@"

$(OUT)/ffs-module-test: tests/ffs_dropin_module_test.c $(MODULE) $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

$(OUT)/ffs-core-test: tests/ffs_core_test.c $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

$(OUT)/ffs-primitives-test: tests/ffs_primitives_test.c $(FFS)/core/ffs_primitives.c | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

test: $(OUT)/FFS.so $(OUT)/ffs-module-test $(OUT)/ffs-core-test $(OUT)/ffs-primitives-test
	$(OUT)/ffs-module-test
	$(OUT)/ffs-core-test
	$(OUT)/ffs-primitives-test
	readelf -Ws $(OUT)/FFS.so | grep -F 'infiltrator_filesystem_module_v1'
	readelf -Ws $(OUT)/FFS.so | grep -F 'infiltrator_filesystem_module_v2'
	readelf -p .ifsmeta $(OUT)/FFS.so | grep -F 'IFS-MODULE:ffs:1'
	readelf -p .ifsmeta $(OUT)/FFS.so | grep -F 'IFS-MODULE:ffs:2'
	! readelf -d $(OUT)/FFS.so | grep -q '[(]NEEDED[)]'

clean:
	rm -rf -- "$(OUT)"
