/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */


#include <sys/cdefs.h>

#include "drivers/virtio.hh"
#include "drivers/virtio-net.hh"
#include "drivers/pci-device.hh"
#include <osv/interrupt.hh>

#include <osv/mempool.hh>
#include <osv/mmu.hh>

#include <string>
#include <string.h>
#include <map>
#include <errno.h>
#include <osv/debug.h>

#include <osv/sched.hh>
#include "osv/trace.hh"


#include <osv/device.h>
#include <osv/ioctl.h>
#include <bsd/sys/net/ethernet.h>
#include <bsd/sys/net/if_types.h>
#include <bsd/sys/sys/param.h>

#include <bsd/sys/net/ethernet.h>
#include <bsd/sys/net/if_vlan_var.h>
#include <bsd/sys/netinet/in.h>
#include <bsd/sys/netinet/ip.h>
#include <bsd/sys/netinet/udp.h>
#include <bsd/sys/netinet/tcp.h>

TRACEPOINT(trace_virtio_net_rx_packet, "if=%d, len=%d", int, int);
TRACEPOINT(trace_virtio_net_rx_wake, "");
TRACEPOINT(trace_virtio_net_fill_rx_ring, "if=%d", int);
TRACEPOINT(trace_virtio_net_fill_rx_ring_added, "if=%d, added=%d", int, int);
TRACEPOINT(trace_virtio_net_tx_packet, "if=%d, len=%d", int, int);
TRACEPOINT(trace_virtio_net_tx_failed_add_buf, "if=%d", int);
TRACEPOINT(trace_virtio_net_tx_no_space_calling_gc, "if=%d", int);
using namespace memory;

// TODO list
// irq thread affinity and tx affinity
// tx zero copy
// vlans?

namespace virtio {

int net::_instance = 0;

#define net_tag "virtio-net"
#define net_d(...)   tprintf_d(net_tag, __VA_ARGS__)
#define net_i(...)   tprintf_i(net_tag, __VA_ARGS__)
#define net_w(...)   tprintf_w(net_tag, __VA_ARGS__)
#define net_e(...)   tprintf_e(net_tag, __VA_ARGS__)

static int if_ioctl(struct ifnet* ifp, u_long command, caddr_t data)
{
    net_d("if_ioctl %x", command);

    int error = 0;
    switch(command) {
    case SIOCSIFMTU:
        net_d("SIOCSIFMTU");
        break;
    case SIOCSIFFLAGS:
        net_d("SIOCSIFFLAGS");
        /* Change status ifup, ifdown */
        if (ifp->if_flags & IFF_UP) {
            ifp->if_drv_flags |= IFF_DRV_RUNNING;
            net_d("if_up");
        } else {
            ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
            net_d("if_down");
        }
        break;
    case SIOCADDMULTI:
    case SIOCDELMULTI:
        net_d("SIOCDELMULTI");
        break;
    default:
        net_d("redirecting to ether_ioctl()...");
        error = ether_ioctl(ifp, command, data);
        break;
    }

    return error;
}

/**
 * Invalidate the local Tx queues.
 * @param ifp upper layer instance handle
 */
static void if_qflush(struct ifnet* ifp)
{
    /*
     * Since virtio_net currently doesn't have any Tx queue we just
     * flush the upper layer queues.
     */
    ::if_qflush(ifp);
}

/**
 * Transmits a single mbuf instance.
 * @param ifp upper layer instance handle
 * @param m_head mbuf to transmit
 *
 * @return 0 in case of success and an appropriate error code
 *         otherwise
 */
static int if_transmit(struct ifnet* ifp, struct mbuf* m_head)
{
    net* vnet = (net*)ifp->if_softc;

    net_d("%s_start", __FUNCTION__);

    /* Process packets */
    vnet->_tx_ring_lock.lock();

    net_d("*** processing packet! ***");

    unsigned idx = vnet->pick_txq(m_head);
    int error = vnet->tx_locked(idx, m_head);

    if (!error)
        vnet->kick(2 * idx + 1);
    else
        printf("if_transmit error %d\n", error);

    vnet->_tx_ring_lock.unlock();

    return error;
}

static void if_init(void* xsc)
{
    net_d("Virtio-net init");
}

/**
 * Return all the statistics we have gathered.
 * @param ifp
 * @param out_data
 */
static void if_getinfo(struct ifnet* ifp, struct if_data* out_data)
{
    net* vnet = (net*)ifp->if_softc;

    // First - take the ifnet data
    memcpy(out_data, &ifp->if_data, sizeof(*out_data));

    // then fill the internal statistics we've gathered
    vnet->fill_stats(out_data);
}

void net::fill_stats(struct if_data* out_data) const
{
    // We currently support _num_queues / 2 pairs of Tx/Rx queues
    for (unsigned idx = 0; idx < _num_queues / 2; idx++) {
        fill_qstats(_rxq[idx], out_data);
        fill_qstats(_txq[idx], out_data);
    }
}

void net::fill_qstats(const struct rxq* rxq,
                             struct if_data* out_data) const
{
    out_data->ifi_ipackets += rxq->stats.rx_packets;
    out_data->ifi_ibytes   += rxq->stats.rx_bytes;
    out_data->ifi_iqdrops  += rxq->stats.rx_drops;
    out_data->ifi_ierrors  += rxq->stats.rx_csum_err;
}

void net::fill_qstats(const struct txq* txq,
                             struct if_data* out_data) const
{
    assert(!out_data->ifi_oerrors && !out_data->ifi_obytes && !out_data->ifi_opackets);
    out_data->ifi_opackets += txq->stats.tx_packets;
    out_data->ifi_obytes   += txq->stats.tx_bytes;
    out_data->ifi_oerrors  += txq->stats.tx_err + txq->stats.tx_drops;
}

bool net::ack_irq(unsigned idx)
{
    auto isr = virtio_conf_readb(VIRTIO_PCI_ISR);

    if (isr) {
        _rxq[idx]->vqueue->disable_interrupts();
        return true;
    } else {
        return false;
    }

}

net::net(pci::device& dev)
    : virtio_driver(dev)
{
    unsigned idx;

printf("net::net num_queues %d\n", _num_queues);
    for (idx = 0; idx < _num_queues / 2; idx++) {
        _rxq[idx] = new rxq(get_virt_queue(2 * idx), [this] { this->receiver(); }, sched::cpus[idx]);
        _txq[idx] = new txq(get_virt_queue(2 * idx + 1));
printf("net::net idx %d\n", idx);
    }
printf("net::net1 idx %d\n", idx);

    _driver_name = "virtio-net";
    virtio_i("VIRTIO NET INSTANCE");
    _id = _instance++;

//    setup_features();
//    read_config();

    _hdr_size = _mergeable_bufs ? sizeof(net_hdr_mrg_rxbuf) : sizeof(net_hdr);

    //initialize the BSD interface _if
    _ifn = if_alloc(IFT_ETHER);
    if (_ifn == NULL) {
       //FIXME: need to handle this case - expand the above function not to allocate memory and
       // do it within the constructor.
       net_w("if_alloc failed!");
       return;
    }

    if_initname(_ifn, "eth", _id);
    _ifn->if_mtu = ETHERMTU;
    _ifn->if_softc = static_cast<void*>(this);
    _ifn->if_flags = IFF_BROADCAST /*| IFF_MULTICAST*/;
    _ifn->if_ioctl = if_ioctl;
    _ifn->if_transmit = if_transmit;
    _ifn->if_qflush = if_qflush;
    _ifn->if_init = if_init;
    _ifn->if_getinfo = if_getinfo;

    int ifn_qsize = 0;
printf("net::net num_queues1 %d\n", _num_queues);
    for (idx = 0; idx < _num_queues / 2; idx++)
        ifn_qsize += _txq[idx]->vqueue->size();

    IFQ_SET_MAXLEN(&_ifn->if_snd, ifn_qsize);

    _ifn->if_capabilities = 0;

    if (_csum) {
        _ifn->if_capabilities |= IFCAP_TXCSUM;
        if (_host_tso4) {
            _ifn->if_capabilities |= IFCAP_TSO4;
            _ifn->if_hwassist = CSUM_TCP | CSUM_UDP | CSUM_TSO;
        }
    }

    if (_guest_csum) {
        _ifn->if_capabilities |= IFCAP_RXCSUM;
        if (_guest_tso4)
            _ifn->if_capabilities |= IFCAP_LRO;
    }

    _ifn->if_capenable = _ifn->if_capabilities | IFCAP_HWSTATS;

    //Start the polling thread before attaching it to the Rx interrupt
printf("net::net num_queues2 %d\n", _num_queues);
    for (idx = 0; idx < _num_queues / 2; idx++)
        _rxq[idx]->poll_task.start();

    ether_ifattach(_ifn, _config.mac);

    for (idx = 0; idx < _num_queues / 2; idx++) {
        if (dev.is_msix()) {
            _msi.easy_register({
                { 2 * idx, [&] { _rxq[idx]->vqueue->disable_interrupts(); }, &_rxq[idx]->poll_task },
                { 2 * idx + 1, [&] { _txq[idx]->vqueue->disable_interrupts(); }, nullptr }
            });
        } else {
            _gsi.set_ack_and_handler(dev.get_interrupt_line(), [=] { return this->ack_irq(idx); }, [=] { _rxq[idx]->poll_task.wake(); });
        }

        fill_rx_ring(idx);
    }

    add_dev_status(VIRTIO_CONFIG_S_DRIVER_OK);
}

net::~net()
{
    //TODO: In theory maintain the list of free instances and gc it
    // including the thread objects and their stack
    // Will need to clear the pending requests in the ring too

    // TODO: add a proper cleanup for a rx.poll_task() here.
    //
    // Since this will involve the rework of the virtio layer - make it for
    // all virtio drivers in a separate patchset.

    ether_ifdetach(_ifn);
    if_free(_ifn);
}

void net::read_config()
{
printf("net::read_config\n");
    virtio_driver::read_config();

    //read all of the net config  in one shot
    virtio_conf_read(virtio_pci_config_offset(), &_config, sizeof(_config));

    if (get_guest_feature_bit(VIRTIO_NET_F_MAC))
        net_i("The mac addr of the device is %x:%x:%x:%x:%x:%x",
                (u32)_config.mac[0],
                (u32)_config.mac[1],
                (u32)_config.mac[2],
                (u32)_config.mac[3],
                (u32)_config.mac[4],
                (u32)_config.mac[5]);

    _mergeable_bufs = get_guest_feature_bit(VIRTIO_NET_F_MRG_RXBUF);
    _status = get_guest_feature_bit(VIRTIO_NET_F_STATUS);
    _tso_ecn = get_guest_feature_bit(VIRTIO_NET_F_GUEST_ECN);
    _host_tso_ecn = get_guest_feature_bit(VIRTIO_NET_F_HOST_ECN);
    _csum = get_guest_feature_bit(VIRTIO_NET_F_CSUM);
    _guest_csum = get_guest_feature_bit(VIRTIO_NET_F_GUEST_CSUM);
    _guest_tso4 = get_guest_feature_bit(VIRTIO_NET_F_GUEST_TSO4);
    _host_tso4 = get_guest_feature_bit(VIRTIO_NET_F_HOST_TSO4);
    _guest_ufo = get_guest_feature_bit(VIRTIO_NET_F_GUEST_UFO);
    if (get_guest_feature_bit(VIRTIO_NET_F_MQ))
       printf("VIRTIO_NET_F_MQ is enabled\n");
    

    net_i("Features: %s=%d,%s=%d", "Status", _status, "TSO_ECN", _tso_ecn);
    net_i("Features: %s=%d,%s=%d", "Host TSO ECN", _host_tso_ecn, "CSUM", _csum);
    net_i("Features: %s=%d,%s=%d", "Guest_csum", _guest_csum, "guest tso4", _guest_tso4);
    net_i("Features: %s=%d", "host tso4", _host_tso4);
    printf("Features: max_queue_pairs %d\n", _config.max_virtqueue_pairs);
}

/**
 * Original comment from FreeBSD
 * Alternative method of doing receive checksum offloading. Rather
 * than parsing the received frame down to the IP header, use the
 * csum_offset to determine which CSUM_* flags are appropriate. We
 * can get by with doing this only because the checksum offsets are
 * unique for the things we care about.
 *
 * @return true if csum is bad and false if csum is ok (!!!)
 */
bool net::bad_rx_csum(struct mbuf* m, struct net_hdr* hdr)
{
    struct ether_header* eh;
    struct ether_vlan_header* evh;
    struct udphdr* udp;
    int csum_len;
    u16 eth_type;

    csum_len = hdr->csum_start + hdr->csum_offset;

    if (csum_len < (int)sizeof(struct ether_header) + (int)sizeof(struct ip))
        return true;
    if (m->m_hdr.mh_len < csum_len)
        return true;

    eh = mtod(m, struct ether_header*);
    eth_type = ntohs(eh->ether_type);
    if (eth_type == ETHERTYPE_VLAN) {
        evh = mtod(m, struct ether_vlan_header*);
        eth_type = ntohs(evh->evl_proto);
    }

    // How come - no support for IPv6?!
    if (eth_type != ETHERTYPE_IP) {
        return true;
    }

    /* Use the offset to determine the appropriate CSUM_* flags. */
    switch (hdr->csum_offset) {
    case offsetof(struct udphdr, uh_sum):
        if (m->m_hdr.mh_len < hdr->csum_start + (int)sizeof(struct udphdr))
            return true;
        udp = (struct udphdr*)(mtod(m, uint8_t*) + hdr->csum_start);
        if (udp->uh_sum == 0)
            return false;

        /* FALLTHROUGH */

    case offsetof(struct tcphdr, th_sum):
        m->M_dat.MH.MH_pkthdr.csum_flags |= CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
        m->M_dat.MH.MH_pkthdr.csum_data = 0xFFFF;
        break;

    default:
        return true;
    }

    return false;
}

void net::receiver()
{
    unsigned idx = sched::cpu::current()->id;
    printf("receiver idx %d\n", idx);
    struct rxq* rxq = _rxq[idx];
    vring* vq = rxq->vqueue;
    while (1) {

        // Wait for rx queue (used elements)
        virtio_driver::wait_for_queue(vq, &vring::used_ring_not_empty);
        trace_virtio_net_rx_wake();

        u32 len;
        int nbufs;
        struct mbuf* m = static_cast<struct mbuf*>(vq->get_buf_elem(&len));
        u32 offset = _hdr_size;
        u64 rx_drops = 0, rx_packets = 0, csum_ok = 0;
        u64 csum_err = 0, rx_bytes = 0;

        // use local header that we copy out of the mbuf since we're
        // truncating it.
        net_hdr_mrg_rxbuf* mhdr;

        while (m != nullptr) {

            // TODO: should get out of the loop
            vq->get_buf_finalize();

            // Bad packet/buffer - discard and continue to the next one
            if (len < _hdr_size + ETHER_HDR_LEN) {
                rx_drops++;
                m_free(m);

                m = static_cast<struct mbuf*>(vq->get_buf_elem(&len));
                continue;
            }

            mhdr = mtod(m, net_hdr_mrg_rxbuf*);

            if (!_mergeable_bufs) {
                nbufs = 1;
            } else {
                nbufs = mhdr->num_buffers;
            }

            m->M_dat.MH.MH_pkthdr.len = len;
            m->M_dat.MH.MH_pkthdr.rcvif = _ifn;
            m->M_dat.MH.MH_pkthdr.csum_flags = 0;
            m->m_hdr.mh_len = len;

            struct mbuf* m_head, *m_tail;
            m_tail = m_head = m;

            // Read the fragments
            while (--nbufs > 0) {
                m = static_cast<struct mbuf*>(vq->get_buf_elem(&len));
                if (m == nullptr) {
                    rx_drops++;
                    break;
                }

                vq->get_buf_finalize();

                if (m->m_hdr.mh_len < (int)len)
                    len = m->m_hdr.mh_len;

                m->m_hdr.mh_len = len;
                m->m_hdr.mh_flags &= ~M_PKTHDR;
                m_head->M_dat.MH.MH_pkthdr.len += len;
                m_tail->m_hdr.mh_next = m;
                m_tail = m;
            }

            // skip over the virtio header bytes (offset)
            // that aren't need for the above layer
            m_adj(m_head, offset);

            if ((_ifn->if_capenable & IFCAP_RXCSUM) &&
                (mhdr->hdr.flags &
                 net_hdr::VIRTIO_NET_HDR_F_NEEDS_CSUM)) {
                if (bad_rx_csum(m_head, &mhdr->hdr))
                    csum_err++;
                else
                    csum_ok++;

            }

            rx_packets++;
            rx_bytes += m_head->M_dat.MH.MH_pkthdr.len;

            bool fast_path = _ifn->if_classifier.post_packet(m_head);
            if (!fast_path) {
                (*_ifn->if_input)(_ifn, m_head);
            }

            trace_virtio_net_rx_packet(_ifn->if_index, rx_bytes);

            // The interface may have been stopped while we were
            // passing the packet up the network stack.
            if ((_ifn->if_drv_flags & IFF_DRV_RUNNING) == 0)
                break;

            // Move to the next packet
            m = static_cast<struct mbuf*>(vq->get_buf_elem(&len));
        }

        if (vq->refill_ring_cond())
            fill_rx_ring(idx);

        // Update the stats
        rxq->stats.rx_drops      += rx_drops;
        rxq->stats.rx_packets    += rx_packets;
        rxq->stats.rx_csum       += csum_ok;
        rxq->stats.rx_csum_err   += csum_err;
        rxq->stats.rx_bytes      += rx_bytes;
    }
}

void net::fill_rx_ring(unsigned idx)
{
    trace_virtio_net_fill_rx_ring(_ifn->if_index);
    int added = 0;
    vring* vq = _rxq[idx]->vqueue;

    while (vq->avail_ring_not_empty()) {
        struct mbuf* m = m_getjcl(M_NOWAIT, MT_DATA, M_PKTHDR, MCLBYTES);
        if (!m)
            break;

        m->m_hdr.mh_len = MCLBYTES;
        u8* mdata = mtod(m, u8*);

        vq->init_sg();
        vq->add_in_sg(mdata, m->m_hdr.mh_len);
        if (!vq->add_buf(m)) {
            m_freem(m);
            break;
        }
        added++;
    }

    trace_virtio_net_fill_rx_ring_added(_ifn->if_index, added);

    if (added)
        vq->kick();
}

// TODO: Does it really have to be "locked"?
int net::tx_locked(unsigned idx, struct mbuf* m_head, bool flush)
{
    DEBUG_ASSERT(_tx_ring_lock.owned(), "_tx_ring_lock is not locked!");

    struct mbuf* m;
    net_req* req = new net_req;
    struct txq* txq = _txq[idx];
    vring* vq = txq->vqueue;
    auto vq_sg_vec = &vq->_sg_vec;
    int rc = 0;
    struct txq_stats* stats = &txq->stats;
    u64 tx_bytes = 0;

    req->um.reset(m_head);

    if (m_head->M_dat.MH.MH_pkthdr.csum_flags != 0) {
        m = tx_offload(m_head, &req->mhdr.hdr);
        if ((m_head = m) == nullptr) {
            delete req;

            /* The buffer is not well-formed */
            rc = EINVAL;
            goto out;
        }
    }

    vq->init_sg();
    vq->add_out_sg(static_cast<void*>(&req->mhdr), _hdr_size);

    for (m = m_head; m != NULL; m = m->m_hdr.mh_next) {
        int frag_len = m->m_hdr.mh_len;

        if (frag_len != 0) {
            net_d("Frag len=%d:", frag_len);
            req->mhdr.num_buffers++;

            vq->add_out_sg(m->m_hdr.mh_data, m->m_hdr.mh_len);
            tx_bytes += frag_len;
        }
    }

    if (!vq->avail_ring_has_room(vq->_sg_vec.size())) {
        // can't call it, this is a get buf thing
        if (vq->used_ring_not_empty()) {
            trace_virtio_net_tx_no_space_calling_gc(_ifn->if_index);
            tx_gc(idx);
        } else {
            net_d("%s: no room", __FUNCTION__);
            delete req;

            rc = ENOBUFS;
            goto out;
        }
    }

    if (!vq->add_buf(req)) {
        trace_virtio_net_tx_failed_add_buf(_ifn->if_index);
        delete req;

        rc = ENOBUFS;
        goto out;
    }

    trace_virtio_net_tx_packet(_ifn->if_index, vq_sg_vec->size());

out:

    /* Update the statistics */
    switch (rc) {
    case 0: /* success */
        stats->tx_bytes += tx_bytes;
        stats->tx_packets++;

        if (req->mhdr.hdr.flags & net_hdr::VIRTIO_NET_HDR_F_NEEDS_CSUM)
            stats->tx_csum++;

        if (req->mhdr.hdr.gso_type)
            stats->tx_tso++;

        break;
    case ENOBUFS:
        stats->tx_drops++;

        break;
    default:
        stats->tx_err++;
    }

    return rc;
}

struct mbuf*
net::tx_offload(struct mbuf* m, struct net_hdr* hdr)
{
    struct ether_header* eh;
    struct ether_vlan_header* evh;
    struct ip* ip;
    struct tcphdr* tcp;
    int ip_offset;
    u16 eth_type, csum_start;
    u8 ip_proto, gso_type = 0;

    ip_offset = sizeof(struct ether_header);
    if (m->m_hdr.mh_len < ip_offset) {
        if ((m = m_pullup(m, ip_offset)) == nullptr)
            return nullptr;
    }

    eh = mtod(m, struct ether_header*);
    eth_type = ntohs(eh->ether_type);
    if (eth_type == ETHERTYPE_VLAN) {
        ip_offset = sizeof(struct ether_vlan_header);
        if (m->m_hdr.mh_len < ip_offset) {
            if ((m = m_pullup(m, ip_offset)) == nullptr)
                return nullptr;
        }
        evh = mtod(m, struct ether_vlan_header*);
        eth_type = ntohs(evh->evl_proto);
    }

    switch (eth_type) {
    case ETHERTYPE_IP:
        if (m->m_hdr.mh_len < ip_offset + (int)sizeof(struct ip)) {
            m = m_pullup(m, ip_offset + sizeof(struct ip));
            if (m == nullptr)
                return nullptr;
        }

        ip = (struct ip*)(mtod(m, uint8_t*) + ip_offset);
        ip_proto = ip->ip_p;
        csum_start = ip_offset + (ip->ip_hl << 2);
        gso_type = net::net_hdr::VIRTIO_NET_HDR_GSO_TCPV4;
        break;

    default:
        return m;
    }

    if (m->M_dat.MH.MH_pkthdr.csum_flags & VIRTIO_NET_CSUM_OFFLOAD) {
        hdr->flags |= net_hdr::VIRTIO_NET_HDR_F_NEEDS_CSUM;
        hdr->csum_start = csum_start;
        hdr->csum_offset = m->M_dat.MH.MH_pkthdr.csum_data;
    }

    if (m->M_dat.MH.MH_pkthdr.csum_flags & CSUM_TSO) {
        if (ip_proto != IPPROTO_TCP)
            return m;

        if (m->m_hdr.mh_len < csum_start + (int)sizeof(struct tcphdr)) {
            m = m_pullup(m, csum_start + sizeof(struct tcphdr));
            if (m == nullptr)
                return nullptr;
        }

        tcp = (struct tcphdr*)(mtod(m, uint8_t*) + csum_start);
        hdr->gso_type = gso_type;
        hdr->hdr_len = csum_start + (tcp->th_off << 2);
        hdr->gso_size = m->M_dat.MH.MH_pkthdr.tso_segsz;

        if (tcp->th_flags & TH_CWR) {
            if (!_tso_ecn) {
                virtio_w("TSO with ECN not supported by host\n");
                m_freem(m);
                return nullptr;
            }

            hdr->flags |= net_hdr::VIRTIO_NET_HDR_GSO_ECN;
        }
    }

    return m;
}

// TODO: it still need more effort
//  1. a better way to select tx
//  2. work together with Vlad Zolotarov's Tx softirq affinity
unsigned net::pick_txq(mbuf* m)
{
//    struct ether_header* eh;
//    u16 hash;

//    eh = mtod(m, struct ether_header*);
//    hash = ntohs(eh->ether_type);

//    return (unsigned) (((u64) hash * (_num_queues / 2)) >> 32);

      unsigned idx = sched::cpu::current()->id;
      if (idx >= _num_queues / 2)
          printf("pick idx %d\n", idx);

      return idx;
}

void net::tx_gc(unsigned idx)
{
    net_req* req;
    u32 len;
    vring* vq = _txq[idx]->vqueue;

    req = static_cast<net_req*>(vq->get_buf_elem(&len));

    while(req != nullptr) {
        delete req;
        vq->get_buf_finalize();

        req = static_cast<net_req*>(vq->get_buf_elem(&len));
    }
    vq->get_buf_gc();
}

u32 net::get_driver_features()
{
printf("net::get_driver_features\n");
    u32 base = virtio_driver::get_driver_features();
    return (base | (1 << VIRTIO_NET_F_MAC)        \
                 | (1 << VIRTIO_NET_F_MRG_RXBUF)  \
                 | (1 << VIRTIO_NET_F_STATUS)     \
                 | (1 << VIRTIO_NET_F_CSUM)       \
                 | (1 << VIRTIO_NET_F_GUEST_CSUM) \
                 | (1 << VIRTIO_NET_F_GUEST_TSO4) \
                 | (1 << VIRTIO_NET_F_HOST_ECN)   \
                 | (1 << VIRTIO_NET_F_HOST_TSO4)  \
                 | (1 << VIRTIO_NET_F_GUEST_ECN)  \
                 | (1 << VIRTIO_NET_F_GUEST_UFO)  \
                 | (1 << VIRTIO_NET_F_MQ)
            );
}

hw_driver* net::probe(hw_device* dev)
{
    return virtio::probe<net, VIRTIO_NET_DEVICE_ID>(dev);
}

}

