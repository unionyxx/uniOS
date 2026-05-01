#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <drivers/storage/usb_msc.h>
#include <kernel/debug.h>
#include <kernel/fs/block_dev.h>
#include <kernel/fs/partition.h>
#include <kernel/fs/storage_guard.h>
#include <kernel/sync/mutex.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>

namespace {

static constexpr uint32_t USB_MSC_CBW_SIGNATURE = 0x43425355;
static constexpr uint32_t USB_MSC_CSW_SIGNATURE = 0x53425355;
static constexpr uint8_t USB_MSC_REQ_BULK_ONLY_RESET = 0xFF;
static constexpr uint8_t USB_MSC_REQ_GET_MAX_LUN = 0xFE;
static constexpr uint32_t USB_MSC_MAX_DEVICES = 8;
static constexpr uint32_t USB_MSC_MAX_TRANSFER_BYTES = 64 * 1024;

static constexpr uint8_t SCSI_TEST_UNIT_READY = 0x00;
static constexpr uint8_t SCSI_REQUEST_SENSE = 0x03;
static constexpr uint8_t SCSI_INQUIRY = 0x12;
static constexpr uint8_t SCSI_READ_CAPACITY_10 = 0x25;
static constexpr uint8_t SCSI_READ_10 = 0x28;
static constexpr uint8_t SCSI_WRITE_10 = 0x2A;
static constexpr uint8_t SCSI_SYNCHRONIZE_CACHE_10 = 0x35;

struct [[gnu::packed]] UsbMscCbw
{
    uint32_t signature;
    uint32_t tag;
    uint32_t data_transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
};

struct [[gnu::packed]] UsbMscCsw
{
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t status;
};

static_assert(sizeof(UsbMscCbw) == 31);
static_assert(sizeof(UsbMscCsw) == 13);

struct UsbMscDevice
{
    bool used;
    bool ready;
    UsbDeviceInfo *usb;
    BlockDevice block_dev;
    Mutex lock;
    uint8_t lun;
    uint32_t tag;
    uint32_t block_size;
    uint64_t block_count;
    char name[16];
};

static UsbMscDevice g_msc_devices[USB_MSC_MAX_DEVICES];
static uint32_t g_next_disk_index = 0;

static uint16_t endpoint_address_from_dci(uint8_t dci)
{
    uint16_t ep = static_cast<uint16_t>(dci / 2);
    if ((dci & 1u) != 0)
        ep |= USB_ENDPOINT_DIR_IN;
    return ep;
}

static uint32_t read_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

static void write_be16(uint8_t *data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

static void write_be32(uint8_t *data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

static void copy_trimmed_ascii(const uint8_t *src, size_t len, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;

    size_t pos = 0;
    bool last_space = true;
    for (size_t i = 0; i < len && pos + 1 < out_size; i++) {
        char c = static_cast<char>(src[i]);
        if (c < 32 || c > 126)
            c = ' ';
        if (c == ' ') {
            if (last_space)
                continue;
            last_space = true;
        } else {
            last_space = false;
        }
        out[pos++] = c;
    }
    while (pos > 0 && out[pos - 1] == ' ')
        pos--;
    out[pos] = '\0';
}

static void usb_msc_reset_recovery(UsbMscDevice *dev)
{
    if (!dev || !dev->usb)
        return;

    uint16_t transferred = 0;
    xhci_control_transfer(dev->usb->slot_id, USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_INTERFACE,
                          USB_MSC_REQ_BULK_ONLY_RESET, 0, dev->usb->msc_interface, 0, nullptr, &transferred, false);

    const uint16_t in_addr = endpoint_address_from_dci(dev->usb->msc_bulk_in_endpoint);
    const uint16_t out_addr = endpoint_address_from_dci(dev->usb->msc_bulk_out_endpoint);
    xhci_control_transfer(dev->usb->slot_id,
                          USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_ENDPOINT,
                          USB_REQ_CLEAR_FEATURE, 0, in_addr, 0, nullptr, &transferred, false);
    xhci_control_transfer(dev->usb->slot_id,
                          USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_ENDPOINT,
                          USB_REQ_CLEAR_FEATURE, 0, out_addr, 0, nullptr, &transferred, false);
    xhci_set_tr_dequeue(dev->usb->slot_id, dev->usb->msc_bulk_in_endpoint);
    xhci_set_tr_dequeue(dev->usb->slot_id, dev->usb->msc_bulk_out_endpoint);
}

static bool usb_msc_command_raw(UsbMscDevice *dev, const uint8_t *cb, uint8_t cb_len, void *data, uint32_t data_len,
                                bool data_in, bool *transport_ok)
{
    if (transport_ok)
        *transport_ok = false;
    if (!dev || !dev->usb || !cb || cb_len == 0 || cb_len > 16)
        return false;

    UsbMscCbw cbw = {};
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_length = data_len;
    cbw.flags = data_in ? 0x80 : 0x00;
    cbw.lun = dev->lun;
    cbw.cb_length = cb_len;
    kstring::memcpy(cbw.cb, cb, cb_len);

    uint32_t actual = 0;
    if (!xhci_bulk_transfer(dev->usb->slot_id, dev->usb->msc_bulk_out_endpoint, false, &cbw, sizeof(cbw), &actual,
                            2000) ||
        actual != sizeof(cbw)) {
        return false;
    }

    if (data_len != 0) {
        const uint8_t endpoint = data_in ? dev->usb->msc_bulk_in_endpoint : dev->usb->msc_bulk_out_endpoint;
        if (!xhci_bulk_transfer(dev->usb->slot_id, endpoint, data_in, data, data_len, &actual, 5000) ||
            actual > data_len) {
            return false;
        }
    }

    UsbMscCsw csw = {};
    actual = 0;
    if (!xhci_bulk_transfer(dev->usb->slot_id, dev->usb->msc_bulk_in_endpoint, true, &csw, sizeof(csw), &actual,
                            2000) ||
        actual != sizeof(csw)) {
        return false;
    }

    if (csw.signature != USB_MSC_CSW_SIGNATURE || csw.tag != cbw.tag)
        return false;
    if (transport_ok)
        *transport_ok = true;
    return csw.status == 0;
}

static bool usb_msc_command(UsbMscDevice *dev, const uint8_t *cb, uint8_t cb_len, void *data, uint32_t data_len,
                            bool data_in)
{
    bool transport_ok = false;
    if (usb_msc_command_raw(dev, cb, cb_len, data, data_len, data_in, &transport_ok))
        return true;
    if (!transport_ok)
        usb_msc_reset_recovery(dev);
    return false;
}

static void usb_msc_request_sense(UsbMscDevice *dev)
{
    uint8_t sense[18] = {};
    uint8_t cmd[6] = {};
    cmd[0] = SCSI_REQUEST_SENSE;
    cmd[4] = sizeof(sense);
    usb_msc_command(dev, cmd, sizeof(cmd), sense, sizeof(sense), true);
}

static bool usb_msc_test_unit_ready(UsbMscDevice *dev)
{
    uint8_t cmd[6] = {};
    cmd[0] = SCSI_TEST_UNIT_READY;
    for (uint32_t attempt = 0; attempt < 20; attempt++) {
        if (usb_msc_command(dev, cmd, sizeof(cmd), nullptr, 0, false))
            return true;
        usb_msc_request_sense(dev);
        timer_poll_wait_ms(50);
    }
    return false;
}

static bool usb_msc_inquiry(UsbMscDevice *dev, char *model, size_t model_size)
{
    uint8_t inquiry[36] = {};
    uint8_t cmd[6] = {};
    cmd[0] = SCSI_INQUIRY;
    cmd[4] = sizeof(inquiry);
    if (!usb_msc_command(dev, cmd, sizeof(cmd), inquiry, sizeof(inquiry), true))
        return false;

    copy_trimmed_ascii(inquiry + 8, 28, model, model_size);
    if (model && model_size > 0 && model[0] == '\0')
        kstring::strncpy(model, "USB Mass Storage", model_size - 1);
    return true;
}

static bool usb_msc_read_capacity(UsbMscDevice *dev, uint64_t *block_count, uint32_t *block_size)
{
    uint8_t capacity[8] = {};
    uint8_t cmd[10] = {};
    cmd[0] = SCSI_READ_CAPACITY_10;
    for (uint32_t attempt = 0; attempt < 8; attempt++) {
        if (usb_msc_command(dev, cmd, sizeof(cmd), capacity, sizeof(capacity), true)) {
            const uint32_t last_lba = read_be32(capacity);
            const uint32_t size = read_be32(capacity + 4);
            if (size != 0) {
                if (block_count)
                    *block_count = static_cast<uint64_t>(last_lba) + 1u;
                if (block_size)
                    *block_size = size;
                return true;
            }
        }
        usb_msc_request_sense(dev);
        timer_poll_wait_ms(50);
    }
    return false;
}

static bool usb_msc_sync_cache(UsbMscDevice *dev)
{
    uint8_t cmd[10] = {};
    cmd[0] = SCSI_SYNCHRONIZE_CACHE_10;
    return usb_msc_command(dev, cmd, sizeof(cmd), nullptr, 0, false);
}

static bool usb_msc_rw10(UsbMscDevice *dev, uint64_t lba, uint16_t blocks, void *buffer, bool write)
{
    if (!dev || !buffer || blocks == 0 || lba > 0xFFFFFFFFull)
        return false;

    uint8_t cmd[10] = {};
    cmd[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
    write_be32(&cmd[2], static_cast<uint32_t>(lba));
    write_be16(&cmd[7], blocks);

    const uint32_t bytes = static_cast<uint32_t>(blocks) * dev->block_size;
    return usb_msc_command(dev, cmd, sizeof(cmd), buffer, bytes, !write);
}

static int64_t usb_msc_read_blocks(BlockDevice *block_dev, uint64_t lba, uint32_t count, void *buffer)
{
    auto *dev = static_cast<UsbMscDevice *>(block_dev ? block_dev->private_data : nullptr);
    if (!dev || !dev->ready || !buffer)
        return -1;
    if (count == 0)
        return 0;
    if (lba > dev->block_count || count > dev->block_count - lba)
        return -1;

    mutex_lock(&dev->lock);
    uint8_t *cursor = static_cast<uint8_t *>(buffer);
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        const uint32_t max_blocks = USB_MSC_MAX_TRANSFER_BYTES / dev->block_size;
        if (chunk > max_blocks)
            chunk = max_blocks;
        if (chunk > 0xFFFFu)
            chunk = 0xFFFFu;
        if (chunk == 0)
            break;
        if (!usb_msc_rw10(dev, lba + done, static_cast<uint16_t>(chunk), cursor + done * dev->block_size, false)) {
            mutex_unlock(&dev->lock);
            return done != 0 ? static_cast<int64_t>(done) : -1;
        }
        done += chunk;
    }
    mutex_unlock(&dev->lock);
    return done;
}

static int64_t usb_msc_write_blocks(BlockDevice *block_dev, uint64_t lba, uint32_t count, const void *buffer)
{
    auto *dev = static_cast<UsbMscDevice *>(block_dev ? block_dev->private_data : nullptr);
    if (!dev || !dev->ready || !buffer || !storage_writes_allowed())
        return -1;
    if (count == 0)
        return 0;
    if (lba > dev->block_count || count > dev->block_count - lba)
        return -1;

    mutex_lock(&dev->lock);
    auto *cursor = const_cast<uint8_t *>(static_cast<const uint8_t *>(buffer));
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        const uint32_t max_blocks = USB_MSC_MAX_TRANSFER_BYTES / dev->block_size;
        if (chunk > max_blocks)
            chunk = max_blocks;
        if (chunk > 0xFFFFu)
            chunk = 0xFFFFu;
        if (chunk == 0)
            break;
        if (!usb_msc_rw10(dev, lba + done, static_cast<uint16_t>(chunk), cursor + done * dev->block_size, true)) {
            mutex_unlock(&dev->lock);
            return done != 0 ? static_cast<int64_t>(done) : -1;
        }
        done += chunk;
    }
    usb_msc_sync_cache(dev);
    mutex_unlock(&dev->lock);
    return done;
}

static UsbMscDevice *alloc_msc_device()
{
    for (auto &dev : g_msc_devices) {
        if (!dev.used) {
            dev = {};
            dev.used = true;
            mutex_init(&dev.lock);
            return &dev;
        }
    }
    return nullptr;
}

static void make_disk_name(char *out, size_t out_size, uint32_t index)
{
    if (!out || out_size == 0)
        return;
    kstring::strncpy(out, "usb", out_size - 1);
    out[out_size - 1] = '\0';
    char num[16];
    kstring::itoa(index, num, 10);
    kstring::strncat(out, num, out_size - 1 - kstring::strlen(out));
}

} // namespace

void usb_msc_init()
{
    for (auto &dev : g_msc_devices)
        dev = {};
    g_next_disk_index = 0;
}

void usb_msc_device_connected(UsbDeviceInfo *usb)
{
    if (!usb || !usb->configured || !usb->has_mass_storage || usb->msc_bulk_in_endpoint == 0 ||
        usb->msc_bulk_out_endpoint == 0) {
        return;
    }

    auto *dev = alloc_msc_device();
    if (!dev) {
        KLOG(LogModule::Usb, LogLevel::Warn, "usb-storage: no free device slots");
        return;
    }

    dev->usb = usb;
    dev->lun = 0;
    dev->tag = 0x554E4900u | (g_next_disk_index & 0xFFu);

    uint8_t max_lun = 0;
    uint16_t transferred = 0;
    if (xhci_control_transfer(usb->slot_id, USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_INTERFACE,
                              USB_MSC_REQ_GET_MAX_LUN, 0, usb->msc_interface, 1, &max_lun, &transferred, false) &&
        transferred == 1) {
        dev->lun = 0;
    }

    char model[64] = {};
    usb_msc_inquiry(dev, model, sizeof(model));
    if (!usb_msc_test_unit_ready(dev) || !usb_msc_read_capacity(dev, &dev->block_count, &dev->block_size)) {
        KLOG(LogModule::Usb, LogLevel::Warn, "usb-storage: device on slot %d is not ready", usb->slot_id);
        dev->used = false;
        return;
    }

    if (dev->block_size < 512 || dev->block_size > 4096 || (dev->block_size & (dev->block_size - 1)) != 0) {
        KLOG(LogModule::Usb, LogLevel::Warn, "usb-storage: unsupported logical block size %u", dev->block_size);
        dev->used = false;
        return;
    }

    make_disk_name(dev->name, sizeof(dev->name), g_next_disk_index++);
    dev->block_dev = {};
    dev->block_dev.name = dev->name;
    kstring::strncpy(dev->block_dev.model, model[0] ? model : "USB Mass Storage", sizeof(dev->block_dev.model) - 1);
    kstring::strncpy(dev->block_dev.display_name, dev->block_dev.model, sizeof(dev->block_dev.display_name) - 1);
    dev->block_dev.block_size = dev->block_size;
    dev->block_dev.total_blocks = dev->block_count;
    dev->block_dev.is_partition = false;
    dev->block_dev.has_partitions = false;
    dev->block_dev.partition_index = 0;
    dev->block_dev.start_lba = 0;
    dev->block_dev.parent = nullptr;
    dev->block_dev.read_blocks = usb_msc_read_blocks;
    dev->block_dev.write_blocks = usb_msc_write_blocks;
    dev->block_dev.private_data = dev;
    dev->ready = true;

    block_dev_register(&dev->block_dev);
    partition_scan_all();
    KLOG(LogModule::Usb, LogLevel::Success, "usb-storage: registered %s (%s, %llu blocks of %u bytes)",
         dev->block_dev.name, dev->block_dev.model, dev->block_count, dev->block_size);
}

void usb_msc_device_disconnected(const UsbDeviceInfo *usb)
{
    if (!usb)
        return;
    for (auto &dev : g_msc_devices) {
        if (dev.used && dev.usb && dev.usb->slot_id == usb->slot_id) {
            dev.ready = false;
            dev.usb = nullptr;
        }
    }
}
