#include "red_wrap.h"

#include <string.h>

#define RED_TIMESTAMP_OFFSET_MAX 0x3fffU

void red_wrap_init(RedWrapState *state, uint8_t primary_payload_type,
                   uint8_t distance) {
  memset(state, 0, sizeof(*state));
  state->primary_payload_type = primary_payload_type & 0x7f;
  state->distance = distance > RED_WRAP_MAX_DISTANCE ? RED_WRAP_MAX_DISTANCE
                                                      : distance;
}

static void remember_primary(RedWrapState *state, const uint8_t *payload,
                             size_t length, uint32_t timestamp) {
  if (state->distance == 0 || length > RED_WRAP_MAX_BLOCK_SIZE) return;
  RedWrapHistoryEntry *entry = &state->history[state->next];
  entry->timestamp = timestamp;
  entry->length = (uint16_t)length;
  memcpy(entry->payload, payload, length);
  state->next = (uint8_t)((state->next + 1) % state->distance);
  if (state->count < state->distance) state->count++;
}

int red_wrap_packet(RedWrapState *state, const uint8_t *primary,
                    size_t primary_length, uint32_t timestamp, uint8_t *output,
                    size_t output_capacity) {
  if (state == NULL || primary == NULL || output == NULL ||
      primary_length + 1 > output_capacity) {
    return -1;
  }

  const RedWrapHistoryEntry *selected[RED_WRAP_MAX_DISTANCE];
  uint16_t offsets[RED_WRAP_MAX_DISTANCE];
  size_t selected_count = 0;
  size_t output_size = primary_length + 1;
  const uint8_t oldest = state->count == state->distance
                             ? state->next
                             : 0;

  for (uint8_t i = 0; i < state->count; ++i) {
    const uint8_t index = (uint8_t)((oldest + i) % state->distance);
    const RedWrapHistoryEntry *entry = &state->history[index];
    const uint32_t offset = timestamp - entry->timestamp;
    if (entry->length == 0 || offset == 0 ||
        offset > RED_TIMESTAMP_OFFSET_MAX) {
      continue;
    }
    if (output_size + 4 + entry->length > output_capacity) {
      continue;
    }
    selected[selected_count] = entry;
    offsets[selected_count] = (uint16_t)offset;
    selected_count++;
    output_size += 4 + entry->length;
  }

  size_t cursor = 0;
  for (size_t i = 0; i < selected_count; ++i) {
    const uint32_t fields =
        ((uint32_t)offsets[i] << 10) | selected[i]->length;
    output[cursor++] = (uint8_t)(0x80 | state->primary_payload_type);
    output[cursor++] = (uint8_t)(fields >> 16);
    output[cursor++] = (uint8_t)(fields >> 8);
    output[cursor++] = (uint8_t)fields;
  }
  output[cursor++] = state->primary_payload_type;
  for (size_t i = 0; i < selected_count; ++i) {
    memcpy(output + cursor, selected[i]->payload, selected[i]->length);
    cursor += selected[i]->length;
  }
  memcpy(output + cursor, primary, primary_length);
  cursor += primary_length;

  remember_primary(state, primary, primary_length, timestamp);
  return (int)cursor;
}
