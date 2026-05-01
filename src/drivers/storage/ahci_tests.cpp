#include <drivers/storage/ahci.h>
#include <kernel/ktest.h>

KTEST(ahci_signature_skip_rules)
{
    KTEST_EXPECT(!ahci_signature_requires_immediate_skip(0x00000101u));
    KTEST_EXPECT(!ahci_signature_requires_immediate_skip(0x00000000u));
    KTEST_EXPECT(!ahci_signature_requires_immediate_skip(0xFFFFFFFFu));
    KTEST_EXPECT(ahci_signature_requires_immediate_skip(0xEB140101u));
    KTEST_EXPECT(ahci_signature_requires_immediate_skip(0xC33C0101u));
    KTEST_EXPECT(ahci_signature_requires_immediate_skip(0x96690101u));
}
