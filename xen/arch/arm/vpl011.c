/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arch/arm/vpl011.c
 *
 * Virtual PL011 UART
 */

#define XEN_WANT_FLEX_CONSOLE_RING 1

#include <xen/errno.h>
#include <xen/event.h>
#include <xen/guest_access.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/console.h>
#include <xen/serial.h>
#include <public/domctl.h>
#include <public/io/console.h>
#include <asm/pl011-uart.h>
#include <asm/vgic-emul.h>
#include <asm/vpl011.h>
#include <asm/vreg.h>

/* PL011 peripheral IDs (ID2 = 0x34 -> UART revision r1p5) */
static const char pl011_periph_id[] = { 0x11, 0x10, 0x34, 0x00 };

/* PL011 cell IDs */
static const char pl011_cell_id[] = { 0x0d, 0xf0, 0x05, 0xb1 };

/*
 * Since pl011 registers are 32-bit registers, all registers
 * are handled similarly allowing 8-bit, 16-bit and 32-bit
 * accesses except 64-bit access.
 */
static bool vpl011_reg32_check_access(struct hsr_dabt dabt)
{
    return (dabt.size != DABT_DOUBLE_WORD);
}

static void vpl011_update_interrupt_status(struct domain *d)
{
    struct vpl011 *vpl011 = &d->arch.vpl011;
    uint32_t uartmis = vpl011->uartris & vpl011->uartimsc;

    /*
     * This function is expected to be called with the lock taken.
     */
    ASSERT(spin_is_locked(&vpl011->lock));

#ifndef CONFIG_NEW_VGIC
    /*
     * TODO: PL011 interrupts are level triggered which means
     * that interrupt needs to be set/clear instead of being
     * injected. However, currently vGIC does not handle level
     * triggered interrupts properly. This function needs to be
     * revisited once vGIC starts handling level triggered
     * interrupts.
     */

    /*
     * Raise an interrupt only if any additional interrupt
     * status bit has been set since the last time.
     */
    if ( uartmis & ~vpl011->shadow_uartmis )
        vgic_inject_irq(d, NULL, vpl011->virq, true);

    vpl011->shadow_uartmis = uartmis;
#else
    vgic_inject_irq(d, NULL, vpl011->virq, uartmis);
#endif
}

static inline bool vpl011_fifo_enabled(const struct vpl011 *vpl011)
{
    /*
     * For SBSA UART, FIFO is always enabled.
     * For PL011, FIFO is enabled if FEN bit of LCR_H register is set.
     */
    return ( (vpl011->sbsa) ? true : !!(vpl011->uartlcr & FEN) );
}

/*
 * Below two helpers are used to return the current RX/TX threshold, on which
 * interrupt assertion/de-assertion depends. @in_size, @out_size parameters
 * are used to pass sizes of TX, RX buffers which can be bigger than the PL011
 * FIFO size (32 bytes) for performance reasons, in which case only the last
 * VPL011_FIFO_SIZE bytes are used to calculate current threshold. When FIFO
 * is disabled, it acts as a one-byte holding register. Therefore, the trigger
 * level (based on the part of the ring buffer used to emulate FIFO):
 * - for RX is (VPL011_FIFO_SIZE - 1), so that IRQ is asserted after receiving
 *   a character and de-asserted when it is gone,
 * - for TX is VPL011_FIFO_SIZE, so that IRQ is asserted as long as there is
 *   a place for a character and de-asserted after write.
 */
static unsigned int vpl011_get_rx_threshold(const struct vpl011 *vpl011,
                                            XENCONS_RING_IDX in_size)
{
    ASSERT(in_size >= VPL011_FIFO_SIZE);

    if ( vpl011_fifo_enabled(vpl011) )
        return in_size - vpl011->rx_fifo_level;

    return in_size - (VPL011_FIFO_SIZE - 1);
}

static unsigned int vpl011_get_tx_threshold(const struct vpl011 *vpl011,
                                            XENCONS_RING_IDX out_size)
{
    ASSERT(out_size >= VPL011_FIFO_SIZE);

    if ( vpl011_fifo_enabled(vpl011) )
        return out_size - vpl011->tx_fifo_level;

    return out_size - VPL011_FIFO_SIZE;
}

static void vpl011_update_fifo_level(struct vpl011 *vpl011)
{
    /* ARM DDI 0183G, Table 3-13 */
    static const unsigned int levels[] = {
        (VPL011_FIFO_SIZE / 8),
        (VPL011_FIFO_SIZE / 4),
        (VPL011_FIFO_SIZE / 2),
        ((VPL011_FIFO_SIZE * 3) / 4),
        ((VPL011_FIFO_SIZE * 7) / 8)
    };
    unsigned int flsel;

    ASSERT(!vpl011->sbsa);

    /* Bits 0:2 store TX FIFO level select */
    flsel = vpl011->uartifls & 0x7;
    if ( flsel <= 4 )
        vpl011->tx_fifo_level = VPL011_FIFO_SIZE - levels[flsel];

    /* Bits 3:5 store RX FIFO level select */
    flsel = (vpl011->uartifls & 0x38) >> 3;
    if ( flsel <= 4 )
        vpl011->rx_fifo_level = VPL011_FIFO_SIZE - levels[flsel];
}

void vpl011_reset_fifo(struct domain *d)
{
    struct vpl011 *vpl011 = &d->arch.vpl011;
    XENCONS_RING_IDX in_prod;

    ASSERT(!vpl011->sbsa);
    ASSERT(spin_is_locked(&vpl011->lock));

    /*
     * FIFO reset caused by setting/clearing FEN bit of LCR_H register should
     * normally occur when there is no data in the FIFOs (otherwise there will
     * be a loss of characters) which can be assessed by checking TXFE/BUSY
     * and RXFE bits. However, due to performance reasons we handle BUSY bit
     * differently (when backend is in domain) which can lead to character loss
     * if a guest relies on BUSY bit and not TXFE. Therefore, we only reset
     * RX FIFO state by levelling ring index of consumer with producer (it is
     * expected that guest waits for RXFE to become set before resetting FIFO).
     */
    if ( !vpl011->backend_in_domain )
    {
        struct vpl011_xen_backend *intf = vpl011->backend.xen;

        in_prod = intf->in_prod;

        smp_mb();

        intf->in_cons = in_prod;
    }
    else
    {
        struct xencons_interface *intf = vpl011->backend.dom.ring_buf;

        in_prod = intf->in_prod;

        smp_mb();

        intf->in_cons = in_prod;

        /* Send an event to console backend to notify the above change */
        notify_via_xen_event_channel(d, vpl011->evtchn);
    }

    /* Guests might expect to see these flags resetted after FIFO reset */
    vpl011->uartfr &= ~(RXFF | TXFF);
    vpl011->uartfr |= (RXFE | TXFE);
}

/*
 * vpl011_write_data_xen writes chars from the vpl011 out buffer to the
 * console. Only to be used when the backend is Xen.
 */
static void vpl011_write_data_xen(struct domain *d, uint8_t data)
{
    unsigned long flags;
    struct vpl011 *vpl011 = &d->arch.vpl011;
    struct vpl011_xen_backend *intf = vpl011->backend.xen;
    struct domain *input = console_input_domain();

    VPL011_LOCK(d, flags);

    intf->out[intf->out_prod++] = data;
    if ( d == input )
    {
        if ( intf->out_prod == 1 )
        {
            printk("%c", data);
            intf->out_prod = 0;
        }
        else
        {
            if ( data != '\n' )
                intf->out[intf->out_prod++] = '\n';
            intf->out[intf->out_prod++] = '\0';
            printk("%s", intf->out);
            intf->out_prod = 0;
        }
    }
    else
    {
        if ( intf->out_prod == VPL011_OUT_BUF_SIZE - 2 ||
             data == '\n' )
        {
            if ( data != '\n' )
                intf->out[intf->out_prod++] = '\n';
            intf->out[intf->out_prod++] = '\0';
            printk("DOM%u: %s", d->domain_id, intf->out);
            intf->out_prod = 0;
        }
    }

    /*
     * When backend is in Xen, we tell guest we are always ready for new data
     * to be written. This is fulfilled by having:
     * - TXI/TXFE -> always set,
     * - TXFF/BUSY -> never set.
     */
    vpl011->uartris |= TXI;
    vpl011->uartfr |= TXFE;
    vpl011_update_interrupt_status(d);

    VPL011_UNLOCK(d, flags);
    if ( input != NULL )
        rcu_unlock_domain(input);
}

/*
 * vpl011_read_data_xen reads data when the backend is xen. Characters
 * are added to the vpl011 receive buffer by vpl011_rx_char_xen.
 */
static uint8_t vpl011_read_data_xen(struct domain *d)
{
    unsigned long flags;
    uint8_t data = 0;
    struct vpl011 *vpl011 = &d->arch.vpl011;
    struct vpl011_xen_backend *intf = vpl011->backend.xen;
    XENCONS_RING_IDX in_cons, in_prod;

    VPL011_LOCK(d, flags);

    in_cons = intf->in_cons;
    in_prod = intf->in_prod;

    smp_rmb();

    /*
     * It is expected that there will be data in the ring buffer when this
     * function is called since the guest is expected to read the data register
     * only if the RXFE flag is not set.
     * If the guest still does read when RXFE bit is set then 0 will be returned.
     */
    if ( xencons_queued(in_prod, in_cons, sizeof(intf->in)) > 0 )
    {
        unsigned int fifo_level;

        data = intf->in[xencons_mask(in_cons, sizeof(intf->in))];
        in_cons += 1;
        smp_mb();
        intf->in_cons = in_cons;

        fifo_level = xencons_queued(in_prod, in_cons, sizeof(intf->in));

        /* If the FIFO is now empty, we clear the receive timeout interrupt. */
        if ( fifo_level == 0 )
        {
            vpl011->uartfr |= RXFE;
            vpl011->uartris &= ~RTI;
        }

        /* If the FIFO is less than RX threshold, we clear the RX interrupt. */
        if ( fifo_level < vpl011_get_rx_threshold(vpl011, sizeof(intf->in)) )
            vpl011->uartris &= ~RXI;

        vpl011_update_interrupt_status(d);
    }
    else
        gprintk(XENLOG_ERR, "vpl011: Unexpected IN ring buffer empty\n");

    /*
     * We have consumed a character or the FIFO was empty, so clear the
     * "FIFO full" bit.
     */
    vpl011->uartfr &= ~RXFF;

    VPL011_UNLOCK(d, flags);

    return data;
}

static uint8_t vpl011_read_data(struct domain *d)
{
    unsigned long flags;
    uint8_t data = 0;
    struct vpl011 *vpl011 = &d->arch.vpl011;
    struct xencons_interface *intf = vpl011->backend.dom.ring_buf;
    XENCONS_RING_IDX in_cons, in_prod;

    VPL011_LOCK(d, flags);

    in_cons = intf->in_cons;
    in_prod = intf->in_prod;

    smp_rmb();

    /*
     * It is expected that there will be data in the ring buffer when this
     * function is called since the guest is expected to read the data register
     * only if the RXFE flag is not set.
     * If the guest still does read when RXFE bit is set then 0 will be returned.
     */
    if ( xencons_queued(in_prod, in_cons, sizeof(intf->in)) > 0 )
    {
        unsigned int fifo_level;

        data = intf->in[xencons_mask(in_cons, sizeof(intf->in))];
        in_cons += 1;
        smp_mb();
        intf->in_cons = in_cons;

        fifo_level = xencons_queued(in_prod, in_cons, sizeof(intf->in));

        /* If the FIFO is now empty, we clear the receive timeout interrupt. */
        if ( fifo_level == 0 )
        {
            vpl011->uartfr |= RXFE;
            vpl011->uartris &= ~RTI;
        }

        /* If the FIFO is less than RX threshold, we clear the RX interrupt. */
        if ( fifo_level < vpl011_get_rx_threshold(vpl011, sizeof(intf->in)) )
            vpl011->uartris &= ~RXI;

        vpl011_update_interrupt_status(d);
    }
    else
        gprintk(XENLOG_ERR, "vpl011: Unexpected IN ring buffer empty\n");

    /*
     * We have consumed a character or the FIFO was empty, so clear the
     * "FIFO full" bit.
     */
    vpl011->uartfr &= ~RXFF;

    VPL011_UNLOCK(d, flags);

    /*
     * Send an event to console backend to indicate that data has been
     * read from the IN ring buffer.
     */
    notify_via_xen_event_channel(d, vpl011->evtchn);

    return data;
}

static void vpl011_update_tx_fifo_status(struct vpl011 *vpl011,
                                         unsigned int fifo_level)
{
    struct xencons_interface *intf = vpl011->backend.dom.ring_buf;

    /* No TX FIFO handling when backend is in Xen */
    ASSERT(vpl011->backend_in_domain);

    BUILD_BUG_ON(sizeof(intf->out) < VPL011_FIFO_SIZE);

    /*
     * Set the TXI bit only when there is space for TX threshold bytes which
     * is the trigger level for asserting/de-assterting the TX interrupt.
     */
    if ( fifo_level <= vpl011_get_tx_threshold(vpl011, sizeof(intf->out)) )
        vpl011->uartris |= TXI;
    else
        vpl011->uartris &= ~TXI;
}

static void vpl011_write_data(struct domain *d, uint8_t data)
{
    unsigned long flags;
    struct vpl011 *vpl011 = &d->arch.vpl011;
    struct xencons_interface *intf = vpl011->backend.dom.ring_buf;
    XENCONS_RING_IDX out_cons, out_prod;

    VPL011_LOCK(d, flags);

    out_cons = intf->out_cons;
    out_prod = intf->out_prod;

    smp_mb();

    /*
     * It is expected that the ring is not full when this function is called
     * as the guest is expected to write to the data register only when the
     * TXFF flag is not set.
     * In case the guest does write even when the TXFF flag is set then the
     * data will be silently dropped.
     */
    if ( xencons_queued(out_prod, out_cons, sizeof(intf->out)) !=
         sizeof (intf->out) )
    {
        unsigned int fifo_level;

        intf->out[xencons_mask(out_prod, sizeof(intf->out))] = data;
        out_prod += 1;
        smp_wmb();
        intf->out_prod = out_prod;

        fifo_level = xencons_queued(out_prod, out_cons, sizeof(intf->out));

        if ( fifo_level == sizeof(intf->out) )
        {
            vpl011->uartfr |= TXFF;

            /*
             * This bit is set only when FIFO becomes full. This ensures that
             * the UART driver can write the early console data as fast as
             * possible, without waiting for the BUSY bit to get cleared before
             * writing each byte.
             */
            vpl011->uartfr |= BUSY;
        }

        vpl011_update_tx_fifo_status(vpl011, fifo_level);

        vpl011_update_interrupt_status(d);
    }
    else
        gprintk(XENLOG_ERR, "vpl011: Unexpected OUT ring buffer full\n");

    vpl011->uartfr &= ~TXFE;

    VPL011_UNLOCK(d, flags);

    /*
     * Send an event to console backend to indicate that there is
     * data in the OUT ring buffer.
     */
    notify_via_xen_event_channel(d, vpl011->evtchn);
}

static int vpl011_mmio_read(struct vcpu *v,
                            mmio_info_t *info,
                            register_t *r,
                            void *priv)
{
    struct hsr_dabt dabt = info->dabt;
    uint32_t vpl011_reg = (uint32_t)(info->gpa -
                                     v->domain->arch.vpl011.base_addr);
    struct vpl011 *vpl011 = &v->domain->arch.vpl011;
    struct domain *d = v->domain;
    unsigned long flags;

    switch ( vpl011_reg )
    {
    case DR:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        if ( vpl011->backend_in_domain )
            *r = vreg_reg32_extract(vpl011_read_data(d), info);
        else
            *r = vreg_reg32_extract(vpl011_read_data_xen(d), info);
        return 1;

    case RSR:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        /* It always returns 0 as there are no physical errors. */
        *r = 0;
        return 1;

    case FR:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartfr, info);
        VPL011_UNLOCK(d, flags);
        return 1;

   case ILPR:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartilpr, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case IBRD:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartibrd, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case FBRD:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartfbrd, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case LCR_H:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartlcr, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case CR:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartcr, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case IFLS:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartifls, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case RIS:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartris, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case MIS:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartris & vpl011->uartimsc,
                                info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case IMSC:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartimsc, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case ICR:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        /* Only write is valid. */
        return 0;

    case DMACR:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = vreg_reg32_extract(vpl011->uartdmacr, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case PERIPH_ID0 ... PERIPH_ID3:
        if ( vpl011->sbsa || (vpl011_reg % 4) ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = pl011_periph_id[(vpl011_reg - PERIPH_ID0) >> 2];
        VPL011_UNLOCK(d, flags);
        return 1;

    case CELL_ID0 ... CELL_ID3:
        if ( vpl011->sbsa || (vpl011_reg % 4) ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        *r = pl011_cell_id[(vpl011_reg - CELL_ID0) >> 2];
        VPL011_UNLOCK(d, flags);
        return 1;

unhandled:
    default:
        gprintk(XENLOG_ERR, "vpl011: unhandled read r%d offset %#08x\n",
                dabt.reg, vpl011_reg);
        goto read_as_zero;
    }

    ASSERT_UNREACHABLE();
    return 1;

read_as_zero:
    *r = 0;
    return 1;

bad_width:
    gprintk(XENLOG_ERR, "vpl011: bad read width %d r%d offset %#08x\n",
            dabt.size, dabt.reg, vpl011_reg);
    return 0;

}

static int vpl011_mmio_write(struct vcpu *v,
                             mmio_info_t *info,
                             register_t r,
                             void *priv)
{
    struct hsr_dabt dabt = info->dabt;
    uint32_t vpl011_reg = (uint32_t)(info->gpa -
                                     v->domain->arch.vpl011.base_addr);
    struct vpl011 *vpl011 = &v->domain->arch.vpl011;
    struct domain *d = v->domain;
    unsigned long flags;

    switch ( vpl011_reg )
    {
    case DR:
    {
        uint32_t data = 0;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        vreg_reg32_update(&data, r, info);
        data &= 0xFF;
        if ( vpl011->backend_in_domain )
            vpl011_write_data(v->domain, data);
        else
            vpl011_write_data_xen(v->domain, data);
        return 1;
    }

    case RSR: /* Nothing to clear. */
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        return 1;

    case FR:
    case RIS:
    case MIS:
        goto write_ignore;

    case ILPR:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        vreg_reg32_update(&vpl011->uartilpr, r, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case IBRD:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        vreg_reg32_update(&vpl011->uartibrd, r, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case FBRD:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        vreg_reg32_update(&vpl011->uartfbrd, r, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case LCR_H:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);

        /* Reset FIFOs on FIFO enable/disable */
        if ( (vpl011->uartlcr ^ r) & FEN )
            vpl011_reset_fifo(d);

        vreg_reg32_update(&vpl011->uartlcr, r, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case CR:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        vreg_reg32_update(&vpl011->uartcr, r, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case IFLS:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        vreg_reg32_update(&vpl011->uartifls, r, info);
        vpl011_update_fifo_level(vpl011);
        VPL011_UNLOCK(d, flags);
        return 1;

    case IMSC:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        vreg_reg32_update(&vpl011->uartimsc, r, info);
        vpl011_update_interrupt_status(v->domain);
        VPL011_UNLOCK(d, flags);
        return 1;

    case ICR:
        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        vreg_reg32_clearbits(&vpl011->uartris, r, info);
        vpl011_update_interrupt_status(d);
        VPL011_UNLOCK(d, flags);
        return 1;

    case DMACR:
        if ( vpl011->sbsa ) goto unhandled;

        if ( !vpl011_reg32_check_access(dabt) ) goto bad_width;

        VPL011_LOCK(d, flags);
        vreg_reg32_update(&vpl011->uartdmacr, r, info);
        VPL011_UNLOCK(d, flags);
        return 1;

    case PERIPH_ID0 ... PERIPH_ID3:
    case CELL_ID0 ... CELL_ID3:
        if ( vpl011->sbsa ) goto unhandled;

        goto write_ignore;

unhandled:
    default:
        gprintk(XENLOG_ERR, "vpl011: unhandled write r%d offset %#08x\n",
                dabt.reg, vpl011_reg);
        goto write_ignore;
    }

write_ignore:
    return 1;

bad_width:
    gprintk(XENLOG_ERR, "vpl011: bad write width %d r%d offset %#08x\n",
            dabt.size, dabt.reg, vpl011_reg);
    return 0;

}

static const struct mmio_handler_ops vpl011_mmio_handler = {
    .read = vpl011_mmio_read,
    .write = vpl011_mmio_write,
};

static void vpl011_data_avail(struct domain *d,
                              XENCONS_RING_IDX in_fifo_level,
                              XENCONS_RING_IDX in_size,
                              XENCONS_RING_IDX out_fifo_level,
                              XENCONS_RING_IDX out_size)
{
    struct vpl011 *vpl011 = &d->arch.vpl011;

    /**** Update the UART RX state ****/

    /* Clear the FIFO_EMPTY bit if the FIFO holds at least one character. */
    if ( in_fifo_level > 0 )
        vpl011->uartfr &= ~RXFE;

    /* Set the FIFO_FULL bit if the Xen buffer is full. */
    if ( in_fifo_level == in_size )
        vpl011->uartfr |= RXFF;

    /* Assert the RX interrupt if the FIFO crossed RX threshold. */
    if ( in_fifo_level >= vpl011_get_rx_threshold(vpl011, in_size) )
        vpl011->uartris |= RXI;

    /*
     * If the input queue is not empty, we assert the receive timeout interrupt.
     * As we don't emulate any timing here, so we ignore the actual timeout
     * of 32 baud cycles.
     */
    if ( in_fifo_level > 0 )
        vpl011->uartris |= RTI;

    /**** Update the UART TX state ****/

    if ( out_fifo_level != out_size )
    {
        vpl011->uartfr &= ~TXFF;

        /*
         * Clear the BUSY bit as soon as space becomes available
         * so that the UART driver can start writing more data
         * without any further delay.
         */
        vpl011->uartfr &= ~BUSY;

        /*
         * When backend is in Xen, we are always ready for new data to be
         * written (i.e. no TX FIFO handling), therefore we do not want
         * to change the TX FIFO status in such case.
         */
        if ( vpl011->backend_in_domain )
            vpl011_update_tx_fifo_status(vpl011, out_fifo_level);
    }

    vpl011_update_interrupt_status(d);

    if ( out_fifo_level == 0 )
        vpl011->uartfr |= TXFE;
}

/*
 * vpl011_rx_char_xen adds a char to a domain's vpl011 receive buffer.
 * It is only used when the vpl011 backend is in Xen.
 */
void vpl011_rx_char_xen(struct domain *d, char c)
{
    unsigned long flags;
    struct vpl011 *vpl011 = &d->arch.vpl011;
    struct vpl011_xen_backend *intf = vpl011->backend.xen;
    XENCONS_RING_IDX in_cons, in_prod, in_fifo_level;

    ASSERT(!vpl011->backend_in_domain);
    VPL011_LOCK(d, flags);

    in_cons = intf->in_cons;
    in_prod = intf->in_prod;
    if ( xencons_queued(in_prod, in_cons, sizeof(intf->in)) == sizeof(intf->in) )
    {
        VPL011_UNLOCK(d, flags);
        return;
    }

    intf->in[xencons_mask(in_prod, sizeof(intf->in))] = c;
    intf->in_prod = ++in_prod;

    in_fifo_level = xencons_queued(in_prod,
                                   in_cons,
                                   sizeof(intf->in));

    vpl011_data_avail(d, in_fifo_level, sizeof(intf->in), 0, VPL011_FIFO_SIZE);
    VPL011_UNLOCK(d, flags);
}

static void vpl011_notification(struct vcpu *v, unsigned int port)
{
    unsigned long flags;
    struct domain *d = v->domain;
    struct vpl011 *vpl011 = &d->arch.vpl011;
    struct xencons_interface *intf = vpl011->backend.dom.ring_buf;
    XENCONS_RING_IDX in_cons, in_prod, out_cons, out_prod;
    XENCONS_RING_IDX in_fifo_level, out_fifo_level;

    VPL011_LOCK(d, flags);

    in_cons = intf->in_cons;
    in_prod = intf->in_prod;
    out_cons = intf->out_cons;
    out_prod = intf->out_prod;

    smp_rmb();

    in_fifo_level = xencons_queued(in_prod,
                                   in_cons,
                                   sizeof(intf->in));

    out_fifo_level = xencons_queued(out_prod,
                                    out_cons,
                                    sizeof(intf->out));

    vpl011_data_avail(v->domain, in_fifo_level, sizeof(intf->in),
                      out_fifo_level, sizeof(intf->out));

    VPL011_UNLOCK(d, flags);
}

int domain_vpl011_init(struct domain *d,
                       struct vpl011_init_info *info,
                       bool sbsa)
{
    int rc;
    struct vpl011 *vpl011 = &d->arch.vpl011;

    if ( vpl011->backend.dom.ring_buf )
        return -EINVAL;

    /*
     * The VPL011 virq is GUEST_VPL011_SPI, except for direct-map domains
     * where the hardware value shall be used.
     * The logic here should stay in sync with the one in
     * create_domUs().
     */
    if ( is_domain_direct_mapped(d) )
    {
        const struct vuart_info *uart = serial_vuart_info(SERHND_DTUART);
        int vpl011_irq = serial_irq(SERHND_DTUART);

        if ( (uart != NULL) && (vpl011_irq > 0) )
        {
            vpl011->base_addr = uart->base_addr;
            vpl011->virq = vpl011_irq;
        }
        else
        {
            printk(XENLOG_ERR
                   "vpl011: Unable to re-use the Xen UART information.\n");
            return -EINVAL;
        }

        /*
         * Since the PL011 we emulate for the guest requires a 4KB region,
         * and on some Hardware (e.g. on some sunxi SoC), the UART MMIO
         * region is less than 4KB, in which case, there may exist multiple
         * devices within the same 4KB region, here adds the following check to
         * prevent potential known pitfalls
         */
        if ( uart->size < GUEST_PL011_SIZE )
        {
            printk(XENLOG_ERR
                   "vpl011: Can't re-use the Xen UART MMIO region as it is too small.\n");
            return -EINVAL;
        }
    }
    else
    {
        vpl011->base_addr = GUEST_PL011_BASE;
        vpl011->virq = GUEST_VPL011_SPI;
    }

    /*
     * info is NULL when the backend is in Xen.
     * info is != NULL when the backend is in a domain.
     */
    if ( info != NULL )
    {
        vpl011->backend_in_domain = true;

        /* Map the guest PFN to Xen address space. */
        rc =  prepare_ring_for_helper(d,
                                      gfn_x(info->gfn),
                                      &vpl011->backend.dom.ring_page,
                                      &vpl011->backend.dom.ring_buf);
        if ( rc < 0 )
            goto out;

        rc = alloc_unbound_xen_event_channel(d, 0, info->console_domid,
                                             vpl011_notification);
        if ( rc < 0 )
            goto out1;

        vpl011->evtchn = info->evtchn = rc;
    }
    else
    {
        vpl011->backend_in_domain = false;

        vpl011->backend.xen = xzalloc(struct vpl011_xen_backend);
        if ( vpl011->backend.xen == NULL )
        {
            rc = -ENOMEM;
            goto out;
        }
    }

    rc = vgic_reserve_virq(d, vpl011->virq);
    if ( !rc )
    {
        rc = -EINVAL;
        goto out1;
    }

    vpl011->sbsa = sbsa;

    vpl011->uartfr = TXFE | RXFE;

    /*
     * Initial TX/RX FIFO trigger level is set to 1/2 way. This stays constant
     * for SBSA UART but can be changed for PL011.
     */
    vpl011->tx_fifo_level = (VPL011_FIFO_SIZE / 2);
    vpl011->rx_fifo_level = (VPL011_FIFO_SIZE / 2);

    /* Additional reset state as required by PL011 */
    if ( !sbsa )
    {
        vpl011->uartcr = TXE | RXE;

        /* TXIFLSEL and RXIFLSEL set to 1/2 way */
        vpl011->uartifls = 0x12;
    }

    spin_lock_init(&vpl011->lock);

    register_mmio_handler(d, &vpl011_mmio_handler,
                          vpl011->base_addr, GUEST_PL011_SIZE, NULL);

    return 0;

out1:
    domain_vpl011_deinit(d);

out:
    return rc;
}

void domain_vpl011_deinit(struct domain *d)
{
    struct vpl011 *vpl011 = &d->arch.vpl011;

    if ( vpl011->virq )
    {
        vgic_free_virq(d, vpl011->virq);

        /*
         * Set to invalid irq (we use SPI) to prevent extra free and to avoid
         * freeing irq that could have already been reserved by someone else.
         */
        vpl011->virq = 0;
    }

    if ( vpl011->backend_in_domain )
    {
        if ( vpl011->backend.dom.ring_buf )
            destroy_ring_for_helper(&vpl011->backend.dom.ring_buf,
                                    vpl011->backend.dom.ring_page);

        if ( vpl011->evtchn )
        {
            free_xen_event_channel(d, vpl011->evtchn);

            /*
             * Set to invalid event channel port to prevent extra free and to
             * avoid freeing port that could have already been allocated for
             * other purposes.
             */
            vpl011->evtchn = 0;
        }
    }
    else
        XFREE(vpl011->backend.xen);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
