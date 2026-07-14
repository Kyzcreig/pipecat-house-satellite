#ifndef PIPECAT_RED_WRAP_H
#define PIPECAT_RED_WRAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RED_WRAP_MAX_DISTANCE 4
#define RED_WRAP_MAX_BLOCK_SIZE 1023

typedef struct RedWrapHistoryEntry {
  uint32_t timestamp;
  uint16_t length;
  uint8_t payload[RED_WRAP_MAX_BLOCK_SIZE];
} RedWrapHistoryEntry;

typedef struct RedWrapState {
  uint8_t primary_payload_type;
  uint8_t distance;
  uint8_t count;
  uint8_t next;
  RedWrapHistoryEntry history[RED_WRAP_MAX_DISTANCE];
} RedWrapState;

void red_wrap_init(RedWrapState *state, uint8_t primary_payload_type,
                   uint8_t distance);

// Returns the RED payload size, or -1 if the primary payload cannot fit.
int red_wrap_packet(RedWrapState *state, const uint8_t *primary,
                    size_t primary_length, uint32_t timestamp, uint8_t *output,
                    size_t output_capacity);

#ifdef __cplusplus
}
#endif

#endif
