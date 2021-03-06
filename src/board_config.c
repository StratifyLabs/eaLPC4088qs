/*

Copyright 2011-2016 Tyler Gilbert

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include <stdint.h>
#include <sys/lock.h>
#include <fcntl.h>
#include <errno.h>
#include <mcu/device.h>
#include <mcu/debug.h>
#include <iface/device_config.h>
#include <dev/microchip/sst25vf.h>
#include <dev/sys.h>
#include <dev/uartfifo.h>
#include <dev/usbfifo.h>
#include <dev/fifo.h>
#include <iface/link.h>
#include <stratify/sysfs.h>
#include <stratify/sffs.h>
#include <stratify/stratify.h>
#include <dev/sys.h>
#include <iface/link.h>

#include "link_transport.h"

#define STFY_SYSTEM_CLOCK 120000000
#define STFY_SYSTEM_MEMORY_SIZE (8192*2)

static void board_event_handler(int event, void * args);

const mcu_board_config_t mcu_board_config = {
		.core_osc_freq = 12000000UL, //12MHZ
		.core_cpu_freq = STFY_SYSTEM_CLOCK,
		.core_periph_freq = STFY_SYSTEM_CLOCK,
		.usb_max_packet_zero = MCU_CORE_USB_MAX_PACKET_ZERO_VALUE,
		.usb_pin_assign = 1,
		.event = board_event_handler,
		.led.port = 2, .led.pin = 19,
		.o_flags = MCU_BOARD_CONFIG_FLAG_LED_ACTIVE_HIGH
};

#define SCHED_TASK_TOTAL 10

void board_event_handler(int event, void * args){
	switch(event){
	case MCU_BOARD_CONFIG_EVENT_PRIV_FATAL:
		//start the bootloader on a fatal event
		mcu_core_invokebootloader(0, 0);
		break;
	case MCU_BOARD_CONFIG_EVENT_START_LINK:
		mcu_debug("Start LED\n");
		stratify_led_startup();
		break;
	case MCU_BOARD_CONFIG_EVENT_START_FILESYSTEM:
		mcu_debug("Started %ld apps\n", *((u32*)args));
		break;
	}
}

const stratify_board_config_t stratify_board_config = {
		.clk_usecond_tmr = 3,
		.task_total = SCHED_TASK_TOTAL,
		.clk_usec_mult = (uint32_t)(STFY_SYSTEM_CLOCK / 1000000),
		.clk_nsec_div = (uint32_t)((uint64_t)1024 * 1000000000 / STFY_SYSTEM_CLOCK),
		.stdin_dev = "/dev/stdio-in" ,
		.stdout_dev = "/dev/stdio-out",
		.stderr_dev = "/dev/stdio-out",
		.o_sys_flags = SYS_FLAGS_STDIO_FIFO | SYS_FLAGS_NOTIFY,
		.sys_name = "EA LPC4088 QS",
		.sys_version = "0.1",
		.sys_memory_size = STFY_SYSTEM_MEMORY_SIZE,
		.start = stratify_default_thread,
		.start_args = &link_transport,
		.start_stack_size = STRATIFY_DEFAULT_START_STACK_SIZE,
		.notify_write = stratify_link_transport_usb_notify
};

volatile sched_task_t stratify_sched_table[SCHED_TASK_TOTAL] MCU_SYS_MEM;
task_t stratify_task_table[SCHED_TASK_TOTAL] MCU_SYS_MEM;

#define USER_ROOT 0
#define GROUP_ROOT 0

/* This is the state information for the sst25vf flash IC driver.
 *
 */
sst25vf_state_t sst25vf_state MCU_SYS_MEM;

/* This is the configuration specific structure for the sst25vf
 * flash IC driver.
 */
const sst25vf_cfg_t sst25vf_cfg = SST25VF_DEVICE_CFG(-1, 0, -1, 0, 0, 8, 2*1024*1024);

#define UART0_DEVFIFO_BUFFER_SIZE 128
char uart0_fifo_buffer[UART0_DEVFIFO_BUFFER_SIZE];
const uartfifo_cfg_t uart0_fifo_cfg = UARTFIFO_DEVICE_CFG(0,
		uart0_fifo_buffer,
		UART0_DEVFIFO_BUFFER_SIZE);
uartfifo_state_t uart0_fifo_state MCU_SYS_MEM;

#define UART1_DEVFIFO_BUFFER_SIZE 64
char uart1_fifo_buffer[UART1_DEVFIFO_BUFFER_SIZE];
const uartfifo_cfg_t uart1_fifo_cfg = UARTFIFO_DEVICE_CFG(1,
		uart1_fifo_buffer,
		UART1_DEVFIFO_BUFFER_SIZE);
uartfifo_state_t uart1_fifo_state MCU_SYS_MEM;

#define UART3_DEVFIFO_BUFFER_SIZE 64
char uart3_fifo_buffer[UART3_DEVFIFO_BUFFER_SIZE];
const uartfifo_cfg_t uart3_fifo_cfg = UARTFIFO_DEVICE_CFG(3,
		uart3_fifo_buffer,
		UART3_DEVFIFO_BUFFER_SIZE);
uartfifo_state_t uart3_fifo_state MCU_SYS_MEM;

#define STDIO_BUFFER_SIZE 128

char stdio_out_buffer[STDIO_BUFFER_SIZE];
char stdio_in_buffer[STDIO_BUFFER_SIZE];

void stdio_out_notify_write(int nbyte){
	link_notify_dev_t notify;
	notify.id = LINK_NOTIFY_ID_DEVICE_WRITE;
	strcpy(notify.name, "stdio-out");
	notify.nbyte = nbyte;
	if( stratify_board_config.notify_write ){
		stratify_board_config.notify_write(&notify, sizeof(link_notify_dev_t));
	}
}

fifo_cfg_t stdio_out_cfg = FIFO_DEVICE_CFG(stdio_out_buffer, STDIO_BUFFER_SIZE, 0, stdio_out_notify_write);
fifo_cfg_t stdio_in_cfg = FIFO_DEVICE_CFG(stdio_in_buffer, STDIO_BUFFER_SIZE, 0, 0);
fifo_state_t stdio_out_state = { .head = 0, .tail = 0, .rop = NULL, .rop_len = 0, .wop = NULL, .wop_len = 0 };
fifo_state_t stdio_in_state = {
		.head = 0, .tail = 0, .rop = NULL, .rop_len = 0, .wop = NULL, .wop_len = 0
};

#define MEM_DEV 0

/* This is the list of devices that will show up in the /dev folder
 * automatically.  By default, the peripheral devices for the MCU are available
 * plus the devices on the microcomputer.
 */
const device_t devices[] = {
		//mcu peripherals
		DEVICE_PERIPH("mem0", mcu_mem, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFBLK),
		DEVICE_PERIPH("core", mcu_core, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("core0", mcu_core, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("adc0", mcu_adc, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("dac0", mcu_dac, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("eint0", mcu_eint, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("eint1", mcu_eint, 1, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("eint2", mcu_eint, 2, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("eint3", mcu_eint, 3, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("pio0", mcu_pio, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("pio1", mcu_pio, 1, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("pio2", mcu_pio, 2, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("pio3", mcu_pio, 3, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("pio4", mcu_pio, 4, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("i2c0", mcu_i2c, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("i2c1", mcu_i2c, 1, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("i2c2", mcu_i2c, 2, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("pwm1", mcu_pwm, 1, 0666, USER_ROOT, GROUP_ROOT, S_IFBLK),
		DEVICE_PERIPH("qei0", mcu_qei, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("rtc", mcu_rtc, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("spi0", mcu_ssp, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("spi1", mcu_ssp, 1, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("spi2", mcu_ssp, 2, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("tmr0", mcu_tmr, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("tmr1", mcu_tmr, 1, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("tmr2", mcu_tmr, 2, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		UARTFIFO_DEVICE("uart0", &uart0_fifo_cfg, &uart0_fifo_state, 0666, USER_ROOT, GROUP_ROOT),
		//DEVICE_PERIPH("uart0", mcu_uart, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		UARTFIFO_DEVICE("uart1", &uart1_fifo_cfg, &uart1_fifo_state, 0666, USER_ROOT, GROUP_ROOT),
		//DEVICE_PERIPH("uart1", mcu_uart, 1, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		DEVICE_PERIPH("uart2", mcu_uart, 2, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),
		UARTFIFO_DEVICE("uart3", &uart3_fifo_cfg, &uart3_fifo_state, 0666, USER_ROOT, GROUP_ROOT),
		DEVICE_PERIPH("usb0", mcu_usb, 0, 0666, USER_ROOT, GROUP_ROOT, S_IFCHR),

		//user devices
		//SST25VF_SSP_DEVICE("disk0", 1, 0, 0, 6, 10000000, &sst25vf_cfg, &sst25vf_state, 0666, USER_ROOT, GROUP_ROOT),

		//FIFO buffers used for std in and std out
		FIFO_DEVICE("stdio-out", &stdio_out_cfg, &stdio_out_state, 0666, USER_ROOT, GROUP_ROOT),
		FIFO_DEVICE("stdio-in", &stdio_in_cfg, &stdio_in_state, 0666, USER_ROOT, GROUP_ROOT),

		//system devices
		USBFIFO_DEVICE("link-phy-usb", &stratify_link_transport_usb_fifo_cfg, &stratify_link_transport_usb_fifo_state, 0666, USER_ROOT, GROUP_ROOT),

		SYS_DEVICE,
		DEVICE_TERMINATOR
};


extern const sffs_cfg_t sffs_cfg;
sffs_state_t sffs_state;
open_file_t sffs_open_file; // Cannot be in HWPL_SYS_MEM because it is accessed in unpriv mode

const sffs_cfg_t sffs_cfg = {
		.open_file = &sffs_open_file,
		.devfs = &(sysfs_list[1]),
		.name = "disk0",
		.state = &sffs_state
};



/*
fatfs_state_t fatfs_state;
open_file_t fatfs_open_file; // Cannot be in HWPL_SYS_MEM because it is accessed in unpriv mode

const fatfs_cfg_t fatfs_cfg = {
		.open_file = &fatfs_open_file,
		.devfs = &(sysfs_list[1]),
		.name = "disk1",
		.state = &fatfs_state,
		.vol_id = 0
};
 */

const sysfs_t const sysfs_list[] = {
		SYSFS_APP("/app", &(devices[MEM_DEV]), SYSFS_ALL_ACCESS), //the folder for ram/flash applications
		SYSFS_DEV("/dev", devices, SYSFS_READONLY_ACCESS), //the list of devices
		//SFFS("/home", &sffs_cfg, SYSFS_ALL_ACCESS), //the stratify file system on external RAM
		//FATFS("/home", &fatfs_cfg, SYSFS_ALL_ACCESS), //fat filesystem with external SD card
		SYSFS_ROOT("/", sysfs_list, SYSFS_READONLY_ACCESS), //the root filesystem (must be last)
		SYSFS_TERMINATOR
};


