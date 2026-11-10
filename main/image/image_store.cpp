#include "image_store.h"

#include <algorithm>
#include <cstring>
#include "crc32.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

namespace {
constexpr uint32_t kMagic = 0x494D4731; // IMG1
constexpr uint32_t kVersion = 1;
constexpr size_t kMaximumImageBytes = 1024 * 1024;
constexpr char kPartitionA[] = "image_a";
constexpr char kPartitionB[] = "image_b";

struct RecordHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t length;
    uint32_t crc32;
    uint32_t fit;
    uint32_t reserved[2];
};
static_assert(sizeof(RecordHeader) == 32);

const esp_partition_t *slots[2] = {};
int active_slot = -1;
uint32_t active_generation = 0;

bool valid_fit(uint32_t value)
{
    return value <= static_cast<uint32_t>(display::FitMode::stretch);
}

bool read_header(int slot, RecordHeader &header)
{
    if (slots[slot] == nullptr || esp_partition_read(slots[slot], 0, &header, sizeof(header)) != ESP_OK) {
        return false;
    }
    return header.magic == kMagic && header.version == kVersion &&
           header.length <= kMaximumImageBytes && header.length + sizeof(header) <= slots[slot]->size &&
           valid_fit(header.fit);
}

esp_err_t write_payload(const esp_partition_t *partition, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset + 4 <= size) {
        const size_t chunk = std::min<size_t>(4096, (size - offset) & ~size_t(3));
        if (chunk == 0) break;
        ESP_RETURN_ON_ERROR(esp_partition_write(partition, sizeof(RecordHeader) + offset,
                                                data + offset, chunk), "image-store", "Image payload write failed");
        offset += chunk;
    }
    if (offset < size) {
        uint8_t tail[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        const size_t remaining = size - offset;
        std::memcpy(tail, data + offset, remaining);
        ESP_RETURN_ON_ERROR(esp_partition_write(partition, sizeof(RecordHeader) + offset, tail, sizeof(tail)),
                            "image-store", "Final image payload write failed");
    }
    return ESP_OK;
}

constexpr size_t round_up_write_size(size_t size)
{
    return (size + 3) & ~size_t(3);
}

// Number of leading sectors that must be erased before writing this record.
// write_payload rounds the payload up to four bytes for its final write.
constexpr size_t record_erase_size(size_t payload_size, size_t sector_size)
{
    const size_t written = sizeof(RecordHeader) + round_up_write_size(payload_size);
    return ((written + sector_size - 1) / sector_size) * sector_size;
}

static_assert(record_erase_size(1, 4096) == 4096);
static_assert(record_erase_size(4096 - sizeof(RecordHeader), 4096) == 4096);
static_assert(record_erase_size(4096 - sizeof(RecordHeader) + 1, 4096) == 8192);
static_assert(record_erase_size(kMaximumImageBytes, 4096) == kMaximumImageBytes + 4096);

esp_err_t commit_header(const esp_partition_t *partition, RecordHeader &header)
{
    header.magic = 0xFFFFFFFF;
    ESP_RETURN_ON_ERROR(esp_partition_write(partition, sizeof(header.magic),
                                            reinterpret_cast<const uint8_t *>(&header) + sizeof(header.magic),
                                            sizeof(header) - sizeof(header.magic)),
                        "image-store", "Image metadata write failed");
    const uint32_t commit_magic = kMagic;
    return esp_partition_write(partition, 0, &commit_magic, sizeof(commit_magic));
}
}

namespace image_store {
esp_err_t initialize()
{
    slots[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), kPartitionA);
    slots[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), kPartitionB);
    if (slots[0] == nullptr || slots[1] == nullptr || slots[0]->size < kMaximumImageBytes + sizeof(RecordHeader) + 4 ||
        slots[1]->size < kMaximumImageBytes + sizeof(RecordHeader) + 4) {
        return ESP_ERR_NOT_FOUND;
    }

    RecordHeader headers[2] = {};
    const bool valid_a = read_header(0, headers[0]);
    const bool valid_b = read_header(1, headers[1]);
    if (valid_a && valid_b) active_slot = static_cast<int32_t>(headers[1].generation - headers[0].generation) > 0 ? 1 : 0;
    else if (valid_a) active_slot = 0;
    else if (valid_b) active_slot = 1;
    if (active_slot >= 0) active_generation = headers[active_slot].generation;
    return ESP_OK;
}

esp_err_t save(const uint8_t *data, size_t size, display::FitMode fit)
{
    if (slots[0] == nullptr || slots[1] == nullptr) return ESP_ERR_INVALID_STATE;
    if (data == nullptr || size == 0 || size > kMaximumImageBytes) return ESP_ERR_INVALID_SIZE;
    const int target = active_slot == 0 ? 1 : 0;
    const esp_partition_t *partition = slots[target];
    // Erase only the header and payload sectors. The header is erased before
    // the payload is written and its magic is committed last, so an interrupted
    // write still leaves this slot invalid and the previous image recoverable.
    const size_t erase_size = record_erase_size(size, partition->erase_size);
    ESP_RETURN_ON_FALSE(erase_size <= partition->size, ESP_ERR_INVALID_SIZE,
                        "image-store", "Image record exceeds the flash slot");
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(partition, 0, erase_size), "image-store", "Image slot erase failed");
    ESP_RETURN_ON_ERROR(write_payload(partition, data, size), "image-store", "Image payload write failed");

    RecordHeader header = {};
    header.magic = kMagic;
    header.version = kVersion;
    header.generation = active_generation + 1;
    header.length = static_cast<uint32_t>(size);
    header.crc32 = image::crc32(data, size);
    header.fit = static_cast<uint32_t>(fit);
    ESP_RETURN_ON_ERROR(commit_header(partition, header), "image-store", "Image record commit failed");
    active_slot = target;
    active_generation = header.generation;
    return ESP_OK;
}

esp_err_t load(uint8_t **data, size_t *size, display::FitMode *fit)
{
    if (data == nullptr || size == nullptr || fit == nullptr) return ESP_ERR_INVALID_ARG;
    *data = nullptr;
    *size = 0;
    if (active_slot < 0) return ESP_ERR_NOT_FOUND;
    RecordHeader header = {};
    if (!read_header(active_slot, header)) return ESP_ERR_INVALID_CRC;
    if (header.length == 0) return ESP_ERR_NOT_FOUND;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(header.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) return ESP_ERR_NO_MEM;
    const esp_err_t error = esp_partition_read(slots[active_slot], sizeof(header), buffer, header.length);
    if (error != ESP_OK || image::crc32(buffer, header.length) != header.crc32) {
        heap_caps_free(buffer);
        return error == ESP_OK ? ESP_ERR_INVALID_CRC : error;
    }
    *data = buffer;
    *size = header.length;
    *fit = static_cast<display::FitMode>(header.fit);
    return ESP_OK;
}

esp_err_t clear()
{
    if (slots[0] == nullptr || slots[1] == nullptr) return ESP_ERR_INVALID_STATE;
    const int target = active_slot == 0 ? 1 : 0;
    const esp_partition_t *partition = slots[target];
    // The tombstone is header-only; erase one sector instead of the whole slot.
    ESP_RETURN_ON_FALSE(partition->erase_size >= sizeof(RecordHeader), ESP_ERR_INVALID_SIZE,
                        "image-store", "Image header does not fit in one erase sector");
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(partition, 0, partition->erase_size),
                        "image-store", "Image clear marker erase failed");
    RecordHeader tombstone = {};
    tombstone.magic = kMagic;
    tombstone.version = kVersion;
    tombstone.generation = active_generation + 1;
    tombstone.fit = static_cast<uint32_t>(display::FitMode::contain);
    ESP_RETURN_ON_ERROR(commit_header(partition, tombstone), "image-store", "Image clear marker commit failed");
    active_slot = target;
    active_generation = tombstone.generation;
    return ESP_OK;
}
}
