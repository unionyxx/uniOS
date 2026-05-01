#include <drivers/class/hid/hid_report_parser.h>
#include <kernel/ktest.h>

namespace {
constexpr uint8_t kBootKeyboardDescriptor[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
                                               0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
                                               0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
                                               0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};

constexpr uint8_t kReportIdKeyboardDescriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};

constexpr uint8_t kNkroDescriptor[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15,
                                       0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x05, 0x07, 0x19, 0x00,
                                       0x29, 0x67, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x68, 0x81, 0x02, 0xC0};
} // namespace

KTEST(hid_parse_boot_keyboard_descriptor)
{
    HidKeyboardReportLayout layout = {};
    KTEST_EXPECT(
        hid_parse_keyboard_report_descriptor(kBootKeyboardDescriptor, sizeof(kBootKeyboardDescriptor), &layout));
    KTEST_EXPECT(layout.valid);
    KTEST_EXPECT(!layout.uses_report_id);
    KTEST_EXPECT_EQ(layout.report_bytes, 8);
    KTEST_EXPECT_EQ(layout.modifier_count, 8);
    KTEST_EXPECT_EQ(layout.array_count, 1);
}

KTEST(hid_decode_boot_keyboard_report)
{
    HidKeyboardReportLayout layout = {};
    KTEST_EXPECT(
        hid_parse_keyboard_report_descriptor(kBootKeyboardDescriptor, sizeof(kBootKeyboardDescriptor), &layout));

    const uint8_t report[8] = {0x02, 0x00, 0x04, 0x05, 0, 0, 0, 0};
    HidDecodedKeyboardReport decoded = {};
    KTEST_EXPECT(hid_decode_keyboard_report(&layout, report, sizeof(report), &decoded) ==
                 HidKeyboardDecodeStatus::Match);
    KTEST_EXPECT_EQ(decoded.modifiers, 0x02);
    KTEST_EXPECT(hid_keyboard_report_has_usage(&decoded, 0x04));
    KTEST_EXPECT(hid_keyboard_report_has_usage(&decoded, 0x05));
}

KTEST(hid_decode_report_id_keyboard_report)
{
    HidKeyboardReportLayout layout = {};
    KTEST_EXPECT(hid_parse_keyboard_report_descriptor(kReportIdKeyboardDescriptor, sizeof(kReportIdKeyboardDescriptor),
                                                      &layout));
    KTEST_EXPECT(layout.uses_report_id);
    KTEST_EXPECT_EQ(layout.report_id, 1);

    const uint8_t keyboard_report[9] = {0x01, 0x00, 0x00, 0x1E, 0, 0, 0, 0, 0};
    HidDecodedKeyboardReport decoded = {};
    KTEST_EXPECT(hid_decode_keyboard_report(&layout, keyboard_report, sizeof(keyboard_report), &decoded) ==
                 HidKeyboardDecodeStatus::Match);
    KTEST_EXPECT(hid_keyboard_report_has_usage(&decoded, 0x1E));

    const uint8_t consumer_report[2] = {0x02, 0x01};
    KTEST_EXPECT(hid_decode_keyboard_report(&layout, consumer_report, sizeof(consumer_report), &decoded) ==
                 HidKeyboardDecodeStatus::Ignore);
}

KTEST(hid_decode_nkro_bitmap_report)
{
    HidKeyboardReportLayout layout = {};
    KTEST_EXPECT(hid_parse_keyboard_report_descriptor(kNkroDescriptor, sizeof(kNkroDescriptor), &layout));
    KTEST_EXPECT_EQ(layout.variable_count, 1);

    uint8_t report[14] = {};
    report[0] = 0x02;      // Left shift
    report[1] = (1u << 4); // Usage 0x04 ('a')
    report[5] = (1u << 2); // Usage 0x22 ('5')

    HidDecodedKeyboardReport decoded = {};
    KTEST_EXPECT(hid_decode_keyboard_report(&layout, report, sizeof(report), &decoded) ==
                 HidKeyboardDecodeStatus::Match);
    KTEST_EXPECT_EQ(decoded.modifiers, 0x02);
    KTEST_EXPECT(hid_keyboard_report_has_usage(&decoded, 0x04));
    KTEST_EXPECT(hid_keyboard_report_has_usage(&decoded, 0x22));
}

KTEST(hid_ignore_rollover_usages)
{
    HidKeyboardReportLayout layout = {};
    KTEST_EXPECT(
        hid_parse_keyboard_report_descriptor(kBootKeyboardDescriptor, sizeof(kBootKeyboardDescriptor), &layout));

    const uint8_t report[8] = {0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    HidDecodedKeyboardReport decoded = {};
    KTEST_EXPECT(hid_decode_keyboard_report(&layout, report, sizeof(report), &decoded) ==
                 HidKeyboardDecodeStatus::Match);
    KTEST_EXPECT_EQ(decoded.usage_count, 0);
}
