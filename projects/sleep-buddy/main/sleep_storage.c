#include "sleep_storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "nvs.h"

#define LAST_SUMMARY_VERSION 2
#define NIGHT_RECORD_MAGIC 0x534C504Eu
#define NIGHT_RECORD_VERSION 2
#define CHECKPOINT_MAGIC 0x534C5043u
#define CHECKPOINT_VERSION 1

static const char *TAG = "sleep_storage";

typedef struct {
    uint16_t version;
    uint16_t size;
    sleep_session_t session;
    sleep_summary_t summary;
} last_summary_blob_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
} night_record_header_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint64_t started_unix_s;
    uint64_t ended_unix_s;
    uint64_t monitored_ms;
    uint64_t snore_ms;
    uint64_t longest_event_ms;
    uint32_t event_count;
    float snore_percent;
    float mean_confidence;
    float loudest_dbfs;
    float strongest_probability;
    uint8_t acoustic_score;
    uint8_t reserved[7];
} night_record_v1_t;

typedef struct {
    night_record_v1_t base;
    uint8_t snore_timeline[SLEEP_TIMELINE_BYTES];
} night_record_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    sleep_session_t session;
    sleep_summary_t summary;
    uint32_t checksum;
} checkpoint_v1_t;

static uint32_t s_checkpoint_sequence;

_Static_assert(offsetof(night_record_v1_t, started_unix_s) == sizeof(night_record_header_t),
               "Night record header must stay packed at the front");

static night_record_v2_t make_night_record(const sleep_session_t *session,
                                            const sleep_summary_t *summary)
{
    night_record_v2_t record = {
        .base = {
            .magic = NIGHT_RECORD_MAGIC,
            .version = NIGHT_RECORD_VERSION,
            .size = sizeof(night_record_v2_t),
            .started_unix_s = session->started_unix_s,
            .ended_unix_s = session->ended_unix_s,
            .monitored_ms = summary->monitored_ms,
            .snore_ms = summary->snore_ms,
            .longest_event_ms = summary->longest_event_ms,
            .event_count = summary->event_count,
            .snore_percent = summary->snore_percent,
            .mean_confidence = summary->mean_confidence,
            .loudest_dbfs = summary->loudest_dbfs,
            .strongest_probability = summary->strongest_probability,
            .acoustic_score = summary->acoustic_score,
        },
    };
    memcpy(record.snore_timeline, session->snore_timeline, SLEEP_TIMELINE_BYTES);
    return record;
}

static bool valid_night_record_v1(const night_record_v1_t *record)
{
    return record->magic == NIGHT_RECORD_MAGIC && record->version == 1 &&
           record->size == sizeof(night_record_v1_t) && record->acoustic_score <= 100;
}

static bool valid_night_record_v2(const night_record_v2_t *record)
{
    return record->base.magic == NIGHT_RECORD_MAGIC &&
           record->base.version == NIGHT_RECORD_VERSION &&
           record->base.size == sizeof(night_record_v2_t) &&
           record->base.acoustic_score <= 100;
}

static sleep_night_record_t public_night_record(const night_record_v1_t *record,
                                                 const uint8_t *timeline)
{
    sleep_night_record_t result = {
        .started_unix_s = record->started_unix_s,
        .ended_unix_s = record->ended_unix_s,
        .summary = {
            .monitored_ms = record->monitored_ms,
            .snore_ms = record->snore_ms,
            .longest_event_ms = record->longest_event_ms,
            .event_count = record->event_count,
            .snore_percent = record->snore_percent,
            .mean_confidence = record->mean_confidence,
            .loudest_dbfs = record->loudest_dbfs,
            .strongest_probability = record->strongest_probability,
            .acoustic_score = record->acoustic_score,
        },
    };
    if (timeline) memcpy(result.snore_timeline, timeline, SLEEP_TIMELINE_BYTES);
    return result;
}

static void keep_recent_record(sleep_night_record_t *records, size_t capacity,
                               size_t *count, const sleep_night_record_t *record)
{
    if (*count < capacity) {
        records[(*count)++] = *record;
        return;
    }
    memmove(records, records + 1, (capacity - 1U) * sizeof(*records));
    records[capacity - 1U] = *record;
}

static uint32_t checkpoint_checksum(const checkpoint_v1_t *checkpoint)
{
    const uint8_t *bytes = (const uint8_t *)checkpoint;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(checkpoint_v1_t, checksum); ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool valid_checkpoint(const checkpoint_v1_t *checkpoint)
{
    return checkpoint->magic == CHECKPOINT_MAGIC &&
           checkpoint->version == CHECKPOINT_VERSION &&
           checkpoint->size == sizeof(checkpoint_v1_t) &&
           checkpoint->checksum == checkpoint_checksum(checkpoint);
}

static esp_err_t load_checkpoint(checkpoint_v1_t *latest)
{
    FILE *file = fopen(BSP_SD_MOUNT_POINT "/CURRENT.BIN", "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    checkpoint_v1_t checkpoint;
    bool found = false;
    while (fread(&checkpoint, sizeof(checkpoint), 1, file) == 1) {
        if (!valid_checkpoint(&checkpoint)) continue;
        if (!found || checkpoint.sequence >= latest->sequence) {
            *latest = checkpoint;
            found = true;
        }
    }
    bool read_error = ferror(file);
    fclose(file);
    if (read_error) return ESP_FAIL;
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t write_checkpoint(const sleep_session_t *session,
                                  const sleep_summary_t *summary, const char *mode)
{
    checkpoint_v1_t checkpoint = {
        .magic = CHECKPOINT_MAGIC,
        .version = CHECKPOINT_VERSION,
        .size = sizeof(checkpoint_v1_t),
        .sequence = ++s_checkpoint_sequence,
        .session = *session,
        .summary = *summary,
    };
    checkpoint.checksum = checkpoint_checksum(&checkpoint);
    FILE *file = fopen(BSP_SD_MOUNT_POINT "/CURRENT.BIN", mode);
    if (!file) return ESP_FAIL;
    bool ok = fwrite(&checkpoint, sizeof(checkpoint), 1, file) == 1;
    if (fflush(file) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t persist_last_summary(const sleep_session_t *session,
                                      const sleep_summary_t *summary)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sleep_ai", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    last_summary_blob_t blob = {
        .version = LAST_SUMMARY_VERSION,
        .size = sizeof(last_summary_blob_t),
        .session = *session,
        .summary = *summary,
    };
    err = nvs_set_blob(handle, "last_summary", &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t sleep_storage_append_event(const snore_event_t *event)
{
    FILE *file = fopen(BSP_SD_MOUNT_POINT "/EVENTS.JL", "a");
    if (!file) return ESP_FAIL;
    int written = fprintf(file,
        "{\"start_ms\":%llu,\"end_ms\":%llu,\"confidence\":%.4f,\"peak_dbfs\":%.2f}\n",
        (unsigned long long)event->started_ms, (unsigned long long)event->ended_ms,
        event->mean_probability, event->peak_dbfs);
    bool ok = written > 0;
    if (fflush(file) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t sleep_storage_append_summary(const sleep_session_t *session, const sleep_summary_t *summary)
{
    night_record_v2_t record = make_night_record(session, summary);
    FILE *binary = fopen(BSP_SD_MOUNT_POINT "/NIGHTS.BIN", "ab");
    if (!binary) return ESP_FAIL;
    bool binary_ok = fwrite(&record, sizeof(record), 1, binary) == 1;
    if (fflush(binary) != 0) binary_ok = false;
    if (fclose(binary) != 0) binary_ok = false;
    if (!binary_ok) return ESP_FAIL;

    FILE *file = fopen(BSP_SD_MOUNT_POINT "/NIGHTS.JL", "a");
    if (file) {
        int written = fprintf(file,
            "{\"start_unix_s\":%llu,\"end_unix_s\":%llu,\"monitored_ms\":%llu,"
            "\"snore_ms\":%llu,\"events\":%lu,\"longest_ms\":%llu,"
            "\"snore_percent\":%.3f,\"mean_confidence\":%.4f,\"peak_dbfs\":%.2f,"
            "\"strongest_probability\":%.4f,\"acoustic_score\":%u}\n",
            (unsigned long long)session->started_unix_s, (unsigned long long)session->ended_unix_s,
            (unsigned long long)summary->monitored_ms, (unsigned long long)summary->snore_ms,
            (unsigned long)summary->event_count, (unsigned long long)summary->longest_event_ms,
            summary->snore_percent, summary->mean_confidence, summary->loudest_dbfs,
            summary->strongest_probability, summary->acoustic_score);
        bool json_ok = written > 0;
        if (fflush(file) != 0) json_ok = false;
        if (fclose(file) != 0) json_ok = false;
        if (!json_ok) ESP_LOGW(TAG, "Human-readable night log could not be updated");
    } else {
        ESP_LOGW(TAG, "Human-readable night log could not be opened");
    }

    esp_err_t cache_result = persist_last_summary(session, summary);
    if (cache_result != ESP_OK) {
        ESP_LOGW(TAG, "NVS summary cache failed: %s", esp_err_to_name(cache_result));
    }
    // NIGHTS.BIN is the authoritative history. JSON and NVS are optional
    // convenience copies and must not turn a successful microSD save into an error.
    return ESP_OK;
}

esp_err_t sleep_storage_load_last_summary(sleep_session_t *session, sleep_summary_t *summary)
{
    if (!session || !summary) return ESP_ERR_INVALID_ARG;
    sleep_night_record_t latest;
    size_t count = 0;
    if (sleep_storage_load_recent(&latest, 1, &count) == ESP_OK && count == 1) {
        memset(session, 0, sizeof(*session));
        session->started_unix_s = latest.started_unix_s;
        session->ended_unix_s = latest.ended_unix_s;
        memcpy(session->snore_timeline, latest.snore_timeline, SLEEP_TIMELINE_BYTES);
        *summary = latest.summary;
        return ESP_OK;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sleep_ai", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    last_summary_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, "last_summary", &blob, &size);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    if (size != sizeof(blob) || blob.version != LAST_SUMMARY_VERSION ||
        blob.size != sizeof(last_summary_blob_t)) {
        return ESP_ERR_INVALID_VERSION;
    }
    *session = blob.session;
    *summary = blob.summary;
    return ESP_OK;
}

esp_err_t sleep_storage_load_recent(sleep_night_record_t *records, size_t capacity, size_t *count)
{
    if (!records || !capacity || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    FILE *file = fopen(BSP_SD_MOUNT_POINT "/NIGHTS.BIN", "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    night_record_header_t header;
    while (fread(&header, sizeof(header), 1, file) == 1) {
        if (header.magic != NIGHT_RECORD_MAGIC || header.size < sizeof(header) || header.size > 4096U) {
            fclose(file);
            return *count ? ESP_OK : ESP_FAIL;
        }
        sleep_night_record_t public_record;
        bool valid = false;
        if (header.version == 1 && header.size == sizeof(night_record_v1_t)) {
            night_record_v1_t record = {
                .magic = header.magic, .version = header.version, .size = header.size,
            };
            size_t remaining = sizeof(record) - sizeof(header);
            if (fread((uint8_t *)&record + sizeof(header), remaining, 1, file) != 1) break;
            if (valid_night_record_v1(&record)) {
                public_record = public_night_record(&record, NULL);
                valid = true;
            }
        } else if (header.version == NIGHT_RECORD_VERSION && header.size == sizeof(night_record_v2_t)) {
            night_record_v2_t record = {
                .base = {.magic = header.magic, .version = header.version, .size = header.size},
            };
            size_t remaining = sizeof(record) - sizeof(header);
            if (fread((uint8_t *)&record + sizeof(header), remaining, 1, file) != 1) break;
            if (valid_night_record_v2(&record)) {
                public_record = public_night_record(&record.base, record.snore_timeline);
                valid = true;
            }
        } else if (fseek(file, header.size - sizeof(header), SEEK_CUR) != 0) {
            break;
        }
        if (valid) keep_recent_record(records, capacity, count, &public_record);
    }
    bool read_error = ferror(file);
    fclose(file);
    return read_error ? ESP_FAIL : (*count ? ESP_OK : ESP_ERR_NOT_FOUND);
}

esp_err_t sleep_storage_checkpoint_begin(const sleep_session_t *session,
                                         const sleep_summary_t *summary)
{
    if (!session || !summary) return ESP_ERR_INVALID_ARG;
    s_checkpoint_sequence = 0;
    return write_checkpoint(session, summary, "wb");
}

esp_err_t sleep_storage_checkpoint_update(const sleep_session_t *session,
                                          const sleep_summary_t *summary)
{
    if (!session || !summary) return ESP_ERR_INVALID_ARG;
    if (!s_checkpoint_sequence) {
        checkpoint_v1_t latest = {0};
        if (load_checkpoint(&latest) == ESP_OK) s_checkpoint_sequence = latest.sequence;
    }
    return write_checkpoint(session, summary, "ab");
}

esp_err_t sleep_storage_checkpoint_clear(void)
{
    s_checkpoint_sequence = 0;
    if (unlink(BSP_SD_MOUNT_POINT "/CURRENT.BIN") == 0 || errno == ENOENT) return ESP_OK;
    return ESP_FAIL;
}

esp_err_t sleep_storage_recover_interrupted(void)
{
    checkpoint_v1_t checkpoint = {0};
    esp_err_t err = load_checkpoint(&checkpoint);
    if (err != ESP_OK) return err;

    // A checkpoint written immediately after START contains no useful night yet.
    if (checkpoint.summary.monitored_ms < 1000U) return sleep_storage_checkpoint_clear();

    sleep_night_record_t latest;
    size_t count = 0;
    bool already_saved = sleep_storage_load_recent(&latest, 1, &count) == ESP_OK && count == 1 &&
                         latest.started_unix_s == checkpoint.session.started_unix_s &&
                         latest.ended_unix_s == checkpoint.session.ended_unix_s &&
                         latest.summary.monitored_ms == checkpoint.summary.monitored_ms &&
                         latest.summary.event_count == checkpoint.summary.event_count;
    if (!already_saved) {
        checkpoint.session.active = false;
        err = sleep_storage_append_summary(&checkpoint.session, &checkpoint.summary);
        if (err != ESP_OK) return err;
        ESP_LOGW(TAG, "Recovered interrupted night from microSD checkpoint");
    }
    return sleep_storage_checkpoint_clear();
}
