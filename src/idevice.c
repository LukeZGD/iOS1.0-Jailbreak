//
//  idevice.c
//  ios1-jb
//
//  Created by Ethan Arbuckle on 3/6/26.
//

#include "idevice.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

const char *device_mode_string(device_mode_t mode) {
    switch (mode) {
        case DEVICE_NORMAL:
            return "Normal";
        case DEVICE_RECOVERY:
            return "Recovery";
        case DEVICE_NOT_FOUND:
            return "Not Found";
        default:
            return "Unknown";
    }
}

int idevice_open(idevice_t *dev) {
    if (libusb_init(&dev->ctx) != LIBUSB_SUCCESS) {
        return 5;
    }
    
    dev->handle = libusb_open_device_with_vid_pid(dev->ctx, APPLE_VID, RECOVERY_PID);
    if (dev->handle != NULL) {
        dev->mode = DEVICE_RECOVERY;
    }
    
    if (dev->handle == NULL) {
        dev->mode = DEVICE_NOT_FOUND;
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
        return 5;
    }
    
    if (dev->mode == DEVICE_NORMAL) {
        for (int i = 0; i < 3; i++) {
            libusb_detach_kernel_driver(dev->handle, i);
        }
    }
    
    libusb_set_auto_detach_kernel_driver(dev->handle, 1);
    
    int config = (dev->mode == DEVICE_NORMAL) ? 3 : 1;
    if (libusb_set_configuration(dev->handle, config) != LIBUSB_SUCCESS) {
        printf("Failed to set configuration\n");
        libusb_close(dev->handle);
        libusb_exit(dev->ctx);
        dev->handle = NULL;
        dev->ctx = NULL;
        return 5;
    }
    
    int interface = (dev->mode == DEVICE_NORMAL) ? 1 : 0;
    if (libusb_claim_interface(dev->handle, interface) != LIBUSB_SUCCESS) {
        printf("Failed to claim interface %d\n", interface);
        libusb_close(dev->handle);
        libusb_exit(dev->ctx);
        dev->handle = NULL;
        dev->ctx = NULL;
        return 5;
    }
    
    if (dev->mode == DEVICE_NORMAL) {
        int ret = libusb_set_interface_alt_setting(dev->handle, 1, 0);
        if (ret != LIBUSB_SUCCESS) {
            fprintf(stderr, "Failed to set alt setting: %s\n", libusb_error_name(ret));
        }
    }

    return 0;
}

void idevice_close(idevice_t *dev) {
    if (dev->handle != NULL) {
        libusb_release_interface(dev->handle, 0);
        libusb_close(dev->handle);
        dev->handle = NULL;
    }

    if (dev->ctx != NULL) {
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
    }
}

static int idevice_send_control(idevice_t *dev, iboot_msg_t *send, iboot_msg_t *recv) {
    int transferred = 0;
    int ret = libusb_interrupt_transfer(dev->handle, EP_CTRL_OUT, (unsigned char *)send, sizeof(iboot_msg_t), &transferred, USB_TIMEOUT);
    if (ret != 0) {
        fprintf(stderr, "Control OUT failed: %s\n", libusb_error_name(ret));
        return 5;
    }

    memset(recv, 0, sizeof(iboot_msg_t));
    ret = libusb_interrupt_transfer(dev->handle, EP_CTRL_IN, (unsigned char *)recv, sizeof(iboot_msg_t), &transferred, USB_TIMEOUT);
    if (ret != 0) {
        fprintf(stderr, "Control IN failed: %s\n", libusb_error_name(ret));
        return 5;
    }

    return 0;
}

int idevice_init_handshake(idevice_t *dev) {
    iboot_msg_t send = {0};
    send.cmdcode = CMD_INIT;
    send.constant = 0x1234;
    send.size = 0;
    send.unknown = 0;

    iboot_msg_t recv = {0};
    return idevice_send_control(dev, &send, &recv);
}

int idevice_send_command(idevice_t *dev, const char *cmd) {
    size_t cmdlen = strlen(cmd);
    size_t padded_len = (((cmdlen - 1) / 0x10) + 1) * 0x10;

    iboot_msg_t send = {0};
    send.cmdcode = CMD_SEND_COMMAND;
    send.constant = 0x1234;
    send.size = (int32_t)padded_len;
    send.unknown = 0;

    iboot_msg_t recv = {0};
    int ret = idevice_send_control(dev, &send, &recv);
    if (ret != 0) {
        return ret;
    }

    if (recv.cmdcode != CMD_ACK) {
        fprintf(stderr, "Command not ACKed: got 0x%04x\n", recv.cmdcode);
        return 5;
    }

    char *buf = calloc(1, padded_len);
    if (buf == NULL) {
        return 3;
    }
    memcpy(buf, cmd, cmdlen);

    int transferred = 0;
    int usb_ret = libusb_bulk_transfer(dev->handle, EP_CMD_OUT, (unsigned char *)buf, (int)padded_len, &transferred, USB_TIMEOUT);
    free(buf);

    if (usb_ret != 0) {
        fprintf(stderr, "Command bulk transfer failed: %s\n", libusb_error_name(usb_ret));
        return 5;
    }

    return 0;
}

int idevice_send_file(idevice_t *dev, void *data, size_t size, uint32_t load_addr) {
    iboot_msg_t send = {0};
    iboot_msg_t recv = {0};

    send.cmdcode = CMD_SEND_FILE;
    send.constant = 0x1234;
    send.size = (int32_t)size;
    send.unknown = (int32_t)load_addr;

    int ret = idevice_send_control(dev, &send, &recv);
    if (ret != 0) {
        return ret;
    }

    if (recv.cmdcode != CMD_ACK) {
        fprintf(stderr, "SendFile not ACKed: got 0x%04x\n", recv.cmdcode);
        return 5;
    }

    size_t sent = 0;
    size_t chunk_size = 0x4000;

    while (sent < size) {
        size_t to_send = (size - sent > chunk_size) ? chunk_size : (size - sent);
        int transferred = 0;
        int usb_ret = libusb_bulk_transfer(dev->handle, EP_FILE_OUT, (unsigned char *)data + sent, (int)to_send, &transferred, USB_TIMEOUT);
        if (usb_ret != 0) {
            fprintf(stderr, "File bulk transfer failed: %s\n", libusb_error_name(usb_ret));
            return 5;
        }

        sent += transferred;
    }

    return 0;
}

int idevice_get_response(idevice_t *dev, char *response, size_t response_size) {
    if (!response || response_size < 2) {
        return 4;
    }
    response[0] = '\0';
    size_t used = 0;

    /* iBoot 1.x produces command output asynchronously. */
    usleep(1000000);

    for (int attempt = 0; attempt < 32; attempt++) {
        iboot_msg_t send = {0};
        send.cmdcode = CMD_DUMP_BUFFER;
        send.constant = 0x1234;
        iboot_msg_t recv = {0};
        int control_result = idevice_send_control(dev, &send, &recv);
        if (control_result == 0 &&
            (recv.cmdcode == 0x0801 || recv.cmdcode == 0x0809) &&
            recv.size == 0) {
            usleep(100000);
            continue;
        }
        if (control_result != 0 ||
            (recv.cmdcode != CMD_ACK && recv.cmdcode != 0x0801)) {
            fprintf(stderr, "DumpBuffer failed: transport=0x%x response=0x%04x size=%d\n",
                control_result, (unsigned short)recv.cmdcode, recv.size);
            return 5;
        }
        if (recv.size <= 0) {
            usleep(100000);
            continue;
        }

        size_t available = response_size - used - 1;
        size_t wanted = (size_t)recv.size < available ? (size_t)recv.size : available;
        int transferred = 0;
        int ret = libusb_bulk_transfer(dev->handle, EP_CMD_IN,
            (unsigned char *)response + used, (int)wanted, &transferred, USB_TIMEOUT);
        if (ret != LIBUSB_SUCCESS) {
            fprintf(stderr, "DumpBuffer bulk read failed: %s (wanted=%zu reported=%d)\n",
                libusb_error_name(ret), wanted, recv.size);
            return 5;
        }
        used += (size_t)transferred;
        response[used] = '\0';
        if (used >= 2 && response[used - 1] == ' ' &&
            (response[used - 2] == ']' || response[used - 2] == '>')) {
            return 0;
        }
        if (used + 1 >= response_size) {
            return 3;
        }
    }
    return used ? 0 : 5;
}
