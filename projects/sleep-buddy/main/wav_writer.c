#include "wav_writer.h"

#include <stdbool.h>
#include <string.h>

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = value & 0xff;
    dst[1] = value >> 8;
}

static void put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = value & 0xff;
    dst[1] = (value >> 8) & 0xff;
    dst[2] = (value >> 16) & 0xff;
    dst[3] = value >> 24;
}

static bool write_header(FILE *file, uint32_t sample_rate, uint16_t channels, uint32_t data_bytes)
{
    uint8_t header[44] = {0};
    memcpy(header, "RIFF", 4);
    put_u32(header + 4, 36 + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_u32(header + 16, 16);
    put_u16(header + 20, 1);
    put_u16(header + 22, channels);
    put_u32(header + 24, sample_rate);
    put_u32(header + 28, sample_rate * channels * 2);
    put_u16(header + 32, channels * 2);
    put_u16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put_u32(header + 40, data_bytes);
    return fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

bool wav_writer_open(wav_writer_t *writer, const char *path, uint32_t sample_rate, uint16_t channels)
{
    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, "wb");
    return writer->file && write_header(writer->file, sample_rate, channels, 0);
}

bool wav_writer_write(wav_writer_t *writer, const int16_t *samples, size_t count)
{
    if (!writer->file || fwrite(samples, sizeof(*samples), count, writer->file) != count) return false;
    writer->data_bytes += count * sizeof(*samples);
    return true;
}

bool wav_writer_close(wav_writer_t *writer, uint32_t sample_rate, uint16_t channels)
{
    if (!writer->file) return true;
    bool ok = fflush(writer->file) == 0 && fseek(writer->file, 0, SEEK_SET) == 0 &&
              write_header(writer->file, sample_rate, channels, writer->data_bytes);
    ok = fclose(writer->file) == 0 && ok;
    writer->file = NULL;
    return ok;
}
