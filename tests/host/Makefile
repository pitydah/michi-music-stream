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
           -I../../firmware/components/michi_session/include \
           -I../../firmware/components/michi_audio \
           -I../../firmware/components/michi_audio/include \
           -I../../firmware/components/michi_volume/include \
           -I../../firmware/components/michi_display/include \
           -I../../firmware/components/michi_board/include \
           -I../../firmware/components/michi_state/include \
           -I../../firmware/components/michi_product_profile/include \
           -I../../firmware/components/michi_dac/include

FIRMWARE = ../../firmware/components

HAS_CJSON := $(shell pkg-config --exists libcjson && echo yes || echo no)
CJSON_CFLAGS := $(shell pkg-config --cflags libcjson 2>/dev/null)
CJSON_LIBDIR := $(shell pkg-config --variable=libdir libcjson 2>/dev/null)
# rpath: devs with cJSON in a non-system prefix (PKG_CONFIG_PATH) get a
# binary that finds libcjson.so at runtime without LD_LIBRARY_PATH.
comma := ,
CJSON_LIBS   := $(shell pkg-config --libs libcjson 2>/dev/null) \
                $(if $(CJSON_LIBDIR),-Wl$(comma)-rpath$(comma)$(CJSON_LIBDIR))

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
         test_michi_pairing test_michi_session test_rtp_guard test_michi_button \
         test_michi_ui \
         $(if $(filter yes,$(HAS_CJSON)),test_json_helpers test_http_canonical test_michi_discovery test_michi_time test_capability_parity test_pairing_http test_session_http test_discovery_disc)

.PHONY: all test clean

all: $(TESTS)

test: all
	@set -e; \
	for t in test_semver test_pairing_validators test_michi_identity test_michi_pairing test_michi_session test_rtp_guard test_michi_button test_michi_ui; do ./$$t; done; \
	if [ "$(HAS_CJSON)" = "yes" ]; then \
	    ./test_json_helpers; \
	    ./test_http_canonical; \
	    ./test_michi_discovery; \
	    ./test_michi_time; \
	    ./test_capability_parity; \
	    ./test_pairing_http; \
	    ./test_session_http; \
	    ./test_discovery_disc; \
	else \
	    echo "SKIP test_json_helpers (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_http_canonical (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_michi_discovery (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_michi_time (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_capability_parity (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_pairing_http (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_session_http (libcjson-dev not installed - CI installs it)"; \
	    echo "SKIP test_discovery_disc (libcjson-dev not installed - CI installs it)"; \
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
                     $(FIRMWARE)/michi_http/include/michi_http.h \
                     $(FIRMWARE)/michi_product_profile/capabilities.c $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CJSON_CFLAGS) -o $@ test_http_canonical.c \
	    $(FIRMWARE)/michi_http/canonical_json.c \
	    $(FIRMWARE)/michi_product_profile/capabilities.c $(CJSON_LIBS)

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

# michi_button: compiles the REAL gesture module (the exact classification
# the debounce task runs + the factory-reset orchestration) and the REAL
# identity sources (the corrupt-identity recovery path, end to end)
# against the test shims. The pairing side is a call-counter test double
# (the REAL pairing erase_all is already covered by test_michi_pairing),
# so the reset wiring is asserted by call counts.
BUTTON_DIR = $(FIRMWARE)/michi_button
BUTTON_SRC = $(BUTTON_DIR)/michi_button_gesture.c \
             shim/michi_pairing_fake.c shim/nvs_flash.c shim/esp_system.c \
             shim/michi_state.c \
             $(IDENTITY_DIR)/michi_identity.c $(IDENTITY_DIR)/identity_nvs.c \
             shim/nvs_fake.c \
             $(IDENTITY_DIR)/monocypher/monocypher.c \
             $(IDENTITY_DIR)/monocypher/monocypher-ed25519.c \
             $(IDENTITY_DIR)/blake3/blake3.c \
             $(IDENTITY_DIR)/blake3/blake3_dispatch.c \
             $(IDENTITY_DIR)/blake3/blake3_portable.c

test_michi_button: test_michi_button.c $(BUTTON_SRC) \
                   $(BUTTON_DIR)/michi_button_gesture.h \
                   $(PAIRING_DIR)/include/michi_pairing.h \
                   $(IDENTITY_DIR)/include/michi_identity.h \
                   $(IDENTITY_DIR)/identity_storage.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(BUTTON_DIR) $(PAIRING_INC) \
	    $(IDENTITY_INC) -I$(IDENTITY_DIR) $(IDENTITY_DEFS) \
	    -o $@ test_michi_button.c $(BUTTON_SRC)

test_michi_identity: test_michi_identity.c $(IDENTITY_SRC) \
                     $(IDENTITY_DIR)/include/michi_identity.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IDENTITY_INC) $(IDENTITY_DEFS) \
	    -o $@ test_michi_identity.c $(IDENTITY_SRC)

# michi_session: compiles the REAL session layer against test doubles
# for the engine (michi_audio_fake), volume, display and the state bus.
# The REAL public headers (michi_audio.h/michi_volume.h/michi_display.h)
# are compiled in - no struct drift.
SESSION_DIR = $(FIRMWARE)/michi_session
SESSION_SRC = $(SESSION_DIR)/michi_session.c shim/esp_timer.c \
              shim/michi_state.c shim/michi_audio_fake.c \
              shim/michi_volume_fake.c shim/michi_display_fake.c
SESSION_INC = -I$(SESSION_DIR)/include

test_michi_session: test_michi_session.c $(SESSION_SRC) \
                    $(SESSION_DIR)/include/michi_session.h \
                    shim/michi_audio_fake.h shim/michi_volume_fake.h \
                    shim/michi_display_fake.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SESSION_INC) -pthread -o $@ \
	    test_michi_session.c $(SESSION_SRC)

# michi_ui: compiles the REAL display primitives (michi_ui_text.c,
# michi_ui_fonts.c with the generated flash tables, michi_ui_components.c)
# - the exact drawing code the screens run. Proves the wrap/ellipsize
# contracts and the band contract (banded renders pixel-identical to a
# full-frame render).
UI_DIR = $(FIRMWARE)/michi_display
UI_INC = -I$(UI_DIR)/include

test_michi_ui: test_michi_ui.c $(UI_DIR)/michi_ui_text.c \
               $(UI_DIR)/michi_ui_fonts.c $(UI_DIR)/michi_ui_components.c \
               $(UI_DIR)/michi_ui_screens.c $(UI_DIR)/michi_ui_icons.c \
               $(UI_DIR)/michi_ui_theme.c \
               $(UI_DIR)/include/michi_ui.h \
               $(UI_DIR)/include/michi_ui_components.h \
               $(UI_DIR)/include/michi_ui_screens.h \
               $(UI_DIR)/include/michi_ui_strings.h \
               $(UI_DIR)/include/michi_ui_text.h \
               $(UI_DIR)/include/michi_ui_fonts.h \
               $(UI_DIR)/include/michi_ui_theme.h \
               $(UI_DIR)/include/michi_ui_icons.h \
               $(UI_DIR)/assets/fonts/michi_ui_fonts_data.h \
               $(UI_DIR)/assets/icons/michi_ui_icons_data.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UI_INC) -o $@ test_michi_ui.c \
	    $(UI_DIR)/michi_ui_text.c $(UI_DIR)/michi_ui_fonts.c \
	    $(UI_DIR)/michi_ui_components.c $(UI_DIR)/michi_ui_screens.c \
	    $(UI_DIR)/michi_ui_icons.c $(UI_DIR)/michi_ui_theme.c

render_ui_previews: render_ui_previews.c $(UI_DIR)/michi_ui_text.c \
                   $(UI_DIR)/michi_ui_fonts.c $(UI_DIR)/michi_ui_components.c \
                   $(UI_DIR)/michi_ui_screens.c $(UI_DIR)/michi_ui_icons.c \
                   $(UI_DIR)/michi_ui_theme.c \
                   $(UI_DIR)/include/michi_ui.h \
                   $(UI_DIR)/include/michi_ui_components.h \
                   $(UI_DIR)/include/michi_ui_screens.h \
                   $(UI_DIR)/include/michi_ui_strings.h \
                   $(UI_DIR)/include/michi_ui_text.h \
                   $(UI_DIR)/include/michi_ui_fonts.h \
                   $(UI_DIR)/include/michi_ui_theme.h \
                   $(UI_DIR)/include/michi_ui_icons.h \
                   $(UI_DIR)/assets/fonts/michi_ui_fonts_data.h \
                   $(UI_DIR)/assets/icons/michi_ui_icons_data.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UI_INC) -o $@ render_ui_previews.c \
	    $(UI_DIR)/michi_ui_text.c $(UI_DIR)/michi_ui_fonts.c \
	    $(UI_DIR)/michi_ui_components.c $(UI_DIR)/michi_ui_screens.c \
	    $(UI_DIR)/michi_ui_icons.c $(UI_DIR)/michi_ui_theme.c

# rtp_guard: the SAME datagram validation the engine runs (pure C).
test_rtp_guard: test_rtp_guard.c $(FIRMWARE)/michi_audio/rtp_guard.c \
                $(FIRMWARE)/michi_audio/rtp_guard.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_rtp_guard.c \
	    $(FIRMWARE)/michi_audio/rtp_guard.c

# Session HTTP body gates: the SAME json_helpers.c + validators.c the
# firmware builds (cJSON-gated like the other HTTP tests).
test_session_http: test_session_http.c $(FIRMWARE)/michi_http/json_helpers.c \
                   $(FIRMWARE)/michi_http/include/michi_http.h \
                   $(FIRMWARE)/michi_pairing/validators.c \
                   $(FIRMWARE)/michi_pairing/validators.h $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CJSON_CFLAGS) -o $@ test_session_http.c \
	    $(FIRMWARE)/michi_http/json_helpers.c \
	    $(FIRMWARE)/michi_pairing/validators.c $(CJSON_LIBS)

# michi_discovery: compiles the REAL announce builder + server_id NVS
# wrapper + the identity sources (signing) against the test shims. The
# canonical capability table (capabilities.c) is linked so the test
# exercises the SAME wiring the firmware runs.
DISCOVERY_DIR = $(FIRMWARE)/michi_discovery
DISCOVERY_SRC = $(DISCOVERY_DIR)/announce.c $(DISCOVERY_DIR)/discovery_nvs.c
DISCOVERY_INC = -I$(DISCOVERY_DIR) -I$(DISCOVERY_DIR)/include

test_michi_discovery: test_michi_discovery.c $(DISCOVERY_SRC) \
                      $(DISCOVERY_DIR)/include/michi_discovery.h \
                      $(DISCOVERY_DIR)/discovery_nvs.h \
                      $(FIRMWARE)/michi_product_profile/capabilities.c \
                      $(IDENTITY_SRC) $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DISCOVERY_INC) $(IDENTITY_INC) \
	    $(IDENTITY_DEFS) $(CJSON_CFLAGS) -o $@ test_michi_discovery.c \
	    $(DISCOVERY_SRC) $(FIRMWARE)/michi_product_profile/capabilities.c \
	    $(IDENTITY_SRC) $(CJSON_LIBS)

# michi_time + the michi_discovery RUNTIME (P0-02 clock gate): compiles
# the REAL michi_time.c (esp_netif_sntp shim + time() override +
# pthread-backed FreeRTOS task shim - the real sync task runs) and the
# REAL michi_discovery.c against socket/mdns/esp_mac shims with
# datagram capture, proving the gate end to end: no signed announce
# without a synchronized clock, immediate resume via the sync callback,
# reconnect conservation/revalidation and SNTP error degradation.
TIME_DIR = $(FIRMWARE)/michi_time
TIME_SRC = $(TIME_DIR)/michi_time.c shim/esp_netif_sntp.c shim/time_shim.c \
           shim/lwip_sockets.c shim/mdns_shim.c shim/esp_mac_shim.c \
           shim/freertos/task.c shim/esp_timer.c \
           shim/michi_product_profile_fake.c
TIME_INC = -I$(TIME_DIR)/include -I$(TIME_DIR)

test_michi_time: test_michi_time.c $(TIME_SRC) $(DISCOVERY_SRC) \
                 $(DISCOVERY_DIR)/michi_discovery.c \
                 $(DISCOVERY_DIR)/include/michi_discovery.h \
                 $(DISCOVERY_DIR)/discovery_nvs.h \
                 $(TIME_DIR)/include/michi_time.h \
                 $(FIRMWARE)/michi_product_profile/capabilities.c \
                 $(IDENTITY_SRC) $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DISCOVERY_INC) $(IDENTITY_INC) \
	    $(TIME_INC) $(IDENTITY_DEFS) $(CJSON_CFLAGS) -pthread -o $@ \
	    test_michi_time.c $(TIME_SRC) $(DISCOVERY_SRC) \
	    $(DISCOVERY_DIR)/michi_discovery.c \
	    $(FIRMWARE)/michi_product_profile/capabilities.c \
	    $(IDENTITY_SRC) $(CJSON_LIBS)

# P0-01 capability parity: compiles the SAME sources the firmware runs -
# the canonical capability table, the discovery announce builder and the
# HTTP /server/info builder - and asserts the shared feature subset
# (session/heartbeat/volume) agrees across both surfaces.
test_capability_parity: test_capability_parity.c \
                        $(FIRMWARE)/michi_product_profile/capabilities.c \
                        $(FIRMWARE)/michi_http/canonical_json.c \
                        $(FIRMWARE)/michi_http/include/michi_http.h \
                        $(DISCOVERY_DIR)/announce.c \
                        $(DISCOVERY_DIR)/include/michi_discovery.h \
                        $(IDENTITY_SRC) $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DISCOVERY_INC) $(IDENTITY_INC) \
	    $(IDENTITY_DEFS) $(CJSON_CFLAGS) -o $@ test_capability_parity.c \
	    $(FIRMWARE)/michi_product_profile/capabilities.c \
	    $(FIRMWARE)/michi_http/canonical_json.c \
	    $(DISCOVERY_DIR)/announce.c $(IDENTITY_SRC) $(CJSON_LIBS)

# DISC suite (runtime discovery hardening): the FULL runtime path -
# product profile -> runtime discovery state -> canonical serialization
# -> Ed25519 signature -> JSON datagram -> Michi Link verification.
# Compiles the SAME firmware sources as test_michi_time (michi_time +
# michi_discovery runtime + identity + capabilities) plus the REAL
# /server/info builder (canonical_json.c) for the DISC-05 parity check.
DISC_SRC = test_discovery_disc.c $(TIME_SRC) $(DISCOVERY_SRC) \
           $(DISCOVERY_DIR)/michi_discovery.c \
           $(FIRMWARE)/michi_product_profile/capabilities.c \
           $(FIRMWARE)/michi_http/canonical_json.c

test_discovery_disc: $(DISC_SRC) \
                     $(DISCOVERY_DIR)/include/michi_discovery.h \
                     $(DISCOVERY_DIR)/discovery_nvs.h \
                     $(TIME_DIR)/include/michi_time.h \
                     $(FIRMWARE)/michi_http/include/michi_http.h \
                     $(IDENTITY_SRC) $(SHIMS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DISCOVERY_INC) $(IDENTITY_INC) \
	    $(TIME_INC) $(IDENTITY_DEFS) $(CJSON_CFLAGS) -pthread -o $@ \
	    $(DISC_SRC) $(IDENTITY_SRC) $(CJSON_LIBS)

clean:
	rm -f test_semver test_pairing_validators test_json_helpers \
	      test_http_canonical test_michi_identity test_michi_discovery \
	      test_michi_time test_michi_pairing test_pairing_http \
	      test_michi_session test_rtp_guard test_session_http \
	      test_capability_parity test_discovery_disc test_michi_button \
	      test_michi_ui render_ui_previews
