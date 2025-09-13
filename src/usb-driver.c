#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "usb_prolific_p2"
#define VENDOR_ID 0x067b
#define PRODUCT_ID 0x23a3
#define BULK_BUF_SIZE 512

static struct usb_driver usb_prolific_driver;

struct prolific {
    struct usb_device *usb_dev;
    struct usb_interface *usb_intf;
    unsigned char *bulk_in_buffer;
    size_t bulk_in_size;
    u8 bulk_in_ep;
    u8 bulk_out_ep;
    struct mutex io_mutex;
};

/* ---------------- File Operations ---------------- */

static int usb_open(struct inode *inode, struct file *file)
{
    int subminor = iminor(inode);
    struct usb_interface *intf;
    struct prolific *dev;

    pr_info("usb_open: called for minor=%d\n", subminor);

    intf = usb_find_interface(&usb_prolific_driver, subminor);
    if (!intf) {
        pr_err("usb_open: no such interface for minor %d\n", subminor);
        return -ENODEV;
    }

    dev = usb_get_intfdata(intf);
    if (!dev) {
        pr_err("usb_open: usb_get_intfdata() returned NULL\n");
        return -ENODEV;
    }

    pr_info("usb_open: success, dev=%p, bulk_in=0x%02x, bulk_out=0x%02x\n",
            dev, dev->bulk_in_ep, dev->bulk_out_ep);

    file->private_data = dev;
    return 0;
}

static int usb_release(struct inode *inode, struct file *file)
{
    pr_info("usb_release: called\n");
    return 0;
}

static ssize_t usb_read(struct file *file, char __user *buf,
                        size_t count, loff_t *offset)
{
    struct prolific *dev = file->private_data;
    int retval, actual;

    pr_info("usb_read: requested count=%zu\n", count);

    if (!dev) {
        pr_err("usb_read: dev is NULL\n");
        return -ENODEV;
    }

    if (count > dev->bulk_in_size)
        count = dev->bulk_in_size;

    mutex_lock(&dev->io_mutex);
    retval = usb_bulk_msg(dev->usb_dev,
                          usb_rcvbulkpipe(dev->usb_dev, dev->bulk_in_ep),
                          dev->bulk_in_buffer,
                          count, &actual, 5000);
    mutex_unlock(&dev->io_mutex);

    if (retval) {
        dev_err(&dev->usb_intf->dev, "usb_read: bulk read failed: %d\n", retval);
        return retval;
    }

    pr_info("usb_read: got %d bytes\n", actual);

    if (copy_to_user(buf, dev->bulk_in_buffer, actual)) {
        pr_err("usb_read: copy_to_user failed\n");
        return -EFAULT;
    }

    return actual;
}

static ssize_t usb_write(struct file *file, const char __user *buf,
                         size_t count, loff_t *offset)
{
    struct prolific *dev = file->private_data;
    void *kbuf;
    int retval, actual;

    pr_info("usb_write: requested count=%zu\n", count);

    if (!dev) {
        pr_err("usb_write: dev is NULL\n");
        return -ENODEV;
    }

    if (count > BULK_BUF_SIZE)
        count = BULK_BUF_SIZE;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        pr_err("usb_write: copy_from_user failed\n");
        return -EFAULT;
    }

    mutex_lock(&dev->io_mutex);
    retval = usb_bulk_msg(dev->usb_dev,
                          usb_sndbulkpipe(dev->usb_dev, dev->bulk_out_ep),
                          kbuf, count, &actual, 5000);
    mutex_unlock(&dev->io_mutex);

    kfree(kbuf);

    if (retval) {
        dev_err(&dev->usb_intf->dev, "usb_write: bulk write failed: %d\n", retval);
        return retval;
    }

    pr_info("usb_write: wrote %d bytes\n", actual);

    return actual;
}

/* ---------------- File Ops + Class ---------------- */

static const struct file_operations prolific_fops = {
    .owner = THIS_MODULE,
    .open = usb_open,
    .release = usb_release,
    .read = usb_read,
    .write = usb_write
};

static struct usb_class_driver prolific_class = {
    .name = "prolific%d",
    .fops = &prolific_fops,
    .minor_base = 0,
};

/* ---------------- Probe / Disconnect ---------------- */

static int usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
    struct prolific *dev;
    struct usb_host_interface *usb_host_intf;
    struct usb_endpoint_descriptor *endpoint;
    int i, retval = -ENOMEM;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->usb_dev = usb_get_dev(interface_to_usbdev(intf));
    dev->usb_intf = intf;
    mutex_init(&dev->io_mutex);
    usb_host_intf = intf->cur_altsetting;

    dev_info(&intf->dev,
             "usb_probe: Device Connected\n"
             " Vendor ID: %04x Product ID: %04x Num EP=%d\n",
             dev->usb_dev->descriptor.idVendor,
             dev->usb_dev->descriptor.idProduct,
             usb_host_intf->desc.bNumEndpoints);

    for (i = 0; i < usb_host_intf->desc.bNumEndpoints; i++) {
        endpoint = &usb_host_intf->endpoint[i].desc;
        dev_info(&intf->dev,
                 " usb_probe: ep[%d]: addr=0x%02x attr=0x%02x maxp=%d interval=%d\n",
                 i, endpoint->bEndpointAddress, endpoint->bmAttributes,
                 usb_endpoint_maxp(endpoint), endpoint->bInterval);

        if (usb_endpoint_is_bulk_in(endpoint)) {
            dev->bulk_in_ep = endpoint->bEndpointAddress;
            dev->bulk_in_size = usb_endpoint_maxp(endpoint);
            dev->bulk_in_buffer = kmalloc(dev->bulk_in_size, GFP_KERNEL);
            if (!dev->bulk_in_buffer)
                goto error;
        }
        if (usb_endpoint_is_bulk_out(endpoint)) {
            dev->bulk_out_ep = endpoint->bEndpointAddress;
        }
    }

    dev_info(&intf->dev,
             "usb_probe: endpoints found: bulk-in=0x%02x bulk-out=0x%02x\n",
             dev->bulk_in_ep, dev->bulk_out_ep);

    if (!(dev->bulk_in_ep && dev->bulk_out_ep)) {
        dev_err(&intf->dev, "usb_probe: required bulk endpoints not found\n");
        retval = -ENODEV;
        goto error;
    }

    usb_set_intfdata(intf, dev);

    retval = usb_register_dev(intf, &prolific_class);
    if (retval) {
        dev_err(&intf->dev, "usb_probe: Failed to register device node\n");
        usb_set_intfdata(intf, NULL);
        goto error;
    }

    dev_info(&intf->dev, "usb_probe: device registered as /dev/prolific%d\n", intf->minor);
    return 0;

error:
    if (dev->bulk_in_buffer)
        kfree(dev->bulk_in_buffer);
    usb_put_dev(dev->usb_dev);
    kfree(dev);
    return retval;
}

static void usb_disconnect(struct usb_interface *intf)
{
    struct prolific *dev = usb_get_intfdata(intf);

    usb_set_intfdata(intf, NULL);
    usb_deregister_dev(intf, &prolific_class);

    if (dev) {
        usb_put_dev(dev->usb_dev);
        if (dev->bulk_in_buffer)
            kfree(dev->bulk_in_buffer);
        mutex_destroy(&dev->io_mutex);
        kfree(dev);
    }

    dev_info(&intf->dev, "usb_disconnect: Device Removed\n");
}

/* ---------------- Device ID Table ---------------- */

static const struct usb_device_id usb_prolific_device_ids[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    {}
};
MODULE_DEVICE_TABLE(usb, usb_prolific_device_ids);

/* ---------------- Driver Registration ---------------- */

static struct usb_driver usb_prolific_driver = {
    .name = DRIVER_NAME,
    .probe = usb_probe,
    .disconnect = usb_disconnect,
    .id_table = usb_prolific_device_ids,
};

module_usb_driver(usb_prolific_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bharath Reddappa");
MODULE_DESCRIPTION("USB Prolific Phase-2 with debug logs");
