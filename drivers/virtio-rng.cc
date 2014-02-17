/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "drivers/virtio-rng.hh"
#include <osv/mmu.hh>
#include <algorithm>
#include <iterator>

using namespace std;

namespace virtio {
rng::rng(pci::device& pci_dev)
    : virtio_driver(pci_dev)
    , _gsi(pci_dev.get_interrupt_line(), [&] { return ack_irq(); }, [&] { handle_irq(); })
    , _thread([&] { worker(); }, sched::thread::attr().name("virtio-rng"))
{
    probe_virt_queues();
    _queue = get_virt_queue(0);

    add_dev_status(VIRTIO_CONFIG_S_DRIVER_OK);

    _thread.start();

    randomdev::random_device::register_source(this);
}

rng::~rng()
{
}

size_t rng::get_random_bytes(char* buf, size_t size)
{
    WITH_LOCK(_mtx) {
        _consumer.wait_until(_mtx, [&] {
            return _entropy.size() > 0;
        });
        auto len = std::min(_entropy.size(), size);
        copy_n(_entropy.begin(), len, buf);
        _entropy.erase(_entropy.begin(), _entropy.begin() + len);
        _producer.wake_one();
        return len;
    }
}

void rng::handle_irq()
{
    _thread.wake();
}

bool rng::ack_irq()
{
    return virtio_conf_readb(VIRTIO_PCI_ISR);
}

void rng::worker()
{
    for (;;) {
        WITH_LOCK(_mtx) {
            _producer.wait_until(_mtx, [&] {
                return _entropy.size() < _pool_size;
            });
            refill();
            _consumer.wake_all();
        }
    }
}

void rng::refill()
{
    auto remaining = _pool_size - _entropy.size();
    vector<char> buf(remaining);
    u32 len;
    DROP_LOCK(_mtx) {
        void* data = buf.data();

        _queue->init_sg();
        _queue->add_in_sg(data, remaining);

        while (!_queue->add_buf(data)) {
            while (!_queue->avail_ring_has_room(_queue->_sg_vec.size())) {
                sched::thread::wait_until([&] {return _queue->used_ring_can_gc();});
                _queue->get_buf_gc();
            }
        }
        _queue->kick();

        wait_for_queue(_queue, &vring::used_ring_not_empty);

        _queue->get_buf_elem(&len);
        _queue->get_buf_finalize();
    }
    copy_n(buf.begin(), len, back_inserter(_entropy));
}

hw_driver* rng::probe(hw_device* dev)
{
    return virtio::probe<rng, VIRTIO_RNG_DEVICE_ID>(dev);
}

}
