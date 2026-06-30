#ifndef LED_H
#define LED_H

#include <stdint.h>

typedef enum {
    LED_BLUE,
    LED_GREEN,
    LED_YELLOW_BLINK,
    LED_RED,
    LED_RED_FAST_BLINK,
    LED_OFF,
} led_state_t;

void led_init(void);
void led_set(led_state_t state);
void led_task(void *arg);

#endif
