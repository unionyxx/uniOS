#include <drivers/class/hid/hid_report_parser.h>
#include <libk/kstring.h>

namespace {
constexpr uint16_t HID_USAGE_PAGE_GENERIC_DESKTOP = 0x01;
constexpr uint16_t HID_USAGE_PAGE_KEYBOARD = 0x07;
constexpr uint8_t HID_USAGE_KEYBOARD = 0x06;
constexpr uint8_t HID_COLLECTION_APPLICATION = 0x01;
constexpr uint8_t HID_ITEM_TYPE_MAIN = 0;
constexpr uint8_t HID_ITEM_TYPE_GLOBAL = 1;
constexpr uint8_t HID_ITEM_TYPE_LOCAL = 2;
constexpr uint8_t HID_MAIN_INPUT = 8;
constexpr uint8_t HID_MAIN_COLLECTION = 10;
constexpr uint8_t HID_MAIN_END_COLLECTION = 12;
constexpr uint8_t HID_GLOBAL_USAGE_PAGE = 0;
constexpr uint8_t HID_GLOBAL_LOGICAL_MIN = 1;
constexpr uint8_t HID_GLOBAL_LOGICAL_MAX = 2;
constexpr uint8_t HID_GLOBAL_REPORT_SIZE = 7;
constexpr uint8_t HID_GLOBAL_REPORT_ID = 8;
constexpr uint8_t HID_GLOBAL_REPORT_COUNT = 9;
constexpr uint8_t HID_GLOBAL_PUSH = 10;
constexpr uint8_t HID_GLOBAL_POP = 11;
constexpr uint8_t HID_LOCAL_USAGE = 0;
constexpr uint8_t HID_LOCAL_USAGE_MIN = 1;
constexpr uint8_t HID_LOCAL_USAGE_MAX = 2;

struct HidGlobalState
{
    uint16_t usage_page;
    int32_t logical_min;
    int32_t logical_max;
    uint8_t report_size;
    uint8_t report_count;
    uint8_t report_id;
};

struct HidLocalState
{
    uint16_t usages[32];
    uint8_t usage_count;
    bool has_usage_min;
    bool has_usage_max;
    uint16_t usage_min;
    uint16_t usage_max;
};

struct ReportCursor
{
    bool used;
    uint8_t report_id;
    uint16_t bits;
};

[[nodiscard]] static int32_t hid_sign_extend(uint32_t value, uint8_t bits)
{
    if (bits == 0 || bits >= 32)
        return static_cast<int32_t>(value);
    const uint32_t sign_bit = 1u << (bits - 1);
    if ((value & sign_bit) == 0)
        return static_cast<int32_t>(value);
    const uint32_t mask = ~((1u << bits) - 1u);
    return static_cast<int32_t>(value | mask);
}

[[nodiscard]] static uint32_t hid_extract_bits(const uint8_t *data, uint16_t bit_offset, uint8_t bit_size)
{
    uint32_t value = 0;
    for (uint8_t bit = 0; bit < bit_size; bit++) {
        const uint16_t absolute_bit = static_cast<uint16_t>(bit_offset + bit);
        const uint8_t byte = data[absolute_bit / 8];
        if (byte & (1u << (absolute_bit & 7)))
            value |= (1u << bit);
    }
    return value;
}

static void hid_reset_local_state(HidLocalState *local)
{
    if (!local)
        return;
    local->usage_count = 0;
    local->has_usage_min = false;
    local->has_usage_max = false;
    local->usage_min = 0;
    local->usage_max = 0;
}

[[nodiscard]] static bool hid_usages_are_contiguous(const HidLocalState &local)
{
    if (local.usage_count == 0)
        return false;
    for (uint8_t i = 1; i < local.usage_count; i++) {
        if (local.usages[i] != static_cast<uint16_t>(local.usages[0] + i))
            return false;
    }
    return true;
}

static ReportCursor *hid_get_report_cursor(ReportCursor *cursors, uint8_t report_id)
{
    for (uint8_t i = 0; i < 8; i++) {
        if (cursors[i].used && cursors[i].report_id == report_id)
            return &cursors[i];
    }
    for (uint8_t i = 0; i < 8; i++) {
        if (!cursors[i].used) {
            cursors[i].used = true;
            cursors[i].report_id = report_id;
            cursors[i].bits = 0;
            return &cursors[i];
        }
    }
    return nullptr;
}

static bool hid_add_usage(HidDecodedKeyboardReport *report, uint8_t usage)
{
    if (!report || usage == 0 || usage == 1 || usage == 2 || usage == 3)
        return true;
    for (uint8_t i = 0; i < report->usage_count; i++) {
        if (report->usages[i] == usage)
            return true;
    }
    if (report->usage_count >= sizeof(report->usages))
        return false;
    report->usages[report->usage_count++] = usage;
    return true;
}
} // namespace

void hid_reset_keyboard_report_layout(HidKeyboardReportLayout *layout)
{
    if (!layout)
        return;
    kstring::zero_memory(layout, sizeof(HidKeyboardReportLayout));
}

void hid_reset_decoded_keyboard_report(HidDecodedKeyboardReport *report)
{
    if (!report)
        return;
    kstring::zero_memory(report, sizeof(HidDecodedKeyboardReport));
}

bool hid_parse_keyboard_report_descriptor(const uint8_t *data, uint16_t length, HidKeyboardReportLayout *out)
{
    if (!data || !out)
        return false;

    hid_reset_keyboard_report_layout(out);

    HidGlobalState global = {0, 0, 0, 0, 0, 0};
    HidGlobalState global_stack[8];
    uint8_t global_depth = 0;
    HidLocalState local = {};
    hid_reset_local_state(&local);

    ReportCursor cursors[8] = {};
    bool collection_stack[8] = {};
    uint8_t collection_depth = 0;
    bool inside_keyboard_collection = false;
    bool saw_field = false;

    uint16_t offset = 0;
    while (offset < length) {
        const uint8_t prefix = data[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > length)
                return false;
            const uint8_t size = data[offset++];
            offset++;
            if (offset + size > length)
                return false;
            offset = static_cast<uint16_t>(offset + size);
            continue;
        }

        uint8_t item_size = prefix & 0x3;
        if (item_size == 3)
            item_size = 4;
        if (offset + item_size > length)
            return false;

        uint32_t value = 0;
        for (uint8_t i = 0; i < item_size; i++) {
            value |= static_cast<uint32_t>(data[offset + i]) << (i * 8);
        }
        offset = static_cast<uint16_t>(offset + item_size);

        const uint8_t type = static_cast<uint8_t>((prefix >> 2) & 0x3);
        const uint8_t tag = static_cast<uint8_t>((prefix >> 4) & 0xF);

        if (type == HID_ITEM_TYPE_MAIN) {
            if (tag == HID_MAIN_COLLECTION) {
                uint16_t usage = 0;
                if (local.usage_count > 0)
                    usage = local.usages[local.usage_count - 1];
                else if (local.has_usage_min)
                    usage = local.usage_min;

                const bool is_keyboard_app = value == HID_COLLECTION_APPLICATION &&
                                             global.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                                             usage == HID_USAGE_KEYBOARD;

                if (collection_depth >= sizeof(collection_stack))
                    return false;
                collection_stack[collection_depth++] = static_cast<bool>(inside_keyboard_collection || is_keyboard_app);
                inside_keyboard_collection = collection_stack[collection_depth - 1];
            } else if (tag == HID_MAIN_END_COLLECTION) {
                if (collection_depth == 0)
                    return false;
                collection_depth--;
                inside_keyboard_collection = collection_depth ? collection_stack[collection_depth - 1] : false;
            } else if (tag == HID_MAIN_INPUT) {
                ReportCursor *cursor = hid_get_report_cursor(cursors, global.report_id);
                if (!cursor)
                    return false;

                const uint16_t field_bits = static_cast<uint16_t>(global.report_size * global.report_count);
                const uint16_t bit_offset = cursor->bits;

                if (inside_keyboard_collection && (value & 0x01) == 0) {
                    if (out->modifier_count == 0 && out->array_count == 0 && out->variable_count == 0) {
                        out->report_id = global.report_id;
                    } else if (out->report_id != global.report_id) {
                        return false;
                    }

                    const bool variable = (value & 0x02) != 0;
                    if (global.usage_page == HID_USAGE_PAGE_KEYBOARD) {
                        if (!variable) {
                            if (global.report_size == 0 || global.report_size > 16 ||
                                out->array_count >= sizeof(out->arrays) / sizeof(out->arrays[0])) {
                                return false;
                            }
                            auto &field = out->arrays[out->array_count++];
                            field.bit_offset = bit_offset;
                            field.report_size = global.report_size;
                            field.report_count = global.report_count;
                            saw_field = true;
                        } else {
                            uint16_t usage_min = 0;
                            uint8_t usage_count = 0;

                            if (local.has_usage_min && local.has_usage_max && local.usage_max >= local.usage_min) {
                                usage_min = local.usage_min;
                                usage_count = static_cast<uint8_t>(local.usage_max - local.usage_min + 1);
                            } else if (hid_usages_are_contiguous(local)) {
                                usage_min = local.usages[0];
                                usage_count = local.usage_count;
                            } else if (local.usage_count == 1) {
                                usage_min = local.usages[0];
                                usage_count = 1;
                            }

                            if (usage_count == 0)
                                return false;

                            if (global.report_size == 1 && usage_min >= 0xE0 && usage_min + usage_count - 1 <= 0xE7) {
                                if (out->modifier_count + usage_count >
                                    sizeof(out->modifiers) / sizeof(out->modifiers[0]))
                                    return false;
                                for (uint8_t i = 0; i < usage_count; i++) {
                                    auto &field = out->modifiers[out->modifier_count++];
                                    field.bit_offset = static_cast<uint16_t>(bit_offset + i);
                                    field.bit_size = 1;
                                    field.usage = static_cast<uint8_t>(usage_min + i);
                                }
                                saw_field = true;
                            } else {
                                if (out->variable_count >= sizeof(out->variables) / sizeof(out->variables[0]))
                                    return false;
                                auto &field = out->variables[out->variable_count++];
                                field.bit_offset = bit_offset;
                                field.report_size = global.report_size;
                                field.report_count = global.report_count;
                                field.usage_min = usage_min;
                                field.usage_count = usage_count;
                                saw_field = true;
                            }
                        }
                    }
                }

                cursor->bits = static_cast<uint16_t>(cursor->bits + field_bits);
                if (out->report_id == global.report_id && cursor->bits > out->report_bits)
                    out->report_bits = cursor->bits;
            }

            hid_reset_local_state(&local);
            continue;
        }

        if (type == HID_ITEM_TYPE_GLOBAL) {
            switch (tag) {
                case HID_GLOBAL_USAGE_PAGE:
                    global.usage_page = static_cast<uint16_t>(value);
                    break;
                case HID_GLOBAL_LOGICAL_MIN:
                    global.logical_min = hid_sign_extend(value, static_cast<uint8_t>(item_size * 8));
                    break;
                case HID_GLOBAL_LOGICAL_MAX:
                    global.logical_max = hid_sign_extend(value, static_cast<uint8_t>(item_size * 8));
                    break;
                case HID_GLOBAL_REPORT_SIZE:
                    global.report_size = static_cast<uint8_t>(value);
                    break;
                case HID_GLOBAL_REPORT_ID:
                    global.report_id = static_cast<uint8_t>(value);
                    if (global.report_id == 0)
                        return false;
                    if (!hid_get_report_cursor(cursors, global.report_id))
                        return false;
                    break;
                case HID_GLOBAL_REPORT_COUNT:
                    global.report_count = static_cast<uint8_t>(value);
                    break;
                case HID_GLOBAL_PUSH:
                    if (global_depth >= sizeof(global_stack) / sizeof(global_stack[0]))
                        return false;
                    global_stack[global_depth++] = global;
                    break;
                case HID_GLOBAL_POP:
                    if (global_depth == 0)
                        return false;
                    global = global_stack[--global_depth];
                    break;
                default:
                    break;
            }
            continue;
        }

        if (type == HID_ITEM_TYPE_LOCAL) {
            switch (tag) {
                case HID_LOCAL_USAGE:
                    if (local.usage_count >= sizeof(local.usages) / sizeof(local.usages[0]))
                        return false;
                    local.usages[local.usage_count++] = static_cast<uint16_t>(value);
                    break;
                case HID_LOCAL_USAGE_MIN:
                    local.has_usage_min = true;
                    local.usage_min = static_cast<uint16_t>(value);
                    break;
                case HID_LOCAL_USAGE_MAX:
                    local.has_usage_max = true;
                    local.usage_max = static_cast<uint16_t>(value);
                    break;
                default:
                    break;
            }
        }
    }

    if (!saw_field || out->report_bits == 0)
        return false;
    out->uses_report_id = out->report_id != 0;
    out->report_bytes = static_cast<uint16_t>((out->report_bits + 7) / 8);
    out->valid = true;
    return true;
}

HidKeyboardDecodeStatus hid_decode_keyboard_report(const HidKeyboardReportLayout *layout, const uint8_t *data,
                                                   uint16_t length, HidDecodedKeyboardReport *out)
{
    if (!layout || !layout->valid || !data || !out)
        return HidKeyboardDecodeStatus::Invalid;

    hid_reset_decoded_keyboard_report(out);

    const uint8_t *payload = data;
    uint16_t payload_length = length;
    if (layout->uses_report_id) {
        if (length < 1)
            return HidKeyboardDecodeStatus::Invalid;
        if (data[0] != layout->report_id)
            return HidKeyboardDecodeStatus::Ignore;
        payload = data + 1;
        payload_length--;
    }

    if (payload_length < layout->report_bytes)
        return HidKeyboardDecodeStatus::Invalid;

    for (uint8_t i = 0; i < layout->modifier_count; i++) {
        const auto &field = layout->modifiers[i];
        const uint32_t value = hid_extract_bits(payload, field.bit_offset, field.bit_size);
        if (value != 0 && field.usage >= 0xE0 && field.usage <= 0xE7) {
            out->modifiers |= static_cast<uint8_t>(1u << (field.usage - 0xE0));
        }
    }

    for (uint8_t i = 0; i < layout->array_count; i++) {
        const auto &field = layout->arrays[i];
        for (uint8_t index = 0; index < field.report_count; index++) {
            const uint16_t offset = static_cast<uint16_t>(field.bit_offset + index * field.report_size);
            const uint32_t value = hid_extract_bits(payload, offset, field.report_size);
            if (value > 0xFF)
                return HidKeyboardDecodeStatus::Invalid;
            if (!hid_add_usage(out, static_cast<uint8_t>(value)))
                return HidKeyboardDecodeStatus::Invalid;
        }
    }

    for (uint8_t i = 0; i < layout->variable_count; i++) {
        const auto &field = layout->variables[i];
        for (uint8_t index = 0; index < field.report_count && index < field.usage_count; index++) {
            const uint16_t offset = static_cast<uint16_t>(field.bit_offset + index * field.report_size);
            const uint32_t value = hid_extract_bits(payload, offset, field.report_size);
            if (field.report_size == 1) {
                if (value != 0) {
                    if (!hid_add_usage(out, static_cast<uint8_t>(field.usage_min + index)))
                        return HidKeyboardDecodeStatus::Invalid;
                }
            } else if (value != 0) {
                if (!hid_add_usage(out, static_cast<uint8_t>(field.usage_min + index)))
                    return HidKeyboardDecodeStatus::Invalid;
            }
        }
    }

    return HidKeyboardDecodeStatus::Match;
}

bool hid_keyboard_report_has_usage(const HidDecodedKeyboardReport *report, uint8_t usage)
{
    if (!report || usage == 0)
        return false;
    for (uint8_t i = 0; i < report->usage_count; i++) {
        if (report->usages[i] == usage)
            return true;
    }
    return false;
}
