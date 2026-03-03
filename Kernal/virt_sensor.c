// kernel/virt_sensor.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/ioctl.h>

#define DRIVER_NAME "virt_sensor"
#define CLASS_NAME  "virt_sensor"

#define VS_IOC_MAGIC 'v'
#define VS_IOC_SET_INTERVAL_MS _IOW(VS_IOC_MAGIC, 1, int)

static dev_t devno;
static struct cdev vs_cdev;
static struct class *vs_class;
static struct device *vs_device;

static DEFINE_MUTEX(vs_lock);

static int temp_milli_c = 42000;     // 42.000 C
static int interval_ms  = 200;       // default sample interval
static struct timer_list vs_timer;

static wait_queue_head_t vs_wq;
static atomic_t vs_data_ready = ATOMIC_INIT(0);

static void vs_reschedule_timer_locked(void)
{
    mod_timer(&vs_timer, jiffies + msecs_to_jiffies(interval_ms));
}

// Simple pseudo-random-ish drift
static void vs_timer_fn(struct timer_list *t)
{
    static int dir = 1;

    mutex_lock(&vs_lock);
    temp_milli_c += dir * 37; // drift by 0.037C per tick
    if (temp_milli_c > 65000) dir = -1;
    if (temp_milli_c < 25000) dir = 1;

    atomic_set(&vs_data_ready, 1);
    mutex_unlock(&vs_lock);

    wake_up_interruptible(&vs_wq);

    mutex_lock(&vs_lock);
    vs_reschedule_timer_locked();
    mutex_unlock(&vs_lock);
}

static ssize_t vs_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
    char kbuf[64];
    int n;

    // return a fresh "line" each time (ignore offset semantics)
    *off = 0;

    mutex_lock(&vs_lock);
    n = scnprintf(kbuf, sizeof(kbuf),
                  "{\"temp_milli_c\":%d,\"interval_ms\":%d}\n",
                  temp_milli_c, interval_ms);
    mutex_unlock(&vs_lock);

    if (len < n) return -EINVAL;
    if (copy_to_user(buf, kbuf, n)) return -EFAULT;

    // consumed latest sample
    atomic_set(&vs_data_ready, 0);

    return n;
}

static __poll_t vs_poll(struct file *f, poll_table *wait)
{
    __poll_t mask = 0;

    poll_wait(f, &vs_wq, wait);

    if (atomic_read(&vs_data_ready))
        mask |= POLLIN | POLLRDNORM;

    return mask;
}

static long vs_unlocked_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    int new_ms;

    if (_IOC_TYPE(cmd) != VS_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case VS_IOC_SET_INTERVAL_MS:
        if (copy_from_user(&new_ms, (void __user *)arg, sizeof(new_ms)))
            return -EFAULT;

        // keep sane bounds
        if (new_ms < 50 || new_ms > 5000)
            return -EINVAL;

        mutex_lock(&vs_lock);
        interval_ms = new_ms;
        vs_reschedule_timer_locked();
        mutex_unlock(&vs_lock);

        return 0;

    default:
        return -ENOTTY;
    }
}

static int vs_open(struct inode *i, struct file *f) { return 0; }
static int vs_release(struct inode *i, struct file *f) { return 0; }

static const struct file_operations vs_fops = {
    .owner          = THIS_MODULE,
    .open           = vs_open,
    .release        = vs_release,
    .read           = vs_read,
    .poll           = vs_poll,
    .unlocked_ioctl = vs_unlocked_ioctl,
};

static int __init vs_init(void)
{
    int rc;

    init_waitqueue_head(&vs_wq);

    rc = alloc_chrdev_region(&devno, 0, 1, DRIVER_NAME);
    if (rc) {
        pr_err(DRIVER_NAME ": alloc_chrdev_region failed (%d)\n", rc);
        return rc;
    }

    cdev_init(&vs_cdev, &vs_fops);
    rc = cdev_add(&vs_cdev, devno, 1);
    if (rc) {
        pr_err(DRIVER_NAME ": cdev_add failed (%d)\n", rc);
        unregister_chrdev_region(devno, 1);
        return rc;
    }

    vs_class = class_create(CLASS_NAME);
    if (IS_ERR(vs_class)) {
        rc = PTR_ERR(vs_class);
        pr_err(DRIVER_NAME ": class_create failed (%d)\n", rc);
        cdev_del(&vs_cdev);
        unregister_chrdev_region(devno, 1);
        return rc;
    }

    vs_device = device_create(vs_class, NULL, devno, NULL, "virt_sensor0");
    if (IS_ERR(vs_device)) {
        rc = PTR_ERR(vs_device);
        pr_err(DRIVER_NAME ": device_create failed (%d)\n", rc);
        class_destroy(vs_class);
        cdev_del(&vs_cdev);
        unregister_chrdev_region(devno, 1);
        return rc;
    }

    mutex_init(&vs_lock);

    timer_setup(&vs_timer, vs_timer_fn, 0);
    mod_timer(&vs_timer, jiffies + msecs_to_jiffies(interval_ms));

    pr_info(DRIVER_NAME ": loaded (major=%d minor=%d)\n", MAJOR(devno), MINOR(devno));
    return 0;
}

static void __exit vs_exit(void)
{
    del_timer_sync(&vs_timer);
    device_destroy(vs_class, devno);
    class_destroy(vs_class);
    cdev_del(&vs_cdev);
    unregister_chrdev_region(devno, 1);
    pr_info(DRIVER_NAME ": unloaded\n");
}

module_init(vs_init);
module_exit(vs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Virtual sensor char device with poll + ioctl interval control");