#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stddef.h>

#define PCI_MAX_DEVICES 32

#define PCI_BAR_MEM 0
#define PCI_BAR_IO  1

typedef struct pci_device {
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t irq_line;
    uint32_t bars[6];
    uint8_t bar_types[6];
} pci_device_t;

void pci_init(void);
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t pci_read_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
void pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);
void pci_write_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val);

void pci_enable_bus_master(const pci_device_t* dev);
const pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id);
const pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);

int pci_device_count(void);
const pci_device_t* pci_get_device(int index);

#endif

