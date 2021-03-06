/*
 * link_transport.c
 *
 *  Created on: May 23, 2016
 *      Author: tgil
 */

#include <fcntl.h>
#include <unistd.h>
#include <iface/dev/pio.h>

#include "link_transport.h"

static link_transport_phy_t link_transport_open(const char * name, int baudrate);

link_transport_driver_t link_transport = {
		.handle = -1,
		.open = link_transport_open,
		.read = stratify_link_boot_transport_usb_read,
		.write = stratify_link_boot_transport_usb_write,
		.close = stratify_link_boot_transport_usb_close,
		.wait = stratify_link_boot_transport_usb_wait,
		.flush = stratify_link_boot_transport_usb_flush,
		.timeout = 500
};

#define USBDEV_CONNECT_PORT 0
#define USBDEV_CONNECT_PINMASK (1<<14)

static usb_dev_context_t m_usb_context;

link_transport_phy_t link_transport_open(const char * name, int baudrate){
	pio_attr_t attr;
	link_transport_phy_t fd;
	//Deassert the Connect line and enable the output
	mcu_pio_setmask(USBDEV_CONNECT_PORT, (void*)(USBDEV_CONNECT_PINMASK));

	attr.mask = (USBDEV_CONNECT_PINMASK);
	attr.mode = PIO_MODE_OUTPUT | PIO_MODE_DIRONLY;
	mcu_pio_setattr(USBDEV_CONNECT_PORT, &attr);

	memset(&m_usb_context, 0, sizeof(m_usb_context));
	m_usb_context.constants = &stratify_link_transport_usb_constants;

	fd = stratify_link_boot_transport_usb_open(name, &m_usb_context);

	mcu_pio_clrmask(USBDEV_CONNECT_PORT, (void*)(USBDEV_CONNECT_PINMASK));

	return fd;
}
