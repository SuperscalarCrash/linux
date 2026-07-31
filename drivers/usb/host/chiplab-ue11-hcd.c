// SPDX-License-Identifier: GPL-2.0
/*
 * HCD (Host Controller Driver) for USB.
 *
 * Heavily based on SL811HS HCD....
 *
 * Copyright (C) 2004 Psion Teklogix (for NetBook PRO)
 * Copyright (C) 2004-2005 David Brownell
 * Copyright (C) 1999 Roman Weissgaerber
 *
 * UE11 support derived from the NonTrivialMIPS driver at commit
 * 380d859dedcb9ce7fa90f58d036cc5f88d86b7e2 and updated for the
 * SuperscalarCrash Chiplab RTL ABI and current Linux HCD interfaces.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/platform_device.h>
#include <linux/prefetch.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/unaligned.h>

//-----------------------------------------------------------------
// Defines:
//-----------------------------------------------------------------
#define USB_CTRL 0x0
#define USB_CTRL_PHY_DMPULLDOWN 7
#define USB_CTRL_PHY_DMPULLDOWN_SHIFT 7
#define USB_CTRL_PHY_DMPULLDOWN_MASK 0x1

#define USB_CTRL_PHY_DPPULLDOWN 6
#define USB_CTRL_PHY_DPPULLDOWN_SHIFT 6
#define USB_CTRL_PHY_DPPULLDOWN_MASK 0x1

#define USB_CTRL_PHY_TERMSELECT 5
#define USB_CTRL_PHY_TERMSELECT_SHIFT 5
#define USB_CTRL_PHY_TERMSELECT_MASK 0x1

#define USB_CTRL_PHY_XCVRSELECT_SHIFT 3
#define USB_CTRL_PHY_XCVRSELECT_MASK 0x3

#define USB_CTRL_PHY_OPMODE_SHIFT 1
#define USB_CTRL_PHY_OPMODE_MASK 0x3

#define USB_CTRL_ENABLE_SOF 0
#define USB_CTRL_ENABLE_SOF_SHIFT 0
#define USB_CTRL_ENABLE_SOF_MASK 0x1

#define USB_STATUS 0x4
#define USB_STATUS_SOF_TIME_SHIFT 16
#define USB_STATUS_SOF_TIME_MASK 0xffff

#define USB_STATUS_RX_ERROR 2
#define USB_STATUS_RX_ERROR_SHIFT 2
#define USB_STATUS_RX_ERROR_MASK 0x1

#define USB_STATUS_LINESTATE_BITS_SHIFT 0
#define USB_STATUS_LINESTATE_BITS_MASK 0x3

#define USB_IRQ_ACK 0x8
#define USB_IRQ_ACK_DEVICE_DETECT 3
#define USB_IRQ_ACK_DEVICE_DETECT_SHIFT 3
#define USB_IRQ_ACK_DEVICE_DETECT_MASK 0x1

#define USB_IRQ_ACK_ERR 2
#define USB_IRQ_ACK_ERR_SHIFT 2
#define USB_IRQ_ACK_ERR_MASK 0x1

#define USB_IRQ_ACK_DONE 1
#define USB_IRQ_ACK_DONE_SHIFT 1
#define USB_IRQ_ACK_DONE_MASK 0x1

#define USB_IRQ_ACK_SOF 0
#define USB_IRQ_ACK_SOF_SHIFT 0
#define USB_IRQ_ACK_SOF_MASK 0x1

#define USB_IRQ_STS 0xc
#define USB_IRQ_STS_DEVICE_DETECT 3
#define USB_IRQ_STS_DEVICE_DETECT_SHIFT 3
#define USB_IRQ_STS_DEVICE_DETECT_MASK 0x1

#define USB_IRQ_STS_ERR 2
#define USB_IRQ_STS_ERR_SHIFT 2
#define USB_IRQ_STS_ERR_MASK 0x1

#define USB_IRQ_STS_DONE 1
#define USB_IRQ_STS_DONE_SHIFT 1
#define USB_IRQ_STS_DONE_MASK 0x1

#define USB_IRQ_STS_SOF 0
#define USB_IRQ_STS_SOF_SHIFT 0
#define USB_IRQ_STS_SOF_MASK 0x1

#define USB_IRQ_MASK 0x10
#define USB_IRQ_MASK_DEVICE_DETECT 3
#define USB_IRQ_MASK_DEVICE_DETECT_SHIFT 3
#define USB_IRQ_MASK_DEVICE_DETECT_MASK 0x1

#define USB_IRQ_MASK_ERR 2
#define USB_IRQ_MASK_ERR_SHIFT 2
#define USB_IRQ_MASK_ERR_MASK 0x1

#define USB_IRQ_MASK_DONE 1
#define USB_IRQ_MASK_DONE_SHIFT 1
#define USB_IRQ_MASK_DONE_MASK 0x1

#define USB_IRQ_MASK_SOF 0
#define USB_IRQ_MASK_SOF_SHIFT 0
#define USB_IRQ_MASK_SOF_MASK 0x1

#define USB_XFER_DATA 0x14
#define USB_XFER_DATA_TX_LEN_SHIFT 0
#define USB_XFER_DATA_TX_LEN_MASK 0xffff

#define USB_XFER_TOKEN 0x18
#define USB_XFER_TOKEN_START 31
#define USB_XFER_TOKEN_START_SHIFT 31
#define USB_XFER_TOKEN_START_MASK 0x1

#define USB_XFER_TOKEN_IN 30
#define USB_XFER_TOKEN_IN_SHIFT 30
#define USB_XFER_TOKEN_IN_MASK 0x1

#define USB_XFER_TOKEN_ACK 29
#define USB_XFER_TOKEN_ACK_SHIFT 29
#define USB_XFER_TOKEN_ACK_MASK 0x1

#define USB_XFER_TOKEN_PID_DATAX 28
#define USB_XFER_TOKEN_PID_DATAX_SHIFT 28
#define USB_XFER_TOKEN_PID_DATAX_MASK 0x1

#define USB_XFER_TOKEN_PID_BITS_SHIFT 16
#define USB_XFER_TOKEN_PID_BITS_MASK 0xff

#define USB_XFER_TOKEN_DEV_ADDR_SHIFT 9
#define USB_XFER_TOKEN_DEV_ADDR_MASK 0x7f

#define USB_XFER_TOKEN_EP_ADDR_SHIFT 5
#define USB_XFER_TOKEN_EP_ADDR_MASK 0xf

#define USB_RX_STAT 0x1c
#define USB_RX_STAT_START_PEND 31
#define USB_RX_STAT_START_PEND_SHIFT 31
#define USB_RX_STAT_START_PEND_MASK 0x1

#define USB_RX_STAT_CRC_ERR 30
#define USB_RX_STAT_CRC_ERR_SHIFT 30
#define USB_RX_STAT_CRC_ERR_MASK 0x1

#define USB_RX_STAT_RESP_TIMEOUT 29
#define USB_RX_STAT_RESP_TIMEOUT_SHIFT 29
#define USB_RX_STAT_RESP_TIMEOUT_MASK 0x1

#define USB_RX_STAT_IDLE 28
#define USB_RX_STAT_IDLE_SHIFT 28
#define USB_RX_STAT_IDLE_MASK 0x1

#define USB_RX_STAT_RESP_BITS_SHIFT 16
#define USB_RX_STAT_RESP_BITS_MASK 0xff

#define USB_RX_STAT_COUNT_BITS_SHIFT 0
#define USB_RX_STAT_COUNT_BITS_MASK 0xffff

#define USB_WR_DATA 0x20
#define USB_WR_DATA_DATA_SHIFT 0
#define USB_WR_DATA_DATA_MASK 0xff

#define USB_RD_DATA 0x20
#define USB_RD_DATA_DATA_SHIFT 0
#define USB_RD_DATA_DATA_MASK 0xff

#define USB_CTRL2 0x24

#define USB_CTRL2_TX_FLUSH 1
#define USB_CTRL2_TX_FLUSH_SHIFT 1

#define USB_CTRL2_PHY_RESET 0
#define USB_CTRL2_PHY_RESET_SHIFT 0

#define USB_VERSION 0x28
#define USB_VERSION_VALUE 0x55453101

#define UE11_DISCONNECT_DEBOUNCE 3

//-----------------------------------------------------------------

#define LOG2_PERIODIC_SIZE 5 /* arbitrary; this matches OHCI */
#define PERIODIC_SIZE (1 << LOG2_PERIODIC_SIZE)

struct ue11 {
	spinlock_t lock;
	void __iomem *reg_base;

	unsigned long stat_insrmv;
	unsigned long stat_wake;
	unsigned long stat_sof;
	unsigned long stat_a;
	unsigned long stat_lost;
	unsigned long stat_overrun;

	/* sw model */
	struct timer_list timer;
	struct ue11h_ep *next_periodic;
	struct ue11h_ep *next_async;

	struct ue11h_ep *active_transfer;
	unsigned long active_start;

	u32 port1;
	u32 irq_enable;
	u16 frame;
	u8 disconnect_samples;
	bool reset_recovery;

	/* async schedule: control, bulk */
	struct list_head async;

	/* periodic schedule: interrupt, iso */
	u16 load[PERIODIC_SIZE];
	struct ue11h_ep *periodic[PERIODIC_SIZE];
	unsigned int periodic_count;
};

static inline struct ue11 *hcd_to_ue11(struct usb_hcd *hcd)
{
	return (struct ue11 *)(hcd->hcd_priv);
}

static inline struct usb_hcd *ue11_to_hcd(struct ue11 *ue11)
{
	return container_of((void *)ue11, struct usb_hcd, hcd_priv);
}

struct ue11h_ep {
	struct usb_host_endpoint *hep;
	struct usb_device *udev;

	u8 maxpacket;
	u8 epnum;
	u8 nextpid;

	u16 error_count;
	u16 nak_count;
	u16 length; /* of current packet */

	/* periodic schedule */
	u16 period;
	u16 branch;
	u16 load;
	struct ue11h_ep *next;

	/* async schedule */
	struct list_head schedule;
};
//-----------------------------------------------------------------

#define DRIVER_VERSION "31 Jul 2026"

static const char hcd_name[] = "chiplab-ue11-hcd";

//-----------------------------------------------------------------
// dbg_get_ctrl_req_str:
//-----------------------------------------------------------------
static const char *dbg_get_ctrl_req_str(uint8_t request)
{
	switch (request) {
	case USB_REQ_GET_STATUS:
		return "GET_STATUS";
	case USB_REQ_CLEAR_FEATURE:
		return "CLEAR_FEATURE";
	case USB_REQ_SET_FEATURE:
		return "SET_FEATURE";
	case USB_REQ_SET_ADDRESS:
		return "SET_ADDRESS";
	case USB_REQ_GET_DESCRIPTOR:
		return "GET_DESCRIPTOR";
	case USB_REQ_SET_DESCRIPTOR:
		return "SET_DESCRIPTOR";
	case USB_REQ_GET_CONFIGURATION:
		return "GET_CONFIGURATION";
	case USB_REQ_SET_CONFIGURATION:
		return "SET_CONFIGURATION";
	case USB_REQ_GET_INTERFACE:
		return "GET_INTERFACE";
	case USB_REQ_SET_INTERFACE:
		return "SET_INTERFACE";
	default:
		return "UNKNOWN";
	}
}
//-----------------------------------------------------------------
// dbg_get_ctrl_req_type_str:
//-----------------------------------------------------------------
static const char *dbg_get_ctrl_req_type_str(uint8_t requestType)
{
	if (requestType & USB_DIR_IN) {
		switch (requestType & USB_RECIP_MASK) {
		case USB_RECIP_DEVICE:
			return "IN_DEVICE";
		case USB_RECIP_INTERFACE:
			return "IN_INTERFACE";
		case USB_RECIP_ENDPOINT:
			return "IN_ENDPOINT";
		default:
			return "UNKNOWN";
		}
	} else {
		switch (requestType & USB_RECIP_MASK) {
		case USB_RECIP_DEVICE:
			return "OUT_DEVICE";
		case USB_RECIP_INTERFACE:
			return "OUT_INTERFACE";
		case USB_RECIP_ENDPOINT:
			return "OUT_ENDPOINT";
		default:
			return "UNKNOWN";
		}
	}
}
//-----------------------------------------------------------------
// dbg_decode_packet:
//-----------------------------------------------------------------
static void dbg_decode_setup_packet(uint8_t *p)
{
	struct usb_ctrlrequest *ctrl = (struct usb_ctrlrequest *)p;

	pr_debug("Debug: SETUP PACKET\n");
	pr_debug("       bRequestType 0x%x (%s)\n", ctrl->bRequestType,
		 dbg_get_ctrl_req_type_str(ctrl->bRequestType));
	pr_debug("       bRequest 0x%x (%s)\n", ctrl->bRequest,
		 dbg_get_ctrl_req_str(ctrl->bRequest));
	pr_debug("       wValue 0x%x, wIndex 0x%x, wLength %d\n", ctrl->wValue,
		 ctrl->wIndex, ctrl->wLength);
}

//-----------------------------------------------------------------
// usbhw_phy_reset: Reset USB3500 and flush UE11's transmit FIFO.
//-----------------------------------------------------------------
static void usbhw_phy_reset(struct ue11 *ue11)
{
	writel(0, ue11->reg_base + USB_IRQ_MASK);
	writel(0, ue11->reg_base + USB_CTRL);
	writel(BIT(USB_CTRL2_PHY_RESET_SHIFT), ue11->reg_base + USB_CTRL2);
	usleep_range(5000, 6000);
	writel(BIT(USB_CTRL2_TX_FLUSH_SHIFT), ue11->reg_base + USB_CTRL2);
	usleep_range(5000, 6000);
}

//-----------------------------------------------------------------
// usbhw_bus_reset_assert: Put the downstream bus into SE0.  The reset
// duration is handled asynchronously by ue11h_timer(), since hub_control()
// runs with the HCD spinlock held and must not sleep.
//-----------------------------------------------------------------
static void usbhw_bus_reset_assert(struct ue11 *ue11)
{
	u32 val = BIT(USB_CTRL_PHY_XCVRSELECT_SHIFT) |
		  BIT(USB_CTRL_PHY_DPPULLDOWN_SHIFT) |
		  BIT(USB_CTRL_PHY_DMPULLDOWN_SHIFT);

	writel(val, ue11->reg_base + USB_CTRL);
}
//-----------------------------------------------------------------
// usbhw_hub_enable: Enable root hub (drive data lines to HiZ)
//                   and optionally start SOF periods
//-----------------------------------------------------------------
static void usbhw_hub_enable(struct ue11 *ue11, bool enable_sof)
{
	u32 val;

	// Host Full Speed
	val = 0;
	val |= (1 << USB_CTRL_PHY_XCVRSELECT_SHIFT);
	val |= (1 << USB_CTRL_PHY_TERMSELECT_SHIFT);
	val |= (0 << USB_CTRL_PHY_OPMODE_SHIFT);
	val |= (1 << USB_CTRL_PHY_DPPULLDOWN_SHIFT);
	val |= (1 << USB_CTRL_PHY_DMPULLDOWN_SHIFT);

	// Enable SOF
	if (enable_sof)
		val |= (1 << USB_CTRL_ENABLE_SOF_SHIFT);

	writel(val, ue11->reg_base + USB_CTRL);
}
//-----------------------------------------------------------------
// port_power: Control USB port power enable
//-----------------------------------------------------------------
static void port_power(struct ue11 *ue11, int is_on)
{
	/* hub is inactive unless the port is powered */
	if (is_on) {
		if (ue11->port1 & USB_PORT_STAT_POWER)
			return;

		ue11->port1 = USB_PORT_STAT_POWER;
		ue11->irq_enable = 0;
	} else {
		ue11->port1 = 0;
		ue11->irq_enable = 0;
	}

	if (is_on)
		usbhw_hub_enable(ue11, false);
	else
		writel(0, ue11->reg_base + USB_CTRL);

	writel(ue11->irq_enable, ue11->reg_base + USB_IRQ_MASK);
}
//-----------------------------------------------------------------
// setup_packet:
// This is a PIO-only HCD.  Queueing appends URBs to the endpoint's queue,
// and may start I/O.  Endpoint queues are scanned during completion irq
// handlers (one per packet: ACK, NAK, faults, etc) and urb cancellation.
// SETUP starts a new control request.  Devices are not allowed to
// STALL or NAK these; they must cancel any pending control requests.
//-----------------------------------------------------------------
static void setup_packet(struct ue11 *ue11, struct ue11h_ep *ep,
			 struct urb *urb)
{
	int l;
	uint32_t ctrl = 0;
	uint32_t token = 0;
	uint32_t device_addr = usb_pipedevice(urb->pipe);
	uint32_t endpoint = usb_pipeendpoint(urb->pipe);

	int len = sizeof(struct usb_ctrlrequest);

	pr_debug("USB: Send SETUP_PACKET");

	pr_debug("TOKEN: SETUP");
	pr_debug("  DEV %d EP %d", device_addr, endpoint);

	// Load DATA0 transfer into address 0+
	pr_debug(" Tx: %02x", USB_PID_DATA0);
	for (l = 0; l < len; l++)
		writel(urb->setup_packet[l], ue11->reg_base + USB_WR_DATA);

	dbg_decode_setup_packet(urb->setup_packet);

	// Transfer data length
	writel(len, ue11->reg_base + USB_XFER_DATA);

	// Configure transfer for DATAx portion
	ctrl = (1 << USB_XFER_TOKEN_START_SHIFT);
	ctrl |= (0 << USB_XFER_TOKEN_IN_SHIFT); // Host -> Device
	ctrl |= (1 << USB_XFER_TOKEN_ACK_SHIFT); // Expect ACK

	// Always DATA0
	ctrl |= (0 << USB_XFER_TOKEN_PID_DATAX_SHIFT);

	// Setup token details (don't start transfer yet)
	token = (((unsigned int)USB_PID_SETUP)
		 << USB_XFER_TOKEN_PID_BITS_SHIFT) |
		(device_addr << USB_XFER_TOKEN_DEV_ADDR_SHIFT) |
		(endpoint << USB_XFER_TOKEN_EP_ADDR_SHIFT);
	writel(token | ctrl, ue11->reg_base + USB_XFER_TOKEN);

	ep->length = 0;
}
//-----------------------------------------------------------------
// status_packet: STATUS finishes control requests, often after
// IN or OUT data packets
//-----------------------------------------------------------------
static void status_packet(struct ue11 *ue11, struct ue11h_ep *ep,
			  struct urb *urb)
{
	int do_out = urb->transfer_buffer_length && usb_pipein(urb->pipe);

	if (do_out) {
		uint32_t ctrl = 0;
		uint32_t token = 0;
		uint32_t device_addr = usb_pipedevice(urb->pipe);
		uint32_t endpoint = usb_pipeendpoint(urb->pipe);

		pr_debug("USB: Send STATUS (OUT)\n");

		pr_debug("TOKEN: OUT (STATUS)");
		pr_debug("  DEV %d EP %d\n", device_addr, endpoint);

		// Transfer data length (zero length packet - just PID)
		writel(0, ue11->reg_base + USB_XFER_DATA);

		// Configure transfer for DATAx portion
		ctrl = (1 << USB_XFER_TOKEN_START_SHIFT);
		ctrl |= (0 << USB_XFER_TOKEN_IN_SHIFT); // Host -> Device
		ctrl |= (1 << USB_XFER_TOKEN_ACK_SHIFT); // Expect ACK

		// Always DATA1
		ctrl |= (1 << USB_XFER_TOKEN_PID_DATAX_SHIFT);

		// Setup token details (don't start transfer yet)
		token = (((uint32_t)USB_PID_OUT)
			 << USB_XFER_TOKEN_PID_BITS_SHIFT) |
			(device_addr << USB_XFER_TOKEN_DEV_ADDR_SHIFT) |
			(endpoint << USB_XFER_TOKEN_EP_ADDR_SHIFT);
		writel(token | ctrl, ue11->reg_base + USB_XFER_TOKEN);

		ep->length = 0;
	} else {
		uint32_t ctrl = 0;
		uint32_t token = 0;
		uint32_t device_addr = usb_pipedevice(urb->pipe);
		uint32_t endpoint = usb_pipeendpoint(urb->pipe);

		pr_debug("USB: Send STATUS (IN)\n");

		pr_debug("TOKEN: IN (STATUS)");
		pr_debug("  DEV %d EP %d\n", device_addr, endpoint);

		// No data to send
		writel(0, ue11->reg_base + USB_XFER_DATA);

		// Configure transfer for DATAx portion
		ctrl = (1 << USB_XFER_TOKEN_START_SHIFT);
		ctrl |= (1 << USB_XFER_TOKEN_IN_SHIFT); // Device -> Host
		ctrl |= (1 << USB_XFER_TOKEN_ACK_SHIFT); // Respond with ACK

		// Always DATA1
		ctrl |= (1 << USB_XFER_TOKEN_PID_DATAX_SHIFT);

		token = (((uint32_t)USB_PID_IN)
			 << USB_XFER_TOKEN_PID_BITS_SHIFT) |
			(device_addr << USB_XFER_TOKEN_DEV_ADDR_SHIFT) |
			(endpoint << USB_XFER_TOKEN_EP_ADDR_SHIFT);
		writel(token | ctrl, ue11->reg_base + USB_XFER_TOKEN);

		ep->length = 0;
	}
}
//-----------------------------------------------------------------
// in_packet: IN packets can be used with any type of endpoint.
//-----------------------------------------------------------------
static void in_packet(struct ue11 *ue11, struct ue11h_ep *ep, struct urb *urb)
{
	uint32_t ctrl = 0;
	uint32_t token = 0;
	uint32_t device_addr = usb_pipedevice(urb->pipe);
	uint32_t endpoint = usb_pipeendpoint(urb->pipe);

	pr_debug("USB: IN Request EP %x (%d/%d)\n", endpoint,
		 urb->actual_length, urb->transfer_buffer_length);

	pr_debug("TOKEN: IN");
	pr_debug("  DEV %d EP %d\n", device_addr, endpoint);

	// No data to send
	writel(0, ue11->reg_base + USB_XFER_DATA);

	// Configure transfer for DATAx portion
	ctrl = (1 << USB_XFER_TOKEN_START_SHIFT);
	ctrl |= (1 << USB_XFER_TOKEN_IN_SHIFT); // Device -> Host
	ctrl |= (1 << USB_XFER_TOKEN_ACK_SHIFT); // Respond with ACK

	// DataX?
	//ctrl|=  usb_gettoggle(urb->dev, usb_pipeendpoint(urb->pipe), usb_pipeout(urb->pipe)) ? (1 << USB_XFER_TOKEN_PID_DATAX_SHIFT) : (0 << USB_XFER_TOKEN_PID_DATAX_SHIFT);

	token = (((uint32_t)USB_PID_IN) << USB_XFER_TOKEN_PID_BITS_SHIFT) |
		(device_addr << USB_XFER_TOKEN_DEV_ADDR_SHIFT) |
		(endpoint << USB_XFER_TOKEN_EP_ADDR_SHIFT);
	writel(token | ctrl, ue11->reg_base + USB_XFER_TOKEN);

	// TODO: This isn't known yet!
	ep->length = min_t(u32, ep->maxpacket,
			   urb->transfer_buffer_length - urb->actual_length);
}
//-----------------------------------------------------------------
// out_packet: OUT packets can be used with any type of endpoint.
//-----------------------------------------------------------------
static void out_packet(struct ue11 *ue11, struct ue11h_ep *ep, struct urb *urb)
{
	int l;
	uint32_t ctrl = 0;
	uint32_t token = 0;
	uint32_t request = 0;
	uint32_t device_addr = usb_pipedevice(urb->pipe);
	uint32_t endpoint = usb_pipeendpoint(urb->pipe);
	uint8_t *buf;
	int len;

	pr_debug("USB: Send OUT_PACKET\n");

	buf = (uint8_t *)urb->transfer_buffer + urb->actual_length;
	prefetch(buf);

	// Limit transmit length to max packet size
	len = min_t(u32, ep->maxpacket,
		    urb->transfer_buffer_length - urb->actual_length);

	pr_debug("TOKEN: OUT");
	pr_debug("  DEV %d EP %d\n", device_addr, endpoint);

	request = usb_gettoggle(urb->dev, usb_pipeendpoint(urb->pipe),
				usb_pipeout(urb->pipe)) ?
			  USB_PID_DATA1 :
			  USB_PID_DATA0;

	pr_debug("USB: OUT EP %x, LEN %d, PID=%x\n", endpoint, len, request);

	// Load DATAx transfer into address 0+
	pr_debug(" Tx: %02x", request);
	for (l = 0; l < len; l++)
		writel(buf[l], ue11->reg_base + USB_WR_DATA);

	// Transfer data length
	writel(len, ue11->reg_base + USB_XFER_DATA);

	// Configure transfer for DATAx portion
	ctrl = (1 << USB_XFER_TOKEN_START_SHIFT);
	ctrl |= (0 << USB_XFER_TOKEN_IN_SHIFT); // Host -> Device
	ctrl |= (1 << USB_XFER_TOKEN_ACK_SHIFT); // Expect ACK

	// Select DATAx
	ctrl |= ((request == USB_PID_DATA0) ?
			 (0 << USB_XFER_TOKEN_PID_DATAX_SHIFT) :
			 (1 << USB_XFER_TOKEN_PID_DATAX_SHIFT));

	// Setup token details (don't start transfer yet)
	token = (((unsigned int)USB_PID_OUT) << USB_XFER_TOKEN_PID_BITS_SHIFT) |
		(device_addr << USB_XFER_TOKEN_DEV_ADDR_SHIFT) |
		(endpoint << USB_XFER_TOKEN_EP_ADDR_SHIFT);
	writel(token | ctrl, ue11->reg_base + USB_XFER_TOKEN);

	ep->length = len;
}
//-----------------------------------------------------------------
// enable_sof_interrupt:
//-----------------------------------------------------------------
static inline void enable_sof_interrupt(struct ue11 *ue11)
{
	if (ue11->irq_enable & (1 << USB_IRQ_MASK_SOF_SHIFT))
		return;
	dev_dbg(ue11_to_hcd(ue11)->self.controller, "enable SOF IRQ\n");
	ue11->irq_enable |= (1 << USB_IRQ_MASK_SOF_SHIFT);
}
//-----------------------------------------------------------------
// disable_sof_interrupt:
//-----------------------------------------------------------------
static inline void disable_sof_interrupt(struct ue11 *ue11)
{
	if (!(ue11->irq_enable & (1 << USB_IRQ_MASK_SOF_SHIFT)))
		return;
	dev_dbg(ue11_to_hcd(ue11)->self.controller, "disable SOF IRQ\n");
	ue11->irq_enable &= ~(1 << USB_IRQ_MASK_SOF_SHIFT);
}
//-----------------------------------------------------------------
// start_transfer: Pick the next endpoint for a transaction, and issue it.
// frames start with periodic transfers (after whatever is pending
// from the previous frame), and the rest of the time is async
// transfers, scheduled round-robin.
//-----------------------------------------------------------------
static void start_transfer(struct ue11 *ue11)
{
	struct ue11h_ep *ep;
	struct urb *urb;

	// Make sure hub port is active
	if (ue11->port1 & USB_PORT_STAT_SUSPEND)
		return;

	// Only do something if no transfer in-progress
	if (ue11->active_transfer != NULL)
		return;

	/* use endpoint at schedule head */
	if (ue11->next_periodic) {
		ep = ue11->next_periodic;
		ue11->next_periodic = ep->next;
	} else {
		if (ue11->next_async)
			ep = ue11->next_async;
		else if (!list_empty(&ue11->async))
			ep = container_of(ue11->async.next, struct ue11h_ep,
					  schedule);
		else {
			/* could set up the first fullspeed periodic
			 * transfer for the next frame ...
			 */
			return;
		}

		if (ep->schedule.next == &ue11->async)
			ue11->next_async = NULL;
		else
			ue11->next_async = container_of(
				ep->schedule.next, struct ue11h_ep, schedule);
	}

	if (unlikely(list_empty(&ep->hep->urb_list))) {
		dev_dbg(ue11_to_hcd(ue11)->self.controller, "empty %p queue?\n",
			ep);
		return;
	}

	urb = container_of(ep->hep->urb_list.next, struct urb, urb_list);

	switch (ep->nextpid) {
	case USB_PID_IN:
		in_packet(ue11, ep, urb);
		break;
	case USB_PID_OUT:
		out_packet(ue11, ep, urb);
		break;
	case USB_PID_SETUP:
		setup_packet(ue11, ep, urb);
		break;
	case USB_PID_ACK: /* for control status */
		status_packet(ue11, ep, urb);
		break;
	default:
		dev_dbg(ue11_to_hcd(ue11)->self.controller,
			"bad ep%p pid %02x\n", ep, ep->nextpid);
		ep = NULL;
	}

#define MIN_JIFFIES ((msecs_to_jiffies(2) > 1) ? msecs_to_jiffies(2) : 2)

	// Record new active transfer details
	ue11->active_transfer = ep;
	ue11->active_start = (jiffies + MIN_JIFFIES);
}
//-----------------------------------------------------------------
// finish_request
//-----------------------------------------------------------------
static void finish_request(struct ue11 *ue11, struct ue11h_ep *ep,
			   struct urb *urb, int status) __releases(ue11->lock)
	__acquires(ue11->lock)
{
	unsigned int i;

	dev_dbg(ue11_to_hcd(ue11)->self.controller, "USB: URB finish %p", urb);

	if (usb_pipecontrol(urb->pipe))
		ep->nextpid = USB_PID_SETUP;

	usb_hcd_unlink_urb_from_ep(ue11_to_hcd(ue11), urb);
	spin_unlock(&ue11->lock);
	usb_hcd_giveback_urb(ue11_to_hcd(ue11), urb, status);
	spin_lock(&ue11->lock);

	/* leave active endpoints in the schedule */
	if (!list_empty(&ep->hep->urb_list))
		return;

	/* async deschedule? */
	if (!list_empty(&ep->schedule)) {
		list_del_init(&ep->schedule);
		if (ep == ue11->next_async)
			ue11->next_async = NULL;
		return;
	}

	/* periodic deschedule */
	dev_dbg(ue11_to_hcd(ue11)->self.controller,
		"deschedule qh%d/%p branch %d\n", ep->period, ep, ep->branch);
	for (i = ep->branch; i < PERIODIC_SIZE; i += ep->period) {
		struct ue11h_ep *temp;
		struct ue11h_ep **prev = &ue11->periodic[i];

		while (*prev && ((temp = *prev) != ep))
			prev = &temp->next;
		if (*prev)
			*prev = ep->next;
		ue11->load[i] -= ep->load;
	}
	ep->branch = PERIODIC_SIZE;
	ue11->periodic_count--;
	ue11_to_hcd(ue11)->self.bandwidth_allocated -= ep->load / ep->period;
	if (ep == ue11->next_periodic)
		ue11->next_periodic = ep->next;

	/* we might turn SOFs back on again for the async schedule */
	if (ue11->periodic_count == 0)
		disable_sof_interrupt(ue11);
}
//-----------------------------------------------------------------
// process_transfer_result: Called on transfer complete / error
//-----------------------------------------------------------------
static void process_transfer_result(struct ue11 *ue11, struct ue11h_ep *ep)
{
	uint32_t status;
	struct urb *urb;
	int urbstat = -EINPROGRESS;
	uint8_t response = 0;
	int l;

	if (unlikely(!ep))
		return;

	status = readl(ue11->reg_base + USB_RX_STAT);
	response = ((status >> USB_RX_STAT_RESP_BITS_SHIFT) &
		    USB_RX_STAT_RESP_BITS_MASK);

	pr_debug("  STAT: %08x\n", status);
	pr_debug("  RESP: %08x\n", response);

	// Request still pending
	if (status & (1 << USB_RX_STAT_START_PEND_SHIFT)) {
		pr_info("USB: request still pending");
		return;
	}

	// CRC error
	if (status & (1 << USB_RX_STAT_CRC_ERR_SHIFT)) {
		// Response PID field will be zero!
		dev_dbg(ue11_to_hcd(ue11)->self.controller,
			"CRC error (last pid=%x)\n", ep->nextpid);
		response = 0;
	}

	// Timeout error
	if (status & (1 << USB_RX_STAT_RESP_TIMEOUT_SHIFT)) {
		// Response PID field will be zero!
		dev_dbg(ue11_to_hcd(ue11)->self.controller,
			"response timeout (last pid=%x)\n", ep->nextpid);
		response = 0;
	}

	urb = container_of(ep->hep->urb_list.next, struct urb, urb_list);

	// IN request sent and response received
	if (((ep->nextpid == USB_PID_IN) || (ep->nextpid == USB_PID_ACK)) &&
	    (response == USB_PID_DATA0 || response == USB_PID_DATA1)) {
		// TODO: Check DATAx is correct

		// Convert to ACK if all is well...
		response = USB_PID_ACK;
	}

	/* we can safely ignore NAKs */
	if (response == USB_PID_NAK) {
		pr_debug("USB: NAK %d\n", ep->nak_count);
		if (!ep->period)
			ep->nak_count++;
		ep->error_count = 0;
	}
	/* ACK advances transfer, toggle, and maybe queue */
	else if (response == USB_PID_ACK) {
		struct usb_device *udev = urb->dev;
		int len;
		unsigned char *buf;

		/* urb->iso_frame_desc is currently ignored here... */

		ep->nak_count = ep->error_count = 0;
		switch (ep->nextpid) {
		case USB_PID_OUT:
			pr_debug("USB: PID_OUT ACK");
			urb->actual_length += ep->length;
			usb_dotoggle(udev, ep->epnum, 1);
			if (urb->actual_length == urb->transfer_buffer_length) {
				if (usb_pipecontrol(urb->pipe))
					ep->nextpid = USB_PID_ACK;

				/*
				 * Some bulk protocols terminate OUT transfers by a
				 * short packet, using ZLPs rather than padding.
				 */
				else if (ep->length < ep->maxpacket ||
					 !(urb->transfer_flags &
					   URB_ZERO_PACKET)) {
					pr_debug("USB: OUT EP %x Complete",
						 ep->epnum);
					urbstat = 0;
				}
			}
			break;
		case USB_PID_IN: {
			unsigned int remaining;
			unsigned int copy_len;

			pr_debug("USB: PID_IN ACK\n");
			buf = urb->transfer_buffer + urb->actual_length;
			prefetchw(buf);

			len = ((status >> USB_RX_STAT_COUNT_BITS_SHIFT) &
			       USB_RX_STAT_COUNT_BITS_MASK);
			pr_debug("USB: Received length %d, requested %d",
				 urb->actual_length + len, ep->length);

			remaining = urb->transfer_buffer_length -
				    urb->actual_length;
			copy_len = min_t(unsigned int, len, remaining);
			if (len > remaining)
				urbstat = -EOVERFLOW;

			for (l = 0; l < len; l++) {
				u8 byte = readl(ue11->reg_base + USB_RD_DATA);

				if (l < copy_len)
					buf[l] = byte;
			}
			urb->actual_length += copy_len;

			usb_dotoggle(udev, ep->epnum, 0);
			if (urbstat == -EINPROGRESS &&
			    (len < ep->maxpacket ||
			     urb->actual_length ==
				     urb->transfer_buffer_length)) {
				if (usb_pipecontrol(urb->pipe))
					ep->nextpid = USB_PID_ACK;
				else {
					dev_dbg(ue11_to_hcd(ue11)
							->self.controller,
						"USB: IN EP %x Complete",
						ep->epnum);
					urbstat = 0;
				}
			}
			break;
		}
		case USB_PID_SETUP:
			pr_debug("USB: PID_SETUP ACK");
			if (urb->transfer_buffer_length == urb->actual_length)
				ep->nextpid = USB_PID_ACK;
			else if (usb_pipeout(urb->pipe)) {
				usb_settoggle(udev, 0, 1, 1);
				ep->nextpid = USB_PID_OUT;
			} else {
				usb_settoggle(udev, 0, 0, 1);
				ep->nextpid = USB_PID_IN;
			}
			break;
		case USB_PID_ACK:
			pr_debug("USB: SETUP PACKET Complete");
			urbstat = 0;
			break;
		}
	}
	/* STALL stops all transfers */
	else if (response == USB_PID_STALL) {
		pr_debug("USB: STALL (sts=%x)!", status);
		ep->nak_count = ep->error_count = 0;
		urbstat = -EPIPE;
	}
	/* error? retry, until "3 strikes" */
	else if (++ep->error_count >= 3) {
		pr_err("USB: Timeout %d (sts=%x)!", ep->error_count, status);
		if (status & (1 << USB_RX_STAT_RESP_TIMEOUT_SHIFT))
			urbstat = -ETIME;
		//else if (status & SL11H_STATMASK_OVF)
		//  urbstat = -EOVERFLOW;
		else
			urbstat = -EPROTO;
		ep->error_count = 0;
	} else {
		pr_err("USB: Timeout %d (sts=%x)!\n", ep->error_count, status);
	}

	if (urb->unlinked)
		urbstat = urb->unlinked;
	if (urbstat != -EINPROGRESS)
		finish_request(ue11, ep, urb, urbstat);
}
//-----------------------------------------------------------------
// ue11h_irq: IRQ handler
//-----------------------------------------------------------------
static irqreturn_t ue11h_irq(struct usb_hcd *hcd)
{
	struct ue11 *ue11 = hcd_to_ue11(hcd);
	uint32_t irqstat;
	irqreturn_t ret = IRQ_NONE;
	unsigned int retries = 5;

	spin_lock(&ue11->lock);

retry:
	irqstat = readl(ue11->reg_base + USB_IRQ_STS);
	// Ack interrupt
	if (irqstat) {
		writel(irqstat, ue11->reg_base + USB_IRQ_ACK);
		irqstat &= ue11->irq_enable;
	}

	// IRQ: Packet transfer complete or error detected
	if (irqstat &
	    ((1 << USB_IRQ_STS_DONE_SHIFT) | (1 << USB_IRQ_STS_ERR_SHIFT))) {
		process_transfer_result(ue11, ue11->active_transfer);
		ue11->active_transfer = NULL;
		ue11->stat_a++;
	}

	// IRQ: Start of frame interrupt
	if (irqstat & (1 << USB_IRQ_STS_SOF_SHIFT)) {
		unsigned int index;

		index = ue11->frame++ & (PERIODIC_SIZE - 1);
		ue11->stat_sof++;

		/* be graceful about almost-inevitable periodic schedule
		 * overruns:  continue the previous frame's transfers iff
		 * this one has nothing scheduled.
		 */
		if (ue11->next_periodic)
			ue11->stat_overrun++;

		if (ue11->periodic[index])
			ue11->next_periodic = ue11->periodic[index];
	}

	if (irqstat) {
		if (ue11->port1 & USB_PORT_STAT_ENABLE)
			start_transfer(ue11);
		ret = IRQ_HANDLED;
		if (retries--)
			goto retry;
	}

	if (ue11->periodic_count == 0 && list_empty(&ue11->async))
		disable_sof_interrupt(ue11);
	writel(ue11->irq_enable, ue11->reg_base + USB_IRQ_MASK);

	spin_unlock(&ue11->lock);

	return ret;
}
//-----------------------------------------------------------------
// balance_load:
// usb 1.1 says max 90% of a frame is available for periodic transfers.
// this driver doesn't promise that much since it's got to handle an
// IRQ per packet; irq handling latencies also use up that time.
//
// NOTE:  the periodic schedule is a sparse tree, with the load for
// each branch minimized.  see fig 3.5 in the OHCI spec for example.
//-----------------------------------------------------------------
static int balance(struct ue11 *ue11, u16 period, u16 load)
{
#define MAX_PERIODIC_LOAD 500 /* out of 1000 usec */
	int i, branch = -ENOSPC;

	/* search for the least loaded schedule branch of that period
	 * which has enough bandwidth left unreserved.
	 */
	for (i = 0; i < period; i++) {
		if (branch < 0 || ue11->load[branch] > ue11->load[i]) {
			int j;

			for (j = i; j < PERIODIC_SIZE; j += period) {
				if ((ue11->load[j] + load) > MAX_PERIODIC_LOAD)
					break;
			}
			if (j < PERIODIC_SIZE)
				continue;
			branch = i;
		}
	}
	return branch;
}
//-----------------------------------------------------------------
// ue11h_urb_enqueue
//-----------------------------------------------------------------
static int ue11h_urb_enqueue(struct usb_hcd *hcd, struct urb *urb,
			     gfp_t mem_flags)
{
	struct ue11 *ue11 = hcd_to_ue11(hcd);
	struct usb_device *udev = urb->dev;
	unsigned int pipe = urb->pipe;
	int is_out = !usb_pipein(pipe);
	int type = usb_pipetype(pipe);
	int epnum = usb_pipeendpoint(pipe);
	struct ue11h_ep *ep = NULL;
	unsigned long flags;
	int i;
	int retval;
	struct usb_host_endpoint *hep = urb->ep;

	dev_dbg(hcd->self.controller, "USB: URB queue %p\n", urb);

	// NOTE: ISO transfer not supported
	if (type == PIPE_ISOCHRONOUS) {
		dev_err(hcd->self.controller,
			"USB: Isochronous transfers not supported");
		return -ENOSPC;
	}

	// NOTE: Low speed devices are not supported!
	if (udev->speed == USB_SPEED_LOW) {
		dev_err(hcd->self.controller,
			"USB: Low speed devices not supported");
		return -ENOSPC;
	}

	/* avoid all allocations within spinlocks */
	if (!hep->hcpriv) {
		ep = kzalloc_obj(*ep, mem_flags);
		if (ep == NULL)
			return -ENOMEM;
	}

	spin_lock_irqsave(&ue11->lock, flags);

	/* don't submit to a dead or disabled port */
	if (!(ue11->port1 & USB_PORT_STAT_ENABLE) ||
	    !HC_IS_RUNNING(hcd->state)) {
		retval = -ENODEV;
		kfree(ep);
		goto fail_not_linked;
	}
	// Link URB to host controller
	retval = usb_hcd_link_urb_to_ep(hcd, urb);
	if (retval) {
		kfree(ep);
		goto fail_not_linked;
	}

	if (hep->hcpriv) {
		kfree(ep);
		ep = hep->hcpriv;
	} else if (!ep) {
		retval = -ENOMEM;
		goto fail;
	} else {
		INIT_LIST_HEAD(&ep->schedule);
		ep->udev = udev;
		ep->epnum = epnum;
		ep->maxpacket = usb_maxpacket(udev, urb->pipe);
		usb_settoggle(udev, epnum, is_out, 0);

		if (type == PIPE_CONTROL)
			ep->nextpid = USB_PID_SETUP;
		else if (is_out)
			ep->nextpid = USB_PID_OUT;
		else
			ep->nextpid = USB_PID_IN;

		switch (type) {
		case PIPE_ISOCHRONOUS:
		case PIPE_INTERRUPT:
			if (urb->interval > PERIODIC_SIZE)
				urb->interval = PERIODIC_SIZE;
			ep->period = urb->interval;
			ep->branch = PERIODIC_SIZE;
			ep->load =
				usb_calc_bus_time(udev->speed, !is_out,
						  (type == PIPE_ISOCHRONOUS),
						  usb_maxpacket(udev, pipe)) /
				1000;
			break;
		}

		ep->hep = hep;
		hep->hcpriv = ep;
	}

	/* maybe put endpoint into schedule */
	switch (type) {
	case PIPE_CONTROL:
	case PIPE_BULK:
		if (list_empty(&ep->schedule))
			list_add_tail(&ep->schedule, &ue11->async);
		break;
	case PIPE_ISOCHRONOUS:
	case PIPE_INTERRUPT:
		urb->interval = ep->period;
		if (ep->branch < PERIODIC_SIZE) {
			/*
			 * The phase is correct here, but the value needs
			 * offsetting by the transfer queue depth.  All current
			 * drivers ignore start_frame, so this is unlikely to
			 * matter.
			 */
			urb->start_frame = (ue11->frame & (PERIODIC_SIZE - 1)) +
					   ep->branch;
			break;
		}

		retval = balance(ue11, ep->period, ep->load);
		if (retval < 0)
			goto fail;
		ep->branch = retval;
		retval = 0;
		urb->start_frame =
			(ue11->frame & (PERIODIC_SIZE - 1)) + ep->branch;

		/*
		 * Sort each schedule branch by period (slow before fast) to
		 * share the faster parts of the tree without placeholder nodes.
		 */
		dev_dbg(hcd->self.controller, "schedule qh%d/%p branch %d\n",
			ep->period, ep, ep->branch);
		for (i = ep->branch; i < PERIODIC_SIZE; i += ep->period) {
			struct ue11h_ep **prev = &ue11->periodic[i];
			struct ue11h_ep *here = *prev;

			while (here && ep != here) {
				if (ep->period > here->period)
					break;
				prev = &here->next;
				here = *prev;
			}
			if (ep != here) {
				ep->next = here;
				*prev = ep;
			}
			ue11->load[i] += ep->load;
		}
		ue11->periodic_count++;
		hcd->self.bandwidth_allocated += ep->load / ep->period;
		enable_sof_interrupt(ue11);
	}

	urb->hcpriv = hep;
	// Start transfer if one is not already in-progress
	start_transfer(ue11);
	// Enable interrupts
	writel(ue11->irq_enable, ue11->reg_base + USB_IRQ_MASK);
fail:
	if (retval)
		usb_hcd_unlink_urb_from_ep(hcd, urb);
fail_not_linked:
	spin_unlock_irqrestore(&ue11->lock, flags);
	return retval;
}
//-----------------------------------------------------------------
// ue11h_urb_dequeue
//-----------------------------------------------------------------
static int ue11h_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	struct ue11 *ue11 = hcd_to_ue11(hcd);
	struct usb_host_endpoint *hep;
	unsigned long flags;
	struct ue11h_ep *ep;
	int retval;

	spin_lock_irqsave(&ue11->lock, flags);
	retval = usb_hcd_check_unlink_urb(hcd, urb, status);
	if (retval)
		goto fail;

	hep = urb->hcpriv;
	ep = hep->hcpriv;
	if (ep) {
		/* finish right away if this urb can't be active ...
		 * note that some drivers wrongly expect delays
		 */
		if (ep->hep->urb_list.next != &urb->urb_list)
			dev_dbg(hcd->self.controller,
				"dequeue non-head URB %p\n", urb);
		/* for active transfers, we expect an IRQ */
		else if (ue11->active_transfer == ep) {
			if (time_before_eq(ue11->active_start, jiffies)) {
				/* happens a lot with lowspeed?? */
				dev_err(hcd->self.controller,
					"USB: Giving up on transfer....\n");
				ue11->active_transfer = NULL;
			} else
				urb = NULL;
		} else {
			/* front of queue for inactive endpoint */
		}

		if (urb)
			finish_request(ue11, ep, urb, status);
		else
			dev_dbg(hcd->self.controller,
				"dequeue, urb %p active %s; wait4irq\n", urb,
				(ue11->active_transfer == ep) ? "A" : "B");
	} else
		retval = -EINVAL;
fail:
	spin_unlock_irqrestore(&ue11->lock, flags);
	return retval;
}
//-----------------------------------------------------------------
// ue11h_endpoint_disable
//-----------------------------------------------------------------
static void ue11h_endpoint_disable(struct usb_hcd *hcd,
				   struct usb_host_endpoint *hep)
{
	struct ue11h_ep *ep = hep->hcpriv;

	if (!ep)
		return;

	/* assume we'd just wait for the irq */
	if (!list_empty(&hep->urb_list))
		usleep_range(3000, 4000);
	if (!list_empty(&hep->urb_list))
		dev_warn(hcd->self.controller, "ep %p not empty?\n", ep);

	kfree(ep);
	hep->hcpriv = NULL;
}
//-----------------------------------------------------------------
// ue11h_get_frame
//-----------------------------------------------------------------
static int ue11h_get_frame(struct usb_hcd *hcd)
{
	struct ue11 *ue11 = hcd_to_ue11(hcd);

	/* wrong except while periodic transfers are scheduled;
	 * never matches the on-the-wire frame;
	 * subject to overruns.
	 */
	return ue11->frame;
}

static bool ue11h_line_connected(struct ue11 *ue11)
{
	u32 status = readl(ue11->reg_base + USB_STATUS);

	if (((status >> USB_STATUS_LINESTATE_BITS_SHIFT) &
	     USB_STATUS_LINESTATE_BITS_MASK) != 0)
		return true;

	/* A Full-Speed EOP may expose SE0 for two bit times. */
	udelay(3);
	status = readl(ue11->reg_base + USB_STATUS);
	return ((status >> USB_STATUS_LINESTATE_BITS_SHIFT) &
		USB_STATUS_LINESTATE_BITS_MASK) != 0;
}

/*
 * UE11's device-detect IRQ is sticky for as long as D+ or D- is high, so it
 * cannot represent both insertion and removal without causing an IRQ storm.
 * The usbcore root-hub poll (at most 256 ms) samples the UTMI line state
 * instead.  A second sample after the USB-defined 2.5 us reset threshold,
 * followed by consecutive root-hub polls, prevents packet EOPs from looking
 * like a physical disconnect.
 */
static void ue11h_update_connection(struct ue11 *ue11)
{
	bool connected;

	if (!(ue11->port1 & USB_PORT_STAT_POWER) ||
	    (ue11->port1 & USB_PORT_STAT_RESET))
		return;

	connected = ue11h_line_connected(ue11);
	if (connected) {
		ue11->disconnect_samples = 0;
		if (!(ue11->port1 & USB_PORT_STAT_CONNECTION)) {
			ue11->port1 |= USB_PORT_STAT_CONNECTION |
				       (USB_PORT_STAT_C_CONNECTION << 16);
			ue11->stat_insrmv++;
		}
		return;
	}

	if (ue11->disconnect_samples < UE11_DISCONNECT_DEBOUNCE)
		ue11->disconnect_samples++;
	if (ue11->disconnect_samples < UE11_DISCONNECT_DEBOUNCE ||
	    !(ue11->port1 & USB_PORT_STAT_CONNECTION))
		return;

	ue11->port1 &= ~(USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE |
			 USB_PORT_STAT_SUSPEND | USB_PORT_STAT_RESET);
	ue11->port1 |= USB_PORT_STAT_C_CONNECTION << 16;
	ue11->active_transfer = NULL;
	ue11->next_periodic = NULL;
	ue11->next_async = NULL;
	ue11->irq_enable = 0;
	writel(0, ue11->reg_base + USB_IRQ_MASK);
	usbhw_hub_enable(ue11, false);
	ue11->stat_insrmv++;
}
//-----------------------------------------------------------------
// ue11h_hub_status_data: Virtual root hub port status check
//-----------------------------------------------------------------
static int ue11h_hub_status_data(struct usb_hcd *hcd, char *buf)
{
	struct ue11 *ue11 = hcd_to_ue11(hcd);
	unsigned long flags;
	bool changed;

	spin_lock_irqsave(&ue11->lock, flags);
	ue11h_update_connection(ue11);
	changed = ue11->port1 & (0xffff << 16);
	spin_unlock_irqrestore(&ue11->lock, flags);

	// No status changes
	if (!changed)
		return 0;

	/* tell hub_wq port 1 changed */
	*buf = (1 << 1);
	return 1;
}
//-----------------------------------------------------------------
// ue11h_hub_descriptor
//-----------------------------------------------------------------
static void ue11h_hub_descriptor(struct ue11 *ue11,
				 struct usb_hub_descriptor *desc)
{
	u16 temp = 0;

	desc->bDescriptorType = USB_DT_HUB;
	desc->bHubContrCurrent = 0;

	desc->bNbrPorts = 1;
	desc->bDescLength = 9;

	/* per-port power switching (gang of one!), or none */
	desc->bPwrOn2PwrGood = 0;

	/* no per port power switching or overcurrent errors detection/handling */
	temp = HUB_CHAR_NO_LPSM | HUB_CHAR_NO_OCPM;

	desc->wHubCharacteristics = cpu_to_le16(temp);

	/* ports removable, and legacy PortPwrCtrlMask */
	desc->u.hs.DeviceRemovable[0] = 0 << 1;
	desc->u.hs.DeviceRemovable[1] = ~0;
}
//-----------------------------------------------------------------
// ue11h_timer: Device detect timer callback
//-----------------------------------------------------------------
static void ue11h_timer(struct timer_list *t)
{
	unsigned long flags;
	struct ue11 *ue11 = timer_container_of(ue11, t, timer);
	struct usb_hcd *hcd = ue11_to_hcd(ue11);
	bool connected;

	spin_lock_irqsave(&ue11->lock, flags);

	if (!ue11->reset_recovery) {
		/* End the 50 ms SE0 interval, then allow 10 ms recovery. */
		usbhw_hub_enable(ue11, false);
		ue11->reset_recovery = true;
		mod_timer(&ue11->timer, jiffies + msecs_to_jiffies(10));
		spin_unlock_irqrestore(&ue11->lock, flags);
		return;
	}

	ue11->reset_recovery = false;
	ue11->port1 &= ~USB_PORT_STAT_RESET;
	ue11->port1 |= USB_PORT_STAT_C_RESET << 16;
	connected = ue11h_line_connected(ue11);
	if (connected)
		ue11->port1 |= USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE;
	else
		ue11->port1 &=
			~(USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE);
	usbhw_hub_enable(ue11, connected);

	if (connected)
		ue11->irq_enable |= ((1 << USB_IRQ_MASK_ERR_SHIFT) |
				     (1 << USB_IRQ_MASK_DONE_SHIFT));
	else
		ue11->irq_enable = 0;

	/* reenable irqs */
	writel(ue11->irq_enable, ue11->reg_base + USB_IRQ_MASK);
	spin_unlock_irqrestore(&ue11->lock, flags);

	usb_hcd_poll_rh_status(hcd);
}
//-----------------------------------------------------------------
// ue11h_hub_control
//-----------------------------------------------------------------
static int ue11h_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
			     u16 wIndex, char *buf, u16 wLength)
{
	struct ue11 *ue11;
	int retval;
	unsigned long flags;

	ue11 = hcd_to_ue11(hcd);
	retval = 0;

	spin_lock_irqsave(&ue11->lock, flags);

	switch (typeReq) {
	case ClearHubFeature:
	case SetHubFeature:
		switch (wValue) {
		case C_HUB_OVER_CURRENT:
		case C_HUB_LOCAL_POWER:
			break;
		default:
			goto error;
		}
		break;
	case ClearPortFeature:
		if (wIndex != 1 || wLength != 0)
			goto error;

		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
			ue11->port1 &= USB_PORT_STAT_POWER;
			ue11->irq_enable = 0;
			writel(ue11->irq_enable, ue11->reg_base + USB_IRQ_MASK);
			usbhw_hub_enable(ue11, false);
			break;
		case USB_PORT_FEAT_SUSPEND:
			if (!(ue11->port1 & USB_PORT_STAT_SUSPEND))
				break;
			usbhw_hub_enable(ue11, true);
			ue11->port1 |= USB_PORT_STAT_C_SUSPEND << 16;
			break;
		case USB_PORT_FEAT_POWER:
			port_power(ue11, 0);
			break;
		case USB_PORT_FEAT_C_ENABLE:
		case USB_PORT_FEAT_C_SUSPEND:
		case USB_PORT_FEAT_C_CONNECTION:
		case USB_PORT_FEAT_C_OVER_CURRENT:
		case USB_PORT_FEAT_C_RESET:
			break;
		default:
			goto error;
		}
		ue11->port1 &= ~(1 << wValue);
		break;
	case GetHubDescriptor:
		ue11h_hub_descriptor(ue11, (struct usb_hub_descriptor *)buf);
		break;
	case GetHubStatus:
		put_unaligned_le32(0, buf);
		break;
	case GetPortStatus:
		if (wIndex != 1)
			goto error;
		ue11h_update_connection(ue11);
		put_unaligned_le32(ue11->port1, buf);
		break;
	case SetPortFeature:
		if (wIndex != 1 || wLength != 0)
			goto error;
		switch (wValue) {
		case USB_PORT_FEAT_SUSPEND:
			if (ue11->port1 & USB_PORT_STAT_RESET)
				goto error;
			if (!(ue11->port1 & USB_PORT_STAT_ENABLE))
				goto error;
			usbhw_hub_enable(ue11, false);
			break;
		case USB_PORT_FEAT_POWER:
			port_power(ue11, 1);
			break;
		case USB_PORT_FEAT_RESET:
			if (ue11->port1 & USB_PORT_STAT_SUSPEND)
				goto error;
			if (!(ue11->port1 & USB_PORT_STAT_POWER))
				break;

			/* 50 msec of reset/SE0 signaling, irqs blocked */
			ue11->irq_enable = 0;
			writel(ue11->irq_enable, ue11->reg_base + USB_IRQ_MASK);
			usbhw_bus_reset_assert(ue11);
			ue11->reset_recovery = false;
			ue11->port1 |= USB_PORT_STAT_RESET;
			mod_timer(&ue11->timer, jiffies + msecs_to_jiffies(50));
			break;
		default:
			goto error;
		}
		ue11->port1 |= 1 << wValue;
		break;

	default:
error:
		/* "protocol stall" on error */
		retval = -EPIPE;
	}

	spin_unlock_irqrestore(&ue11->lock, flags);
	return retval;
}

//-----------------------------------------------------------------
// ue11h_stop
//-----------------------------------------------------------------
static void ue11h_stop(struct usb_hcd *hcd)
{
	struct ue11 *ue11 = hcd_to_ue11(hcd);
	unsigned long flags;

	timer_delete_sync(&ue11->timer);

	spin_lock_irqsave(&ue11->lock, flags);
	writel(0, ue11->reg_base + USB_IRQ_MASK);
	port_power(ue11, 0);
	spin_unlock_irqrestore(&ue11->lock, flags);
}
//-----------------------------------------------------------------
// ue11h_start
//-----------------------------------------------------------------
static int ue11h_start(struct usb_hcd *hcd)
{
	struct ue11 *ue11 = hcd_to_ue11(hcd);

	/* chip has been reset, VBUS power is off */
	hcd->state = HC_STATE_RUNNING;
	/* enable power and interrupts */
	port_power(ue11, 1);

	return 0;
}
//-----------------------------------------------------------------
// ue11h_hc_driver structure
//-----------------------------------------------------------------
static const struct hc_driver ue11h_hc_driver = {
	.description = hcd_name,
	.hcd_priv_size = sizeof(struct ue11),

	/*
	 * generic hardware linkage
	 */
	.irq = ue11h_irq,
	.flags = HCD_USB11 | HCD_MEMORY,

	/* Basic lifecycle operations */
	.start = ue11h_start,
	.stop = ue11h_stop,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue = ue11h_urb_enqueue,
	.urb_dequeue = ue11h_urb_dequeue,
	.endpoint_disable = ue11h_endpoint_disable,

	/*
	 * periodic schedule support
	 */
	.get_frame_number = ue11h_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data = ue11h_hub_status_data,
	.hub_control = ue11h_hub_control,
};
//-----------------------------------------------------------------
// ue11h_remove:
//-----------------------------------------------------------------
static void ue11h_remove(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);

	usb_remove_hcd(hcd);
	usb_put_hcd(hcd);
}
//-----------------------------------------------------------------
// ue11h_probe:
//-----------------------------------------------------------------
static int ue11h_probe(struct platform_device *dev)
{
	struct usb_hcd *hcd;
	struct ue11 *ue11;
	struct resource *iores;
	int irq;
	int retval;
	void __iomem *dev_base;
	u32 version;

	if (usb_disabled())
		return -ENODEV;

	iores = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (!iores)
		return -ENODEV;

	// Get IRQ for device
	irq = platform_get_irq(dev, 0);
	if (irq < 0)
		return irq;

	// Get device memory
	dev_base = devm_ioremap_resource(&dev->dev, iores);
	if (IS_ERR(dev_base))
		return PTR_ERR(dev_base);

	version = readl(dev_base + USB_VERSION);
	if (version != USB_VERSION_VALUE)
		return dev_err_probe(
			&dev->dev, -ENODEV,
			"unsupported UE11 ABI 0x%08x (expected 0x%08x)\n",
			version, USB_VERSION_VALUE);

	/* allocate and initialize hcd */
	hcd = usb_create_hcd(&ue11h_hc_driver, &dev->dev, dev_name(&dev->dev));
	if (!hcd)
		return -ENOMEM;

	hcd->regs = dev_base;
	hcd->rsrc_start = iores->start;
	hcd->rsrc_len = resource_size(iores);
	ue11 = hcd_to_ue11(hcd);

	spin_lock_init(&ue11->lock);
	INIT_LIST_HEAD(&ue11->async);
	timer_setup(&ue11->timer, ue11h_timer, 0);
	ue11->reg_base = dev_base;
	platform_set_drvdata(dev, hcd);

	usbhw_phy_reset(ue11);
	writel(0xf, ue11->reg_base + USB_IRQ_ACK);

	/* The controller has a dedicated, active-high level interrupt. */
	retval = usb_add_hcd(hcd, irq, 0);
	if (retval)
		goto err6;

	dev_info(&dev->dev, "UE11 Full-Speed host, ABI 0x%08x, irq %d\n",
		 version, irq);

	return 0;

err6:
	platform_set_drvdata(dev, NULL);
	usb_put_hcd(hcd);
	return retval;
}

static const struct of_device_id ue11h_of_match[] = {
	{ .compatible = "superscalarcrash,chiplab-ue11" },
	{},
};
MODULE_DEVICE_TABLE(of, ue11h_of_match);

static struct platform_driver ue11h_driver = {
	.probe = ue11h_probe,
	.remove = ue11h_remove,
	.shutdown = usb_hcd_platform_shutdown,
	.driver = {
		.name = hcd_name,
		.of_match_table = ue11h_of_match,
	},
};

module_platform_driver(ue11h_driver);

MODULE_DESCRIPTION("Chiplab UE11 USB Full-Speed Host Controller Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:chiplab-ue11-hcd");
