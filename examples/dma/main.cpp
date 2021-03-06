#include "sd_raw.h"
#include "sd_raw_dma.h"
#undef LOW
#undef HIGH
#undef min
#undef max

#include <stdio.h>
#include <Arduino.h>
#include <SPI.h>

#define FKFS_FILE_LOG                     0
#define FKFS_FILE_DATA                    1
#define FKFS_FILE_PRIORITY_LOWEST         255
#define FKFS_FILE_PRIORITY_HIGHEST        0

#define SD_PIN_CS1                        10
#define SD_PIN_CS2                        4

#define DATA_LENGTH                       512

void log_buffer(uint8_t *buffer) {
    for (uint32_t i = 0; i < DATA_LENGTH; i++) {
        Serial.print(buffer[i], HEX); Serial.print(' ');
        if ((i % 32) == 31)
            Serial.println();
    }
}

void setup() {
    Serial.begin(115200);

    // LoRa radio's CS.
    pinMode(8, OUTPUT);
    digitalWrite(8, HIGH);
    pinMode(SD_PIN_CS1, OUTPUT);
    digitalWrite(SD_PIN_CS1, HIGH);
    pinMode(SD_PIN_CS2, OUTPUT);
    digitalWrite(SD_PIN_CS2, HIGH);

    while (!Serial) {
        delay(100);
    }

    sd_raw_t sd;

    if (!sd_raw_initialize(&sd, SD_PIN_CS1)) {
        if (!sd_raw_initialize(&sd, SD_PIN_CS2)) {
            Serial.println("sd_raw_initialize failed");
            while (true) {
                delay(100);
            }
        }
    }

    uint8_t status = 0;
    uint8_t source_memory[DATA_LENGTH];
    uint8_t destination_memory[DATA_LENGTH];
    uint32_t block = 0;

    if (false) {
        for (uint16_t i = 0; i < DATA_LENGTH; ++i) {
            source_memory[i] = 0xc5;
            destination_memory[i] = 0xff;
        }

        if (!sd_raw_write_block(&sd, block, source_memory)) {
            Serial.println("sd_raw_write_block block failed");
            while (true) {
                delay(100);
            }
        }

        if (!sd_raw_read_block(&sd, block, destination_memory)) {
            Serial.println("sd_raw_read_block block failed");
            while (true) {
                delay(100);
            }
        } else {
            Serial.println("write_block/read_block");
            log_buffer(destination_memory);
        }
    } else {
        for (uint16_t i = 0; i < DATA_LENGTH; ++i) {
            source_memory[i] = 0xff;
            destination_memory[i] = 0x0;
        }

        if (!sd_raw_write_block(&sd, block, source_memory)) {
            Serial.println("sd_raw_write_block block failed");
            while (true) {
                delay(100);
            }
        }
    }

    sd_raw_dma_t sd_dma = { 0 };

    if (!sd_raw_dma_initialize(&sd_dma, &sd, source_memory, destination_memory, 512)) {
        Serial.println("sd_raw_dma_initialize failed");
        while (true) {
            delay(100);
        }
    }

    status = sd_raw_dma_read_block(&sd_dma, block);
    if (!status) {
        Serial.println("dma_read_block failed");
    }
    else {
        Serial.println("dma_read_block (should be 0xff)");
        log_buffer(destination_memory);
    }

    for (uint32_t i = 0; i < DATA_LENGTH; i++) {
        source_memory[i] = i;
        destination_memory[i] = i;
    }

    status = sd_raw_dma_write_block(&sd_dma, block);
    if (!status) {
        Serial.println("dma_write_block failed");
    }
    else {
        for (uint32_t i = 0; i < DATA_LENGTH; i++) {
            source_memory[i] = 0;
            destination_memory[i] = 0;
        }

        status = sd_raw_dma_read_block(&sd_dma, block);
        if (!status) {
            Serial.println("dma_read_block failed");
        }
        else {
            Serial.println("dma_read_block (should be increasing)");
            log_buffer(destination_memory);
        }
    }

    Serial.println("done");

    while (true) {
        delay(100);
    }
}


void loop() {

}
