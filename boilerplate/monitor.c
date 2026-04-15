//monitor.c
// ===================== COMPLETE monitor.c =====================
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/workqueue.h> // REPLACED timer.h for process-context safety!
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

// ===================== NODE =====================
typedef struct monitor_entry {
    pid_t pid;
    char container_id[32];
    unsigned long soft_limit;
    unsigned long hard_limit;
    int soft_triggered;

    struct list_head list;
} monitor_entry_t;

// ===================== GLOBAL LIST =====================
static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

// ===================== REPLACED TIMER WITH WORKQUEUE =====================
static struct delayed_work monitor_dwork;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

// ===================== RSS =====================
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm); // DANGER: mmput() sleeps! Cannot be called in interrupt context!
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

// ===================== EVENTS =====================
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    unsigned long rss_mib_x10 = (rss_bytes * 10) / (1024 * 1024);
    unsigned long lim_mib_x10 = (limit_bytes * 10) / (1024 * 1024);

    printk(KERN_WARNING "engine_monitor: SOFT LIMIT EXCEEDED for container=%s\n", container_id);
    printk(KERN_WARNING "engine_monitor: Current Memory: %lu.%lu MiB | Soft Limit: %lu.%lu MiB\n",
           rss_mib_x10 / 10, rss_mib_x10 % 10, lim_mib_x10 / 10, lim_mib_x10 % 10);
    printk(KERN_WARNING "engine_monitor: WARNING: container '%s' approaching hard isolation boundary.\n", container_id);
}

static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;
    unsigned long rss_mib_x10 = (rss_bytes * 10) / (1024 * 1024);
    unsigned long lim_mib_x10 = (limit_bytes * 10) / (1024 * 1024);

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    // Send standard SIGKILL internally
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING "engine_monitor: HARD LIMIT EXCEEDED for container=%s (OOM)\n", container_id);
    printk(KERN_WARNING "engine_monitor: Current Memory: %lu.%lu MiB | Hard Limit: %lu.%lu MiB\n",
           rss_mib_x10 / 10, rss_mib_x10 % 10, lim_mib_x10 / 10, lim_mib_x10 % 10);
    printk(KERN_WARNING "engine_monitor: Terminating container '%s' (PID %d). SIGKILL sent.\n", container_id, pid);
}

// ===================== WORKQUEUE CALLBACK =====================
static void monitor_work_func(struct work_struct *work)
{
    monitor_entry_t *entry, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(entry, tmp, &monitor_list, list)
    {
        long rss = get_rss_bytes(entry->pid);

        // process ended independently
        if (rss < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        // soft limit crossed
        if (!entry->soft_triggered && rss > entry->soft_limit) {
            log_soft_limit_event(entry->container_id, entry->pid, entry->soft_limit, rss);
            entry->soft_triggered = 1;
        }

        // hard limit crossed
        if (rss > entry->hard_limit) {
            kill_process(entry->container_id, entry->pid, entry->hard_limit, rss);
            list_del(&entry->list);
            kfree(entry);
        }
    }

    mutex_unlock(&monitor_lock);

    // Reschedule work
    schedule_delayed_work(&monitor_dwork, CHECK_INTERVAL_SEC * HZ);
}

// ===================== IOCTL =====================
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    monitor_entry_t *entry, *tmp;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER)
    {
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        strncpy(entry->container_id, req.container_id, sizeof(entry->container_id));
        entry->container_id[sizeof(entry->container_id) - 1] = '\0';
        
        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;
        entry->soft_triggered = 0;

        mutex_lock(&monitor_lock);
        list_add(&entry->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        printk(KERN_INFO "engine_monitor: Container '%s' memory usage tracking initiated.\n", entry->container_id);

        return 0;
    }
    else if (cmd == MONITOR_UNREGISTER)
    {
        mutex_lock(&monitor_lock);

        list_for_each_entry_safe(entry, tmp, &monitor_list, list)
        {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                mutex_unlock(&monitor_lock);
                return 0;
            }
        }

        mutex_unlock(&monitor_lock);
        return -ENOENT;
    }

    return -EINVAL;
}

// ===================== FILE OPS =====================
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

// ===================== INIT =====================
static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    device_create(cl, NULL, dev_num, NULL, DEVICE_NAME);

    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, dev_num, 1);

    // Initialize Workqueue instead of Timer
    INIT_DELAYED_WORK(&monitor_dwork, monitor_work_func);
    schedule_delayed_work(&monitor_dwork, CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "engine_monitor: Module successfully loaded and polling active.\n");
    return 0;
}

// ===================== EXIT =====================
static void __exit monitor_exit(void)
{
    monitor_entry_t *entry, *tmp;

    // Purge workqueue safely
    cancel_delayed_work_sync(&monitor_dwork);

    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &monitor_list, list)
    {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "engine_monitor: Module unloaded safely.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");