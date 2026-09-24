#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "michi_board.h"

/* External hooks from esp_lcd_shim.c */
void test_lcd_reset(void);
void test_lcd_set_async_delay_ms(int ms);
void test_lcd_set_drop_completion(bool drop);
bool test_lcd_is_buffer_in_flight(void);
int test_lcd_get_draw_count(void);
bool test_lcd_has_violation(void);
void test_lcd_report_access(const void *ptr);

static int s_render_callbacks_invoked = 0;
static bool s_premature_reuse_detected = false;

static void tracking_render_fn(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin)
{
    (void)fb_w;
    (void)fb_h;
    (void)y_origin;
    s_render_callbacks_invoked++;

    /* If the buffer is currently in-flight in the DMA hardware, this is a violation of DMA lifetime! */
    if (test_lcd_is_buffer_in_flight()) {
        s_premature_reuse_detected = true;
    }
    test_lcd_report_access(fb);

    /* Write test pattern */
    if (fb != NULL) {
        fb[0] = 0x1234;
    }
}

static void test_dma_01_synchronous_completion(void)
{
    printf("DMA-01: synchronous completion cleanly flushes all bands\n");
    test_lcd_reset();
    s_render_callbacks_invoked = 0;
    s_premature_reuse_detected = false;

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_OK);
    assert(s_render_callbacks_invoked == 6);
    assert(test_lcd_get_draw_count() == 6);
    assert(!s_premature_reuse_detected);
    assert(!test_lcd_has_violation());

    michi_board_shutdown();
}

static void test_dma_01_async_dma_lifetime(void)
{
    printf("DMA-01: async DMA transfer completes before buffer reuse\n");
    test_lcd_reset();
    /* Set 5ms transfer duration per band */
    test_lcd_set_async_delay_ms(5);
    s_render_callbacks_invoked = 0;
    s_premature_reuse_detected = false;

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_OK);
    assert(s_render_callbacks_invoked == 6);
    assert(test_lcd_get_draw_count() == 6);
    assert(!s_premature_reuse_detected);
    assert(!test_lcd_has_violation());

    michi_board_shutdown();
}

static void test_dma_timeout_recovery(void)
{
    printf("DMA-01: stalled DMA times out and returns ESP_ERR_TIMEOUT\n");
    test_lcd_reset();
    test_lcd_set_drop_completion(true);
    s_render_callbacks_invoked = 0;
    s_premature_reuse_detected = false;

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    /* flush_band should time out after 500ms when hardware completion callback never fires */
    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_ERR_TIMEOUT);
    assert(s_render_callbacks_invoked == 1);
    assert(test_lcd_get_draw_count() == 1);

    michi_board_shutdown();
}

static void test_dma_clear_and_boot_screen(void)
{
    printf("DMA-01: clear and boot screen respect DMA completion\n");
    test_lcd_reset();
    test_lcd_set_async_delay_ms(2);

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    err = michi_board_display_clear();
    assert(err == ESP_OK);
    assert(test_lcd_get_draw_count() == 6);

    const michi_board_info_t *bi = michi_board_get_info();
    michi_board_selftest_t st = michi_board_self_test();
    err = michi_board_display_boot_screen(bi, &st, "Test Boot");
    assert(err == ESP_OK);
    assert(test_lcd_get_draw_count() == 12);

    michi_board_shutdown();
}

int main(void)
{
    printf("--- RUN test_michi_display_dma ---\n");
    test_dma_01_synchronous_completion();
    test_dma_01_async_dma_lifetime();
    test_dma_timeout_recovery();
    test_dma_clear_and_boot_screen();
    printf("test_michi_display_dma: all DMA-01 checks PASSED\n");
    return 0;
}
