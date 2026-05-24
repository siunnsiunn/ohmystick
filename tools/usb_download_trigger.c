// Send voxstick's HID recovery sequence via raw USB control transfers.
//
// This is a fallback for macOS sessions where IOHID refuses SetReport on
// keyboard-class devices. It sends the standard HID SET_REPORT(Output)
// request directly to the HID interface.

#include <libusb.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#define VOX_VID 0x303a
#define VOX_PID 0x8001

#define HID_SET_REPORT 0x09
#define HID_REPORT_TYPE_OUTPUT 0x02
#define HID_REPORT_ID_KBD 1
#define HID_LED_MAGIC 0x1f
#define USB_CLASS_HID 0x03
#define USB_CLASS_VENDOR_SPECIFIC 0xff
#define VENDOR_DOWNLOAD_REQUEST 0x5d
#define VENDOR_DOWNLOAD_VALUE 0x5344
#define VENDOR_DOWNLOAD_INDEX 0x4c44
#define VENDOR_DOWNLOAD_MAGIC "VOXSTICK_DOWNLOAD"

static int find_interface(libusb_device *device,
                          uint8_t class_code,
                          uint16_t *iface_out,
                          uint8_t *out_ep)
{
    struct libusb_config_descriptor *cfg = NULL;
    int rc = libusb_get_active_config_descriptor(device, &cfg);
    if (rc != 0) {
        rc = libusb_get_config_descriptor(device, 0, &cfg);
    }
    if (rc != 0) {
        return rc;
    }

    for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *interface = &cfg->interface[i];
        for (int j = 0; j < interface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt =
                &interface->altsetting[j];
            if (alt->bInterfaceClass != class_code) {
                continue;
            }

            *iface_out = alt->bInterfaceNumber;
            if (out_ep) {
                *out_ep = 0;
                for (uint8_t ep = 0; ep < alt->bNumEndpoints; ep++) {
                    const struct libusb_endpoint_descriptor *desc =
                        &alt->endpoint[ep];
                    if ((desc->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                            LIBUSB_TRANSFER_TYPE_BULK &&
                        (desc->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                            LIBUSB_ENDPOINT_OUT) {
                        *out_ep = desc->bEndpointAddress;
                        break;
                    }
                }
            }
            libusb_free_config_descriptor(cfg);
            return 0;
        }
    }

    libusb_free_config_descriptor(cfg);
    return LIBUSB_ERROR_NOT_FOUND;
}

static int find_hid_interface(libusb_device *device, uint16_t *iface_out)
{
    return find_interface(device, USB_CLASS_HID, iface_out, NULL);
}

static int send_one(libusb_device_handle *dev,
                    uint16_t iface,
                    uint16_t report_id,
                    const uint8_t *report,
                    uint16_t report_len)
{
    const uint8_t request_type =
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE;
    const uint16_t value = (HID_REPORT_TYPE_OUTPUT << 8) | report_id;

    return libusb_control_transfer(dev, request_type, HID_SET_REPORT,
                                   value, iface,
                                   (unsigned char *)report, report_len,
                                   1000);
}

static int send_magic(libusb_device_handle *dev,
                      uint16_t iface,
                      uint16_t report_id,
                      const uint8_t *report,
                      uint16_t report_len)
{
    for (int i = 0; i < 3; i++) {
        int rc = send_one(dev, iface, report_id, report, report_len);
        if (rc < 0) {
            return rc;
        }
        usleep(150000);
    }
    return 0;
}

static int send_vendor_magic(libusb_device_handle *dev)
{
    const uint8_t recipients[] = {
        LIBUSB_RECIPIENT_DEVICE,
        LIBUSB_RECIPIENT_INTERFACE,
    };
    const uint16_t values[] = {
        VENDOR_DOWNLOAD_VALUE,
        0x4453,
        0x0000,
    };
    const uint16_t indexes[] = {
        VENDOR_DOWNLOAD_INDEX,
        0x444c,
        0x0003,
        0x0000,
    };
    int last_rc = LIBUSB_ERROR_PIPE;

    for (size_t r = 0; r < sizeof(recipients) / sizeof(recipients[0]); r++) {
        const uint8_t request_type =
            LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | recipients[r];
        for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
            for (size_t i = 0; i < sizeof(indexes) / sizeof(indexes[0]); i++) {
                last_rc = libusb_control_transfer(dev, request_type,
                                                  VENDOR_DOWNLOAD_REQUEST,
                                                  values[v], indexes[i],
                                                  NULL, 0, 1000);
                if (last_rc >= 0) {
                    return last_rc;
                }
            }
        }
    }

    return last_rc;
}

static int send_vendor_bulk_magic(libusb_device_handle *dev)
{
    libusb_device *usb_dev = libusb_get_device(dev);
    uint16_t vendor_iface = 0;
    uint8_t vendor_out_ep = 0;
    int rc = find_interface(usb_dev, USB_CLASS_VENDOR_SPECIFIC,
                            &vendor_iface, &vendor_out_ep);
    if (rc != 0) {
        fprintf(stderr, "vendor interface discovery failed: %s\n",
                libusb_error_name(rc));
        return rc;
    }
    if (vendor_out_ep == 0) {
        fprintf(stderr, "vendor interface %u has no bulk OUT endpoint\n",
                vendor_iface);
        return LIBUSB_ERROR_NOT_FOUND;
    }

    libusb_set_auto_detach_kernel_driver(dev, 1);
    rc = libusb_claim_interface(dev, vendor_iface);
    if (rc != 0) {
        fprintf(stderr, "claim vendor interface %u failed: %s\n",
                vendor_iface, libusb_error_name(rc));
        return rc;
    }

    const unsigned char payload[] = VENDOR_DOWNLOAD_MAGIC "\n";
    int transferred = 0;
    rc = libusb_bulk_transfer(dev, vendor_out_ep, (unsigned char *)payload,
                              (int)sizeof(payload) - 1, &transferred, 1000);
    libusb_release_interface(dev, vendor_iface);
    if (rc != 0) {
        fprintf(stderr, "vendor bulk write failed: %s\n", libusb_error_name(rc));
        return rc;
    }
    return transferred == (int)sizeof(payload) - 1 ? 0 : LIBUSB_ERROR_IO;
}

int main(void)
{
    int rc = libusb_init(NULL);
    if (rc != 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(rc));
        return 1;
    }

    libusb_device_handle *dev = libusb_open_device_with_vid_pid(NULL,
                                                                VOX_VID,
                                                                VOX_PID);
    if (!dev) {
        fprintf(stderr, "StickS3-Mic USB device not found\n");
        libusb_exit(NULL);
        return 1;
    }
    rc = send_vendor_magic(dev);
    if (rc >= 0) {
        printf("sent USB vendor download trigger to StickS3-Mic\n");
        libusb_close(dev);
        libusb_exit(NULL);
        return 0;
    }
    fprintf(stderr, "USB vendor control trigger failed: %s\n",
            libusb_error_name(rc));

    rc = send_vendor_bulk_magic(dev);
    if (rc == 0) {
        printf("sent USB vendor bulk download trigger to StickS3-Mic\n");
        libusb_close(dev);
        libusb_exit(NULL);
        return 0;
    }

    libusb_device *usb_dev = libusb_get_device(dev);
    uint16_t hid_iface = 2;
    rc = find_hid_interface(usb_dev, &hid_iface);
    if (rc != 0) {
        fprintf(stderr, "HID interface discovery failed: %s; trying interface 2\n",
                libusb_error_name(rc));
    } else {
        fprintf(stderr, "found HID interface %u\n", hid_iface);
    }
    libusb_set_auto_detach_kernel_driver(dev, 1);
    rc = libusb_claim_interface(dev, hid_iface);
    if (rc != 0) {
        fprintf(stderr, "claim HID interface failed: %s; trying control anyway\n",
                libusb_error_name(rc));
    }

    uint8_t payload_only[1] = { HID_LED_MAGIC };
    uint8_t with_report_id[2] = { HID_REPORT_ID_KBD, HID_LED_MAGIC };
    uint8_t padded_payload[8] = { HID_LED_MAGIC };
    uint8_t padded_with_id[8] = { HID_REPORT_ID_KBD, HID_LED_MAGIC };
    int last_rc = LIBUSB_ERROR_NOT_FOUND;

    struct {
        uint16_t iface;
        uint16_t report_id;
        uint8_t *report;
        uint16_t report_len;
    } attempts[] = {
        { 0xffff, HID_REPORT_ID_KBD, payload_only, sizeof(payload_only) },
        { 0xffff, 0, with_report_id, sizeof(with_report_id) },
        { 0xffff, HID_REPORT_ID_KBD, with_report_id, sizeof(with_report_id) },
        { 0xffff, 0, payload_only, sizeof(payload_only) },
        { 0xffff, HID_REPORT_ID_KBD, padded_payload, sizeof(padded_payload) },
        { 0xffff, 0, padded_with_id, sizeof(padded_with_id) },
        { 1, HID_REPORT_ID_KBD, payload_only, sizeof(payload_only) },
        { 3, HID_REPORT_ID_KBD, payload_only, sizeof(payload_only) },
    };

    for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        uint16_t iface = attempts[i].iface == 0xffff ? hid_iface : attempts[i].iface;
        last_rc = send_magic(dev,
                             iface,
                             attempts[i].report_id,
                             attempts[i].report,
                             attempts[i].report_len);
        if (last_rc == 0) {
            printf("sent USB HID download trigger to StickS3-Mic\n");
            if (rc == 0) {
                libusb_release_interface(dev, hid_iface);
            }
            libusb_close(dev);
            libusb_exit(NULL);
            return 0;
        }
    }

    fprintf(stderr, "USB SET_REPORT failed: %s\n", libusb_error_name(last_rc));
    if (rc == 0) {
        libusb_release_interface(dev, hid_iface);
    }
    libusb_close(dev);
    libusb_exit(NULL);
    return 1;
}
