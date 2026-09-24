#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"

struct esp_lcd_panel_io_t {
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
};

struct esp_lcd_panel_t {
    esp_lcd_panel_io_handle_t io;
    bool enabled;
};

static struct esp_lcd_panel_io_t s_io_inst;
static struct esp_lcd_panel_t s_panel_inst;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_buffer_in_flight = false;
static const void *s_in_flight_ptr = NULL;
static int s_draw_count = 0;
static int s_async_delay_ms = 0;
static bool s_drop_completion = false;
static bool s_violation_detected = false;

void test_lcd_reset(void)
{
    pthread_mutex_lock(&s_lock);
    s_buffer_in_flight = false;
    s_in_flight_ptr = NULL;
    s_draw_count = 0;
    s_async_delay_ms = 0;
    s_drop_completion = false;
    s_violation_detected = false;
    memset(&s_io_inst, 0, sizeof(s_io_inst));
    memset(&s_panel_inst, 0, sizeof(s_panel_inst));
    pthread_mutex_unlock(&s_lock);
}

void test_lcd_set_async_delay_ms(int ms)
{
    pthread_mutex_lock(&s_lock);
    s_async_delay_ms = ms;
    pthread_mutex_unlock(&s_lock);
}

void test_lcd_set_drop_completion(bool drop)
{
    pthread_mutex_lock(&s_lock);
    s_drop_completion = drop;
    pthread_mutex_unlock(&s_lock);
}

bool test_lcd_is_buffer_in_flight(void)
{
    pthread_mutex_lock(&s_lock);
    bool in_flight = s_buffer_in_flight;
    pthread_mutex_unlock(&s_lock);
    return in_flight;
}

int test_lcd_get_draw_count(void)
{
    pthread_mutex_lock(&s_lock);
    int cnt = s_draw_count;
    pthread_mutex_unlock(&s_lock);
    return cnt;
}

bool test_lcd_has_violation(void)
{
    pthread_mutex_lock(&s_lock);
    bool v = s_violation_detected;
    pthread_mutex_unlock(&s_lock);
    return v;
}

void test_lcd_report_access(const void *ptr)
{
    pthread_mutex_lock(&s_lock);
    if (s_buffer_in_flight && ptr == s_in_flight_ptr) {
        s_violation_detected = true;
    }
    pthread_mutex_unlock(&s_lock);
}

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t *io_config,
                                   esp_lcd_panel_io_handle_t *ret_io)
{
    (void)bus;
    if (io_config == NULL || ret_io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_io_inst.on_color_trans_done = io_config->on_color_trans_done;
    s_io_inst.user_ctx = io_config->user_ctx;
    *ret_io = &s_io_inst;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io)
{
    (void)io;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    (void)panel_dev_config;
    if (io == NULL || ret_panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_panel_inst.io = io;
    s_panel_inst.enabled = false;
    *ret_panel = &s_panel_inst;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel)
{
    (void)panel;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel)
{
    (void)panel;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel)
{
    (void)panel;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t panel, bool invert_color_data)
{
    (void)panel;
    (void)invert_color_data;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y)
{
    (void)panel;
    (void)mirror_x;
    (void)mirror_y;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap_axes)
{
    (void)panel;
    (void)swap_axes;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on_off)
{
    if (panel != NULL) {
        panel->enabled = on_off;
    }
    return ESP_OK;
}

struct async_ctx {
    esp_lcd_panel_io_color_trans_done_cb_t cb;
    esp_lcd_panel_io_handle_t io;
    void *user_ctx;
    int delay_ms;
};

static void *async_completion_thread(void *arg)
{
    struct async_ctx *ctx = (struct async_ctx *)arg;
    usleep((useconds_t)ctx->delay_ms * 1000);

    pthread_mutex_lock(&s_lock);
    s_buffer_in_flight = false;
    s_in_flight_ptr = NULL;
    pthread_mutex_unlock(&s_lock);

    if (ctx->cb != NULL) {
        ctx->cb(ctx->io, NULL, ctx->user_ctx);
    }
    free(ctx);
    return NULL;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    (void)panel;
    (void)x_start;
    (void)y_start;
    (void)x_end;
    (void)y_end;

    pthread_mutex_lock(&s_lock);
    s_draw_count++;
    s_buffer_in_flight = true;
    s_in_flight_ptr = color_data;

    int delay = s_async_delay_ms;
    bool drop = s_drop_completion;
    esp_lcd_panel_io_color_trans_done_cb_t cb = s_io_inst.on_color_trans_done;
    void *uctx = s_io_inst.user_ctx;
    esp_lcd_panel_io_handle_t io = &s_io_inst;
    pthread_mutex_unlock(&s_lock);

    if (drop) {
        return ESP_OK;
    }

    if (delay > 0) {
        struct async_ctx *ctx = malloc(sizeof(*ctx));
        if (ctx != NULL) {
            ctx->cb = cb;
            ctx->io = io;
            ctx->user_ctx = uctx;
            ctx->delay_ms = delay;
            pthread_t th;
            pthread_create(&th, NULL, async_completion_thread, ctx);
            pthread_detach(th);
        }
    } else {
        pthread_mutex_lock(&s_lock);
        s_buffer_in_flight = false;
        s_in_flight_ptr = NULL;
        pthread_mutex_unlock(&s_lock);

        if (cb != NULL) {
            cb(io, NULL, uctx);
        }
    }

    return ESP_OK;
}
