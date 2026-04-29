/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Public interface for the handler from Partition Manager messages.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_PTM_MSG_H
#define DRIVERS__GPU_MALI_G78AE_PTM_MSG_H

#include <xen/delay.h>
#include <xen/errno.h>
#include <xen/kernel.h>
#include <xen/random.h>
#include <xen/sched.h>
#include <xen/spinlock.h>
#include <xen/tasklet.h>
#include <xen/types.h>
#include <xen/xvmalloc.h>
#include <asm/io.h>
#include "common.h"

#define PTM_MESSAGE_SIZE 0x0020
#define PTM_INCOMING_MESSAGE0 0x0000
#define PTM_INCOMING_MESSAGE1 0x0004
#define PTM_OUTGOING_MESSAGE_STATUS 0x0008
#define PTM_OUTGOING_MESSAGE0 0x000c
#define PTM_OUTGOING_MESSAGE1 0x0010
#define PTM_OUTGOING_MSG_STATUS_MASK 0x01

#define ARB_TO_VM_BUFF_SIZE 3
#define VM_TO_ARB_BUFF_SIZE 3
#define PTM_SEND_RETRY_LIMIT 1000
#define PTM_MESSAGE_OFFSET(aw) ((aw)*PTM_MESSAGE_SIZE)
#define RETRY_MASK_RESET 0xFFFFFFFF

/*
 * Fifo Buffer to store PTM messages
 * This is a simple implementation of a circular buffer where overwriting is
 * possible and the first element will be discarded if a new element needs to be
 * added while buffer is full.
 */
struct msg_buff {
    spinlock_t buff_lock;
    uint64_t *msg;
    int head;
    int tail;
    int size;
    int retry_count;
};

struct ptm_msgs {
    struct msg_buff *msgs;
    unsigned int n_buffers;
    /* Bitmask to indicate which buffers have messages available to be read */
    unsigned long mask;
    /*
     * Bitmask to indicate which buffers have yet not reached the maximum
     * number of sending retries.
     */
    uint32_t can_retry_mask;
    /* The last member of msgs which was processed */
    uint32_t last_aw_processed;
};

struct ptm_msg_handler {
    struct ptm_msgs send_msgs;
    struct ptm_msgs recv_msgs;
    void __iomem *base_addr;
    struct tasklet ptm_send_wq;
    spinlock_t lock;
};

struct msg_worker_params {
    void *data;
    unsigned int aw;
};

static inline int ptm_msg_buff_read(struct ptm_msgs *msgs, uint32_t buff_id,
                                    uint64_t *msg);
static inline bool ptm_msg_buff_retry(struct ptm_msgs *msgs, uint32_t buff_id);
static inline void ptm_msg_write(struct ptm_msg_handler *msg_handler,
                                 uint32_t msg_id, uint64_t *message);
static inline void ptm_msg_process_msgs(struct ptm_msgs *msgs, void *data,
                                    void (*worker)(struct msg_worker_params *));

static inline unsigned int random_domain_cpu_not_current(void)
{
    struct domain *d = current->domain;
    unsigned int max_vcpus = d->max_vcpus;
    unsigned int pick;
    unsigned int i;

    if ( max_vcpus <= 1 )
        return current->processor;

    pick = get_random() % max_vcpus;
    for ( i = 0; i < max_vcpus; i++ )
    {
        unsigned int idx = (pick + i) % max_vcpus;

        if ( d->vcpu[idx] && d->vcpu[idx] != current )
            return d->vcpu[idx]->processor;
    }

    return current->processor;
}

/**
 * tasklet_schedule_on_rnd() - Schedule a tasklet on a random pCPU based on the
 * current domain's vCPUs
 * @tasklet:   Pointer to the tasklet to schedule
 *
 * Schedule the specified tasklet on a random pCPU which is not the current one
 * and select from the vCPUs of the current domain.
 *
 * This function is needed whenever a communication needs to be retried as the
 * current pCPU at EL1 (Kbase driver) might be waiting for a message from the
 * other end (partition manager or GPU) and scheduling the tasklet on the
 * same pCPU would result in a deadlock.
 */
#define tasklet_schedule_on_rnd(tasklet) \
    tasklet_schedule_on_cpu((tasklet), random_domain_cpu_not_current())

/**
 * send_msg_worker() - Send worker function for use with send_msg_worker
 *
 * @params:    Pointer to a msg_worker_params struct containing the data
 *             needed to process a message
 *
 * If the PTM_OUTGOING_MESSAGE_STATUS register is clear, write the message to
 * the send registers
 */
static inline void send_msg_worker(struct msg_worker_params *params)
{
    uint64_t message = 0;
    uint32_t message_status;
    struct ptm_msg_handler *msg_handler;
    unsigned int aw;
    int error = 0;

    if ( WARN_ON(!params) || WARN_ON(!params->data) )
        return;

    aw = params->aw;
    if ( WARN_ON(aw >= MAX_AW_NUM) )
        return;

    msg_handler = (struct ptm_msg_handler *)params->data;
    spin_lock(&msg_handler->lock);

    if ( ptm_msg_buff_retry(&msg_handler->send_msgs, aw) )
        /*
         * If the maximum number of retries is exceeded we need to clear
         * the can_retry_mask so that this buffer is not tried anymore
         * after the current retry cycle.
         */
        clear_bit(aw, &msg_handler->send_msgs.can_retry_mask);

    /* Check if the message to send is actually still in the send buffer */
    if ( !test_bit(aw, &msg_handler->send_msgs.mask) )
        goto cleanup_spinlock;

    message_status = readl(msg_handler->base_addr +
                        PTM_MESSAGE_OFFSET(aw) +
                        PTM_OUTGOING_MESSAGE_STATUS) &
                        PTM_OUTGOING_MSG_STATUS_MASK;

    if ( message_status == 0 )
    {
        error = ptm_msg_buff_read(&msg_handler->send_msgs, aw, &message);
        if (!error)
            ptm_msg_write(msg_handler, aw, &message);
        else
            printk(XENLOG_ERR "PTM msg: AW%u end buffer read failed\n", aw);
    }
    else
    {
        if ( msg_handler->send_msgs.msgs[aw].retry_count == 1 ||
             msg_handler->send_msgs.msgs[aw].retry_count == PTM_SEND_RETRY_LIMIT )
            printk(XENLOG_DEBUG
                   "PTM msg: AW%u end buffer busy (retry %d/%d)\n",
                   aw, msg_handler->send_msgs.msgs[aw].retry_count,
                   PTM_SEND_RETRY_LIMIT);
    }

cleanup_spinlock:
    spin_unlock(&msg_handler->lock);
}

/**
 * ptm_send_message_worker() - Worker thread for sending messages to
 *                              a PTM_MESSAGE pipe
 *
 * @data:      Work contained within the device data.
 *
 * The PTM send register was busy so this work item was scheduled.
 * Check the AW send_msgs and send any valid messages
 */
static inline void ptm_send_message_worker(void *data)
{
    struct ptm_msg_handler *msg_handler = data;

    if ( !msg_handler )
    {
        printk(XENLOG_ERR "PTM msg: Message handler is null\n");
        return;
    }

    if ( msg_handler->send_msgs.can_retry_mask & msg_handler->send_msgs.mask )
        ptm_msg_process_msgs(&msg_handler->send_msgs, msg_handler,
                             send_msg_worker);

    /* Still messages to send, so schedule the tasklet again */
    if ( msg_handler->send_msgs.can_retry_mask & msg_handler->send_msgs.mask )
        tasklet_schedule_on_rnd(&msg_handler->ptm_send_wq);
    else
        /*
         * Resetting the can_retry_mask so that next time this worker runs it at
         * least try one more time the buffers that already reached the maximum
         * number of retries.
         */
        msg_handler->send_msgs.can_retry_mask = RETRY_MASK_RESET;
}

/**
 * ptm_msg_handler_destroy() - Destroy the contents of a ptm_message_handler
 *                              structure
 *
 * @msg_handler:    Pointer to the message handler
 *
 * Destroy any dynamically generated members of the msg_handler structure and
 * destroy the send work queue.
 */
static inline void ptm_msg_handler_destroy(struct ptm_msg_handler *msg_handler)
{
    unsigned int n_buffers;
    unsigned int i;

    if ( !msg_handler )
    {
        printk(XENLOG_ERR "PTM msg: Message handler is null\n");
        return;
    }

    n_buffers = msg_handler->recv_msgs.n_buffers;
    tasklet_kill(&msg_handler->ptm_send_wq);
    for ( i = 0; i < n_buffers; i++ )
    {
        if ( msg_handler->recv_msgs.msgs )
            xvfree(msg_handler->recv_msgs.msgs[i].msg);
        if ( msg_handler->send_msgs.msgs )
            xvfree(msg_handler->send_msgs.msgs[i].msg);
    }
    xvfree(msg_handler->send_msgs.msgs);
    xvfree(msg_handler->recv_msgs.msgs);
}

/**
 * ptm_msg_handler_init() - Initialise a ptm_message_handler structure
 *
 * @msg_handler:    Pointer to the message handler
 * @base_addr:      PTM_MESSAGE base address
 * @n_buffers:      Number of send/receive buffers
 * @send_buff_size: Size of the buffers used for sending messages
 * @rcv_buff_size:  Size of the buffers used for receiving messages
 *
 * Populate the msg_handler structure, allocating appropriately sized buffers.
 * ptm_msg_handler_destroy() needs to be called in order to free resources
 * allocated in this function.
 *
 * Return: 0 if successfully or an error code
 */
static inline int ptm_msg_handler_init(struct ptm_msg_handler *msg_handler,
    void __iomem *base_addr, unsigned int n_buffers, int send_buff_size,
    int rcv_buff_size)
{
    unsigned int i = 0;

    *msg_handler = (struct ptm_msg_handler) {
        .send_msgs = {
            .msgs = xvzalloc_array(struct msg_buff, n_buffers),
            .n_buffers = n_buffers,
            .mask = 0,
            .can_retry_mask = RETRY_MASK_RESET,
            .last_aw_processed = 0
        },
        .recv_msgs = {
            .msgs = xvzalloc_array(struct msg_buff, n_buffers),
            .n_buffers = n_buffers,
            .mask = 0,
            .can_retry_mask = RETRY_MASK_RESET,
            .last_aw_processed = 0,
        },
        .base_addr = base_addr
    };

    /*
     * Initialize the tasklet and lock early so that
     * ptm_msg_handler_destroy() is safe to call from any error
     * path below (tasklet_kill on an already-initialized but
     * never-scheduled tasklet is harmless).
     */
    softirq_tasklet_init(&msg_handler->ptm_send_wq, ptm_send_message_worker,
                         msg_handler);
    spin_lock_init(&msg_handler->lock);

    if ( !msg_handler->send_msgs.msgs ||
         !msg_handler->recv_msgs.msgs )
    {
        printk(XENLOG_ERR "PTM msg: Failed to allocate message buffers\n");
        ptm_msg_handler_destroy(msg_handler);
        return -ENOMEM;
    }

    for ( i = 0; i < n_buffers; i++ )
    {
        struct msg_buff *msg_buff = &(msg_handler->recv_msgs.msgs[i]);
        spin_lock_init(&(msg_buff->buff_lock));
        msg_buff->msg = xvzalloc_array(uint64_t, rcv_buff_size + 1);
        if ( !msg_buff->msg )
        {
            printk(XENLOG_ERR
                   "PTM msg: Failed to allocate recv buffer %d\n", i);
            ptm_msg_handler_destroy(msg_handler);
            return -ENOMEM;
        }
        msg_buff->size = rcv_buff_size + 1;

        msg_buff = &(msg_handler->send_msgs.msgs[i]);
        spin_lock_init(&(msg_buff->buff_lock));
        msg_buff->msg = xvzalloc_array(uint64_t, send_buff_size + 1);
        if ( !msg_buff->msg )
        {
            printk(XENLOG_ERR
                   "PTM msg: Failed to allocate send buffer %d\n", i);
            ptm_msg_handler_destroy(msg_handler);
            return -ENOMEM;
        }
        msg_buff->size = send_buff_size + 1;
    }

    return 0;
}

/**
 * ptm_msg_buff_read() - Read and clear the specified message buffer
 *
 * @msgs:       Pointer to a ptm_msgs struct containing send or receive buffers
 * @buff_id:    Index from which to read the message
 * @msg:        Destination pointer for the retrieved message
 *
 * Read the buffer of the message specified by the index and clear the
 * corresponding bit in the mask to indicate that the buffer is empty.
 *
 * Return: 0 if successful or an error code
 */
static inline int ptm_msg_buff_read(struct ptm_msgs *msgs, uint32_t buff_id,
                                    uint64_t *msg)
{
    unsigned long flags;
    struct msg_buff *buffer;

    if ( WARN_ON(!msgs) || WARN_ON(!msg) ||
         buff_id >= (uint32_t)msgs->n_buffers )
        return -EINVAL;

    buffer = &msgs->msgs[buff_id];

    spin_lock_irqsave(&buffer->buff_lock, flags);
    /* If empty just return. */
    if ( buffer->tail == buffer->head )
    {
        spin_unlock_irqrestore(&buffer->buff_lock, flags);
        return -ENODATA;
    }
    *msg = buffer->msg[buffer->tail];
    buffer->msg[buffer->tail] = 0;
    buffer->tail = (buffer->tail + 1) % buffer->size;
    if ( buffer->tail == buffer->head )
        clear_bit(buff_id, &msgs->mask);
    /*
     * If we are taking a message from the buffer it means that it should be
     * ready to send, so the retry can be reset.
     */
    buffer->retry_count = 0;
    spin_unlock_irqrestore(&buffer->buff_lock, flags);

    return 0;
}

/**
 * ptm_msg_buff_flush() - Discard all pending messages for a buffer
 *
 * @msgs:       Pointer to a ptm_msgs struct
 * @buff_id:    Index of the buffer to flush
 *
 * Resets the ring buffer head/tail and clears the mask bit.
 * Used before force-sending GPU_LOST when the channel is stuck.
 */
static inline void ptm_msg_buff_flush(struct ptm_msgs *msgs, uint32_t buff_id)
{
    unsigned long flags;
    struct msg_buff *buffer;

    if ( WARN_ON(!msgs) || buff_id >= (uint32_t)msgs->n_buffers )
        return;

    buffer = &msgs->msgs[buff_id];

    spin_lock_irqsave(&buffer->buff_lock, flags);
    buffer->head = 0;
    buffer->tail = 0;
    buffer->retry_count = 0;
    clear_bit(buff_id, &msgs->mask);
    spin_unlock_irqrestore(&buffer->buff_lock, flags);
}

/**
 * ptm_msg_buff_write() - Write the message to the specified message buffer
 *
 * @msgs:       Pointer to a ptm_msgs struct containing send or receive buffers
 * @buff_id:    Index from which to write the message
 * @payload:    The message to be stored
 *
 * Store the the message in the buffer of the message specified by the index and
 * set the corresponding bit in the message bitmask to indicate that the message
 * is valid
 *
 * Return:
 * * 1    - if write was successful but overwrite happened
 * * 0    - if write was successful and no overwrite happened
 * * < 0  - if error
 */
static inline int ptm_msg_buff_write(struct ptm_msgs *msgs, uint32_t buff_id,
                                     uint64_t payload)
{
    unsigned long flags;
    int overwrite = 0;
    struct msg_buff *buffer;

    if ( WARN_ON(!msgs) || buff_id >= (uint32_t)msgs->n_buffers )
        return -EINVAL;

    buffer = &msgs->msgs[buff_id];

    spin_lock_irqsave(&buffer->buff_lock, flags);
    /* Overwrite the first message if the buffer is full */
    if ( ((buffer->head + 1) % buffer->size) == buffer->tail )
    {
        buffer->tail = (buffer->tail + 1) % buffer->size;
        overwrite = 1;
    }
    buffer->msg[buffer->head] = payload;
    buffer->head = (buffer->head + 1) % buffer->size;
    set_bit(buff_id, &msgs->mask);
    spin_unlock_irqrestore(&buffer->buff_lock, flags);

    return overwrite;
}

/**
 * ptm_msg_buff_retry() - Increment and check the buffer retry counter
 *
 * @msgs:       Pointer to a ptm_msgs struct containing send or receive buffers
 * @buff_id:    Index from which to increment and check the retry counter
 *
 * Return: TRUE if reached the maximum number of retries or FALSE
 */
static inline bool ptm_msg_buff_retry(struct ptm_msgs *msgs, uint32_t buff_id)
{
    unsigned long flags;

    if ( WARN_ON(!msgs) || buff_id >= (uint32_t)msgs->n_buffers )
        return false;

    if ( msgs->msgs[buff_id].retry_count >= PTM_SEND_RETRY_LIMIT )
        return true;

    spin_lock_irqsave(&msgs->msgs[buff_id].buff_lock, flags);
    msgs->msgs[buff_id].retry_count++;
    spin_unlock_irqrestore(&msgs->msgs[buff_id].buff_lock, flags);

    return false;
}

/**
 * ptm_msg_write() - Write a PTM_MESSAGE
 *
 * @msg_handler:    Pointer to the message handler
 * @msg_id:         Message index to target. It is equal to the AW_ID if
 *                  targeting AW and zero if targeting RG.
 * @message:        64-bit message to send
 *
 * Send a message to the specified register related to the msg_id index. This is
 * simply a raw write and assumes that the status register has already been
 * checked.
 */
static inline void ptm_msg_write(struct ptm_msg_handler *msg_handler,
                                 uint32_t msg_id, uint64_t *message)
{
    uint32_t message_lo = *message & UINT32_MAX;
    uint32_t message_hi = *message >> 32;

    writel(message_lo,
           msg_handler->base_addr + PTM_MESSAGE_OFFSET(msg_id) +
           PTM_OUTGOING_MESSAGE0);

    /*
     * The PTM_OUTGOING_MESSAGE1 write must be last because it triggers
     * the message copy and raises an interrupt on the recipient.
     */
    writel(message_hi,
           msg_handler->base_addr + PTM_MESSAGE_OFFSET(msg_id) +
           PTM_OUTGOING_MESSAGE1);
}

/**
 * ptm_msg_read() - Reads the last message received
 *
 * @msg_handler:    Pointer to the message handler
 * @msg_id:         Message index to target. It is equal to the AW_ID if
 *                  targeting AW and zero if targeting RG.
 * @message:        Destination pointer for the 64-bit message
 *
 * Receive a message from the specified register related to the msg_id index.
 * This is simply a raw read and assumes that the caller should clear any IRQ
 * related to this message if necessary.
 */
static inline void ptm_msg_read(struct ptm_msg_handler *msg_handler,
                                uint32_t msg_id, uint64_t *message)
{
    if ( !msg_handler || !message )
        return;

    *message = readl(msg_handler->base_addr + PTM_MESSAGE_OFFSET(msg_id) +
                PTM_INCOMING_MESSAGE1);
    *message <<= 32;
    *message |= readl(msg_handler->base_addr + PTM_MESSAGE_OFFSET(msg_id) +
                 PTM_INCOMING_MESSAGE0);
}

/**
 * ptm_msg_flush_send_buffers() - Schedule a worker to flush the send buffers.
 * @msg_handler:    Pointer to the message handler
 *
 * Schedule a message send worker to flush the message sending buffers.
 */
static inline void ptm_msg_flush_send_buffers(struct ptm_msg_handler *msg_handler)
{
    if (!msg_handler)
        return;

    tasklet_schedule_on_rnd(&msg_handler->ptm_send_wq);
}

/**
 * ptm_msg_send() - Send message to the other end (AW or RG).
 *
 * @msg_handler:    Pointer to the message handler
 * @buff_id:        Message index to target. It is equal to the AW_ID if
 *                  targeting AW and zero if targeting RG.
 *
 * Send a message payload to  the specified register related to the buff_id
 * index It could be AW or RG depending on the msg_handler passed.
 *
 * Return: 0 if successful, otherwise a negative error code.
 */
static inline int ptm_msg_send(struct ptm_msg_handler *msg_handler,
                               uint32_t buff_id)
{
    uint32_t msg_status;
    uint64_t payload;
    int error = 0;

    if ( !msg_handler )
        return -EINVAL;

    spin_lock(&msg_handler->lock);

    /* Check if the message to send is actually still in the send buffer */
    if ( !test_bit(buff_id, &msg_handler->send_msgs.mask))
    {
        printk(XENLOG_ERR "Send buffer %d empty\n", buff_id);
        goto cleanup_spinlock;
    }

    msg_status = readl(msg_handler->base_addr + PTM_MESSAGE_OFFSET(buff_id) +
                  PTM_OUTGOING_MESSAGE_STATUS) &
             PTM_OUTGOING_MSG_STATUS_MASK;

    if ( msg_status == 0 )
    {
        error = ptm_msg_buff_read(&msg_handler->send_msgs, buff_id, &payload);
        if ( !error )
            ptm_msg_write(msg_handler, buff_id, &payload);
        else
            printk(XENLOG_ERR "Buffer%d send buffer read failed\n", buff_id);

        if ( test_bit(buff_id, &msg_handler->send_msgs.mask) )
            tasklet_schedule_on_rnd(&msg_handler->ptm_send_wq);
    }
    else
    {
        printk(XENLOG_DEBUG
               "PTM msg: AW%u status busy, scheduling retry tasklet\n",
               buff_id);
        tasklet_schedule_on_rnd(&msg_handler->ptm_send_wq);
    }

cleanup_spinlock:
    spin_unlock(&msg_handler->lock);
    return error;
}

/**
 * ptm_msg_send_force() - Force-send a message, bypassing status check
 *
 * @msg_handler:  Pointer to the message handler
 * @buff_id:      AW index to target
 * @message:      64-bit message payload to send
 *
 * Flush all stale messages from the software send buffer for this AW,
 * then write the message directly to the hardware PTM registers regardless
 * of the outgoing status.
 *
 * This is used for GPU_LOST delivery after a timeout: the old message
 * (typically GPU_STOP) is stale because the guest never consumed it.
 * Overwriting it with GPU_LOST and re-triggering the interrupt gives the
 * guest a chance to handle the notification.
 */
static inline void ptm_msg_send_force(struct ptm_msg_handler *msg_handler,
                                      uint32_t buff_id, uint64_t *message)
{
    if ( !msg_handler || !message )
        return;

    if ( buff_id >= (uint32_t)msg_handler->send_msgs.n_buffers )
        return;

    spin_lock(&msg_handler->lock);

    /* Discard any stale messages in the software queue */
    ptm_msg_buff_flush(&msg_handler->send_msgs, buff_id);

    /* Ensure the AW is retryable for future messages after flush */
    set_bit(buff_id, &msg_handler->send_msgs.can_retry_mask);

    /* Write directly, don't check status, the old message is stale */
    ptm_msg_write(msg_handler, buff_id, message);

    spin_unlock(&msg_handler->lock);
}

/**
 * ptm_msg_process_msgs() - Process valid messages in the specified buffer
 *
 * @msgs:   Pointer to a ptm_msgs struct containing send or receive buffers
 * @data:   An opaque pointer passed to the worker
 * @worker: A worker function which is called for every valid message
 *
 * For every message in the buffer which is flagged as being valid, the worker
 * function is called. The ptm_msgs structure records the last message
 * processed so that successive calls to this function result in fair
 * processing of messages. It is the worker function's responsibility
 * to retrieve the message with ptm_msg_buff_read() which will clear that
 * message's valid flag
 */
static inline void ptm_msg_process_msgs(struct ptm_msgs *msgs, void *data,
                    void (*worker)(struct msg_worker_params *))
{
    uint32_t first_aw_to_process = msgs->last_aw_processed;
    uint32_t mask = 1U << first_aw_to_process;

    struct msg_worker_params params =
                { .data = data, .aw = first_aw_to_process };

    do {
        if ( mask & msgs->mask & msgs->can_retry_mask )
        {
            (*worker)(&params);

            msgs->last_aw_processed = params.aw;
        }

        if ( ++params.aw >= MAX_AW_NUM )
        {
            mask = 1;
            params.aw = 0;
        } else
            mask <<= 1;
    } while ( (uint32_t)params.aw != first_aw_to_process );
}

#endif /* DRIVERS__GPU_MALI_G78AE_PTM_MSG_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */