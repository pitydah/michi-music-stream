#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>

typedef void (*session_state_cb_t)(bool started, const char *session_id);

void session_init(void);
void session_set_state_callback(session_state_cb_t cb);
bool session_start(const char *session_id, const char *codec,
                   int sample_rate, int bit_depth, int channels,
                   int stream_port, int buffer_ms, int volume);
bool session_stop(void);
bool session_is_active(void);

#endif
