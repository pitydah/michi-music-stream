# michi_identity

Persistent device identity for Michi Music Stream (MS-04): Ed25519
(RFC 8032) + BLAKE3 under the canonical scheme `ed25519-blake3-v1`,
byte-compatible with the Michi Link `michi-identity` crate.

## Derivation contract

```text
michi_id  = base64url-nopad( blake3( public_key_bytes ) )      # 43 chars
public_key = RFC 8032 Ed25519 public key, base64url-nopad      # 43 chars
signature  = RFC 8032 Ed25519 signature,  base64url-nopad      # 86 chars
```

- The 32-byte Ed25519 seed is generated ONCE with `esp_fill_random`
  (hardware RNG) and persisted in NVS (namespace `michi_identity`, key
  `seed`, versioned 40-byte blob). It is never regenerated implicitly.
- States: `UNINITIALIZED`, `READY`, `CORRUPT`. A corrupt store requires
  an explicit factory reset (`michi_identity_factory_reset()`); the
  component never heals corruption by itself.
- Interop with ed25519-dalek (used by the Rust crate): both implement
  RFC 8032; host tests verify crate-generated golden signatures from the
  vendored contract bundle (`contracts/michi-link/vectors/`).

## Vendored libraries (pinned)

Human-reviewed gate decision (convergence spec MS-04): Ed25519 =
**Monocypher**, michi_id derivation = **official BLAKE3 C**.

### Monocypher 4.0.3 — `monocypher/`

- Tag `4.0.3`, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`
  (2026-06-15). License: BSD-2-Clause OR CC0-1.0 (`monocypher/LICENCE.md`).
- Files vendored unmodified: `monocypher.c`, `monocypher.h`, plus the
  official optional module `monocypher-ed25519.c`/`monocypher-ed25519.h`.
- **Why the optional module**: Monocypher's default `crypto_eddsa_*` API
  hashes with BLAKE2b; RFC 8032 Ed25519 (SHA-512, interoperable with
  ed25519-dalek) is exactly the official `monocypher-ed25519` module of
  the same audited release (`crypto_ed25519_sign`/`crypto_ed25519_check`/
  `crypto_ed25519_key_pair`). 4.0.3 includes the Ed25519 timing-leak fix.
- `crypto_ed25519_key_pair` wipes its seed argument: the component passes
  a copy (the original is what gets persisted).

### BLAKE3 1.8.6 — `blake3/`

- Tag `1.8.6`, commit `77b257eee7da5cd608eaf6be8343d3a4c9776af2`.
  License: CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception
  (`blake3/LICENSE_CC0`, `LICENSE_A2`, `LICENSE_A2LLVM`).
- Files vendored unmodified: `blake3.c`, `blake3.h`, `blake3_impl.h`,
  `blake3_dispatch.c`, `blake3_portable.c`.
- **Portable-only config** (Xtensa/ESP32-S3 has no SIMD): the component
  build defines `BLAKE3_NO_SSE2`, `BLAKE3_NO_SSE41`, `BLAKE3_NO_AVX2`,
  `BLAKE3_NO_AVX512` and `BLAKE3_USE_NEON=0` (the NEON switch of the
  1.8.x tree; `BLAKE3_NO_NEON` does not exist in this release). All
  dispatch paths resolve to `blake3_portable.c`, so target and host
  produce byte-identical hashes.
- `blake3_dispatch.c` is part of the official C implementation since
  1.5.0 (it owns SIMD dispatch; portable-only still requires it).

## NVS layout

| Offset | Size | Field |
|---|---:|---|
| 0  | 4 | `version` (u32, `MICHI_IDENTITY_BLOB_VERSION` = 1) |
| 4  | 32 | `seed` (Ed25519 seed) |
| 36 | 4 | `reserved` (deterministic tail padding) |

NVS blobs are CRC-checked on read; a torn write surfaces as a read error
and maps to `CORRUPT` (factory reset required), never to regeneration.

## Host tests

`tests/host/test_michi_identity.c` compiles the REAL firmware sources
(michi_identity.c + identity_nvs.c + vendored crypto) against test shims
(fake NVS in RAM, deterministic `esp_fill_random`), and verifies:

- state machine (first boot, reload, sticky CORRUPT, factory reset);
- seed persisted exactly once; identity stable across simulated reboots;
- base64url-nopad encode/decode (strict) ;
- michi_id derivation against the golden vectors of
  `contracts/michi-link/vectors/identity/` and `vectors/pairing/`;
- Ed25519 interop: crate-generated signatures (discovery announce and
  pairing challenge vectors) verified by Monocypher; altered
  signatures/nonces rejected.
