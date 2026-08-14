#!/usr/bin/env python3
"""
sign_manifest.py — sign a Michi OTA update manifest (phase 13 + 17).

Builds the canonical signed payload

    version|board|min_version|url|sha256

computes RSA-SHA256 (PKCS#1 v1.5) with the given private key and emits the
JSON manifest the firmware consumes through the OTA engine
(michi_ota_start, HTTPS manifest fetch - the canonical HTTP trigger
POST /api/v1/receiver-lite/firmware is DEFERRED, 501 NOT_IMPLEMENTED)
or from the onboard microSD (local OTA, phase 17).

The private key is an INPUT ONLY: it is never embedded, logged or stored by
this script, and it must live OUTSIDE the repository (see
firmware/README.md, OTA section). The firmware verifies the signature with
the embedded public key (components/michi_ota/include/michi_ota_pubkey.h).

Requires: cryptography (pip install cryptography) or the openssl CLI.

Usage (HTTPS OTA):
  python3 scripts/sign_manifest.py \
      --key /path/to/ota_private.pem \
      --version 0.3.0 \
      --board "Waveshare ESP32-S3-LCD-2" \
      --min-version 0.2.0 \
      --url "https://dl.example.com/michi/0.3.0/firmware.bin" \
      --sha256 <64 hex chars of the firmware binary> \
      --out manifest.json

Usage (local OTA from SD, phase 17): the url is file://<base-name> and
must name the binary file that sits on the card:
  python3 scripts/sign_manifest.py \
      --key /path/to/ota_private.pem \
      --version 0.3.0 \
      --board "Waveshare ESP32-S3-LCD-2" \
      --min-version 0.2.0 \
      --url "file://michi-update.bin" \
      --sha256 <64 hex chars of the firmware binary> \
      --out michi-update.json

  Copy michi-update.json + the binary (named exactly as the file:// base
  name, e.g. michi-update.bin) onto a FAT32-formatted microSD card,
  insert it in the Waveshare ESP32-S3-LCD-2, and the receiver applies the
  update at boot (or keeps serving it for michi_ota_start_local()).

  python3 scripts/sign_manifest.py --help   # full options
"""

import argparse
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit

PAYLOAD_SEPARATOR = "|"
MAX_FIELD = {
    "version": 32,
    "board": 64,
    "min_version": 32,
    "url": 256,
    "sha256": 64,
}
# Local (SD) OTA: the file:// base name limit enforced by the firmware
# (MICHI_OTA_FILE_NAME_MAX).
FILE_URL_BASE_MAX = 64


def canonical_payload(version, board, min_version, url, sha256):
    return PAYLOAD_SEPARATOR.join([version, board, min_version, url, sha256])


def validate_url(url):
    """Mirror of the firmware rules: https:// (no userinfo, <= url max)
    for the network flow, or file://<base-name> (plain base name only)
    for the local SD flow."""
    if url.startswith("https://"):
        if len(url) > MAX_FIELD["url"]:
            sys.exit(f"error: url longer than {MAX_FIELD['url']} chars")
        netloc = url[8:].split("/", 1)[0]
        if "@" in netloc:
            sys.exit("error: url contains userinfo (user@host) — rejected by firmware")
        return
    if url.startswith("file://"):
        base = url[7:]
        if not base or len(base) > FILE_URL_BASE_MAX:
            sys.exit(
                f"error: file:// url must be file://<base-name> "
                f"(1..{FILE_URL_BASE_MAX} chars)"
            )
        if "/" in base or "\\" in base or ".." in base:
            sys.exit(
                "error: file:// url must be a plain base name "
                "(no '/', no '\\\\', no '..') — the firmware rejects path traversal"
            )
        return
    sys.exit("error: url must start with https:// or file://<base-name>")


def validate_fields(fields):
    for name in ("version", "min_version"):
        value = fields[name]
        if len(value) > MAX_FIELD[name]:
            sys.exit(f"error: {name} longer than {MAX_FIELD[name]} chars")
        for part in value.split("."):
            if not part.isdigit():
                sys.exit(f"error: {name} is not semver (x.y.z digits only): {value!r}")
    board = fields["board"]
    if not board or len(board) > MAX_FIELD["board"]:
        sys.exit(f"error: board must be 1..{MAX_FIELD['board']} chars")
    validate_url(fields["url"])
    sha256 = fields["sha256"]
    if len(sha256) != 64 or any(c not in "0123456789abcdefABCDEF" for c in sha256):
        sys.exit("error: sha256 must be exactly 64 hex chars")


def sign_with_cryptography(pem_path, payload: bytes) -> bytes:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding

    with open(pem_path, "rb") as fh:
        key = serialization.load_pem_private_key(fh.read(), password=None)
    return key.sign(payload, padding.PKCS1v15(), hashes.SHA256())


def sign_with_openssl(pem_path, payload: bytes) -> bytes:
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp.write(payload)
        tmp_path = tmp.name
    try:
        proc = subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", pem_path, tmp_path],
            check=True, capture_output=True,
        )
        return proc.stdout
    except subprocess.CalledProcessError as exc:
        sys.exit(f"error: openssl signing failed: {exc.stderr.decode(errors='replace')}")
    finally:
        os.unlink(tmp_path)


def main():
    parser = argparse.ArgumentParser(
        description="Sign a Michi OTA manifest (RSA-2048 PKCS#1 v1.5 SHA-256).")
    parser.add_argument("--key", required=True,
                        help="path to the RSA private key PEM (never committed)")
    parser.add_argument("--version", required=True, help="new firmware version (semver, x.y.z)")
    parser.add_argument("--board", required=True,
                        help="exact board model, must match the receiver profile board_model")
    parser.add_argument("--min-version", required=True,
                        help="minimum firmware version allowed to install this update")
    parser.add_argument("--url", required=True,
                        help="https:// URL of the firmware binary, or "
                             "file://<base-name> for a local SD update "
                             "(sha256 = the binary's digest)")
    parser.add_argument("--sha256", required=True, help="SHA-256 of the firmware binary (64 hex)")
    parser.add_argument("--out", required=True, help="output manifest JSON path")
    args = parser.parse_args()

    fields = {
        "version": args.version,
        "board": args.board,
        "min_version": args.min_version,
        "url": args.url,
        "sha256": args.sha256.lower(),
    }
    validate_fields(fields)

    payload = canonical_payload(**fields).encode("utf-8")
    # The canonical payload is NOT printed in full: it embeds the binary
    # URL, whose query string may carry a token (same rule the firmware
    # logs by: host + path length only). Show the signed fields except the
    # URL, and the URL as a sanitized host/path-length (https) or the
    # base name (file:// - no tokens possible, path-traversal already
    # rejected by validate_url).
    parts = urlsplit(args.url)
    if args.url.startswith("file://"):
        url_log = f"url_file={args.url[7:]}"
    else:
        url_log = f"url_host={parts.netloc} url_path_len={len(parts.path)}"
    print(
        "signing payload: "
        f"version={fields['version']} board={fields['board']} "
        f"min_version={fields['min_version']} sha256={fields['sha256']} "
        f"{url_log} "
        "(url omitted from the log: query strings may carry tokens)"
    )

    try:
        signature = sign_with_cryptography(args.key, payload)
        print("signing backend: cryptography")
    except ImportError:
        print("signing backend: openssl CLI (cryptography not installed)")
        signature = sign_with_openssl(args.key, payload)
    except Exception as exc:  # noqa: BLE001 - surface any key error clearly
        sys.exit(f"error: cryptography signing failed: {exc}")

    manifest = dict(fields)
    manifest["signature"] = base64.b64encode(signature).decode("ascii")

    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)
        fh.write("\n")
    print(f"wrote {args.out} ({len(manifest['signature'])} char signature)")


if __name__ == "__main__":
    main()
