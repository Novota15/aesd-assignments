/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h>
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Grant Novota"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    filp->private_data = container_of(inode->i_cdev, struct aesd_dev, cdev);
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    filp->private_data = NULL;
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t read_bytes = 0;
    ssize_t ret_entry_offset = 0;
    struct aesd_buffer_entry *buffer_entry = NULL;
    struct aesd_dev *aesd_dev = NULL;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    if((filp == NULL) || (buf == NULL))
    {
        return -EINVAL;
    }
    aesd_dev = filp->private_data;

    // Lock mutex before acessing the global data
    if (mutex_lock_interruptible(&aesd_dev->aesd_mutex))
    {
        PDEBUG("mutex_lock_interruptible failed");
        return -ERESTARTSYS;
    }

    // Find entry for the position specified
    buffer_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&aesd_dev->buffer, *f_pos, &ret_entry_offset);
    if(buffer_entry == NULL)
    {
        PDEBUG("aesd_circular_buffer_find_entry_offset_for_fpos returned nothing");
        mutex_unlock(&aesd_dev->aesd_mutex);
        return read_bytes;
    }

    // calculate the number of bytes that can be read
    if((buffer_entry->size - ret_entry_offset) > count)
    {
        read_bytes = count;
    }
    else
    {
        read_bytes = buffer_entry->size - ret_entry_offset;
    }

    // copy the read_bytes number of bytes from kernel buffer to user buffer
    if (copy_to_user(buf, buffer_entry->buffptr+ret_entry_offset, read_bytes))
    {
        PDEBUG("copy_to_user failed");
        mutex_unlock(&aesd_dev->aesd_mutex);
        return -EFAULT;
    }

    // advance the pointer by the number of bytes read
    *f_pos += read_bytes;

    // unlock mutex once done accessing global data
    mutex_unlock(&aesd_dev->aesd_mutex);

    return read_bytes;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t write_bytes = 0;
    char *write_buf = NULL;
    struct aesd_dev *aesd_dev = NULL;
    char *last_byte = NULL;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if((filp == NULL) || (buf == NULL))
    {
        return -EINVAL;
    }

    // allocate count number of bytes for kernel buffer
    write_buf = kmalloc(count, GFP_KERNEL);
    if(write_buf == NULL)
    {
        PDEBUG("kmalloc failed");
        return -ENOMEM;
    }

    // Copy buffer from user space into kernel space
    if (copy_from_user(write_buf, buf, count))
    {
        PDEBUG("copy_from_user failed");
        kfree(write_buf);
        return -EFAULT;
    }

    // Check for the position of new line character
    last_byte = memchr(write_buf, '\n', count);
    if(last_byte != NULL)
    {
        // If new line, calculate the number of bytes to write
        write_bytes = (last_byte - write_buf) + 1;
    }
    else
    {
        // If no new line, write upto the capacity
        write_bytes = count;
    }

    aesd_dev = filp->private_data;

    // Lock mutex before acessing the global data
    if (mutex_lock_interruptible(&aesd_dev->aesd_mutex))
    {
        PDEBUG("mutex_lock_interruptible failed");
        kfree(write_buf);
        return -ERESTARTSYS;
    }

    // Reallocate the memory based on write bytes
    aesd_dev->buffer_entry.buffptr = krealloc(aesd_dev->buffer_entry.buffptr, aesd_dev->buffer_entry.size + write_bytes, GFP_KERNEL);
    if(aesd_dev->buffer_entry.buffptr == NULL)
    {
        PDEBUG("krealloc failed");
        mutex_unlock(&aesd_dev->aesd_mutex);
        kfree(write_buf);
        return -ENOMEM;
    }

    // Copy the kernel space buffer content to the circular buffer entry
    memcpy((void *)aesd_dev->buffer_entry.buffptr+aesd_dev->buffer_entry.size, write_buf, write_bytes);
    aesd_dev->buffer_entry.size += write_bytes;

    if(last_byte)
    {
        const char *ret_ptr = NULL;
        // If new line was detected, write to the circular buffer
        ret_ptr = aesd_circular_buffer_add_entry(&aesd_dev->buffer, &aesd_dev->buffer_entry);
        if(ret_ptr)
        {
            // Free the pointer to the oldest data
            kfree(ret_ptr);
        }
        // Reset the pointer and size for the new request
        aesd_dev->buffer_entry.size = 0;
        aesd_dev->buffer_entry.buffptr = NULL;
    }

    // unlock mutex once done accessing global data
    mutex_unlock(&aesd_dev->aesd_mutex);
    kfree(write_buf);

    return count;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    mutex_init(&aesd_device.aesd_mutex);

    aesd_circular_buffer_init(&aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    uint8_t index = 0;
    struct aesd_buffer_entry *entry;
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    // Referenced from aesd-circular-buffer.h

    AESD_CIRCULAR_BUFFER_FOREACH(entry,&aesd_device.buffer,index) {
        kfree(entry->buffptr);
    }
    mutex_destroy(&aesd_device.aesd_mutex);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);