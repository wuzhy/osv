/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <string.h>

#include "drivers/virtio.hh"
#include "virtio-vring.hh"
#include <osv/debug.h>
#include "osv/trace.hh"

using namespace pci;

TRACEPOINT(trace_virtio_wait_for_queue, "queue(%p) have_elements=%d", void*, int);

namespace virtio {

int virtio_driver::_disk_idx = 0;

virtio_driver::virtio_driver(pci::device& dev)
    : hw_driver()
    , _dev(dev)
    , _msi(&dev)
    , _num_queues(0)
    , _bar1(nullptr)
    , _cap_indirect_buf(false)
{
    for (unsigned i = 0; i < max_virtqueues_nr; i++) {
        _queues[i] = nullptr;
    }
    parse_pci_config();

    _dev.set_bus_master(true);

    _dev.msix_enable();

    //make sure the queue is reset
    reset_host_side();

    // Acknowledge device
    add_dev_status(VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    // Generic init of virtqueues
    // probe_virt_queues();
}

virtio_driver::~virtio_driver()
{
    reset_host_side();
    free_queues();
}

void virtio_driver::setup_features()
{
    u32 dev_features = get_device_features();
    u32 drv_features = this->get_driver_features();

    u32 subset = dev_features & drv_features;

    //notify the host about the features in used according
    //to the virtio spec
    for (int i = 0; i < 32; i++)
        if (subset & (1 << i))
            virtio_d("%s: found feature intersec of bit %d", __FUNCTION__,  i);

    if (subset & (1 << VIRTIO_RING_F_INDIRECT_DESC))
        set_indirect_buf_cap(true);

    if (subset & (1 << VIRTIO_RING_F_EVENT_IDX))
            set_event_idx_cap(true);

    set_guest_features(subset);
}

void virtio_driver::dump_config()
{
    u8 B, D, F;
    _dev.get_bdf(B, D, F);

    _dev.dump_config();
    virtio_d("%s [%x:%x.%x] vid:id=%x:%x", get_name().c_str(),
        (u16)B, (u16)D, (u16)F,
        _dev.get_vendor_id(),
        _dev.get_device_id());

    virtio_d("    virtio features: ");
    for (int i = 0; i < 32; i++)
        virtio_d(" %d ", get_device_feature_bit(i));
}

bool virtio_driver::parse_pci_config()
{
    if (!_dev.parse_pci_config()) {
        return false;
    }

    // Test whether bar1 is present
    _bar1 = _dev.get_bar(1);
    if (_bar1 == nullptr) {
        return false;
    }

    // Check ABI version
    u8 rev = _dev.get_revision_id();
    if (rev != VIRTIO_PCI_ABI_VERSION) {
        virtio_e("Wrong virtio revision=%x", rev);
        return false;
    }

    // Check device ID
    u16 dev_id = _dev.get_device_id();
    if ((dev_id < VIRTIO_PCI_ID_MIN) || (dev_id > VIRTIO_PCI_ID_MAX)) {
        virtio_e("Wrong virtio dev id %x", dev_id);
        return false;
    }

    return true;
}

void virtio_driver::reset_host_side()
{
    set_dev_status(0);
}

void virtio_driver::free_queues()
{
    for (unsigned i = 0; i < max_virtqueues_nr; i++) {
        if (nullptr != _queues[i]) {
            delete (_queues[i]);
            _queues[i] = nullptr;
        }
    }
}

bool virtio_driver::kick(int queue)
{
    virtio_conf_writew(VIRTIO_PCI_QUEUE_NOTIFY, queue);
    return true;
}

void virtio_driver::probe_virt_queues()
{
    u16 qsize = 0;

    do {

        if (_num_queues >= max_virtqueues_nr) {
            return;
        }

        // Read queue size
        virtio_conf_writew(VIRTIO_PCI_QUEUE_SEL, _num_queues);
        qsize = virtio_conf_readw(VIRTIO_PCI_QUEUE_NUM);
        if (0 == qsize) {
            break;
        }

        // Init a new queue
        vring* queue = new vring(this, qsize, _num_queues);
        _queues[_num_queues] = queue;

        if (_dev.is_msix()) {
            // Setup queue_id:entry_id 1:1 correlation...
            virtio_conf_writew(VIRTIO_MSI_QUEUE_VECTOR, _num_queues);
            if (virtio_conf_readw(VIRTIO_MSI_QUEUE_VECTOR) != _num_queues) {
                virtio_e("Setting MSIx entry for queue %d failed.", _num_queues);
                return;
            }
        }

        _num_queues++;

        // Tell host about pfn
        // TODO: Yak, this is a bug in the design, on large memory we'll have PFNs > 32 bit
        // Dor to notify Rusty
        virtio_conf_writel(VIRTIO_PCI_QUEUE_PFN, (u32)(queue->get_paddr() >> VIRTIO_PCI_QUEUE_ADDR_SHIFT));

        // Debug print
        virtio_d("Queue[%d] -> size %d, paddr %x", (_num_queues-1), qsize, queue->get_paddr());

        // Make sure that qnum is 2 * ncpus + 1 if ctlq is supported,
        // otherwise it's 2 * ncpus
        if (_num_queues >= 2 * sched::cpus.size())
            return;
    } while (true);
}

vring* virtio_driver::get_virt_queue(unsigned idx)
{
    if (idx >= _num_queues) {
        return nullptr;
    }

    return _queues[idx];
}

void virtio_driver::wait_for_queue(vring* queue, bool (vring::*pred)() const)
{
    sched::thread::wait_until([queue,pred] {
        bool have_elements = (queue->*pred)();
        if (!have_elements) {
            queue->enable_interrupts();

            // we must check that the ring is not empty *after*
            // we enable interrupts to avoid a race where a packet
            // may have been delivered between queue->used_ring_not_empty()
            // and queue->enable_interrupts() above
            have_elements = (queue->*pred)();
            if (have_elements) {
                queue->disable_interrupts();
            }
        }

        trace_virtio_wait_for_queue(queue, have_elements);
        return have_elements;
    });
}

u32 virtio_driver::get_device_features()
{
    return virtio_conf_readl(VIRTIO_PCI_HOST_FEATURES);
}

bool virtio_driver::get_device_feature_bit(int bit)
{
    return get_virtio_config_bit(VIRTIO_PCI_HOST_FEATURES, bit);
}

void virtio_driver::set_guest_features(u32 features)
{
    virtio_conf_writel(VIRTIO_PCI_GUEST_FEATURES, features);
}

void virtio_driver::set_guest_feature_bit(int bit, bool on)
{
    set_virtio_config_bit(VIRTIO_PCI_GUEST_FEATURES, bit, on);
}

u32 virtio_driver::get_guest_features()
{
    return virtio_conf_readl(VIRTIO_PCI_GUEST_FEATURES);
}

bool virtio_driver::get_guest_feature_bit(int bit)
{
    return get_virtio_config_bit(VIRTIO_PCI_GUEST_FEATURES, bit);
}


u8 virtio_driver::get_dev_status()
{
    return virtio_conf_readb(VIRTIO_PCI_STATUS);
}

void virtio_driver::set_dev_status(u8 status)
{
    virtio_conf_writeb(VIRTIO_PCI_STATUS, status);
}

void virtio_driver::add_dev_status(u8 status)
{
    set_dev_status(get_dev_status() | status);
}

void virtio_driver::del_dev_status(u8 status)
{
    set_dev_status(get_dev_status() & ~status);
}

bool virtio_driver::get_virtio_config_bit(u32 offset, int bit)
{
    return virtio_conf_readl(offset) & (1 << bit);
}

void virtio_driver::set_virtio_config_bit(u32 offset, int bit, bool on)
{
    u32 val = virtio_conf_readl(offset);
    u32 newval = ( val & ~(1 << bit) ) | ((int)(on)<<bit);
    virtio_conf_writel(offset, newval);
}

void virtio_driver::virtio_conf_write(u32 offset, void* buf, int length)
{
    u8* ptr = reinterpret_cast<u8*>(buf);
    for (int i = 0; i < length; i++)
        _bar1->writeb(offset + i, ptr[i]);
}

void virtio_driver::virtio_conf_read(u32 offset, void* buf, int length)
{
    unsigned char* ptr = reinterpret_cast<unsigned char*>(buf);
    for (int i = 0; i < length; i++)
        ptr[i] = _bar1->readb(offset + i);
}

}

