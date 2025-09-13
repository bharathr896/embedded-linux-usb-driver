#include<linux/module.h>
#include<linux/usb.h>

#define DRIVER_NAME "usb_prolific_p1"
#define VENDOR_ID 0x067b
#define PRODUCT_ID 0x23a3

static int usb_probe (struct usb_interface *intf,const struct usb_device_id *id);
static void usb_disconnect (struct usb_interface *intf);

static int usb_probe (struct usb_interface *intf,const struct usb_device_id *id){

    struct usb_device *usb_dev = interface_to_usbdev(intf);
    struct usb_host_interface *usb_host_intf = intf->cur_altsetting;
    struct usb_endpoint_descriptor *endpoint;
    int i;

    dev_info(&intf->dev,"prolific_probe: Device Connected.\n Vendor ID: %04x Product ID: %04x \n",usb_dev->descriptor.idVendor,usb_dev->descriptor.idProduct);
    dev_info(&intf->dev,"num_endpoints = %d \n",usb_host_intf->desc.bNumEndpoints);

    unsigned int n_endpoints = usb_host_intf->desc.bNumEndpoints;

    for(i=0;i<n_endpoints;i++){
        endpoint = &usb_host_intf->endpoint[i].desc;
        dev_info(&intf->dev,
		         "ep[%d]: addr=0x%02x attr=0x%02x maxp=%d interval=%d\n",
		         i, endpoint->bEndpointAddress, endpoint->bmAttributes,
		         usb_endpoint_maxp(endpoint), endpoint->bInterval);


    }

    return 0;

}


static void usb_disconnect (struct usb_interface *intf){
    dev_info(&intf->dev,"prolific_disconnect: Device Removed \n");
}


static const struct usb_device_id usb_prolific_device_ids [] ={
    {USB_DEVICE(VENDOR_ID,PRODUCT_ID)},
    {}
};

static struct usb_driver usb_prolific_driver = {
    .name = DRIVER_NAME,
    .probe = usb_probe,
    .disconnect = usb_disconnect,
    .id_table = usb_prolific_device_ids,
    
};


module_usb_driver(usb_prolific_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bharath Reddappa");
MODULE_DESCRIPTION("A Simple USB Driver");
