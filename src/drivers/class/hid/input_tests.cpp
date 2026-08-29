#include <drivers/class/hid/input.h>
#include <kernel/ktest.h>
#include <uapi/input.h>

// Verifies the pointer-speed multiplier math (Q8, 256 == 1.0x) and device-id
// validation. Only pure functions are exercised; no live device state is
// toggled so the boot-time cursor is unaffected.

KTEST(input_pointer_scaling)
{
    // Default 1.0x: identity.
    input_set_pointer_speed(256);
    KTEST_EXPECT_EQ(input_scale_pointer(0), 0);
    KTEST_EXPECT_EQ(input_scale_pointer(100), 100);
    KTEST_EXPECT_EQ(input_scale_pointer(-100), -100);

    // 2.0x: deltas double (both signs).
    input_set_pointer_speed(512);
    KTEST_EXPECT_EQ(input_scale_pointer(50), 100);
    KTEST_EXPECT_EQ(input_scale_pointer(-50), -100);

    // 0.5x: deltas halve.
    input_set_pointer_speed(128);
    KTEST_EXPECT_EQ(input_scale_pointer(100), 50);

    // Out-of-range multipliers clamp to [16, 1024]; the floor keeps the pointer
    // movable (a delta of 256 still yields 16px at the floor) rather than frozen.
    input_set_pointer_speed(0);
    KTEST_EXPECT_EQ(input_scale_pointer(256), 16);
    input_set_pointer_speed(1);
    KTEST_EXPECT_EQ(input_scale_pointer(256), 16);
    input_set_pointer_speed(100000);
    KTEST_EXPECT_EQ(input_scale_pointer(1), 4); // 1 * 1024 / 256

    // At the ceiling multiplier a near-max delta overflows int32 and clamps.
    KTEST_EXPECT_EQ(input_scale_pointer(2000000000), INT32_MAX);
    KTEST_EXPECT_EQ(input_scale_pointer(-2000000000), INT32_MIN);

    // Restore the default so the live cursor is unaffected after the suite.
    input_set_pointer_speed(256);
}

KTEST(input_device_id_validation)
{
    KTEST_EXPECT(input_is_valid_device_id(INPUT_DEVICE_ID_PS2_MOUSE));
    KTEST_EXPECT(input_is_valid_device_id(INPUT_DEVICE_ID_PS2_KEYBOARD));
    KTEST_EXPECT(input_is_valid_device_id(1));
    KTEST_EXPECT(input_is_valid_device_id(15));
    KTEST_EXPECT(!input_is_valid_device_id(0));
    KTEST_EXPECT(!input_is_valid_device_id(16));
    KTEST_EXPECT(!input_is_valid_device_id(0x100));
    KTEST_EXPECT(!input_is_valid_device_id(0xFFFFFFFFu));
}
