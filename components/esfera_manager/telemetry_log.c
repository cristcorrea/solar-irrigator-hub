#include "esfera_manager.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "time_sync.h"

#define TELEMETRY_LABEL "telemetry"
#define SECTOR_SIZE 4096U
#define SECTOR_COUNT CONFIG_TELEMETRY_LOG_SECTOR_COUNT
#define ACK_WORDS 8U
#define RECORDS_PER_SECTOR 202U
#define PAGE_MAX 100U
#define PAGE_JSON_BASE_SIZE 128U
#define PAGE_JSON_ITEM_SIZE 160U
#define WRITE_QUEUE_LEN 64U
#define SECTOR_MAGIC 0x5347544cUL

typedef struct __attribute__((packed)) {
    uint32_t ts;
    uint8_t mac[6];
    int16_t hum;
    int16_t temp;
    uint16_t vbat;
    uint8_t riego;
    uint8_t flags;
    uint16_t crc;
} telemetry_rec_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t reserved;
    uint32_t ack[ACK_WORDS];
} sector_header_t;

typedef struct {
    bool valid;
    bool sealed;
    uint32_t seq;
    uint16_t used;
    uint32_t ack[ACK_WORDS];
} sector_info_t;

typedef struct {
    uint8_t sector;
    uint8_t slot;
    uint32_t seq;
} page_pos_t;

typedef struct {
    telemetry_rec_t record;
    uint32_t generation;
#if CONFIG_TELEMETRY_LOG_TEST_MODE
    bool barrier;
    SemaphoreHandle_t completion;
    esp_err_t *completion_result;
    size_t *completion_written;
#endif
} queued_record_t;

_Static_assert(sizeof(telemetry_rec_t) == 20, "telemetry record must be 20 bytes");
_Static_assert(sizeof(sector_header_t) == 44, "sector header must be 44 bytes");
_Static_assert(SECTOR_COUNT >= 3 && SECTOR_COUNT <= 128,
               "telemetry sector count must be between 3 and 128");

static const char *TAG = "TELEMETRY_LOG";
static const esp_partition_t *s_partition;
static sector_info_t s_sectors[SECTOR_COUNT];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_write_queue;
static int s_write_sector = -1;
static uint16_t s_write_slot;
static size_t s_pending;
static uint32_t s_next_page_id = 1;
static volatile uint32_t s_generation;
static struct {
    bool valid;
    uint32_t page_id;
    uint32_t last_seq;
    size_t count;
    page_pos_t pos[PAGE_MAX];
} s_page;

static uint16_t crc16(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint16_t crc = 0xffff;
    while (len--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool all_ff(const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) if (*p++ != 0xff) return false;
    return true;
}

static esp_err_t range_is_erased(size_t offset, size_t len, bool *erased)
{
    uint8_t buffer[64];
    *erased = true;
    while (len > 0) {
        size_t chunk = len < sizeof(buffer) ? len : sizeof(buffer);
        esp_err_t err = esp_partition_read(s_partition, offset, buffer, chunk);
        if (err != ESP_OK) return err;
        if (!all_ff(buffer, chunk)) {
            *erased = false;
            return ESP_OK;
        }
        offset += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static size_t sector_offset(unsigned sector)
{
    return (size_t)sector * SECTOR_SIZE;
}

static size_t record_offset(unsigned sector, unsigned slot)
{
    return sector_offset(sector) + sizeof(sector_header_t) + slot * sizeof(telemetry_rec_t);
}

static bool bit_pending(const sector_info_t *sector, unsigned slot)
{
    return (sector->ack[slot / 32] & (1UL << (slot % 32))) != 0;
}

static size_t pending_in_sector(const sector_info_t *sector)
{
    size_t count = 0;
    for (unsigned i = 0; i < sector->used; ++i) if (bit_pending(sector, i)) ++count;
    return count;
}

static esp_err_t erase_sector_locked(unsigned sector)
{
    int64_t start = esp_timer_get_time();
    esp_err_t err = esp_partition_erase_range(s_partition, sector_offset(sector), SECTOR_SIZE);
    int64_t elapsed = esp_timer_get_time() - start;
    if (err == ESP_OK) {
        uint8_t probe[32];
        memset(probe, 0, sizeof(probe));
        esp_err_t read_err = esp_partition_read(s_partition, sector_offset(sector),
                                                probe, sizeof(probe));
        if (read_err != ESP_OK || !all_ff(probe, sizeof(probe))) {
            ESP_LOGE(TAG,
                     "Verificación de borrado falló: sector=%u read_status=0x%x bytes=%02X %02X %02X %02X",
                     sector, (unsigned)read_err, probe[0], probe[1], probe[2], probe[3]);
            return read_err != ESP_OK ? read_err : ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGI(TAG,
                 "Borrado verificado: sector=%u bytes=%02X %02X %02X %02X ...",
                 sector, probe[0], probe[1], probe[2], probe[3]);
        if (s_sectors[sector].valid) {
            size_t removed = pending_in_sector(&s_sectors[sector]);
            s_pending = removed > s_pending ? 0 : s_pending - removed;
        }
        memset(&s_sectors[sector], 0, sizeof(s_sectors[sector]));
        ESP_LOGI(TAG, "Sector %u borrado en %lld us", sector, (long long)elapsed);
    }
    return err;
}

static esp_err_t open_sector_locked(unsigned sector, uint32_t seq)
{
    sector_header_t header = {
        .magic = SECTOR_MAGIC,
        .seq = seq,
        .reserved = ~seq,
    };
    for (unsigned i = 0; i < ACK_WORDS; ++i) header.ack[i] = UINT32_MAX;
    esp_err_t err = esp_partition_write(s_partition, sector_offset(sector), &header, sizeof(header));
    if (err != ESP_OK) return err;
    s_sectors[sector].valid = true;
    s_sectors[sector].seq = seq;
    s_sectors[sector].used = 0;
    memcpy(s_sectors[sector].ack, header.ack, sizeof(header.ack));
    s_write_sector = (int)sector;
    s_write_slot = 0;
    return ESP_OK;
}

static esp_err_t ensure_spare_locked(void)
{
    unsigned spare = s_write_sector < 0 ? 1U : ((unsigned)s_write_sector + 1U) % SECTOR_COUNT;
    if (!s_sectors[spare].valid) {
        sector_header_t probe;
        esp_err_t err = esp_partition_read(s_partition, sector_offset(spare), &probe, sizeof(probe));
        if (err != ESP_OK) return err;
        if (all_ff(&probe, sizeof(probe))) return ESP_OK;
    }
    ESP_LOGW(TAG, "Log circular lleno: se sobrescribe el sector más viejo %u", spare);
    return erase_sector_locked(spare);
}

static esp_err_t rotate_locked(void)
{
    unsigned next = s_write_sector < 0 ? 0U : ((unsigned)s_write_sector + 1U) % SECTOR_COUNT;
    uint32_t seq = s_write_sector < 0 ? 1U : s_sectors[s_write_sector].seq + 1U;
    sector_header_t probe;
    esp_err_t probe_err = esp_partition_read(s_partition, sector_offset(next), &probe, sizeof(probe));
    if (probe_err != ESP_OK) return probe_err;
    if (s_sectors[next].valid || !all_ff(&probe, sizeof(probe))) {
        esp_err_t err = erase_sector_locked(next);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = open_sector_locked(next, seq);
    if (err == ESP_OK) err = ensure_spare_locked();
    return err;
}

static esp_err_t write_record_locked(unsigned sector, unsigned slot,
                                     const telemetry_rec_t *record)
{
    size_t relative = record_offset(sector, slot);
    telemetry_rec_t probe;
    memset(&probe, 0, sizeof(probe));
    esp_err_t read_err = esp_partition_read(s_partition, relative, &probe, sizeof(probe));
    bool erased = read_err == ESP_OK && all_ff(&probe, sizeof(probe));

    ESP_LOGI(TAG,
             "Append sector=%u slot=%u abs=0x%08lx rel=0x%05x size=%u erased=%s first=%02X%02X%02X%02X read_status=0x%x (%s)",
             sector, slot, (unsigned long)(s_partition->address + relative),
             (unsigned)relative, (unsigned)sizeof(*record), erased ? "si" : "no",
             probe.ts & 0xff, (probe.ts >> 8) & 0xff,
             (probe.ts >> 16) & 0xff, (probe.ts >> 24) & 0xff,
             (unsigned)read_err, esp_err_to_name(read_err));
    if (read_err != ESP_OK) return read_err;

    esp_err_t err = esp_partition_write(s_partition, relative, record, sizeof(*record));
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Append write fallo: sector=%u slot=%u abs=0x%08lx rel=0x%05x size=%u err=0x%x (%s)",
                 sector, slot, (unsigned long)(s_partition->address + relative),
                 (unsigned)relative, (unsigned)sizeof(*record), (unsigned)err,
                 esp_err_to_name(err));
        return err;
    }

    telemetry_rec_t verify;
    esp_err_t verify_err = esp_partition_read(s_partition, relative, &verify, sizeof(verify));
    if (verify_err != ESP_OK) {
        ESP_LOGE(TAG, "Append readback falló: sector=%u slot=%u err=0x%x (%s)",
                 sector, slot, (unsigned)verify_err, esp_err_to_name(verify_err));
        return verify_err;
    }
    if (memcmp(&verify, record, sizeof(verify)) != 0) {
        ESP_LOGE(TAG, "Append readback no coincide: sector=%u slot=%u err=0x%x",
                 sector, slot, (unsigned)ESP_ERR_INVALID_RESPONSE);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t append_locked(const telemetry_rec_t *record)
{
    if (s_write_sector < 0 || s_write_slot >= RECORDS_PER_SECTOR) {
        esp_err_t err = rotate_locked();
        if (err != ESP_OK) return err;
    }
    esp_err_t err = write_record_locked((unsigned)s_write_sector, s_write_slot, record);
    if (err == ESP_OK) {
        ++s_write_slot;
        s_sectors[s_write_sector].used = s_write_slot;
        ++s_pending;
        return ESP_OK;
    }

    unsigned failed_sector = (unsigned)s_write_sector;
    unsigned failed_slot = s_write_slot;
    ESP_LOGW(TAG, "Fallo escribiendo sector %u slot %u (%s); se rota y reintenta",
             failed_sector, failed_slot, esp_err_to_name(err));
    s_sectors[failed_sector].sealed = true;
    s_write_slot = RECORDS_PER_SECTOR;

    err = rotate_locked();
    if (err != ESP_OK) return err;

    err = write_record_locked((unsigned)s_write_sector, s_write_slot, record);
    if (err == ESP_OK) {
        ++s_write_slot;
        s_sectors[s_write_sector].used = s_write_slot;
        ++s_pending;
        ESP_LOGI(TAG, "Telemetría recuperada en sector %u tras fallo del sector %u",
                 (unsigned)s_write_sector, failed_sector);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Falló también el reintento en sector %u: %s",
             (unsigned)s_write_sector, esp_err_to_name(err));
    s_sectors[s_write_sector].sealed = true;
    s_write_slot = RECORDS_PER_SECTOR;
    return err;
}

static void writer_task(void *arg)
{
    queued_record_t queued;
    for (;;) {
        if (xQueueReceive(s_write_queue, &queued, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err = ESP_FAIL;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
#if CONFIG_TELEMETRY_LOG_TEST_MODE
            if (queued.barrier) {
                err = ESP_OK;
            } else
#endif
            {
                err = queued.generation == s_generation
                          ? append_locked(&queued.record)
                          : ESP_ERR_INVALID_STATE;
            }
            xSemaphoreGive(s_lock);
            if (err == ESP_OK
#if CONFIG_TELEMETRY_LOG_TEST_MODE
                && !queued.barrier
#endif
            ) {
                ESP_LOGI(TAG, "Telemetría almacenada: %02X%02X%02X%02X%02X%02X",
                         queued.record.mac[0], queued.record.mac[1], queued.record.mac[2],
                         queued.record.mac[3], queued.record.mac[4], queued.record.mac[5]);
            } else if (err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "No se pudo guardar telemetría: err=0x%x (%s)",
                         (unsigned)err, esp_err_to_name(err));
            }
        }
#if CONFIG_TELEMETRY_LOG_TEST_MODE
        if (queued.completion_result != NULL) {
            if (err == ESP_OK && !queued.barrier) {
                ++*queued.completion_written;
            } else if (*queued.completion_result == ESP_OK) {
                *queued.completion_result = err;
            }
        }
        if (queued.completion != NULL) xSemaphoreGive(queued.completion);
#endif
    }
}

static bool seq_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static esp_err_t scan_partition(void)
{
    uint32_t newest_seq = 0;
    int newest_sector = -1;
    s_pending = 0;
    memset(s_sectors, 0, sizeof(s_sectors));

    for (unsigned sector = 0; sector < SECTOR_COUNT; ++sector) {
        sector_header_t header;
        esp_err_t err = esp_partition_read(s_partition, sector_offset(sector), &header, sizeof(header));
        if (err != ESP_OK) return err;
        if (header.magic != SECTOR_MAGIC || header.reserved != ~header.seq) continue;

        sector_info_t *info = &s_sectors[sector];
        info->valid = true;
        info->seq = header.seq;
        memcpy(info->ack, header.ack, sizeof(info->ack));
        for (unsigned slot = 0; slot < RECORDS_PER_SECTOR; ++slot) {
            telemetry_rec_t rec;
            err = esp_partition_read(s_partition, record_offset(sector, slot), &rec, sizeof(rec));
            if (err != ESP_OK) return err;
            if (all_ff(&rec, sizeof(rec))) {
                bool tail_erased;
                size_t tail_offset = record_offset(sector, slot);
                err = range_is_erased(tail_offset,
                                      sector_offset(sector + 1U) - tail_offset,
                                      &tail_erased);
                if (err != ESP_OK) return err;
                if (!tail_erased) {
                    ESP_LOGW(TAG, "Zona no borrada desde sector %u slot %u; se sella el sector",
                             sector, slot);
                    info->sealed = true;
                }
                break;
            }
            if (rec.crc != crc16(&rec, offsetof(telemetry_rec_t, crc))) {
                ESP_LOGW(TAG, "CRC inválido en sector %u registro %u; fin del área válida", sector, slot);
                info->sealed = true;
                break;
            }
            ++info->used;
        }
        s_pending += pending_in_sector(info);
        if (newest_sector < 0 || seq_after(info->seq, newest_seq)) {
            newest_seq = info->seq;
            newest_sector = (int)sector;
        }
    }

    s_write_sector = newest_sector;
    s_write_slot = newest_sector < 0 ? 0 :
                   (s_sectors[newest_sector].sealed ? RECORDS_PER_SECTOR : s_sectors[newest_sector].used);
    ESP_LOGI(TAG, "Log reconstruido: head sector=%d slot=%u, pendientes=%u",
             s_write_sector, s_write_slot, (unsigned)s_pending);
    return ensure_spare_locked();
}

static void remove_legacy_blob(void)
{
    nvs_handle_t h;
    if (nvs_open("telemetry", NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_erase_key(h, "pending");
        if (err == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool mac_to_bytes(const char *mac, uint8_t out[6])
{
    if (!mac || strlen(mac) != 12) return false;
    for (unsigned i = 0; i < 6; ++i) {
        int hi = hex_nibble(mac[i * 2]);
        int lo = hex_nibble(mac[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

esp_err_t esfera_manager_init(void)
{
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, TELEMETRY_LABEL);
    if (!s_partition || s_partition->size < SECTOR_SIZE * SECTOR_COUNT) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "Log circular configurado con %u sectores (%u registros utiles)",
             (unsigned)SECTOR_COUNT,
             (unsigned)((SECTOR_COUNT - 1U) * RECORDS_PER_SECTOR));
    s_lock = xSemaphoreCreateMutex();
    s_write_queue = xQueueCreate(WRITE_QUEUE_LEN, sizeof(queued_record_t));
    if (!s_lock || !s_write_queue) return ESP_ERR_NO_MEM;
    remove_legacy_blob();
    s_next_page_id = esp_random();
    if (s_next_page_id == 0) s_next_page_id = 1;
    esp_err_t err = scan_partition();
    if (err != ESP_OK) return err;
    if (xTaskCreate(writer_task, "telemetry_writer", 4096, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t esfera_manager_add(const char *raw_payload, const char *mac_origen)
{
    float h, t, v;
    int r;
    char claimed[13], trailing;
    uint8_t mac[6];
    if (!raw_payload || !mac_to_bytes(mac_origen, mac) ||
        sscanf(raw_payload, "%f,%f,%f,%d %12s %c", &h, &t, &v, &r, claimed, &trailing) != 5 ||
        !isfinite(h) || !isfinite(t) || !isfinite(v) || h < 0 || h > 100 ||
        t < -50 || t > 100 || v < 0 || v > 20 || (r != 0 && r != 1) ||
        strcasecmp(claimed, mac_origen) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    queued_record_t queued = {
        .record = {
            .ts = (uint32_t)time(NULL),
            .hum = (int16_t)lroundf(h * 10.0f),
            .temp = (int16_t)lroundf(t * 10.0f),
            .vbat = (uint16_t)lroundf(v * 1000.0f),
            .riego = (uint8_t)r,
            .flags = time_sync_is_valid() ? 1U : 0U,
        },
        .generation = s_generation,
    };
    memcpy(queued.record.mac, mac, sizeof(queued.record.mac));
    queued.record.crc = crc16(&queued.record, offsetof(telemetry_rec_t, crc));
    return xQueueSend(s_write_queue, &queued, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

#if CONFIG_TELEMETRY_LOG_TEST_MODE
static uint32_t newest_timestamp_locked(void)
{
    uint32_t newest = 0;
    for (unsigned sector = 0; sector < SECTOR_COUNT; ++sector) {
        if (!s_sectors[sector].valid) continue;
        for (unsigned slot = 0; slot < s_sectors[sector].used; ++slot) {
            telemetry_rec_t rec;
            if (esp_partition_read(s_partition, record_offset(sector, slot),
                                   &rec, sizeof(rec)) == ESP_OK &&
                rec.crc == crc16(&rec, offsetof(telemetry_rec_t, crc)) &&
                rec.ts > newest) {
                newest = rec.ts;
            }
        }
    }
    return newest;
}

esp_err_t esfera_manager_debug_fill(size_t count, size_t *written)
{
    static const uint8_t debug_mac[6] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
    if (written == NULL || count == 0 || count > 3000) return ESP_ERR_INVALID_ARG;
    *written = 0;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    uint32_t now = (uint32_t)time(NULL);
    uint32_t newest = newest_timestamp_locked();
    uint32_t first_ts = newest >= now ? newest + 1U : now;
    uint32_t generation = s_generation;
    xSemaphoreGive(s_lock);
    if (first_ts > UINT32_MAX - (uint32_t)(count - 1U)) return ESP_ERR_INVALID_ARG;

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < count; ++i) {
        queued_record_t queued = {
            .record = {
                .ts = first_ts + (uint32_t)i,
                .hum = (int16_t)((i + 1U) * 10U),
                .temp = 200,
                .vbat = 3800,
                .riego = (uint8_t)(i & 1U),
                .flags = time_sync_is_valid() ? 1U : 0U,
            },
            .generation = generation,
            .completion = NULL,
            .completion_result = &result,
            .completion_written = written,
        };
        memcpy(queued.record.mac, debug_mac, sizeof(debug_mac));
        queued.record.crc = crc16(&queued.record, offsetof(telemetry_rec_t, crc));
        if (xQueueSend(s_write_queue, &queued, portMAX_DELAY) != pdTRUE) {
            result = ESP_FAIL;
            break;
        }
    }
    queued_record_t barrier = {
        .generation = generation,
        .barrier = true,
        .completion = done,
        .completion_result = &result,
        .completion_written = written,
    };
    if (xQueueSend(s_write_queue, &barrier, portMAX_DELAY) != pdTRUE ||
        xSemaphoreTake(done, portMAX_DELAY) != pdTRUE) {
        result = ESP_FAIL;
    }
    vSemaphoreDelete(done);
    return result;
}
#endif

static size_t ordered_sectors(uint8_t order[SECTOR_COUNT])
{
    size_t count = 0;
    for (unsigned i = 0; i < SECTOR_COUNT; ++i) if (s_sectors[i].valid) order[count++] = (uint8_t)i;
    for (size_t i = 1; i < count; ++i) {
        uint8_t key = order[i];
        size_t j = i;
        while (j > 0 && seq_after(s_sectors[order[j - 1]].seq, s_sectors[key].seq)) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }
    return count;
}

static bool json_append(char **cursor, size_t *available, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(*cursor, *available, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= *available) return false;
    *cursor += length;
    *available -= (size_t)length;
    return true;
}

#if 0 /* Replaced by direct snprintf serialization below. */
char *esfera_manager_page_json(size_t max, uint32_t *page_id, uint32_t *last_seq, size_t *pend)
{
    if (max == 0) max = 50;
    if (max > PAGE_MAX) max = PAGE_MAX;
    if (!page_id || !last_seq || !pend || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return NULL;

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t heap_min_before = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    uint32_t id = s_next_page_id++;
    if (s_next_page_id == 0) s_next_page_id = 1;
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root); cJSON_Delete(items); xSemaphoreGive(s_lock); return NULL;
    }
    cJSON_AddNumberToObject(root, "page_id", id);
    cJSON_AddNumberToObject(root, "last_seq", 0);
    cJSON_AddNumberToObject(root, "pend", (double)s_pending);
    cJSON_AddItemToObject(root, "items", items);

    s_page.valid = true;
    s_page.page_id = id;
    s_page.last_seq = 0;
    s_page.count = 0;
    uint8_t order[SECTOR_COUNT];
    size_t sector_count = ordered_sectors(order);
    for (size_t oi = 0; oi < sector_count && s_page.count < max; ++oi) {
        unsigned sector = order[oi];
        for (unsigned slot = 0; slot < s_sectors[sector].used && s_page.count < max; ++slot) {
            if (!bit_pending(&s_sectors[sector], slot)) continue;
            telemetry_rec_t rec;
            if (esp_partition_read(s_partition, record_offset(sector, slot), &rec, sizeof(rec)) != ESP_OK) continue;
            cJSON *item = cJSON_CreateObject();
            if (!item) { cJSON_Delete(root); xSemaphoreGive(s_lock); return NULL; }
            char mac[13];
            snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X",
                     rec.mac[0], rec.mac[1], rec.mac[2], rec.mac[3], rec.mac[4], rec.mac[5]);
            time_t stamp = (time_t)rec.ts;
            struct tm tm = {0};
            char timestamp[20];
            localtime_r(&stamp, &tm);
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &tm);
            cJSON_AddStringToObject(item, "mac", mac);
            cJSON_AddNumberToObject(item, "humedad", rec.hum / 10.0);
            cJSON_AddNumberToObject(item, "temperatura", rec.temp / 10.0);
            cJSON_AddNumberToObject(item, "bateria", rec.vbat / 1000.0);
            cJSON_AddNumberToObject(item, "riego", rec.riego);
            cJSON_AddStringToObject(item, "timestamp", timestamp);
            cJSON_AddBoolToObject(item, "tsOk", (rec.flags & 1U) != 0);
            cJSON_AddItemToArray(items, item);
            uint32_t logical_seq = s_sectors[sector].seq * RECORDS_PER_SECTOR + slot;
            s_page.pos[s_page.count++] = (page_pos_t){(uint8_t)sector, (uint8_t)slot, logical_seq};
            s_page.last_seq = logical_seq;
        }
    }
    cJSON_ReplaceItemInObject(root, "last_seq", cJSON_CreateNumber(s_page.last_seq));
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Página %u (%u items): heap libre antes=%u después=%u mínimo=%u (previo=%u)",
             id, (unsigned)s_page.count, (unsigned)heap_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_min_before);
    *page_id = id;
    *last_seq = s_page.last_seq;
    *pend = s_pending;
    xSemaphoreGive(s_lock);
    return json;
}
#endif

char *esfera_manager_page_json(size_t max, uint32_t *page_id, uint32_t *last_seq, size_t *pend)
{
    if (max == 0) max = 50;
    if (max > PAGE_MAX) max = PAGE_MAX;
    if (!page_id || !last_seq || !pend ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return NULL;

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t heap_min_before = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    uint32_t id = s_next_page_id++;
    if (s_next_page_id == 0) s_next_page_id = 1;

    page_pos_t positions[PAGE_MAX];
    size_t count = 0;
    uint32_t page_last_seq = 0;
    uint8_t order[SECTOR_COUNT];
    size_t sector_count = ordered_sectors(order);
    for (size_t oi = 0; oi < sector_count && count < max; ++oi) {
        unsigned sector = order[oi];
        for (unsigned slot = 0; slot < s_sectors[sector].used && count < max; ++slot) {
            if (!bit_pending(&s_sectors[sector], slot)) continue;
            uint32_t logical_seq = s_sectors[sector].seq * RECORDS_PER_SECTOR + slot;
            positions[count++] = (page_pos_t){(uint8_t)sector, (uint8_t)slot, logical_seq};
            page_last_seq = logical_seq;
        }
    }

    size_t remaining_pending = s_pending >= count ? s_pending - count : 0;
    size_t buffer_size = PAGE_JSON_BASE_SIZE + count * PAGE_JSON_ITEM_SIZE;
    char *json = malloc(buffer_size);
    if (json == NULL) {
        xSemaphoreGive(s_lock);
        return NULL;
    }
    char *cursor = json;
    size_t available = buffer_size;
    bool ok = json_append(&cursor, &available,
                          "{\"page_id\":%u,\"last_seq\":%u,\"pend\":%u,\"items\":[",
                          (unsigned)id, (unsigned)page_last_seq,
                          (unsigned)remaining_pending);

    for (size_t i = 0; ok && i < count; ++i) {
        telemetry_rec_t rec;
        page_pos_t pos = positions[i];
        if (esp_partition_read(s_partition, record_offset(pos.sector, pos.slot),
                               &rec, sizeof(rec)) != ESP_OK ||
            rec.crc != crc16(&rec, offsetof(telemetry_rec_t, crc))) {
            ok = false;
            break;
        }
        char mac[13];
        snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X",
                 rec.mac[0], rec.mac[1], rec.mac[2], rec.mac[3], rec.mac[4], rec.mac[5]);
        time_t stamp = (time_t)rec.ts;
        struct tm tm = {0};
        char timestamp[20];
        localtime_r(&stamp, &tm);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &tm);
        ok = json_append(&cursor, &available,
                         "%s{\"mac\":\"%s\",\"humedad\":%.1f,"
                         "\"temperatura\":%.1f,\"bateria\":%.3f,\"riego\":%u,"
                         "\"timestamp\":\"%s\",\"tsOk\":%s}",
                         i == 0 ? "" : ",", mac, rec.hum / 10.0,
                         rec.temp / 10.0, rec.vbat / 1000.0,
                         (unsigned)rec.riego, timestamp,
                         (rec.flags & 1U) != 0 ? "true" : "false");
    }
    ok = ok && json_append(&cursor, &available, "]}");
    if (!ok) {
        free(json);
        xSemaphoreGive(s_lock);
        return NULL;
    }

    s_page.valid = true;
    s_page.page_id = id;
    s_page.last_seq = page_last_seq;
    s_page.count = count;
    memcpy(s_page.pos, positions, count * sizeof(positions[0]));
    ESP_LOGI(TAG, "Página %u (%u items, pend=%u, %u bytes): heap libre antes=%u después=%u mínimo=%u (previo=%u)",
             id, (unsigned)count, (unsigned)remaining_pending,
             (unsigned)(cursor - json), (unsigned)heap_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_min_before);
    *page_id = id;
    *last_seq = page_last_seq;
    *pend = remaining_pending;
    xSemaphoreGive(s_lock);
    return json;
}

esp_err_t esfera_manager_ack(uint32_t page_id, uint32_t last_seq)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    if (!s_page.valid || s_page.page_id != page_id || s_page.last_seq != last_seq) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    for (unsigned sector = 0; sector < SECTOR_COUNT && result == ESP_OK; ++sector) {
        uint32_t clear[ACK_WORDS] = {0};
        for (size_t i = 0; i < s_page.count; ++i) {
            if (s_page.pos[i].sector == sector) clear[s_page.pos[i].slot / 32] |= 1UL << (s_page.pos[i].slot % 32);
        }
        for (unsigned word = 0; word < ACK_WORDS; ++word) {
            if (!clear[word]) continue;
            uint32_t old = s_sectors[sector].ack[word];
            uint32_t updated = old & ~clear[word];
            if (updated == old) continue;
            result = esp_partition_write(s_partition,
                                         sector_offset(sector) + offsetof(sector_header_t, ack) + word * 4,
                                         &updated, sizeof(updated));
            if (result == ESP_OK) {
                s_pending -= __builtin_popcount(old ^ updated);
                s_sectors[sector].ack[word] = updated;
            }
        }
    }
    if (result == ESP_OK) s_page.valid = false;
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t esfera_manager_erase_all(void)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    xQueueReset(s_write_queue);
    ++s_generation;
    esp_err_t err = esp_partition_erase_range(s_partition, 0, s_partition->size);
    if (err == ESP_OK) {
        memset(s_sectors, 0, sizeof(s_sectors));
        s_write_sector = -1;
        s_write_slot = 0;
        s_pending = 0;
        s_page.valid = false;
    }
    xSemaphoreGive(s_lock);
    return err;
}

size_t esfera_manager_count(void)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return 0;
    size_t count = s_pending;
    xSemaphoreGive(s_lock);
    return count;
}
