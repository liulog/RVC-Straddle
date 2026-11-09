CROSS_COMPILE ?= riscv64-unknown-elf-
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
QEMU ?= qemu-system-riscv64
QEMU_TIMEOUT ?= 30s
LOG ?= serial.log
CC = $(CROSS_COMPILE)gcc
CFLAGS = -march=rv64gc_zicsr_zifencei -mabi=lp64d -mcmodel=medany \
         -O2 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin \
         -fno-stack-protector -fno-pie -fno-common -msmall-data-limit=0
LDFLAGS = -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none \
          -Wl,-T,link.ld -Wl,-Map,straddle.map

.PHONY: all run qemu
all: straddle.elf
straddle.elf: start.S main.c link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) start.S main.c -o $@
run: straddle.elf
	$(MAKE) qemu

# Run an existing ELF without implicitly compiling. Require the new experiment's
# completion marker so an older ELF cannot be mistaken for a successful test.
qemu:
	test -f straddle.elf
	timeout $(QEMU_TIMEOUT) $(QEMU) -machine virt -cpu rv64 -m 128M -smp 1 \
		-nographic -bios default -kernel straddle.elf 2>&1 | tee "$(LOG)"
	! grep -q '^FAIL:' "$(LOG)"
	grep -Fq 'ALL PASS: cross-page, remap-only CSR, and middle-entry satp' "$(LOG)"
