#pragma once

const size_t bar_count = 6;
const size_t bar_index_bits = 3;

const size_t maximum_bus_count = 256;
const size_t bus_bits = 8;

const size_t device_count = 32;
const size_t device_bits = 5;

const size_t function_count = 8;
const size_t function_bits = 3;

const size_t configuration_area_size = 4096;

const size_t bar_type_bits = 1;

const size_t memory_bar_type_bits = 2;
const size_t memory_bar_prefetchable_bits = 1;

const size_t io_bar_reserved_bits = 1;

struct PCIHeader {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision;
    uint8_t interface;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
    uint32_t bars[6];
    uint32_t cardbus_cis_pointer;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t expansion_rom_address;
    uint8_t capabilities_offset;
    uint8_t reserved[7];
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t minimum_grant;
    uint8_t maximum_latency;
};