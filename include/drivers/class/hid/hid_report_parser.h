#pragma once

#include <stdint.h>

struct HidKeyboardBitField
{
    uint16_t bit_offset;
    uint8_t bit_size;
    uint8_t usage;
};

struct HidKeyboardArrayField
{
    uint16_t bit_offset;
    uint8_t report_size;
    uint8_t report_count;
};

struct HidKeyboardVariableField
{
    uint16_t bit_offset;
    uint8_t report_size;
    uint8_t report_count;
    uint16_t usage_min;
    uint8_t usage_count;
};

struct HidKeyboardReportLayout
{
    bool valid;
    bool uses_report_id;
    uint8_t report_id;
    uint16_t report_bits;
    uint16_t report_bytes;

    uint8_t modifier_count;
    HidKeyboardBitField modifiers[8];

    uint8_t array_count;
    HidKeyboardArrayField arrays[8];

    uint8_t variable_count;
    HidKeyboardVariableField variables[8];
};

struct HidDecodedKeyboardReport
{
    uint8_t modifiers;
    uint8_t usages[64];
    uint8_t usage_count;
};

enum class HidKeyboardDecodeStatus : uint8_t
{
    Match = 0,
    Ignore = 1,
    Invalid = 2,
};

void hid_reset_keyboard_report_layout(HidKeyboardReportLayout *layout);
void hid_reset_decoded_keyboard_report(HidDecodedKeyboardReport *report);
bool hid_parse_keyboard_report_descriptor(const uint8_t *data, uint16_t length, HidKeyboardReportLayout *out);
HidKeyboardDecodeStatus hid_decode_keyboard_report(const HidKeyboardReportLayout *layout, const uint8_t *data,
                                                   uint16_t length, HidDecodedKeyboardReport *out);
bool hid_keyboard_report_has_usage(const HidDecodedKeyboardReport *report, uint8_t usage);
