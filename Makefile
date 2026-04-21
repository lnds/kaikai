.PHONY: all kaic0 kaic1 test test-stage0 test-stage1 selfhost clean

all: kaic1 bin/kai

kaic0:
	$(MAKE) -C stage0 kaic0

kaic1: kaic0
	$(MAKE) -C stage1 kaic1

# bin/kai is already a checked-in shell script; this target just confirms it
# is executable and visible.
bin/kai:
	@chmod +x bin/kai
	@echo "kai driver: $$(realpath bin/kai 2>/dev/null || pwd)/bin/kai"

test: test-stage0 test-stage1

test-stage0:
	$(MAKE) -C stage0 test

test-stage1:
	$(MAKE) -C stage1 test

selfhost:
	$(MAKE) -C stage1 selfhost

clean:
	$(MAKE) -C stage0 clean
	$(MAKE) -C stage1 clean
