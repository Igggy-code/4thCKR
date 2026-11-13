#pragma once
#include "qemu/osdep.h"
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_CLASS_REVISION 0x08
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_SUBSYSTEM_ID 0x2e
#define PCI_INTERRUPT_PIN 0x3d
static inline uint32_t pci_get_long(const uint8_t *c) { uint32_t v; memcpy(&v, c, 4); return v; }
static inline void pci_set_long(uint8_t *c, uint32_t v) { memcpy(c, &v, 4); }
static inline uint16_t pci_get_word(const uint8_t *c) { uint16_t v; memcpy(&v, c, 2); return v; }
static inline void pci_set_word(uint8_t *c, uint16_t v) { memcpy(c, &v, 2); }
