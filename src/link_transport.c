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
		.notify_handle = -1,
		.open = link_transport_open,
		.read = stratify_link_transport_usb_read,
		.write = stratify_link_transport_usb_write,
		.close = stratify_link_transport_usb_close,
		.wait = stratify_link_transport_usb_wait,
		.flush = stratify_link_transport_usb_flush,
		.timeout = 500
};

#define CONNECT_PORT "/dev/pio0"
#define CONNECT_PINMASK (1<<14)

static usb_dev_context_t m_usb_context;

link_transport_phy_t link_transport_open(const char * name, int baudrate){
	pio_attr_t attr;
	link_transport_phy_t fd;
	int fd_pio;

	//Deassert the Connect line and enable the output
	fd_pio = open(CONNECT_PORT, O_RDWR);
	if( fd_pio < 0 ){
		return -1;
	}
	attr.mask = (CONNECT_PINMASK);
	ioctl(fd_pio, I_PIO_SETMASK, (void*)attr.mask);
	attr.mode = PIO_MODE_OUTPUT | PIO_MODE_DIRONLY;
	ioctl(fd_pio, I_PIO_SETATTR, &attr);

	memset(&m_usb_context, 0, sizeof(m_usb_context));
	m_usb_context.constants = &stratify_link_transport_usb_constants;
	fd = stratify_link_transport_usb_open(name, &m_usb_context);

	ioctl(fd_pio, I_PIO_CLRMASK, (void*)attr.mask);
	close(fd_pio);

	return fd;
}
