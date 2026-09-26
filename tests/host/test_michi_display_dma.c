#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "michi_board.h"

/* External hooks from esp_lcd_shim.c */
void test_lcd_reset(void);
void test_lcd_set_async_delay_ms(int ms);
void test_lcd_set_drop_completion(bool drop);
void test_lcd_trigger_late_completion(void);
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
    /* First shutdown preserves resources and returns ESP_ERR_TIMEOUT */
    err = michi_board_shutdown();
    assert(err == ESP_ERR_TIMEOUT);

    /* Trigger late completion and tear down cleanly */
    test_lcd_trigger_late_completion();
    err = michi_board_shutdown();
    assert(err == ESP_OK);
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

static void test_dma_02_late_completion_no_premature_reuse(void)
{
    printf("DMA-02: late completion after timeout does not allow premature buffer reuse\n");
    test_lcd_reset();
    /* Set 600ms async delay: flush_band times out at 500ms */
    test_lcd_set_async_delay_ms(600);
    s_render_callbacks_invoked = 0;
    s_premature_reuse_detected = false;

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    /* Render should time out after 500ms */
    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_ERR_TIMEOUT);
    assert(michi_board_display_is_quarantined());

    /* While quarantined, immediate subsequent render is rejected and does not touch buffer */
    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_ERR_INVALID_STATE);
    assert(!s_premature_reuse_detected);

    /* Wait for the late completion (at 600ms) to arrive */
    usleep(150000);

    /* Recover display - late completion is confirmed and quarantine lifted */
    err = michi_board_display_recover();
    assert(err == ESP_OK);
    assert(!michi_board_display_is_quarantined());

    /* Subsequent render after recovery proceeds cleanly */
    test_lcd_set_async_delay_ms(0);
    s_render_callbacks_invoked = 0;
    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_OK);
    assert(s_render_callbacks_invoked == 6);
    assert(!s_premature_reuse_detected);
    assert(!test_lcd_has_violation());

    michi_board_shutdown();
}

static void test_dma_shut_01_clean_shutdown(void)
{
    printf("DMA-SHUT-01: clean shutdown when idle releases resources without error\n");
    test_lcd_reset();
    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    err = michi_board_shutdown();
    assert(err == ESP_OK);
    assert(!test_lcd_has_violation());
}

static void test_dma_shut_02_in_flight_completion(void)
{
    printf("DMA-SHUT-02: in-flight DMA completing within timeout allows clean teardown\n");
    test_lcd_reset();
    /* Set 600ms async delay: flush_band times out at 500ms leaving DMA in flight */
    test_lcd_set_async_delay_ms(600);
    s_render_callbacks_invoked = 0;

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_ERR_TIMEOUT);
    assert(michi_board_display_is_quarantined());

    /* Shutdown waits up to 1000ms. Since DMA completes at 600ms (~100ms from now),
     * shutdown joins cleanly without timeout or violation. */
    err = michi_board_shutdown();
    assert(err == ESP_OK);
    assert(!test_lcd_has_violation());
}

static void test_dma_shut_03_timeout_preserves_resources(void)
{
    printf("DMA-SHUT-03: stalled DMA times out and preserves resources without UAF\n");
    test_lcd_reset();
    test_lcd_set_drop_completion(true);
    s_render_callbacks_invoked = 0;

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_ERR_TIMEOUT);
    assert(test_lcd_is_buffer_in_flight());

    /* Shutdown while buffer is in flight with dropped completion:
     * wait times out, returns ESP_ERR_TIMEOUT, preserves panel_io/sem/fb to prevent UAF. */
    err = michi_board_shutdown();
    assert(err == ESP_ERR_TIMEOUT);
    assert(!test_lcd_has_violation());
    assert(michi_board_display_is_quarantined());

    /* Clean up for subsequent tests */
    test_lcd_trigger_late_completion();
    err = michi_board_shutdown();
    assert(err == ESP_OK);
    assert(!test_lcd_has_violation());
}

static void test_dma_shut_04_late_completion_retry(void)
{
    printf("DMA-SHUT-04: late completion after shutdown timeout allows clean subsequent retry\n");
    test_lcd_reset();
    test_lcd_set_drop_completion(true);
    s_render_callbacks_invoked = 0;

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_ERR_TIMEOUT);
    assert(test_lcd_is_buffer_in_flight());

    /* First shutdown times out */
    err = michi_board_shutdown();
    assert(err == ESP_ERR_TIMEOUT);
    assert(!test_lcd_has_violation());

    /* Now late completion arrives */
    test_lcd_trigger_late_completion();

    /* Second shutdown completes cleanly */
    err = michi_board_shutdown();
    assert(err == ESP_OK);
    assert(!test_lcd_has_violation());
    assert(!michi_board_display_is_quarantined());
}

static void test_dma_04_subsequent_draw_rejected_until_recovery(void)
{
    printf("DMA-04: subsequent draw rejected until explicit recovery\n");
    test_lcd_reset();
    test_lcd_set_drop_completion(true);
    s_render_callbacks_invoked = 0;

    esp_err_t err = michi_board_init();
    assert(err == ESP_OK);

    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_ERR_TIMEOUT);
    assert(michi_board_display_is_quarantined());

    /* All display operations must fail closed with ESP_ERR_INVALID_STATE */
    err = michi_board_display_clear();
    assert(err == ESP_ERR_INVALID_STATE);

    const michi_board_info_t *bi = michi_board_get_info();
    michi_board_selftest_t st = michi_board_self_test();
    err = michi_board_display_boot_screen(bi, &st, "Test Boot");
    assert(err == ESP_ERR_INVALID_STATE);

    err = michi_board_display_render(tracking_render_fn);
    assert(err == ESP_ERR_INVALID_STATE);

    /* If late completion never arrives, recovery fails with ESP_ERR_TIMEOUT */
    err = michi_board_display_recover();
    assert(err == ESP_ERR_TIMEOUT);
    assert(michi_board_display_is_quarantined());

    test_lcd_trigger_late_completion();
    err = michi_board_shutdown();
    assert(err == ESP_OK);
}

int main(void)
{
    printf("--- RUN test_michi_display_dma ---\n");
    test_dma_01_synchronous_completion();
    test_dma_01_async_dma_lifetime();
    test_dma_timeout_recovery();
    test_dma_clear_and_boot_screen();
    test_dma_02_late_completion_no_premature_reuse();
    test_dma_shut_01_clean_shutdown();
    test_dma_shut_02_in_flight_completion();
    test_dma_shut_03_timeout_preserves_resources();
    test_dma_shut_04_late_completion_retry();
    test_dma_04_subsequent_draw_rejected_until_recovery();
    printf("test_michi_display_dma: all DMA-01..04 & DMA-SHUT-01..04 checks PASSED\n");
    return 0;
}
