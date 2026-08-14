/* Shim for host-side tests: esp_netif_sntp stand-in (see the header).
 * The sync semaphore is a pthread condition-variable count that mirrors
 * the FreeRTOS binary semaphore semantics of esp_netif_sntp.c 5.3
 * (give on a full semaphore fails silently). TEST-ONLY. */

#include "esp_netif_sntp.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "time_shim.h"

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;

static bool s_initialized;
static bool s_client_started;
static bool s_dead;
static int s_sync_count;      /* pending sync notifications (max 1) */
static esp_sntp_time_cb_t s_sync_cb;
static char s_server[64];
static int s_init_count;
static int s_start_count;
static bool s_fail_init;
static bool s_fail_start;

static void clock_get(struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
}

void test_sntp_reset(void)
{
    pthread_mutex_lock(&s_mutex);
    s_initialized = false;
    s_client_started = false;
    s_dead = false;
    s_sync_count = 0;
    s_sync_cb = NULL;
    s_server[0] = '\0';
    s_init_count = 0;
    s_start_count = 0;
    s_fail_init = false;
    s_fail_start = false;
    pthread_mutex_unlock(&s_mutex);
}

void test_sntp_fire_sync(uint64_t unix_sec)
{
    test_time_set_sec(unix_sec);
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized || s_dead) {
        pthread_mutex_unlock(&s_mutex);
        return;
    }
    /* 5.3 order: give the sync semaphore, THEN the user sync_cb. */
    if (s_sync_count == 0) {
        s_sync_count = 1;
        pthread_cond_broadcast(&s_cond);
    }
    const esp_sntp_time_cb_t cb = s_sync_cb;
    pthread_mutex_unlock(&s_mutex);
    if (cb != NULL) {
        struct timeval tv = { .tv_sec = (time_t)unix_sec, .tv_usec = 0 };
        cb(&tv);
    }
}

void test_sntp_set_fail_init(bool fail)
{
    pthread_mutex_lock(&s_mutex);
    s_fail_init = fail;
    pthread_mutex_unlock(&s_mutex);
}

void test_sntp_set_fail_start(bool fail)
{
    pthread_mutex_lock(&s_mutex);
    s_fail_start = fail;
    pthread_mutex_unlock(&s_mutex);
}

int test_sntp_init_count(void)
{
    pthread_mutex_lock(&s_mutex);
    const int v = s_init_count;
    pthread_mutex_unlock(&s_mutex);
    return v;
}

int test_sntp_start_count(void)
{
    pthread_mutex_lock(&s_mutex);
    const int v = s_start_count;
    pthread_mutex_unlock(&s_mutex);
    return v;
}

bool test_sntp_client_started(void)
{
    pthread_mutex_lock(&s_mutex);
    const bool v = s_client_started;
    pthread_mutex_unlock(&s_mutex);
    return v;
}

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_mutex);
    if (s_initialized || s_fail_init) {
        pthread_mutex_unlock(&s_mutex);
        return s_fail_init ? ESP_FAIL : ESP_ERR_INVALID_STATE;
    }
    s_initialized = true;
    s_dead = false;
    s_sync_count = 0;
    s_sync_cb = config->sync_cb;
    if (config->num_of_servers > 0 && config->servers[0] != NULL) {
        snprintf(s_server, sizeof(s_server), "%s", config->servers[0]);
    }
    if (config->start) {
        s_client_started = true;
    }
    s_init_count++;
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

esp_err_t esp_netif_sntp_start(void)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized || s_dead) {
        pthread_mutex_unlock(&s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_fail_start) {
        pthread_mutex_unlock(&s_mutex);
        return ESP_FAIL;
    }
    /* 5.3 start() semantics: stop + init (restart, fresh query burst). */
    s_client_started = true;
    s_start_count++;
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

void esp_netif_sntp_deinit(void)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized || s_dead) {
        pthread_mutex_unlock(&s_mutex);
        return;
    }
    s_dead = true;
    /* Wake any blocked sync_wait: it observes s_dead and returns
     * ESP_ERR_INVALID_STATE (mirrors FreeRTOS vQueueDelete unblocking
     * waiters with a failure). */
    s_sync_count = 1;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_mutex);
}

esp_err_t esp_netif_sntp_sync_wait(TickType_t tout)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized || s_dead) {
        pthread_mutex_unlock(&s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (s_sync_count > 0) {
        s_sync_count = 0; /* consume the (possibly stale) give */
    } else {
        struct timespec ts;
        clock_get(&ts);
        ts.tv_sec += (time_t)(tout / 1000);
        ts.tv_nsec += (long)(tout % 1000) * 1000000L;
        while (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        while (s_sync_count == 0 && !s_dead) {
            if (pthread_cond_timedwait(&s_cond, &s_mutex, &ts) != 0) {
                break; /* timeout */
            }
        }
        if (s_sync_count > 0) {
            s_sync_count = 0;
        } else if (s_dead) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = ESP_ERR_TIMEOUT;
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return err;
}
