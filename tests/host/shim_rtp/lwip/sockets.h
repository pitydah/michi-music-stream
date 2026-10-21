#pragma once
/* H18 native-loopback RTP integration: real POSIX UDP, never a firmware header.
 * This directory is passed ahead of shim/ ONLY to test_michi_audio_engine. */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
