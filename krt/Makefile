# perl5-mt kernel runtime adapter: standalone build and tests.
#
#   make -C krt test              build libkrt.a and run the C tests
#   make -C krt test SAN=thread   same under ThreadSanitizer
#   make -C krt test SAN=address  same under AddressSanitizer
#
# The perl build itself compiles these sources directly (see Makefile.SH);
# this Makefile exists for the unit tests and for hacking on krt alone.

CC      ?= cc
CFLAGS  ?= -O2 -g
SAN     ?= none

KRT_CFLAGS = -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
             -D_GNU_SOURCE -D_DARWIN_C_SOURCE $(CFLAGS)
ifeq ($(SAN),none)
SANFLAGS =
else
SANFLAGS = -fsanitize=$(SAN) -fno-omit-frame-pointer
endif
BUILD   = build/$(SAN)

ifneq ($(SAN),none)
KRT_CFLAGS += -DKRT_SAN=1
endif

SRCS    = krt_linux.c krt_darwin.c krt_aix.c krt_zos.c krt_stubs.c
HDRS    = krt.h krt_ring.h krt_atomic.h krt_internal.h \
          krt_wait.h krt_wait_tier.h krt_vec.h
OBJS    = $(SRCS:%.c=$(BUILD)/%.o)
TESTS   = t_shm t_ring_spsc t_ring_mpsc t_ring_xproc t_comp t_futex t_aio t_notify t_key \
          t_wait_line t_vec t_vec_xproc t_platform_abi
TESTBIN = $(TESTS:%=$(BUILD)/%)

all: $(BUILD)/libkrt.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c $(HDRS) | $(BUILD)
	$(CC) $(KRT_CFLAGS) $(SANFLAGS) -c $< -o $@

$(BUILD)/libkrt.a: $(OBJS)
	rm -f $@
	ar rcs $@ $(OBJS)

$(BUILD)/t_%: t_%.c krt_test.h $(HDRS) $(BUILD)/libkrt.a
	$(CC) $(KRT_CFLAGS) $(SANFLAGS) -o $@ $< $(BUILD)/libkrt.a -lpthread

test: $(TESTBIN)
	@fail=0; for t in $(TESTBIN); do \
	    echo "# $$t"; \
	    if ! $$t; then fail=1; fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "krt: FAILED"; exit 1; fi; \
	echo "krt: all tests passed ($(SAN))"

test-tsan:
	$(MAKE) test SAN=thread

test-asan:
	$(MAKE) test SAN=address

clean:
	rm -rf build

.PHONY: all test test-tsan test-asan clean
