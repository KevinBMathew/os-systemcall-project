#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/errno.h>

#define MAX_MSG_LEN  4096
#define QUEUE_DEPTH  16     /* messages retained per topic; must be a power of 2 */

/* ── per-message record ──────────────────────────────────────────────────── */

struct pubsub_msg {
    char         *data;   /* kmalloc'd payload                               */
    int           len;    /* payload length in bytes                         */
    unsigned long seq;    /* monotonic sequence number (1-based)             */
};

/* ── per-topic object ────────────────────────────────────────────────────── */

struct pubsub_topic {
    int               topic_id;
    wait_queue_head_t wq;           /* subscriber wait queue                 */
    spinlock_t        lock;         /* guards all fields below               */
    struct list_head  list;         /* node in the global topic_list         */

    /*
     * Circular message queue.
     *
     * Invariant: queue[head] is the oldest valid slot; the newest is at
     *   (head + count - 1) % QUEUE_DEPTH.
     * Sequence numbers run 1 .. latest_seq (latest_seq == 0 means nothing
     *   has been published yet).
     * The oldest seq still in the buffer is: latest_seq - count + 1.
     */
    struct pubsub_msg queue[QUEUE_DEPTH];
    unsigned int      head;         /* index of oldest slot                  */
    unsigned int      count;        /* live slots (0 .. QUEUE_DEPTH)         */
    unsigned long     latest_seq;   /* seq of most-recently published msg    */
};

/* ── global topic registry ───────────────────────────────────────────────── */

static LIST_HEAD(topic_list);
static DEFINE_SPINLOCK(topic_list_lock);   /* guards topic_list structure    */

/* Must be called with topic_list_lock held. */
static struct pubsub_topic *find_topic(int topic_id)
{
    struct pubsub_topic *t;
    list_for_each_entry(t, &topic_list, list)
        if (t->topic_id == topic_id)
            return t;
    return NULL;
}

/* ── sys_create_topic ────────────────────────────────────────────────────── */

SYSCALL_DEFINE1(create_topic, int, topic_id)
{
    struct pubsub_topic *t;
    unsigned long flags;

    if (topic_id < 0)
        return -EINVAL;

    /*
     * FIX — TOCTOU race:
     *
     * The original code dropped topic_list_lock between the duplicate check
     * and the list_add, so two concurrent callers could both pass the check
     * before either inserted.
     *
     * New pattern: allocate *before* acquiring the lock (kmalloc may sleep,
     * so it cannot be called with a spinlock held), then do the duplicate
     * check and the insertion atomically under a *single* lock acquisition.
     * If a duplicate is found we free the pre-allocated object and bail.
     */
    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
        return -ENOMEM;

    t->topic_id   = topic_id;
    t->head       = 0;
    t->count      = 0;
    t->latest_seq = 0;   /* 0 == "nothing published yet" sentinel            */
    init_waitqueue_head(&t->wq);
    spin_lock_init(&t->lock);
    INIT_LIST_HEAD(&t->list);

    spin_lock_irqsave(&topic_list_lock, flags);
    if (find_topic(topic_id)) {          /* check + insert in ONE section    */
        spin_unlock_irqrestore(&topic_list_lock, flags);
        kfree(t);
        return -EEXIST;
    }
    list_add(&t->list, &topic_list);
    spin_unlock_irqrestore(&topic_list_lock, flags);

    printk(KERN_INFO "pubsub: topic %d created\n", topic_id);
    return 0;
}

/* ── sys_subscribe ───────────────────────────────────────────────────────── */
/*
 * New signature — four arguments:
 *
 *   topic_id   topic to read from
 *   buffer     user-space destination buffer
 *   max_len    capacity of buffer in bytes
 *   seq_ptr    IN/OUT pointer to an unsigned long in user space:
 *                 on entry : seq of the last message already received
 *                            (pass 0 on the very first call)
 *                 on exit  : seq of the message just delivered
 *
 * Typical subscriber loop:
 *
 *   unsigned long seq = 0;
 *   char buf[4096];
 *   while (1) {
 *       ssize_t n = syscall(NR_subscribe, id, buf, sizeof(buf), &seq);
 *       if (n > 0) process(buf, n);
 *   }
 *
 * If the subscriber falls behind and messages have been overwritten, it is
 * fast-forwarded to the oldest message still in the buffer.  The caller can
 * detect a gap by checking whether the returned *seq_ptr == old_seq + 1.
 *
 * Returns: number of bytes delivered, or a negative error code.
 */
SYSCALL_DEFINE4(subscribe,
                int,                    topic_id,
                char __user *,          buffer,
                int,                    max_len,
                unsigned long __user *, seq_ptr)
{
    struct pubsub_topic *t;
    unsigned long        flags, last_seq, want_seq, oldest_seq, out_seq;
    unsigned int         idx;
    char                *tmp;
    int                  copy_len, ret;

    if (!buffer || max_len <= 0 || !seq_ptr)
        return -EINVAL;

    /* Read the caller's last-seen sequence number from user space. */
    if (get_user(last_seq, seq_ptr))
        return -EFAULT;

    spin_lock_irqsave(&topic_list_lock, flags);
    t = find_topic(topic_id);
    spin_unlock_irqrestore(&topic_list_lock, flags);
    if (!t)
        return -ENOENT;

    /*
     * FIX — spurious-wakeup / single-shot sleep:
     *
     * The original code called schedule() exactly once inside a plain if().
     * If the process was woken spuriously (which the scheduler is allowed to
     * do), it would proceed with msg_buf still NULL and return -EAGAIN,
     * silently dropping the subscription.
     *
     * wait_event_interruptible() wraps the entire prepare_to_wait /
     * check-condition / schedule / finish_wait sequence in a loop that
     * re-evaluates the condition after *every* wakeup, so spurious wakeups
     * are completely harmless.
     *
     * READ_ONCE() tells the compiler it must reload latest_seq from memory
     * on every loop iteration rather than caching it in a register.
     *
     * The condition "latest_seq > last_seq" is true as soon as at least one
     * new message has been published after what the caller already received.
     */
    ret = wait_event_interruptible(t->wq,
                                   READ_ONCE(t->latest_seq) > last_seq);
    if (ret)            /* woken by a signal (e.g. SIGINT / Ctrl-C)          */
        return -EINTR;

    /*
     * FIX — copy_to_user inside a spinlock:
     *
     * copy_to_user() may need to handle a page fault to bring a swapped-out
     * user page back into RAM.  Page-fault handling schedules, which is
     * illegal inside a spinlock.
     *
     * Solution: allocate a temporary kernel-side bounce buffer *before*
     * taking the lock.  Under the lock we do only a fast kernel→kernel
     * memcpy (no sleeping possible).  After releasing the lock we call
     * copy_to_user() safely.
     */
    tmp = kmalloc(max_len, GFP_KERNEL);
    if (!tmp)
        return -ENOMEM;

    spin_lock_irqsave(&t->lock, flags);

    /* Defensive: the queue could be empty if all messages were evicted. */
    if (t->count == 0) {
        spin_unlock_irqrestore(&t->lock, flags);
        kfree(tmp);
        return -EAGAIN;
    }

    /*
     * Decide which message to deliver.
     *
     * oldest_seq is the sequence number of queue[head] — the least-recently
     * published message still resident in the ring buffer.
     */
    oldest_seq = t->latest_seq - t->count + 1;
    want_seq   = last_seq + 1;

    if (want_seq < oldest_seq) {
        /*
         * The subscriber was too slow.  Messages want_seq .. oldest_seq-1
         * have already been overwritten.  Jump to the oldest surviving
         * message; the caller can detect the gap via the returned seq.
         */
        want_seq = oldest_seq;
    }

    /* Map sequence number → ring-buffer index. */
    idx      = (t->head + (unsigned int)(want_seq - oldest_seq)) % QUEUE_DEPTH;
    copy_len = min(t->queue[idx].len, max_len);
    memcpy(tmp, t->queue[idx].data, copy_len); /* fast kernel→kernel copy    */
    out_seq  = t->queue[idx].seq;

    spin_unlock_irqrestore(&t->lock, flags);

    /* Safe to sleep here — we no longer hold the spinlock. */
    if (copy_to_user(buffer, tmp, copy_len)) {
        kfree(tmp);
        return -EFAULT;
    }
    kfree(tmp);

    /* Write the delivered sequence number back to the caller. */
    if (put_user(out_seq, seq_ptr))
        return -EFAULT;

    printk(KERN_INFO "pubsub: topic %d seq %lu -> %d bytes\n",
           topic_id, out_seq, copy_len);
    return copy_len;
}

/* ── sys_publish ─────────────────────────────────────────────────────────── */
/*
 * Unchanged interface, but the backing store is now a ring buffer instead of
 * a single slot, so subscribers that call subscribe() in a tight loop will
 * see every message in order rather than only the latest one.
 *
 * When the ring buffer is full the oldest message is evicted (its data
 * pointer is saved and freed *after* releasing the lock so we don't hold
 * the spinlock during slab work).
 */
SYSCALL_DEFINE3(publish,
                int,           topic_id,
                char __user *, message,
                int,           msg_len)
{
    struct pubsub_topic *t;
    unsigned long        flags;
    char                *kbuf, *evicted = NULL;
    unsigned int         slot;

    if (!message || msg_len <= 0 || msg_len > MAX_MSG_LEN)
        return -EINVAL;

    spin_lock_irqsave(&topic_list_lock, flags);
    t = find_topic(topic_id);
    spin_unlock_irqrestore(&topic_list_lock, flags);
    if (!t)
        return -ENOENT;

    kbuf = kmalloc(msg_len, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, message, msg_len)) {
        kfree(kbuf);
        return -EFAULT;
    }

    spin_lock_irqsave(&t->lock, flags);

    if (t->count == QUEUE_DEPTH) {
        /*
         * Ring buffer is full.  Evict the oldest slot so a slow subscriber
         * loses the oldest message rather than blocking the publisher.
         * Save the pointer to free it outside the lock.
         */
        evicted              = t->queue[t->head].data;
        t->queue[t->head].data = NULL;
        t->head              = (t->head + 1) % QUEUE_DEPTH;
        t->count--;
    }

    /* Assign the next slot and stamp it with the next sequence number. */
    slot                   = (t->head + t->count) % QUEUE_DEPTH;
    t->latest_seq++;
    t->queue[slot].data    = kbuf;
    t->queue[slot].len     = msg_len;
    t->queue[slot].seq     = t->latest_seq;
    t->count++;

    spin_unlock_irqrestore(&t->lock, flags);

    kfree(evicted);         /* NULL-safe; frees evicted message outside lock */
    wake_up_all(&t->wq);

    printk(KERN_INFO "pubsub: topic %d published seq %lu (%d bytes)\n",
           topic_id, t->latest_seq, msg_len);
    return 0;
}