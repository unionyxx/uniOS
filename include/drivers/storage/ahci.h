#pragma once
#include <stdbool.h>
#include <stdint.h>

bool ahci_signature_requires_immediate_skip(uint32_t signature);
void ahci_init();
