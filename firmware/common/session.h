#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>

void session_init(void);
bool session_start(const char *session_id, const char *codec,
                   int sample_rate, int bit_depth, int channels,
                   int stream_port, int buffer_ms, int volume);
bool session_stop(void);
bool session_is_active(void);

#endif
