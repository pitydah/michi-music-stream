#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Signed device discovery (MS-05): mDNS + UDP multicast announce with the
 * canonical Michi Link signed identity.
 *
 * Contract (convergence spec, section 2.2):
 * - mDNS service type: `_michi-link._tcp.local` on the REAL HTTP port
 *   (michi_http serves 80; see MICHI_DISCOVERY_HTTP_PORT). Instance name
 *   = the visible device name (product profile). TXT records are exactly
 *   device_id, service, api_version, roles (plain "audio_receiver" string,
 *   never JSON) and michi_id.
 * - UDP multicast: group 224.0.0.167, port 53318, IP TTL 1, ONE compact
 *   JSON datagram <= 1200 bytes. Announced on network up (boot / IP
 *   change) and every 30 s (+-3 s jitter).
 * - device_id IS server_id: the same persistent UUID v4 (generated once,
 *   stored in the NVS "michi_discovery" namespace).
 * - The signed group (michi_id, public_key, nonce, timestamp_ms,
 *   signature) is MANDATORY for michi-stream-*: the Ed25519 signature
 *   covers the canonical payload (lexicographically ordered keys,
 *   signature excluded), byte-identical to the michi-identity Rust crate
 *   DiscoveryEngine::canonical_bytes (golden vectors:
 *   contracts/michi-link/vectors/discovery/).
 *
 * Ownership: michi_wifi owns the network bring-up and calls
 * michi_discovery_start()/stop() on GOT_IP / disconnect. All announce
 * machinery (mDNS service, UDP socket, announce timer) lives here; the
 * socket and timer are closed/stopped on disconnect and re-opened on the
 * next network-up (contract 2.2).
 */

/* Canonical mDNS service type (the mDNS stack appends ".local"). */
#define MICHI_DISCOVERY_MDNS_SERVICE "_michi-link"
#define MICHI_DISCOVERY_MDNS_PROTO "_tcp"

/* Canonical UDP multicast group and port (michi-identity crate). */
#define MICHI_DISCOVERY_MULTICAST_GROUP "224.0.0.167"
#define MICHI_DISCOVERY_MULTICAST_PORT 53318

/* IP TTL of the announce datagrams (link-local, contract 2.2). */
#define MICHI_DISCOVERY_UDP_TTL 1

/* Upper bound of ONE announce datagram (contract 2.2). */
#define MICHI_DISCOVERY_MAX_DATAGRAM_BYTES 1200

/* Announce cadence: every 30 s, +-3 s uniform jitter. */
#define MICHI_DISCOVERY_ANNOUNCE_INTERVAL_MS 30000
#define MICHI_DISCOVERY_ANNOUNCE_JITTER_MS 3000

/* The real HTTP port of the device. michi_http serves port 80
 * (http_server.c MICHI_HTTP_PORT, not exported): the announce must
 * advertise the REAL port, so the constant is mirrored here with that
 * coupling documented - http_server.c is the serving owner, this is the
 * announce mirror. */
#define MICHI_DISCOVERY_HTTP_PORT 80

/* The single canonical role of a stream receiver (contract 2.1/2.2). */
#define MICHI_DISCOVERY_ROLE "audio_receiver"

/* Canonical API contract version announced by michi-stream-*. */
#define MICHI_DISCOVERY_API_VERSION "v1-lite"

/* NVS namespace/key of the persistent server_id (== device_id). */
#define MICHI_DISCOVERY_NVS_NAMESPACE "michi_discovery"
#define MICHI_DISCOVERY_NVS_KEY "server_id"

/** Length of a formatted UUID v4 string (36 chars + NUL). */
#define MICHI_DISCOVERY_UUID_LEN 37

/**
 * @brief Announce field set. The fields mirror the discovery announce
 *        schema exactly; the signature is added by the builder (it is
 *        NOT part of the canonical payload).
 */
typedef struct {
    const char *device_id;   /*!< Persistent server_id UUID (contract 2.2:
                                  device_id == server_id) */
    const char *name;        /*!< Visible device name */
    const char *service;     /*!< michi-stream-standard | michi-stream-hifi */
    const char *api_version; /*!< "v1-lite" */
    const char *host;        /*!< Current IPv4 */
    uint16_t port;           /*!< Real HTTP port */
    bool feature_session;    /*!< Truthful capability flag */
    bool feature_heartbeat;  /*!< Truthful capability flag */
    bool feature_volume;     /*!< Truthful capability flag */
    const char *michi_id;    /*!< base64url-nopad, 43 chars */
    const char *public_key;  /*!< Ed25519 pk, base64url-nopad, 43 chars */
    int64_t timestamp_ms;    /*!< Epoch ms when the announce is signed */
    const char *nonce;       /*!< base64url-nopad, >= 22 chars (16 bytes) */
} michi_discovery_announce_t;

/**
 * @brief Initialize discovery: persistent server_id (generated exactly
 *        once), identity readiness (michi_identity_init - idempotent, so
 *        a future boot-path owner can call it again), mDNS stack
 *        (mdns_init + hostname) and the announce timer.
 *
 * Does NOT open the UDP socket or advertise: that happens on network up
 * (michi_discovery_start). Idempotent while initialized.
 *
 * @return ESP_OK; NVS/mdns/timer errors are propagated unchanged (the
 *         caller keeps running degraded: no discovery, no announce).
 */
esp_err_t michi_discovery_init(void);

/**
 * @brief Network up (boot GOT_IP / IP change): open (or re-open on IP
 *        change) the UDP socket, advertise the mDNS service, send one
 *        announce immediately and (re-)arm the 30 s +-3 s timer.
 *
 * The mDNS service is advertised with the canonical TXT records; on a
 * periodic timer tick a failed/raced advertise is retried (self-healing).
 * A zero/invalid IPv4 is rejected (logged, no announce).
 *
 * @param ipv4 Current IPv4 of the STA (e.g. "192.168.1.102").
 * @return ESP_OK; ESP_ERR_INVALID_ARG on a NULL/zero IP;
 *         ESP_ERR_INVALID_STATE when identity is not READY (CORRUPT
 *         requires factory reset - announces stay off, logged at init);
 *         socket/mdns errors are logged and degrade the announce cycle.
 */
esp_err_t michi_discovery_start(const char *ipv4);

/**
 * @brief Network down (disconnect): close the UDP socket, stop the
 *        announce timer and retire the mDNS service.
 *
 * Idempotent: safe to call when not active.
 *
 * @return ESP_OK.
 */
esp_err_t michi_discovery_stop(void);

/**
 * @brief Shut discovery down completely: stop() plus mdns_free() and
 *        timer deletion. Idempotent.
 *
 * @return ESP_OK.
 */
esp_err_t michi_discovery_shutdown(void);

/**
 * @brief Copy the persistent server_id (== the announce device_id).
 *
 * @param out     Buffer (>= MICHI_DISCOVERY_UUID_LEN bytes).
 * @param out_len Size of out.
 * @return ESP_OK; ESP_ERR_INVALID_STATE when the store is not usable
 *         (corrupt - factory reset required); ESP_ERR_INVALID_SIZE when
 *         the buffer is too small.
 */
esp_err_t michi_discovery_get_server_id(char *out, size_t out_len);

/**
 * @brief Build the CANONICAL payload of an announce: the exact JSON the
 *        signature covers. Fields serialized with lexicographically
 *        ordered keys (api_version, device_id, features, host, michi_id,
 *        name, nonce, port, public_key, roles, service, timestamp_ms),
 *        the signature EXCLUDED - byte-identical to the Rust crate's
 *        DiscoveryEngine::canonical_bytes. Pure C, host-testable.
 *
 * @param a       Announce fields (all strings mandatory, non-empty).
 * @param out     Output buffer.
 * @param out_len Size of out.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL/empty mandatory fields or
 *         port == 0; ESP_ERR_INVALID_SIZE when the buffer is too small.
 */
esp_err_t michi_discovery_canonical_json(const michi_discovery_announce_t *a,
                                         char *out, size_t out_len);

/**
 * @brief Build the complete signed announce datagram: canonical payload
 *        -> Ed25519 signature (michi_identity) -> one compact JSON
 *        object including the mandatory signed group. Enforced <=
 *        MICHI_DISCOVERY_MAX_DATAGRAM_BYTES bytes. Pure C (cJSON +
 *        michi_identity), host-testable.
 *
 * @param a            Announce fields (see canonical_json).
 * @param out          Output buffer (>= MICHI_DISCOVERY_MAX_DATAGRAM_BYTES
 *                     + 1 recommended).
 * @param out_len      Size of out.
 * @param out_written  Receives the datagram length (excluding NUL).
 * @return ESP_OK; ESP_ERR_INVALID_ARG on invalid fields;
 *         ESP_ERR_INVALID_STATE when identity is not READY;
 *         ESP_ERR_INVALID_SIZE when the datagram exceeds the buffer or
 *         the 1200-byte contract limit.
 */
esp_err_t michi_discovery_build_announce(const michi_discovery_announce_t *a,
                                         char *out, size_t out_len,
                                         size_t *out_written);

#ifdef __cplusplus
}
#endif
