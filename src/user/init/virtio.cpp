#include "virtio.h"
#include "pcie.h"

volatile virtio_pci_cap *find_capability(size_t configuration_address, uint8_t type) {
    auto configuration_header = (volatile PCIHeader*)configuration_address;

    auto current_capability = (volatile virtio_pci_cap*)(configuration_address + configuration_header->capabilities_offset);
    if(current_capability->cap_len != 0) {
        while(true) {
            if(current_capability->cfg_type == type && (configuration_header->bars[current_capability->bar] & 1) == 0) {
                return current_capability;
            }

            if(current_capability->cap_next != 0) {
                current_capability = (volatile virtio_pci_cap*)(configuration_address + current_capability->cap_next);
            } else {
                break;
            }
        }
    }

    return nullptr;
}

bool init_virtio_device(
    size_t pci_device_location,
    size_t *result_configuration_address,
    volatile virtio_pci_common_cfg **result_common_configuration
) {
    auto configuration_address = syscall(SyscallType::MapPCIEConfiguration, pci_device_location, 0);
    if(configuration_address == 0) {
        return false;
    }

    auto common_config_capability = find_capability(configuration_address, 1);
    if(common_config_capability == nullptr) {
        return false;
    }

    auto common_bar_address = syscall(SyscallType::MapPCIEBar, common_config_capability->bar | pci_device_location << bar_index_bits, 0);
    if(common_bar_address == 0) {
        return false;
    }

    auto common_configuration = (volatile virtio_pci_common_cfg*)(common_bar_address + common_config_capability->offset);

    common_configuration->device_status = 0; // Reset device
    common_configuration->device_status |= 1; // Set ACKNOWLEDGE flag
    common_configuration->device_status |= 1 << 1; // Set DRIVER flag
    
    common_configuration->driver_feature_select = 0;
    common_configuration->driver_feature = 0;

    common_configuration->device_feature_select = 1;
    common_configuration->device_feature = 1; // Set VIRTIO_F_VERSION_1 flag

    common_configuration->device_status |= 1 << 3; // Set FEATURES_OK flag

    if((common_configuration->device_status & ~(1 << 3)) == 0) { // Check for FEATURES_OK flag
        return false;
    }

    *result_configuration_address = configuration_address;
    *result_common_configuration = common_configuration;
    return true;
}