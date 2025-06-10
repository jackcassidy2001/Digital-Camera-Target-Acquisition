
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>

MODULE_AUTHOR("CprE 488 - Students");
MODULE_DESCRIPTION("A USB driver for controlling a missle launcher");

// Launcher control message values
#define LAUNCHER_CLASS_NAME            "launcher%d"
#define LAUNCHER_DRIVER_NAME           "launcher"
#define LAUNCHER_CTRL_BUFFER_SIZE       8
#define LAUNCHER_CTRL_REQUEST_TYPE      0x21
#define LAUNCHER_CTRL_REQUEST           0x09
#define LAUNCHER_CTRL_VALUE             0x0        
#define LAUNCHER_CTRL_INDEX             0x0
#define LAUNCHER_CTRL_COMMAND_PREFIX    0x02
#define LAUNCHER_INT_BUFFER_SIZE        8

#define LAUNCHER_STOP                   0x20
#define LAUNCHER_UP                     0x02
#define LAUNCHER_DOWN                   0x01
#define LAUNCHER_LEFT                   0x04
#define LAUNCHER_RIGHT                  0x08

#define LAUNCHER_MAX_UP                 0x80            /* 80 00 00 00 00 00 00 00 */
#define LAUNCHER_MAX_DOWN               0x40            /* 40 00 00 00 00 00 00 00 */
#define LAUNCHER_MAX_LEFT               0x04            /* 00 04 00 00 00 00 00 00 */
#define LAUNCHER_MAX_RIGHT              0x08            /* 00 08 00 00 00 00 00 00 */

/* Define these values to match your devices */
#define LAUNCHER_VENDOR_ID              0x2123
#define LAUNCHER_PRODUCT_ID             0x1010

/* table of devices that work with this driver */
static const struct usb_device_id launcher_table[] = {
	{ USB_DEVICE(LAUNCHER_VENDOR_ID , LAUNCHER_PRODUCT_ID) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, launcher_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_LAUNCHER_MINOR_BASE	0

/* our private defines. if this grows any larger, use your own .h file */
#define MAX_TRANSFER		(PAGE_SIZE - 512)
/* MAX_TRANSFER is chosen so that the VM is not stressed by
   allocations > PAGE_SIZE and the number of packets in a page
   is an integer 512 is the largest possible packet on EHCI */
#define WRITES_IN_FLIGHT	8
/* arbitrarily chosen */

/* Structure to hold all of our device specific stuff */
struct usb_launcher {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */

	unsigned char cmd;
	unsigned char correction_required;
	struct urb *correction_urb;
	unsigned char *correction_buffer;
	unsigned char *setup_packet;

	struct usb_endpoint_descriptor *int_in_endpoint; // Member to store interrupt IN endpoint descriptor
	struct urb *int_in_urb;
	unsigned char *int_in_buffer;
	
	// BULK -- NOT USED --
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char           *bulk_in_buffer;	/* the buffer to receive data */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	size_t			bulk_in_copied;		/* already copied to user space */
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
	int			errors;			/* the last request tanked */
	bool			ongoing_read;		/* a read is going on */
	bool			processed_urb;		/* indicates we haven't processed the urb */
	spinlock_t		err_lock;		/* lock for errors */
	struct kref		kref;
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	struct completion	bulk_in_completion;	/* to wait for an ongoing read */
};
#define to_launcher_dev(d) container_of(d, struct usb_launcher, kref)

static struct usb_driver launcher_driver;
static void launcher_draw_down(struct usb_launcher *dev);

static void launcher_delete(struct kref *kref)
{
	struct usb_launcher *dev = to_launcher_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}

static int launcher_open(struct inode *inode, struct file *file)
{
	struct usb_launcher *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&launcher_driver, subminor);
	if (!interface) {
		pr_err("%s - error, can't find device for minor %d\n",
			__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(interface);
	if (retval)
		goto exit;

	/* increment our usage count for the device */
	kref_get(&dev->kref);

	/* save our object in the file's private structure */
	file->private_data = dev;

exit:
	return retval;
}

static int launcher_release(struct inode *inode, struct file *file)
{
	struct usb_launcher *dev;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* allow the device to be autosuspended */
	mutex_lock(&dev->io_mutex);
	if (dev->interface)
		usb_autopm_put_interface(dev->interface);
	mutex_unlock(&dev->io_mutex);

	/* decrement the count on our device */
	kref_put(&dev->kref, launcher_delete);
	return 0;
}

static int launcher_flush(struct file *file, fl_owner_t id)
{
	struct usb_launcher *dev;
	int res;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* wait for io to stop */
	mutex_lock(&dev->io_mutex);
	launcher_draw_down(dev);

	/* read out errors, leave subsequent opens a clean slate */
	spin_lock_irq(&dev->err_lock);
	res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
	dev->errors = 0;
	spin_unlock_irq(&dev->err_lock);

	mutex_unlock(&dev->io_mutex);

	return res;
}

// Bulk Endpoint - Ignore
static void launcher_read_bulk_callback(struct urb *urb)
{
	struct usb_launcher *dev;

	dev = urb->context;

	spin_lock(&dev->err_lock);
	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		dev->errors = urb->status;
	} else {
		dev->bulk_in_filled = urb->actual_length;
	}
	dev->ongoing_read = 0;
	spin_unlock(&dev->err_lock);

	complete(&dev->bulk_in_completion);
}

static int launcher_do_read_io(struct usb_launcher *dev, size_t count)
{
	int rv;

	/* prepare a read */
	usb_fill_bulk_urb(dev->bulk_in_urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev,
				dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			min(dev->bulk_in_size, count),
			launcher_read_bulk_callback,
			dev);
	/* tell everybody to leave the URB alone */
	spin_lock_irq(&dev->err_lock);
	dev->ongoing_read = 1;
	spin_unlock_irq(&dev->err_lock);

	/* do it */
	rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, rv);
		dev->bulk_in_filled = 0;
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irq(&dev->err_lock);
		dev->ongoing_read = 0;
		spin_unlock_irq(&dev->err_lock);
	}

	return rv;
}

// Read Operation - Ignore
static ssize_t launcher_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos)
{
	struct usb_launcher *dev;
	int rv;
	bool ongoing_io;

	dev = file->private_data;

	/* if we cannot read at all, return EOF */
	if (!dev->bulk_in_urb || !count)
		return 0;

	/* no concurrent readers */
	rv = mutex_lock_interruptible(&dev->io_mutex);
	if (rv < 0)
		return rv;

	if (!dev->interface) {		/* disconnect() was called */
		rv = -ENODEV;
		goto exit;
	}

	/* if IO is under way, we must not touch things */
retry:
	spin_lock_irq(&dev->err_lock);
	ongoing_io = dev->ongoing_read;
	spin_unlock_irq(&dev->err_lock);

	if (ongoing_io) {
		/* nonblocking IO shall not wait */
		if (file->f_flags & O_NONBLOCK) {
			rv = -EAGAIN;
			goto exit;
		}
		/*
		 * IO may take forever
		 * hence wait in an interruptible state
		 */
		rv = wait_for_completion_interruptible(&dev->bulk_in_completion);
		if (rv < 0)
			goto exit;
		/*
		 * by waiting we also semiprocessed the urb
		 * we must finish now
		 */
		dev->bulk_in_copied = 0;
		dev->processed_urb = 1;
	}

	if (!dev->processed_urb) {
		/*
		 * the URB hasn't been processed
		 * do it now
		 */
		wait_for_completion(&dev->bulk_in_completion);
		dev->bulk_in_copied = 0;
		dev->processed_urb = 1;
	}

	/* errors must be reported */
	rv = dev->errors;
	if (rv < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		rv = (rv == -EPIPE) ? rv : -EIO;
		/* no data to deliver */
		dev->bulk_in_filled = 0;
		/* report it */
		goto exit;
	}

	/*
	 * if the buffer is filled we may satisfy the read
	 * else we need to start IO
	 */

	if (dev->bulk_in_filled) {
		/* we had read data */
		size_t available = dev->bulk_in_filled - dev->bulk_in_copied;
		size_t chunk = min(available, count);

		if (!available) {
			/*
			 * all data has been used
			 * actual IO needs to be done
			 */
			rv = launcher_do_read_io(dev, count);
			if (rv < 0)
				goto exit;
			else
				goto retry;
		}
		/*
		 * data is available
		 * chunk tells us how much shall be copied
		 */

		if (copy_to_user(buffer,
				 dev->bulk_in_buffer + dev->bulk_in_copied,
				 chunk))
			rv = -EFAULT;
		else
			rv = chunk;

		dev->bulk_in_copied += chunk;

		/*
		 * if we are asked for more than we have,
		 * we start IO but don't wait
		 */
		if (available < count)
			launcher_do_read_io(dev, count - chunk);
	} else {
		/* no data in the buffer */
		rv = launcher_do_read_io(dev, count);
		if (rv < 0)
			goto exit;
		else if (!(file->f_flags & O_NONBLOCK))
			goto retry;
		rv = -EAGAIN;
	}
exit:
	mutex_unlock(&dev->io_mutex);
	return rv;
}

// Bulk Endpoint - Ignore
static void launcher_write_bulk_callback(struct urb *urb)
{
	struct usb_launcher *dev;

	dev = urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		spin_lock(&dev->err_lock);
		dev->errors = urb->status;
		spin_unlock(&dev->err_lock);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);
	up(&dev->limit_sem);
}

// Clear correction required on successful completion.
static void correction_callback(struct urb *urb)
{
    // Extract the device pointer from the URB context
    struct usb_launcher *dev = urb->context;

    // Check if the URB completed successfully
    if (urb->status == 0) {
        dev_dbg(&dev->udev->dev, "Correction URB completed successfully\n");
		dev->correction_required = 0;
    } else {
        dev_err(&dev->udev->dev, "Correction URB failed with error: %d\n", urb->status);
    }
}

// Define a completion handler function
static void int_in_callback(struct urb *urb)
{
    struct usb_launcher *dev = urb->context;
    int err;

    // Check if the URB completed successfully
    if (urb->status == 0) {
        // Process data received in urb->transfer_buffer
        if (dev->int_in_buffer[0] & LAUNCHER_MAX_UP && dev->cmd & LAUNCHER_UP)
        {
            dev->correction_required = 1;
        } else if (dev->int_in_buffer[0] & LAUNCHER_MAX_DOWN && dev->cmd & LAUNCHER_DOWN)
        {
            dev->correction_required = 1;
        } else if (dev->int_in_buffer[1] & LAUNCHER_MAX_LEFT && dev->cmd & LAUNCHER_LEFT)
        {
            dev->correction_required = 1;
        } else if (dev->int_in_buffer[1] & LAUNCHER_MAX_RIGHT && dev->cmd & LAUNCHER_RIGHT)
        {
            dev->correction_required = 1;
        } else 
		{
            dev->correction_required = 0;
        }

        if (dev->correction_required) {
            // Initialize the correction buffer
            dev->correction_buffer[0] = LAUNCHER_CTRL_COMMAND_PREFIX;
            dev->correction_buffer[1] = LAUNCHER_STOP;

			usb_fill_control_urb(dev->correction_urb,
								dev->udev,
								usb_sndctrlpipe(dev->udev, 0),
								dev->setup_packet,
								dev->correction_buffer,
								LAUNCHER_CTRL_BUFFER_SIZE,
								correction_callback,
								dev);

            err = usb_submit_urb(dev->correction_urb, GFP_ATOMIC);
            if (err) {
                dev_err(&dev->udev->dev, "Error submitting correction URB: %d\n", err);
            }
        }
    }

    // Re-submit the interrupt URB
    err = usb_submit_urb(urb, GFP_ATOMIC);
    if (err) {
        dev_err(&dev->udev->dev, "Error resubmitting interrupt URB: %d\n", err);
    }
}

// ** Write Operation ** 
// Replace filling and submission of bulk urb
/*
The “write” operations are packed into a control URB and sent to the device. In function
launcher_write(), you will need to receive the user data and call usb_control_msg():
	o The call to usb_control_msg() should replace the calls that fill and submit a bulk urb in
	  function launcher_write().
	o We are always sending 8 byte messages, with byte 0 being 0x02
	  (LAUNCHER_CTRL_CMD_PREFIX), byte 1 corresponding to the user input byte (the
	  command), and the rest of the bytes being 0x0. 
*/

static ssize_t launcher_write(struct file *file, const char __user *user_cmd,
                              size_t count, loff_t *ppos)
{
    struct usb_launcher *dev;
    int retval = 0;
    char *cmd_buf = NULL;
	int i;
	size_t writesize = count;


	printk(KERN_INFO "Entering Launcher Write..\n");

	if(count == 0){
		printk(KERN_INFO "Count 0..Exit\n");
		goto exit;
	}


	cmd_buf = kzalloc(LAUNCHER_CTRL_BUFFER_SIZE, GFP_KERNEL);
	if (!cmd_buf) {
		printk(KERN_INFO "Could not allocate memory for command BUFFER\n");
        retval = -ENOMEM;
        goto error;
    }


    dev = file->private_data;

	printk(KERN_INFO "Dev Initilized\n");
    /* verify that we actually have some data to write */
    if (count == 0)
        goto exit;


	printk(KERN_INFO "About to Store to CMD_BUF\n");
    /* Create the control message */
    cmd_buf[0] = LAUNCHER_CTRL_COMMAND_PREFIX;  // Byte 0 is the command prefix
    if (copy_from_user(&cmd_buf[1], user_cmd, 1)) {  // Byte 1 is the user input byte
        retval = -EFAULT;
        goto exit;
    }
	printk(KERN_INFO "About to Store to DEV_CMD\n");
    dev->cmd = cmd_buf[1];

	for(i = 0; i < LAUNCHER_CTRL_BUFFER_SIZE; i++){
		printk(KERN_INFO "CMD Byte %d : %d\n", i, cmd_buf[i]);
	}

    /* Send the control message */
    retval = usb_control_msg(dev->udev,
                             usb_sndctrlpipe(dev->udev, 0),
                             LAUNCHER_CTRL_REQUEST,
                             LAUNCHER_CTRL_REQUEST_TYPE,
                             LAUNCHER_CTRL_VALUE, /* Value */
                             LAUNCHER_CTRL_INDEX , /* Index */
                             cmd_buf, /* Data buffer */
                             LAUNCHER_CTRL_BUFFER_SIZE , /* Data buffer length */
                             5000 /* Timeout in ms */);


	printk(KERN_INFO "retval usb_control_msg(): %d\n", retval);


    if (retval < 0) {
        dev_err(&dev->interface->dev,
                "%s - failed submitting write urb, error %d\n",
                __func__, retval);
        goto error;
    }



	kfree(cmd_buf);

    return writesize;

error:
	printk(KERN_INFO "Error in Launcher Write");

exit:
    return retval;
}



// Look into
static const struct file_operations launcher_fops = {
	.owner =	THIS_MODULE,
	.read =		launcher_read,
	.write =	launcher_write,
	.open =		launcher_open,
	.release =	launcher_release,
	.flush =	launcher_flush,
	.llseek =	noop_llseek,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver launcher_class = {
	.name =		LAUNCHER_CLASS_NAME,
	.fops =		&launcher_fops,
	.minor_base =	USB_LAUNCHER_MINOR_BASE,
};

// After module loaded, probe usb interfaces for compatible devices and intilize them.
// Our device does not have any bulk or isochronous endpoints, and just 1 input interrupt endpoint.
// Consequently you can simplify the code in launcher_probe() (or just comment out the error
// handling) to setup a control URB.
static int launcher_probe(struct usb_interface *interface,
                          const struct usb_device_id *id)
{
    struct usb_launcher *dev = NULL;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    int err;
    int i;
    int retval = -ENOMEM;
    struct urb *urb = NULL;

	printk(KERN_INFO "Entering Laucnher Probe..\n");

    // Allocate memory for the device state and initialize it
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        dev_err(&interface->dev, "Out of memory\n");
        retval = -ENOMEM;
        goto error;
    }

	printk(KERN_INFO "Finished Allocating memory for device..\n");
    dev->cmd = LAUNCHER_STOP;
    dev->correction_required = 0;

    // Initialize reference count, semaphores, mutexes, and locks
    kref_init(&dev->kref);
    sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
    mutex_init(&dev->io_mutex);
    spin_lock_init(&dev->err_lock);
    init_usb_anchor(&dev->submitted);
    init_completion(&dev->bulk_in_completion);


	printk(KERN_INFO "Getting interface and registering endpoint..\n");
    // Get a reference to the USB device and interface
    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = interface;

    // Set up the endpoint information for interrupt IN
    iface_desc = interface->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        endpoint = &iface_desc->endpoint[i].desc;

        if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN &&
            (endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT) {
            // Found an interrupt IN endpoint - setup & store
			printk(KERN_INFO "Found int endpoint..\n");
            dev->int_in_endpoint = endpoint;
            dev->int_in_buffer = kmalloc(LAUNCHER_INT_BUFFER_SIZE, GFP_KERNEL);
            if (!dev->int_in_buffer) {
                dev_err(&interface->dev, "Could not allocate int_in_buffer\n");
                retval = -ENOMEM;
                goto error;
            }
			printk(KERN_INFO "Allocating urb..\n");
            urb = usb_alloc_urb(0, GFP_KERNEL);
            if (!urb) {
                dev_err(&interface->dev, "Could not allocate int_in_urb\n");
                retval = -ENOMEM;
                goto error;
            }
			printk(KERN_INFO "Filling urb..\n");
            // Initialize the URB for interrupt transfer
            usb_fill_int_urb(urb,
                             dev->udev,
                             usb_rcvintpipe(dev->udev, dev->int_in_endpoint->bEndpointAddress),
                             dev->int_in_buffer, // Buffer to receive data
                             LAUNCHER_INT_BUFFER_SIZE, // Buffer size
                             int_in_callback,    // Completion handler
                             dev,                // Context pointer
                             dev->int_in_endpoint->bInterval);

			printk(KERN_INFO "Submitting urb..\n");
            // Submit the URB
            err = usb_submit_urb(urb, GFP_KERNEL);
            if (err) {
                dev_err(&interface->dev, "Could not submit int_in_urb: %d\n", err);
                retval = err;
                goto error;
            }
            break;
        }
    }

	printk(KERN_INFO "Allocating correction urb..\n");
    // Save the URB for later use
    dev->int_in_urb = urb;

    // Setup correction URB
    dev->correction_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!dev->correction_urb) {
        dev_err(&interface->dev, "Could not allocate correction_urb\n");
        retval = -ENOMEM;
        goto error;
    }
	printk(KERN_INFO "Allocating correction buffer..\n");
    dev->correction_buffer = kmalloc(LAUNCHER_CTRL_BUFFER_SIZE, GFP_KERNEL);
    if (!dev->correction_buffer) {
        dev_err(&interface->dev, "Could not allocate correction_buffer\n");
        retval = -ENOMEM;
        goto error;
    }
	printk(KERN_INFO "Allocating setup packet..\n");
    // Allocate memory for setup_packet and store values
    dev->setup_packet = kmalloc(8 * sizeof(unsigned char), GFP_KERNEL);
    if (!dev->setup_packet) {
        dev_err(&interface->dev, "Could not allocate memory for setup_packet\n");
        retval = -ENOMEM;
        goto error;
    }

	printk(KERN_INFO "Assigning setuppacket..\n");
    // Fill the setup_packet with appropriate values
    dev->setup_packet[0] = LAUNCHER_CTRL_REQUEST_TYPE;
    dev->setup_packet[1] = LAUNCHER_CTRL_REQUEST;
    dev->setup_packet[2] = LAUNCHER_CTRL_VALUE & 0xFF;
    dev->setup_packet[3] = (LAUNCHER_CTRL_VALUE >> 8) & 0xFF;
    dev->setup_packet[4] = LAUNCHER_CTRL_INDEX & 0xFF;
    dev->setup_packet[5] = (LAUNCHER_CTRL_INDEX >> 8) & 0xFF;
    dev->setup_packet[6] = LAUNCHER_CTRL_BUFFER_SIZE & 0xFF;
    dev->setup_packet[7] = (LAUNCHER_CTRL_BUFFER_SIZE >> 8) & 0xFF;

    // Save our data pointer in this interface device
    usb_set_intfdata(interface, dev);

	printk(KERN_INFO "Registering device..\n");
    // Register the device now that it is ready
    retval = usb_register_dev(interface, &launcher_class);
    if (retval) {
        dev_err(&interface->dev, "Not able to get a minor for this device.\n");
        usb_set_intfdata(interface, NULL);
        goto error;
    }
	printk(KERN_INFO "Attaching device..\n");
    // Let the user know what node this device is now attached to
    dev_info(&interface->dev, "USB Launcher device now attached to USBlauncher-%d", interface->minor);
    return 0;

error:
    // Cleanup on error
    if (urb)
        usb_free_urb(urb);
    if (dev) {
        kfree(dev->int_in_buffer);
        kfree(dev->correction_buffer);
        kfree(dev->setup_packet);
        kref_put(&dev->kref, launcher_delete);
    }
    return retval;
}



// Usb interface disconnected 
static void launcher_disconnect(struct usb_interface *interface)
{
	struct usb_launcher *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &launcher_class);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->interface = NULL;
	mutex_unlock(&dev->io_mutex);

	usb_kill_anchored_urbs(&dev->submitted);

	/* decrement our usage count */
	kref_put(&dev->kref, launcher_delete);

	dev_info(&interface->dev, "USB Launcher #%d now disconnected", minor);
}

/*ensures that any pending URBs associated with the USB launchereton device are canceled during the shutdown process*/
static void launcher_draw_down(struct usb_launcher *dev)
{
	int time;

	time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&dev->submitted);
	usb_kill_urb(dev->bulk_in_urb);
}

static int launcher_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_launcher *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;
	launcher_draw_down(dev);
	return 0;
}

static int launcher_resume(struct usb_interface *intf)
{
	return 0;
}

static int launcher_pre_reset(struct usb_interface *intf)
{
	struct usb_launcher *dev = usb_get_intfdata(intf);

	mutex_lock(&dev->io_mutex);
	launcher_draw_down(dev);

	return 0;
}

static int launcher_post_reset(struct usb_interface *intf)
{
	struct usb_launcher *dev = usb_get_intfdata(intf);

	/* we are sure no URBs are active - no locking needed */
	dev->errors = -EPIPE;
	mutex_unlock(&dev->io_mutex);

	return 0;
}

static struct usb_driver launcher_driver = {
	.name =		LAUNCHER_DRIVER_NAME,
	.probe =	launcher_probe,
	.disconnect =	launcher_disconnect,
	.suspend =	launcher_suspend,
	.resume =	launcher_resume,
	.pre_reset =	launcher_pre_reset,
	.post_reset =	launcher_post_reset,
	.id_table =	launcher_table,
	.supports_autosuspend = 1,
};

module_usb_driver(launcher_driver);

MODULE_LICENSE("GPL");
