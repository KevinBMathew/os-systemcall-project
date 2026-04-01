#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/atomic.h>

#define MAX_MSG_LEN  4096
#define RING_SIZE    16    //number of messages kept per topic (must be a power of 2)

struct pubsub_topic {
    int                topic_id;
    atomic_t           refcount;              //reference count; struct freed when it reaches zero
    bool               deleted;              //set true by delete_topic so sleeping subscribers bail out
    wait_queue_head_t  wq;

    //circular ring buffer: slot N (0-based) holds the message with pub_seq == N+1
    char              *ring_data[RING_SIZE];  //per-slot data pointer (NULL if slot unused)
    int                ring_len[RING_SIZE];   //per-slot message length
    unsigned long      pub_seq;              //monotonic publish counter; 0 = no messages published yet

    spinlock_t         lock;      //protects ring_data, ring_len, pub_seq, and deleted
    struct list_head   list;      //linked list node in the global table
};

//global registry declaration
static LIST_HEAD(topic_list);
static DEFINE_SPINLOCK(topic_list_lock);  //protects the topic_list linked list

// find_topic : raw lookup, MUST be called with topic_list_lock held.
// Does NOT bump refcount so the pointer is only safe while the lock is held.
static struct pubsub_topic *find_topic(int topic_id)
{
    struct pubsub_topic *t;

    list_for_each_entry(t, &topic_list, list) {
        if (t->topic_id == topic_id)
            return t;
    }
    return NULL;
}

// find_topic_get : same as find_topic but atomically increments refcount.
// MUST be called with topic_list_lock held.
// Returned pointer stays valid until topic_put() is called, even after the
// caller releases topic_list_lock.
static struct pubsub_topic *find_topic_get(int topic_id)
{
    struct pubsub_topic *t = find_topic(topic_id);

    if (t)
        atomic_inc(&t->refcount);
    return t;
}

// topic_put : drops one reference to t.
// When the last reference is released, frees all ring buffer slots and
// the topic struct itself.
static void topic_put(struct pubsub_topic *t)
{
    int i;

    if (atomic_dec_and_test(&t->refcount)) {
        for (i = 0; i < RING_SIZE; i++)
            kfree(t->ring_data[i]);  //kfree(NULL) is safe
        kfree(t);
    }
}

// sys_create_topic : creates a topic with the user-supplied topic_id.
// Returns 0 on success, negative error code on failure.
SYSCALL_DEFINE1(create_topic, int, topic_id)
{
    struct pubsub_topic *t;
    unsigned long flags;
    int i;

    if (topic_id < 0)
        return -EINVAL; //invalid argument returns -22

    // allocate and initialise BEFORE taking the list lock because GFP_KERNEL
    // can sleep; spinlocks must never be held across a sleeping allocation.
    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
        return -ENOMEM; //if malloc fails, returning Out of Memory code: -12

    t->topic_id = topic_id;
    atomic_set(&t->refcount, 1);  //the global list holds one reference
    t->deleted  = false;
    t->pub_seq  = 0;
    for (i = 0; i < RING_SIZE; i++) {
        t->ring_data[i] = NULL;
        t->ring_len[i]  = 0;
    }
    init_waitqueue_head(&t->wq);
    spin_lock_init(&t->lock);
    INIT_LIST_HEAD(&t->list);

    // lock, re-check for duplicate, then insert — all in one critical section.
    // this closes the TOCTOU window the original code had between the unlock
    // after the duplicate check and the re-lock before the list_add.
    spin_lock_irqsave(&topic_list_lock, flags);
    if (find_topic(topic_id)) {
        spin_unlock_irqrestore(&topic_list_lock, flags);
        kfree(t);
        return -EEXIST; //using the file-exists code since the topic already exists: -17
    }
    list_add(&t->list, &topic_list);
    spin_unlock_irqrestore(&topic_list_lock, flags);

    printk(KERN_INFO "pubsub: topic %d created\n", topic_id);
    return 0;
}

// sys_delete_topic : removes a topic from the global registry.
// Any subscribers blocked in sys_subscribe will be woken and will return -ENOENT.
// It is safe to call while subscribers/publishers are active; the struct is freed
// only once every in-flight syscall drops its reference via topic_put().
// Returns 0 on success, negative error code on failure.
SYSCALL_DEFINE1(delete_topic, int, topic_id)
{
    struct pubsub_topic *t;
    unsigned long flags;

    if (topic_id < 0)
        return -EINVAL; //invalid argument returns -22

    // detach from the global list so no new lookups can find it,
    // but keep the struct alive via the existing list reference until we drop it.
    spin_lock_irqsave(&topic_list_lock, flags);
    t = find_topic(topic_id);  //raw lookup is safe here because we hold the list lock
    if (!t) {
        spin_unlock_irqrestore(&topic_list_lock, flags);
        return -ENOENT; //no such entry error code
    }
    list_del(&t->list);  //remove from global list; holders of existing refs are unaffected
    spin_unlock_irqrestore(&topic_list_lock, flags);

    //mark as deleted so any subscriber that wakes up knows to bail out
    spin_lock_irqsave(&t->lock, flags);
    t->deleted = true;
    spin_unlock_irqrestore(&t->lock, flags);

    //wake any sleeping subscribers so they can observe t->deleted and return -ENOENT
    wake_up_all(&t->wq);

    topic_put(t);  //drop the list's reference; frees struct if no one else holds a ref

    printk(KERN_INFO "pubsub: topic %d deleted\n", topic_id);
    return 0;
}

// sys_subscribe : blocks until a message newer than *p_seq arrives on topic_id and copies it into the user buffer.
//returns number of bytes copied on success, negative error code on failure.
//p_seq is a user-space pointer to the subscriber's sequence cursor (in/out). we can initialise *p_seq = 0 before the first call; pass the returned value on every subsequent call.  Each sequence number identifies one published message uniquely.
SYSCALL_DEFINE4(subscribe, int,                   topic_id,
                            char __user *,         buffer,
                            int,                   max_len,
                            unsigned long __user *, p_seq)
{
    struct pubsub_topic *t;
    unsigned long flags, user_seq, read_seq;
    DEFINE_WAIT(wait);
    char *kbuf;
    int ring_idx, copy_len;

    if (!buffer || max_len <= 0 || !p_seq)    //sanity check: null pointer or impossible length
        return -EINVAL; //invalid argument

    if (get_user(user_seq, p_seq))  //read the subscriber's last-seen sequence number
        return -EFAULT;

    //look up topic and grab a reference so it can't be freed under us
    spin_lock_irqsave(&topic_list_lock, flags);
    t = find_topic_get(topic_id);
    spin_unlock_irqrestore(&topic_list_lock, flags);

    if (!t)
        return -ENOENT; //no such entry error code

    // pre-allocate a kernel staging buffer so the actual copy_to_user can happen
    // outside the spinlock.  copy_to_user can sleep on a user-space page fault,
    // which is illegal while a spinlock is held.
    kbuf = kmalloc(MAX_MSG_LEN, GFP_KERNEL);
    if (!kbuf) {
        topic_put(t);
        return -ENOMEM;
    }

    //wait loop thathandles spurious wakeups and signals
    // the loop condition is checked under the lock to avoid missing a wakeup that arrives after the check but before schedule()
    for (;;) {
        prepare_to_wait(&t->wq, &wait, TASK_INTERRUPTIBLE);

        spin_lock_irqsave(&t->lock, flags);

        if (t->deleted) {  //topic was deleted while we were sleeping
            spin_unlock_irqrestore(&t->lock, flags);
            finish_wait(&t->wq, &wait);
            kfree(kbuf);
            topic_put(t);
            return -ENOENT;
        }

        if (t->pub_seq > user_seq)  //new message available; exit loop still holding the lock
            break;

        spin_unlock_irqrestore(&t->lock, flags);

        if (signal_pending(current)) {  //interrupted by a signal before any message arrived
            finish_wait(&t->wq, &wait);
            kfree(kbuf);
            topic_put(t);
            return -EINTR; //returns interrupt error code
        }

        schedule();  //lets the CPU do other work; publisher will wake us via wake_up_all()
    }

    //arrive here holding t->lock with at least one unread message guaranteed
    finish_wait(&t->wq, &wait);

    // if the subscriber fell behind by more than RING_SIZE messages, the oldest
    // slot has already been overwritten; skip ahead to the oldest message still
    // in the ring rather than reading garbage.
    if (t->pub_seq - user_seq > RING_SIZE)
        read_seq = t->pub_seq - RING_SIZE + 1;  //oldest message still in ring
    else
        read_seq = user_seq + 1;                //next sequential message

    ring_idx  = (int)((read_seq - 1) % RING_SIZE);  //ring index: seq 1->slot 0, seq 2->slot 1, ...
    copy_len  = min(t->ring_len[ring_idx], max_len);
    memcpy(kbuf, t->ring_data[ring_idx], copy_len);  //kernel-to-kernel copy; safe under spinlock

    spin_unlock_irqrestore(&t->lock, flags);

    //copy staged data to user space now that the spinlock is released
    if (copy_to_user(buffer, kbuf, copy_len)) {
        kfree(kbuf);
        topic_put(t);
        return -EFAULT;
    }

    //write the new sequence number back so the caller knows where to resume
    if (put_user(read_seq, p_seq)) {
        kfree(kbuf);
        topic_put(t);
        return -EFAULT;
    }

    kfree(kbuf);
    topic_put(t);

    printk(KERN_INFO "pubsub: topic %d seq %lu delivered %d bytes\n",
           topic_id, read_seq, copy_len);
    return copy_len;
}

// sys_publish : copies a message from user space into the next ring slot and
// wakes all subscribers simultaneously.
// Returns 0 on success, negative error code on failure.
SYSCALL_DEFINE3(publish, int,          topic_id,
                         char __user *, message,
                         int,           msg_len)
{
    struct pubsub_topic *t;
    unsigned long flags, new_seq;
    char *kbuf;
    int ring_idx;

    if (!message || msg_len <= 0 || msg_len > MAX_MSG_LEN)
        return -EINVAL;

    //look up topic and grab a reference so it can't be freed under us
    spin_lock_irqsave(&topic_list_lock, flags);
    t = find_topic_get(topic_id);
    spin_unlock_irqrestore(&topic_list_lock, flags);

    if (!t)
        return -ENOENT;

    // copy the message from user space into a kernel buffer BEFORE taking the
    // topic lock; copy_from_user can sleep on a page fault.
    kbuf = kmalloc(msg_len, GFP_KERNEL);
    if (!kbuf) {
        topic_put(t);
        return -ENOMEM;
    }

    if (copy_from_user(kbuf, message, msg_len)) {
        kfree(kbuf);
        topic_put(t);
        return -EFAULT;
    }

    //write into the next ring slot, overwriting the oldest message if the ring is full
    spin_lock_irqsave(&t->lock, flags);
    ring_idx = (int)(t->pub_seq % RING_SIZE);  //slot to write into (wraps around)
    kfree(t->ring_data[ring_idx]);             //free the message being evicted from this slot
    t->ring_data[ring_idx] = kbuf;
    t->ring_len[ring_idx]  = msg_len;
    t->pub_seq++;                              //advance monotonic counter; makes message visible
    new_seq = t->pub_seq;
    spin_unlock_irqrestore(&t->lock, flags);

    //wake up all the processes (subscribers) in the wait queue
    wake_up_all(&t->wq);

    topic_put(t);

    printk(KERN_INFO "pubsub: topic %d seq %lu published %d bytes\n",
           topic_id, new_seq, msg_len);
    return 0;
}
