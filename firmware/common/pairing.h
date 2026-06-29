#ifndef PAIRING_H
#define PAIRING_H

#include <stdbool.h>

#define PAIRING_WINDOW_SECONDS  120
#define MAX_PAIRED_CONTROLLERS  4

void pairing_init(void);
bool pairing_start(const char *initiator_id, char *nonce_out, size_t nonce_size);
bool pairing_confirm(const char *nonce, const char *initiator_id, const char *token);
bool pairing_is_window_open(void);
bool pairing_is_paired(void);
bool pairing_validate_token(const char *token);
void pairing_factory_reset(void);
void pairing_button_pressed(void);

#endif
