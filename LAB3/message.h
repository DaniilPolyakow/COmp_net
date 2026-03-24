#pragma once
#include <stdint.h>

#define MAX_PAYLOAD 1024

typedef struct {
    uint32_t length;
    uint8_t  type;
    char     payload[MAX_PAYLOAD];
} Message;

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6
};