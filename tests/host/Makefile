# Host-side unit tests for the universal firmware (F15).
#
# Compiles the REAL firmware sources (semver.c, validators.c,
# json_helpers.c, canonical_json.c, michi_pairing.c, michi_identity.c) -
# no reimplementations. The firmware components keep these files on their
# SRCS list; the tests compile the SAME .c.
#
#   make test   - build + run every test (needs cc; the cJSON tests
#                 additionally need libcjson-dev - CI installs it via
#                 `apt install libcjson-dev`; without it they are
#                 skipped with a clear message)
#   make clean  - remove the test binaries

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -D_DEFAULT_SOURCE
CPPFLAGS = -Ishim \
           -I../../firmware/components/michi_ota \
           -I../../firmware/components/michi_pairing \
           -I../../firmware/components/michi_pairing/include \
           -I../../firmware/components/michi_identity/include \
           -I../../firmware/components/michi_http/include \
           -I../../firmware/components/michi_product_profile/include \
           -I../../firmware/components/michi_dac/include

FIRMWARE = ../../firmware/components

HAS_CJSON := $(shell pkg-config --exists libcjson && echo yes || echo no)
CJSON_CFLAGS := $(shell pkg-config --cflags libcjson 2>/dev/null)
CJSON_LIBS   := $(shell pkg-config --libs libcjson 2>/dev/null)

# Every shim header is a dependency: a shim change must rebuild all tests.
SHIMS := $(wildcard shim/*.h) $(wildcard shim/*/*.h)

# michi_identity: compiles the REAL component sources + vendored crypto
# (Monocypher, BLAKE3 portable-only) against the test shims.
IDENTITY_DIR = $(FIRMWARE)/michi_identity
IDENTITY_SRC = $(IDENTITY_DIR)/michi_identity.c $(IDENTITY_DIR)/identity_nvs.c \
               shim/nvs_fake.c \
               $(IDENTITY_DIR)/monocypher/monocypher.c \
               $(IDENTITY_DIR)/monocypher/monocypher-ed25519.c \
               $(IDENTITY_DIR)/blake3/blake3.c \
               $(IDENTITY_DIR)/blake3/blake3_dispatch.c \
               $(IDENTITY_DIR)/blake3/blake3_portable.c
IDENTITY_DEFS = -DMICHI_IDENTITY_TESTING \
                -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 \
                -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0
IDENTITY_INC = -I$(IDENTITY_DIR)/include -I$(IDENTITY_DIR)/monocypher \
               -I$(IDENTITY_DIR)/blake3

TESTS := test_semver test_pairing_validators test_michi_identity \
         test_michi_pairing \
         $(if $(filter yes,$(HAS_CJSON)),test_json_helpers test_http_canonical test_michi_discovery test_pairing_http)

.PHONY: all test clean

all: $(TESTS)

test: all
	@set -e; \
	for t in test_semver test_pairing_validators test_michi_identity test_michi_pairing; do ./$$t; done; \
	if [ "$(HAS_CJSON)" = "yes" ]; then \
	    ./test_json_helpers; \
	    ./test_http_canonical; \
	    ./test_michi_discovery; \
	    ./test_pairing_http; \
	else \
	    echo "SKIP test_json_helpers (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_http_canonical (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_michi_discovery (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_pairing_http (libcjson-dev not installed - CI installs it)"; \
	fi

test_semver: test_semver.c $(FIRMWARE)/michi_ota/semver.c \
             $(FIRMWARE)/michi_ota/semver.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_semver.c $(FIRMWARE)/michi_ota/semver.c

test_pairing_validators: test_pairing_validators.c $(FIRMWARE)/michi_pairing/validators.c \
                         $(FIRMWARE)/michi_pairing/validators.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_pairing_validators.c \
	    $(FIRMWARE)/michi_pairing/validators.c

test_json_helpers: test_json_helpers.c $(FIRMWARE)/michi_http/json_helpers.c \
                   $(FIRMWARE)/michi_http/include/michi_http.h \
                   $(FIRMWARE)/michi_pairing/validators.c \
                   $(FIRMWARE)/michi_pairing/validators.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CJSON_CFLAGS) -I../../firmware/components/michi_pairing/include \
	    -I../../firmware/components/michi_identity/include \
	    -o $@ test_json_helpers.c \
	    $(FIRMWARE)/michi_http/json_helpers.c \
	    $(FIRMWARE)/michi_pairing/validators.c $(CJSON_LIBS)

test_http_canonical: test_http_canonical.c $(FIRMWARE)/michi_http/canonical_json.c \
                     $(FIRMWARE)/michi_http/include/michi_http.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CJSON_CFLAGS) -o $@ test_http_canonical.c \
	    $(FIRMWARE)/michi_http/canonical_json.c $(CJSON_LIBS)

# Pairing HTTP body gates: the SAME json_helpers.c + validators.c the
# firmware builds (cJSON-gated like the other HTTP tests).
test_pairing_http: test_pairing_http.c $(FIRMWARE)/michi_http/json_helpers.c \
                   $(FIRMWARE)/michi_http/include/michi_http.h \
                   $(FIRMWARE)/michi_pairing/validators.c \
                   $(FIRMWARE)/michi_pairing/validators.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CJSON_CFLAGS) -o $@ test_pairing_http.c \
	    $(FIRMWARE)/michi_http/json_helpers.c \
	    $(FIRMWARE)/michi_pairing/validators.c $(CJSON_LIBS)

# michi_pairing: compiles the REAL component (pairing core + validators +
# identity for verify/derive/base64url) against the test shims. The host
# SHA-256 test double stands in for mbedTLS (KAT-checked by the test).
PAIRING_DIR = $(FIRMWARE)/michi_pairing
PAIRING_SRC = $(PAIRING_DIR)/michi_pairing.c $(PAIRING_DIR)/validators.c \
              shim/esp_timer.c shim/michi_state.c shim/mbedtls/sha256.c \
              shim/nvs_fake.c \
              $(IDENTITY_DIR)/michi_identity.c $(IDENTITY_DIR)/identity_nvs.c \
              $(IDENTITY_DIR)/monocypher/monocypher.c \
              $(IDENTITY_DIR)/monocypher/monocypher-ed25519.c \
              $(IDENTITY_DIR)/blake3/blake3.c \
              $(IDENTITY_DIR)/blake3/blake3_dispatch.c \
              $(IDENTITY_DIR)/blake3/blake3_portable.c
PAIRING_INC = -I$(PAIRING_DIR)/include -I$(PAIRING_DIR)

test_michi_pairing: test_michi_pairing.c $(PAIRING_SRC) \
                    $(PAIRING_DIR)/include/michi_pairing.h \
                    $(PAIRING_DIR)/validators.h \
                    $(IDENTITY_DIR)/include/michi_identity.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PAIRING_INC) $(IDENTITY_INC) \
	    $(IDENTITY_DEFS) -pthread -o $@ test_michi_pairing.c $(PAIRING_SRC)

test_michi_identity: test_michi_identity.c $(IDENTITY_SRC) \
                     $(IDENTITY_DIR)/include/michi_identity.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IDENTITY_INC) $(IDENTITY_DEFS) \
	    -o $@ test_michi_identity.c $(IDENTITY_SRC)

# michi_discovery: compiles the REAL announce builder + server_id NVS
# wrapper + the identity sources (signing) against the test shims.
DISCOVERY_DIR = $(FIRMWARE)/michi_discovery
DISCOVERY_SRC = $(DISCOVERY_DIR)/announce.c $(DISCOVERY_DIR)/discovery_nvs.c
DISCOVERY_INC = -I$(DISCOVERY_DIR) -I$(DISCOVERY_DIR)/include

test_michi_discovery: test_michi_discovery.c $(DISCOVERY_SRC) \
                      $(DISCOVERY_DIR)/include/michi_discovery.h \
                      $(DISCOVERY_DIR)/discovery_nvs.h \
                      $(IDENTITY_SRC) $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DISCOVERY_INC) $(IDENTITY_INC) \
	    $(IDENTITY_DEFS) $(CJSON_CFLAGS) -o $@ test_michi_discovery.c \
	    $(DISCOVERY_SRC) $(IDENTITY_SRC) $(CJSON_LIBS)

clean:
	rm -f test_semver test_pairing_validators test_json_helpers \
	      test_http_canonical test_michi_identity test_michi_discovery \
	      test_michi_pairing test_pairing_http
