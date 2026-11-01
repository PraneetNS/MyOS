#include "pci.h"
#include "io.h"
#include "console.h"
#include "serial.h"
#include <string.h>

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA    0x0CFC

static pci_device_t s_devices[PCI_MAX_DEVICES];
static int s_device_count = 0;

static inline uint32_t pci_box_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (uint32_t)((1u << 31) |
                     ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) |
                     ((uint32_t)func << 8) |
                     (offset & 0xFC));
}

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_box_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_box_address(bus, slot, func, offset));
    return (uint16_t)((inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_box_address(bus, slot, func, offset));
    return (uint8_t)((inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDRESS, pci_box_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t dword = pci_read_config_dword(bus, slot, func, offset);
    int shift = (offset & 2) * 8;
    dword &= ~(0xFFFFu << shift);
    dword |= ((uint32_t)val << shift);
    pci_write_config_dword(bus, slot, func, offset, dword);
}

void pci_write_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val) {
    uint32_t dword = pci_read_config_dword(bus, slot, func, offset);
    int shift = (offset & 3) * 8;
    dword &= ~(0xFFu << shift);
    dword |= ((uint32_t)val << shift);
    pci_write_config_dword(bus, slot, func, offset, dword);
}

void pci_enable_bus_master(const pci_device_t* dev) {
    if (!dev) return;
    uint16_t cmd = pci_read_config_word(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 0) | (1 << 1) | (1 << 2); /* IO, Mem, Bus Master */
    pci_write_config_word(dev->bus, dev->dev, dev->func, 0x04, cmd);
}

const pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].vendor_id == vendor_id && s_devices[i].device_id == device_id) {
            return &s_devices[i];
        }
    }
    return NULL;
}

const pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].class_code == class_code && s_devices[i].subclass == subclass) {
            return &s_devices[i];
        }
    }
    return NULL;
}

int pci_device_count(void) {
    return s_device_count;
}

const pci_device_t* pci_get_device(int index) {
    if (index < 0 || index >= s_device_count) return NULL;
    return &s_devices[index];
}

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_read_config_word(bus, slot, func, 0x00);
    if (vendor == 0xFFFF || vendor == 0x0000) return;

    if (s_device_count >= PCI_MAX_DEVICES) return;

    pci_device_t* dev = &s_devices[s_device_count++];
    memset(dev, 0, sizeof(pci_device_t));

    dev->bus = bus;
    dev->dev = slot;
    dev->func = func;
    dev->vendor_id = vendor;
    dev->device_id = pci_read_config_word(bus, slot, func, 0x02);
    dev->class_code = pci_read_config_byte(bus, slot, func, 0x0B);
    dev->subclass = pci_read_config_byte(bus, slot, func, 0x0A);
    dev->prog_if = pci_read_config_byte(bus, slot, func, 0x09);
    dev->header_type = pci_read_config_byte(bus, slot, func, 0x0E);
    dev->irq_line = pci_read_config_byte(bus, slot, func, 0x3C);

    /* Read BARs for header type 0 (standard PCI devices) */
    if ((dev->header_type & 0x7F) == 0x00) {
        for (int b = 0; b < 6; b++) {
            uint32_t bar = pci_read_config_dword(bus, slot, func, 0x10 + (b * 4));
            if (bar & 1) {
                dev->bar_types[b] = PCI_BAR_IO;
                dev->bars[b] = bar & ~0x3u;
            } else {
                dev->bar_types[b] = PCI_BAR_MEM;
                dev->bars[b] = bar & ~0xFu;
            }
        }
    }

    kprintf("[pci] %02x:%02x.%d: vendor=%04x device=%04x class=%02x:%02x irq=%d\n",
            bus, slot, func, dev->vendor_id, dev->device_id, dev->class_code, dev->subclass, dev->irq_line);
}

void pci_init(void) {
    s_device_count = 0;
    kprintf("[pci] Enumerating PCI configuration space...\n");

    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read_config_word((uint8_t)bus, slot, 0, 0x00);
            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            pci_scan_function((uint8_t)bus, slot, 0);

            uint8_t header_type = pci_read_config_byte((uint8_t)bus, slot, 0, 0x0E);
            if (header_type & 0x80) {
                /* Multi-function device */
                for (uint8_t func = 1; func < 8; func++) {
                    pci_scan_function((uint8_t)bus, slot, func);
                }
            }
        }
    }

    kprintf("[pci] Discovered %d PCI devices\n", s_device_count);
}
