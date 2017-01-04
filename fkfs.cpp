#include "fkfs.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include <Arduino.h>

static uint8_t fkfs_printf(const char *format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    Serial.print(buffer);
    va_end(args);

    return 0;
}

#define FKFS_FIRST_BLOCK           6000
#define FKFS_SEEK_BLOCKS_MAX       5

// This is for testing wrap around.
#define FKFS_TESTING_LAST_BLOCK    FKFS_FIRST_BLOCK + 100

#define fkfs_log(f, ...)           fkfs_printf(f, __VA_ARGS__)

#define fkfs_log_verbose(f, ...)   fkfs_printf(f, __VA_ARGS__)

static uint32_t crc16_table[16] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400
};

static uint16_t crc16_update(uint16_t start, uint8_t *p, uint16_t n) {
    uint16_t crc = start;
    uint16_t r;

    while (n-- > 0) {
        /* compute checksum of lower four bits of *p */
        r = crc16_table[crc & 0xF];
        crc = (crc >> 4) & 0x0FFF;
        crc = crc ^ r ^ crc16_table[*p & 0xF];

        /* now compute checksum of upper four bits of *p */
        r = crc16_table[crc & 0xF];
        crc = (crc >> 4) & 0x0FFF;
        crc = crc ^ r ^ crc16_table[(*p >> 4) & 0xF];

        p++;
    }

    return crc;
}

static uint8_t fkfs_header_crc_valid(fkfs_header_t *header) {
    uint16_t actual = crc16_update(0, (uint8_t *)header, FKFS_HEADER_SIZE_MINUS_CRC);
    return header->crc == actual;
}

static uint8_t fkfs_header_crc_update(fkfs_header_t *header) {
    uint16_t actual = crc16_update(0, (uint8_t *)header, FKFS_HEADER_SIZE_MINUS_CRC);
    header->crc = actual;
    return actual;
}

uint8_t fkfs_create(fkfs_t *fs) {
    memzero(fs, sizeof(fkfs_t));

    return true;
}

uint8_t fkfs_initialize_file(fkfs_t *fs, uint8_t fileNumber, uint8_t priority, uint8_t sync, const char *name) {
    fs->files[fileNumber].sync = sync;
    fs->files[fileNumber].priority = priority;

    fkfs_file_t *file = &fs->header.files[fileNumber];
    strncpy(file->name, name, sizeof(file->name));
    file->version = random(UINT16_MAX);
    file->startBlock = FKFS_FIRST_BLOCK;

    return true;
}

static uint8_t fkfs_header_write(fkfs_t *fs, uint8_t *temp) {
    fkfs_header_t *headers = (fkfs_header_t *)temp;

    fkfs_header_crc_update(&fs->header);

    memcpy((void *)&headers[fs->headerIndex], (void *)&fs->header, sizeof(fkfs_header_t));

    if (!sd_raw_write_block(&fs->sd, 0, (uint8_t *)temp)) {
        return false;
    }

    return true;
}

static uint8_t fkfs_block_ensure(fkfs_t *fs, uint32_t block) {
    if (fs->cachedBlockDirty) {
    }

    if (fs->cachedBlockNumber != block) {
        if (!sd_raw_read_block(&fs->sd, block, (uint8_t *)fs->buffer)) {
            return false;
        }
        fs->cachedBlockNumber = block;
        fs->cachedBlockDirty = false;
    }
    return true;
}

uint8_t fkfs_initialize(fkfs_t *fs, bool wipe) {
    fkfs_header_t *headers = (fkfs_header_t *)fs->buffer;

    fs->numberOfBlocks = sd_raw_card_size(&fs->sd);

    memzero(fs->buffer, sizeof(SD_RAW_BLOCK_SIZE));

    if (!sd_raw_read_block(&fs->sd, 0, (uint8_t *)fs->buffer)) {
        return false;
    }

    // If both checksums fail, then we're on a new card.
    // TODO: May want to make this configurable?
    if ((!fkfs_header_crc_valid(&headers[0]) &&
         !fkfs_header_crc_valid(&headers[1])) || wipe) {

        fkfs_printf("fkfs: initialize/wipe\r\n");

        // New filesystem... initialize a blank header and new versions of all files.
        for (uint8_t i = 0; i < FKFS_FILES_MAX; ++i) {
            fs->header.files[i].version = random(UINT16_MAX);
            fkfs_printf("file.version = %d\r\n", fs->header.files[i].version);
        }
        fs->header.block = FKFS_FIRST_BLOCK;
    }
    else {
        if (!fkfs_header_crc_valid(&headers[1])) {
            fs->headerIndex = 0;
        }
        else if (!fkfs_header_crc_valid(&headers[0])) {
            fs->headerIndex = 1;
        }
        else if (headers[0].generation > headers[1].generation) {
            fs->headerIndex = 0;
        }
        else {
            fs->headerIndex = 1;
        }

        memcpy((void *)&fs->header, (void *)&headers[fs->headerIndex], sizeof(fkfs_header_t));
    }

    return true;
}

static uint16_t fkfs_block_crc(fkfs_t *fs, fkfs_file_t *file, fkfs_entry_t *entry, uint8_t *data) {
    uint16_t crc = file->version;

    crc = crc16_update(crc, (uint8_t *)entry, FKFS_ENTRY_SIZE_MINUS_CRC);
    crc = crc16_update(crc, (uint8_t *)data, entry->size);

    return crc;
}

#define FKFS_OFFSET_SEARCH_STATUS_GOOD      0
#define FKFS_OFFSET_SEARCH_STATUS_FILE      1
#define FKFS_OFFSET_SEARCH_STATUS_SIZE      2
#define FKFS_OFFSET_SEARCH_STATUS_CRC       3
#define FKFS_OFFSET_SEARCH_STATUS_PRIORITY  4
#define FKFS_OFFSET_SEARCH_STATUS_EOB       5

typedef struct fkfs_offset_search_t {
    uint16_t offset;
    uint8_t status;
} fkfs_offset_search_t;

static uint8_t fkfs_block_check(fkfs_t *fs, uint8_t *ptr) {
    fkfs_entry_t *entry = (fkfs_entry_t *)ptr;

    if (entry->file >= FKFS_FILES_MAX) {
        return FKFS_OFFSET_SEARCH_STATUS_SIZE;
    }

    if (entry->size == 0 || entry->size >= SD_RAW_BLOCK_SIZE ||
        entry->available == 0 || entry->available >= SD_RAW_BLOCK_SIZE) {
        return FKFS_OFFSET_SEARCH_STATUS_SIZE;
    }

    fkfs_file_t *blockFile = &fs->header.files[entry->file];
    uint8_t *data = ptr + sizeof(fkfs_entry_t);
    uint16_t expected = fkfs_block_crc(fs, blockFile, entry, data);
    if (entry->crc != expected) {
        return FKFS_OFFSET_SEARCH_STATUS_CRC;
    }

    return FKFS_OFFSET_SEARCH_STATUS_GOOD;
}

static uint8_t fkfs_block_available_offset(fkfs_t *fs, fkfs_file_t *file, uint8_t priority, uint16_t required, uint8_t *buffer, fkfs_offset_search_t *search) {
    uint8_t *iter = buffer + search->offset;
    fkfs_entry_t *entry = (fkfs_entry_t *)iter;

    fkfs_log_verbose("fkfs: block_available_offset(%d, %d) ", search->offset, required);

    do {
        fkfs_log_verbose("%d ", search->offset);

        search->status = fkfs_block_check(fs, iter);

        switch (search->status) {
        case FKFS_OFFSET_SEARCH_STATUS_FILE:
            fkfs_log_verbose("FILE %d\r\n", search->offset);
            break;
        case FKFS_OFFSET_SEARCH_STATUS_SIZE:
            fkfs_log_verbose("SIZE %d\r\n", search->offset);
            return true;
        case FKFS_OFFSET_SEARCH_STATUS_CRC:
            fkfs_log_verbose("CRC %d\r\n", search->offset);
            return true;
        case FKFS_OFFSET_SEARCH_STATUS_GOOD:
            break;
        }

        // We have precedence over this entry?
        uint8_t blockPriority = fs->files[entry->file].priority;
        if (blockPriority >= priority) {
            if (entry->available >= required) {
                search->status = FKFS_OFFSET_SEARCH_STATUS_PRIORITY;
                fkfs_log_verbose(" [%d > %d][%d >= %d] PRI\r\n",
                                 blockPriority, priority,
                                 entry->available, required);
                return true;
            }
        }

        uint16_t occupied = sizeof(fkfs_entry_t) + entry->available;

        search->offset += occupied;
        iter = buffer + search->offset;
        entry = (fkfs_entry_t *)iter;
    }
    while (search->offset + required < SD_RAW_BLOCK_SIZE);

    fkfs_log_verbose("EOB %d\r\n", search->offset);
    search->status = FKFS_OFFSET_SEARCH_STATUS_EOB;

    return false;
}

static uint8_t fkfs_fsync(fkfs_t *fs) {
    if (fs->cachedBlockDirty) {
        if (!sd_raw_write_block(&fs->sd, fs->header.block, (uint8_t *)fs->buffer)) {
            return false;
        }
        fs->cachedBlockNumber = UINT32_MAX;
        fs->cachedBlockDirty = false;
    }

    fs->header.generation++;
    fs->headerIndex = (fs->headerIndex + 1) % 2;

    if (!fkfs_header_write(fs, fs->buffer)) {
        return false;
    }

    fs->cachedBlockNumber = UINT32_MAX;
    fs->cachedBlockDirty = false;

    fkfs_printf("fkfs: sync!\r\n");

    return true;
}

static uint8_t fkfs_file_allocate_block(fkfs_t *fs, uint8_t fileNumber, uint16_t required, uint16_t size, fkfs_entry_t *entry) {
    fkfs_file_t *file = &fs->header.files[fileNumber];
    uint16_t newOffset = fs->header.offset;
    uint16_t visitedBlocks = 0;
    uint32_t newBlock = fs->header.block;

    fkfs_log_verbose("fkfs: file_allocate_block(%d, %d)\r\n", fileNumber, required);

    do {
        if (required + newOffset >= SD_RAW_BLOCK_SIZE) {
            fkfs_log_verbose("fkfs: new block #%d required=%d offset=%d\r\n",
                             fs->header.block + 1, required, newOffset);

            if (fs->cachedBlockDirty) {
                if (!fkfs_fsync(fs)) {
                    return false;
                }
            }

            fs->header.block++;
            fs->header.offset = newOffset = 0;
            visitedBlocks++;

            // Wrap around logic.
            if (fs->header.block == fs->numberOfBlocks - 2 || fs->header.block == FKFS_TESTING_LAST_BLOCK) {
                fs->header.block = FKFS_FIRST_BLOCK;
            }
        }

        if (fs->cachedBlockNumber != fs->header.block) {
            if (!sd_raw_read_block(&fs->sd, fs->header.block, (uint8_t *)fs->buffer)) {
                return false;
            }
            fs->cachedBlockNumber = fs->header.block;
            fs->cachedBlockDirty = false;
        }

        fkfs_offset_search_t search = { 0 };
        search.offset = newOffset;
        if (fkfs_block_available_offset(fs, file, fs->files[fileNumber].priority, required, fs->buffer, &search)) {
            fs->header.offset = search.offset;
            return true;
        }
        else {
            newOffset = SD_RAW_BLOCK_SIZE; // Force a move to the following block.
        }
    }
    while (visitedBlocks < FKFS_SEEK_BLOCKS_MAX);

    return false;
}

uint8_t fkfs_file_append(fkfs_t *fs, uint8_t fileNumber, uint16_t size, uint8_t *data) {
    fkfs_entry_t entry = { 0 };
    fkfs_file_t *file = &fs->header.files[fileNumber];

    uint16_t required = sizeof(fkfs_entry_t) + size;
    if (size == 0 || required >= SD_RAW_BLOCK_SIZE) {
        return false;
    }

    /*
    fkfs_log("fkfs: allocating f#%d.%-3d.%-5d %3d[%-3d] need=%d\r\n",
             fileNumber, fs->files[fileNumber].priority, file->version,
             fs->header.block, fs->header.offset, required);
    */
    if (!fkfs_file_allocate_block(fs, fileNumber, required, size, &entry)) {
        return false;
    }

    fkfs_log("fkfs: allocated  f#%d.%-3d.%-5d %3d[%-3d -> %-3d] %d\r\n",
             fileNumber, fs->files[fileNumber].priority, file->version,
             fs->header.block, fs->header.offset, fs->header.offset + required,
             SD_RAW_BLOCK_SIZE - (fs->header.offset + required));

    entry.file = fileNumber;
    entry.size = size;
    entry.available = size;
    entry.crc = fkfs_block_crc(fs, file, &entry, data);

    // TODO: Maybe just cast the buffer to this?
    memcpy(((uint8_t *)fs->buffer) + fs->header.offset, (uint8_t *)&entry, sizeof(fkfs_entry_t));
    memcpy(((uint8_t *)fs->buffer) + fs->header.offset + sizeof(fkfs_entry_t), data, size);

    fs->cachedBlockDirty = true;
    fs->header.offset += required;

    if (fs->files[fileNumber].sync) {
        if (!fkfs_fsync(fs)) {
            return false;
        }
    }

    return true;
}

uint8_t fkfs_file_truncate(fkfs_t *fs, uint8_t fileNumber) {
    fkfs_file_t *file = &fs->header.files[fileNumber];

    file->version++;
    file->startBlock = fs->header.block;

    return true;
}

uint8_t fkfs_file_iterate(fkfs_t *fs, uint8_t fileNumber, fkfs_file_iter_t *iter) {
    fkfs_file_t *file = &fs->header.files[fileNumber];

    if (iter->block == 0) {
        iter->block = file->startBlock;
        iter->offset = 0;
    }

    do {
        if (!fkfs_block_ensure(fs, iter->block)) {
            return false;
        }

        uint8_t *ptr = fs->buffer + iter->offset;
        if (fkfs_block_check(fs, ptr) == FKFS_OFFSET_SEARCH_STATUS_GOOD) {
            fkfs_entry_t *entry = (fkfs_entry_t *)ptr;
            if (entry->file == fileNumber) {
                iter->size = entry->size;
                iter->data = ptr + sizeof(fkfs_entry_t);
                iter->offset += entry->available;
                return true;
            }

            iter->offset += entry->available;
        }
        else {
            iter->block++;
            iter->offset = 0;
        }

    }
    while (true);

    return iter->block <= fs->header.block;
}

uint8_t fkfs_log_statistics(fkfs_t *fs) {
    fkfs_printf("fkfs: index=%d gen=%d block=%d offset=%d\r\n",
                fs->headerIndex, fs->header.generation,
                fs->header.block, fs->header.offset);
    return true;
}
