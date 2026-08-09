#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *file;
    uint32_t data_bytes;
} wav_writer_t;

bool wav_writer_open(wav_writer_t *writer, const char *path, uint32_t sample_rate, uint16_t channels);
bool wav_writer_write(wav_writer_t *writer, const int16_t *samples, size_t count);
bool wav_writer_close(wav_writer_t *writer, uint32_t sample_rate, uint16_t channels);
