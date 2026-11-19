# perl5-mt native acceleration & transparent offload library: standalone build and tests.
#
#   make -C accel test              build libperl_accel.a and run C tests
#   make -C accel test SAN=address  same under AddressSanitizer

CC      ?= cc
CFLAGS  ?= -O2 -g
SAN     ?= none
UNAME_S := $(shell uname -s)

ACCEL_CFLAGS = -std=gnu11 -Wall -Wextra -ffp-contract=off \
               -D_GNU_SOURCE -D_DARWIN_C_SOURCE $(CFLAGS)

ifeq ($(SAN),none)
SANFLAGS =
else
SANFLAGS = -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

ifeq ($(UNAME_S),Darwin)
ACCEL_LDFLAGS = -framework Accelerate -lm
else
ACCEL_LDFLAGS = -ldl -lm
endif

BUILD   = build/$(SAN)

SRCS    = perl_accel.c \
          accel_scalar.c \
          accel_darwin_accelerate.c \
          accel_metal.c \
          accel_ane.c \
          accel_mesa_opencl.c \
          accel_intel_npu.c \
          accel_amd_npu.c \
          accel_power_nx.c \
          accel_s390x_nnpa.c

HDRS    = perl_accel.h accel_internal.h
OBJS    = $(SRCS:%.c=$(BUILD)/%.o)
SIMD_OBJ = $(BUILD)/perl_simd_standalone.o

TESTS   = t_simd_probe t_accel_core t_accel_cr t_accel_opencl_filter t_accel_backends
TESTBIN = $(TESTS:%=$(BUILD)/%)

all: $(BUILD)/libperl_accel.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c $(HDRS) | $(BUILD)
	$(CC) $(ACCEL_CFLAGS) $(SANFLAGS) -c $< -o $@

$(SIMD_OBJ): ../simd/perl_simd.c ../simd/perl_simd_dispatch.h | $(BUILD)
	$(CC) $(ACCEL_CFLAGS) $(SANFLAGS) -DPERL_SIMD_STANDALONE -I../simd -c $< -o $@

$(BUILD)/libperl_accel.a: $(OBJS) $(SIMD_OBJ)
	rm -f $@
	ar rcs $@ $(OBJS) $(SIMD_OBJ)

$(BUILD)/t_%: t_%.c accel_test.h $(HDRS) $(BUILD)/libperl_accel.a
	$(CC) $(ACCEL_CFLAGS) $(SANFLAGS) -DPERL_SIMD_STANDALONE -I. -I../simd \
		-o $@ $< $(BUILD)/libperl_accel.a $(ACCEL_LDFLAGS)

test: $(TESTBIN)
	@fail=0; for t in $(TESTBIN); do \
	    echo "# $$t"; \
	    if ! $$t; then fail=1; fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "accel: FAILED"; exit 1; fi; \
	echo "accel: all tests passed ($(SAN))"

test-asan:
	$(MAKE) test SAN=address

clean:
	rm -rf build

.PHONY: all test test-asan clean
