idf_component_register(
    SRCS "michi_identity.c" "identity_nvs.c"
         "monocypher/monocypher.c" "monocypher/monocypher-ed25519.c"
         "blake3/blake3.c" "blake3/blake3_dispatch.c" "blake3/blake3_portable.c"
    INCLUDE_DIRS "include" "monocypher" "blake3"
    REQUIRES nvs_flash esp_hw_support
)

# BLAKE3 portable-only configuration (see README.md): the official C
# implementation runs its portable path everywhere - mandatory on Xtensa
# (ESP32-S3, no SIMD) and keeps host test results byte-identical. The
# BLAKE3_NO_* macros drop the x86 paths from the dispatch; BLAKE3_USE_NEON=0
# disables the AArch64 autodetect (no blake3_neon.c is vendored).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41
    BLAKE3_NO_AVX2
    BLAKE3_NO_AVX512
    BLAKE3_USE_NEON=0
)
