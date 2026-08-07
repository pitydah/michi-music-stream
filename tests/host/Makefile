# Host-side unit tests for the universal firmware (F15).
#
# Compiles the REAL firmware sources (semver.c, validators.c,
# json_helpers.c) - no reimplementations. The firmware components keep
# these files on their SRCS list; the tests compile the SAME .c.
#
#   make test   - build + run every test (needs cc; test_json_helpers
#                 additionally needs libcjson-dev - CI installs it via
#                 `apt install libcjson-dev`; without it the test is
#                 skipped with a clear message)
#   make clean  - remove the test binaries

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS = -Ishim \
           -I../../firmware/components/michi_ota \
           -I../../firmware/components/michi_pairing \
           -I../../firmware/components/michi_http/include

FIRMWARE = ../../firmware/components

HAS_CJSON := $(shell pkg-config --exists libcjson && echo yes || echo no)
CJSON_CFLAGS := $(shell pkg-config --cflags libcjson 2>/dev/null)
CJSON_LIBS   := $(shell pkg-config --libs libcjson 2>/dev/null)

# Every shim header is a dependency: a shim change must rebuild all tests.
SHIMS := $(wildcard shim/*.h)

TESTS := test_semver test_pairing_validators \
         $(if $(filter yes,$(HAS_CJSON)),test_json_helpers)

.PHONY: all test clean

all: $(TESTS)

test: all
	@set -e; \
	for t in test_semver test_pairing_validators; do ./$$t; done; \
	if [ "$(HAS_CJSON)" = "yes" ]; then \
	    ./test_json_helpers; \
	else \
	    echo "SKIP test_json_helpers (libcjson-dev not installed - CI installs it)"; \
	fi

test_semver: test_semver.c $(FIRMWARE)/michi_ota/semver.c \
             $(FIRMWARE)/michi_ota/semver.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_semver.c $(FIRMWARE)/michi_ota/semver.c

test_pairing_validators: test_pairing_validators.c $(FIRMWARE)/michi_pairing/validators.c \
                         $(FIRMWARE)/michi_pairing/validators.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_pairing_validators.c \
	    $(FIRMWARE)/michi_pairing/validators.c

test_json_helpers: test_json_helpers.c $(FIRMWARE)/michi_http/json_helpers.c \
                   $(FIRMWARE)/michi_http/include/michi_http.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CJSON_CFLAGS) -o $@ test_json_helpers.c \
	    $(FIRMWARE)/michi_http/json_helpers.c $(CJSON_LIBS)

clean:
	rm -f test_semver test_pairing_validators test_json_helpers
