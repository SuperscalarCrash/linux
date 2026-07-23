// SPDX-License-Identifier: GPL-2.0
/* MAC driver
 * 2007-11-1 created by liyunhua
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/compiler.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/completion.h>
#include <linux/crc32.h>
#include <linux/dma-mapping.h>
#include <asm/io.h>
#include <linux/uaccess.h>
#include <asm/irq.h>
#include <linux/platform_device.h>

#define	DMFE1_IRQ		0x01
#define DMFE2_IRQ		0x02
#define TX_DESC_CNT		0x20			/* Allocated Tx descriptors */
#define RX_DESC_CNT		0x40			/* Allocated Rx descriptors */
#define RX_BUF_SIZE		0x620
#define TX_BUF_ALLOC		0x600
#define MAX_PACKET_SIZE		1514
#define TX_MAX_SEND_CNT		0x1			/* Maximum tx packet per time */
#define TX_FREE_DESC_CNT	(TX_DESC_CNT - 2)	/* Max TX packet count */

#define CR0_DEFAULT	0x00E00000	/* TX & RX burst mode  */
#define CR6_DEFAULT	0x00080000	/* HD */
#define CR7_DEFAULT	0x180c1		/* Interrupt enable    */
#define CR15_DEFAULT	0x06		/* TxJabber RxWatchdog */
#define TDES0_ERR_MASK	0x4302		/* TXJT, LC, EC, FUE   */

#define DMFE_10MHF		0
#define DMFE_100MHF		1
#define DMFE_10MFD		4
#define DMFE_100MFD		5
#define DMFE_AUTO		8
#define DMFE_1M_HPNA		0x10
#define MAX_CHECK_PACKET	0x8000
#define TOUT_LOOP		100000

#define DMFE_TIMER_WUT	(jiffies + HZ * 1)	/* timer wakeup time : 1 second */

#define DMFE_MAX_MULTICAST	14

#define IRQ2CHIPID(irq)		irq
#define IRQ2PHYADDR(irq)	(irq + 4)	// (irq ? 1:5)
#define TIMEOUT			3*HZ

#define PCI_DM9132_ID	0x91321282	/* Davicom DM9132 ID */
#define PCI_DM9102_ID	0x91021282	/* Davicom DM9102 ID */
#define PCI_DM9100_ID	0x91001282	/* Davicom DM9100 ID */
#define PCI_DM9009_ID	0x90091282	/* Davicom DM9009 ID */

#define PHY_DATA_1	0x20000
#define PHY_DATA_0	0x00000
#define MDCLKH		0x10000
//#define DES0_BASE	0xa0000000
#define DES0_BASE	0x80000000
#define dw32(reg, val)	iowrite32(val, tp->ioaddr + (reg))
#define dr32(reg)	ioread32(tp->ioaddr + (reg))

#define SHOW_MEDIA_TYPE(mode) \
	printk(" dmfe: Change Speed to %sMhz %s duplex\n",mode & 1 ?"100":"10", mode & 4 ? "full":"half");

#define CONFIG_SOC_MAC_HARDWARE_ACCELERATE	1
//#define DBG_FLAG3	1
//#define DBG_FLAG	1
//#define DBG_FLAG2	1
#define MAC_REG_BASE	0xbf005200
#define RX_COPY_SIZE	100
#define DM910X_RESET	1

enum dmfe_offsets {
	CSR0 = 0x00,
	CSR1 = 0x08,
	CSR2 = 0x10,
	CSR3 = 0x18,
	CSR4 = 0x20,
	CSR5 = 0x28,
	CSR6 = 0x30,
	CSR7 = 0x38,
	CSR8 = 0x40,
	CSR9 = 0x48,
	CSR10 = 0x50,
	CSR11 = 0x58,
	CSR12 = 0x60,
	CSR13 = 0x68,
	CSR14 = 0x70,
	CSR15 = 0x78,
};
enum dmfe_CR6_bits {
	CR6_RXSC = 0x2,
	CR6_PBF = 0x8,
	CR6_PM = 0x40,
	CR6_PAM = 0x80,
	CR6_FDM = 0x200,
	CR6_TXSC = 0x2000,
	CR6_STI = 0x100000,
	CR6_SFT = 0x200000,
	CR6_RXA = 0x40000000,
	CR6_NO_PURGE = 0x20000000,
};

enum dmfe_control {
	DMFE_RESET = 0x01
};

struct tx_desc {
	volatile u32 	tdes0;
	volatile u32	tdes1;
	volatile u32	tdes2;
	volatile u32	tdes3;		/* Data for the card */
	char		*tx_buf_ptr;
	struct sk_buff	*skb;		/* Data for us */
	struct tx_desc	*next_desc;
} __attribute__((aligned(32)));

struct rx_desc {
	volatile u32	rdes0;
	volatile u32	rdes1;
	volatile u32	rdes2;
	volatile u32	rdes3;		/* Data for the card */
	struct sk_buff	*skb;		/* Data for us */
	struct rx_desc	*next_desc;
} __attribute__((aligned(32)));

struct dmfe_private {
	u32			chip_id;
	struct net_device	*next_dev;
	struct net_device	*ndev;
	struct device		*dma_dev;
	spinlock_t		lock;

	void			*ioaddr;
	u32			cr0_data;
	u32			cr5_data;
	u32			cr6_data;
	u32			cr7_data;
	u32			cr15_data;
	u16			PHY_reg4;

	u8			phy_addr;
	u8 			media_mode;		/* user specify media mode */
	u8 			op_mode;		/* real work media mode */
	u8 			link_failed;		/* Ever link failed */
	u8			dm910x_chk_mode;	/* Operating mode check */

	struct tx_desc		*tx_desc_head;
	dma_addr_t		tx_desc_dma_head;
	struct rx_desc		*rx_desc_head;
	dma_addr_t		rx_desc_dma_head;

	dma_addr_t		buf_pool_dma_ptr;	/* Tx buffer pool memory */
	dma_addr_t		buf_pool_dma_start;	/* Tx buffer pool align dword */

	unsigned char		*buf_pool_ptr;		/* Tx buffer pool memory */
	unsigned char		*buf_pool_start;	/* Tx buffer pool align dword */
	struct tx_desc		*cpu_cur_tx;
	struct tx_desc		*mac_cur_tx;
	struct rx_desc		*cpu_cur_rx;
	struct rx_desc		*mac_cur_rx;

	u32			tx_packet_cnt;
	u32			tx_queue_cnt;

	u32			rx_avail_cnt;
	u32			tx_avail_cnt;
	u32			tx_packets;

	struct timer_list	timer;
	struct net_device_stats	stats;
};

static int ether_set=0;
//static char hwaddr[ETH_ALEN]={0xAA, 0x02, 0x03, 0x04, 0x05, 0x06};
static char hwaddr[ETH_ALEN]={0x00, 0x98, 0x76, 0x64, 0x32, 0x19};


static int dmfe_open(struct net_device *dev);
static netdev_tx_t dmfe_start_xmit(struct sk_buff *skb,
				   struct net_device *dev);
static int dmfe_close(struct net_device *dev);
static struct net_device_stats *dmfe_get_stats(struct net_device *dev);
static void dmfe_set_filter_mode(struct net_device *dev);
static irqreturn_t dmfe_interrupt (int irq, void *dev_instance);
static void dmfe_hw_init(struct net_device *dev);
static int dmfe_descriptor_init(struct net_device *dev);
static void dmfe_set_phyxcer(struct net_device *dev);
static u8 dmfe_sense_speed(struct net_device *dev);
static void dmfe_process_mode(struct net_device *dev);
static void update_csr6(u32 val, void *ioaddr);
static void send_filter_frame(struct net_device *dev, int mc_cnt);
static void phy_write(void *iobase, u8 phy_addr, u8 offset, u16 phy_data, u32 chip_id);
static void dmfe_timer(struct timer_list *t);
static u16 phy_read(void *iobase, u8 phy_addr, u8 offset, u32 chip_id);
static void phy_write_1bit(void *ioaddr, u32 phy_data);
static u16 phy_read_1bit(void *ioaddr);
static const struct net_device_ops dmfe_netdev_ops = {
	.ndo_open = dmfe_open,
	.ndo_start_xmit = dmfe_start_xmit,
	.ndo_stop = dmfe_close,
	.ndo_set_rx_mode = dmfe_set_filter_mode,
	.ndo_get_stats = dmfe_get_stats,
};

/*
 *	Calculate the CRC valude of the Rx packet
 *	flag = 	1 : return the reverse CRC (for the received packet CRC)
 *		0 : return the normal CRC (for Hash Table index)
 */

#if 0
static inline u32 cal_CRC(unsigned char * Data, unsigned int Len, u8 flag)
{
	u32 crc = crc32(~0, Data, Len);
	if (flag) crc = ~crc;
	return crc;
}
#endif

static struct net_device *dmfe_init_one(struct device *device, void *base_addr, int irq)
{
	struct dmfe_private 	*tp;
	struct net_device 	*dev;
	int 			err;

	dev = alloc_etherdev(sizeof(struct dmfe_private));
	if (dev == NULL) {
		printk("dmfe alloc etherdev failed\n");
		return NULL;
	}
	SET_NETDEV_DEV(dev, device);
	tp = netdev_priv(dev);

	dev->irq = irq;
	tp->ndev = dev;
	tp->dma_dev = device;
	tp->ioaddr = base_addr;
	tp->phy_addr = 0x1; //IRQ2PHYADDR(dev->irq);
	tp->chip_id = IRQ2CHIPID(irq);

	dev->netdev_ops = &dmfe_netdev_ops;

	spin_lock_init(&tp->lock);
	if (ether_set) {
		eth_hw_addr_set(dev, hwaddr);
	} else if (device_get_ethdev_address(device, dev)) {
		static const u8 default_addr[ETH_ALEN] = {
			0x00, 0x98, 0x76, 0x64, 0x32, 0x19
		};

		//dev->dev_addr[0] = 0x00;
		//dev->dev_addr[1] = 0x00;
		//dev->dev_addr[2] = 0x6c;
		//get_random_bytes(&dev->dev_addr[3], 3);

		eth_hw_addr_set(dev, default_addr);
	}
	//dev->dev_addr[5] += irq-DMFE1_IRQ;

	strcpy(dev->name, "eth%d");
	err = register_netdev(dev);
	if (err) {
		printk("dmfe eth driver register netdev failed\n");
		free_netdev(dev);
		return NULL;
	}

	return dev;
}

static unsigned int RANDOM_SEED = 0;

static inline unsigned int random(unsigned int ubound)
{
	static unsigned int a = 1588635695,
		q = 2,
		r = 1117695901;
	if(!RANDOM_SEED)RANDOM_SEED=jiffies;
	RANDOM_SEED = a*(RANDOM_SEED % q) - r*(RANDOM_SEED / q);
	return RANDOM_SEED % ubound;
}

static int __init setether(char *str)
{
	int i;
	for(i=0;i<6;i++,str+=3)
		hwaddr[i]=simple_strtoul(str,0,16);
	ether_set=1;
	return 1;
}

__setup("etheraddr=", setether);


static int dmfe_descriptor_init(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	struct tx_desc		*tx;
	struct rx_desc		*rx;
	dma_addr_t		tx_dma;
	dma_addr_t		rx_dma;
	dma_addr_t		rx_buf_dma;
	size_t			desc_pool_size;
	size_t			buf_pool_size;
	int			i;
	struct sk_buff		*skb;

	unsigned char *tmp_buf;
	dma_addr_t tmp_buf_dma;

	desc_pool_size = sizeof(struct tx_desc) * TX_DESC_CNT +
			 sizeof(struct rx_desc) * RX_DESC_CNT;
	buf_pool_size = TX_BUF_ALLOC * TX_DESC_CNT;

	tp->tx_desc_head = dma_alloc_coherent(tp->dma_dev, desc_pool_size,
					     &tp->tx_desc_dma_head, GFP_KERNEL);
	if (tp->tx_desc_head == NULL) {
		return -ENOMEM;
	}
	tp->rx_desc_head = (void *)tp->tx_desc_head +
			   sizeof(struct tx_desc) * TX_DESC_CNT;
	tp->rx_desc_dma_head = tp->tx_desc_dma_head + sizeof(struct tx_desc) * TX_DESC_CNT;

	tp->buf_pool_ptr = dma_alloc_coherent(tp->dma_dev, buf_pool_size,
					     &tp->buf_pool_dma_ptr, GFP_KERNEL);
	if (!tp->buf_pool_ptr) {
		dma_free_coherent(tp->dma_dev, desc_pool_size, tp->tx_desc_head,
				  tp->tx_desc_dma_head);
		tp->tx_desc_head = NULL;
		tp->rx_desc_head = NULL;
		return -ENOMEM;
	}
	tp->buf_pool_start = tp->buf_pool_ptr;
	tp->buf_pool_dma_start = tp->buf_pool_dma_ptr;
#ifdef DBG_FLAG3
	printk("dmfe_descriptor_init===>tp->tx_desc_head:%x,tp->tx_desc_dma_head:%x\n",tp->tx_desc_head,tp->tx_desc_dma_head);
	printk("dmfe_descriptor_init===>tp->rx_desc_head:%x,tp->rx_desc_dma_head:%x\n",tp->rx_desc_head,tp->rx_desc_dma_head);
	printk("dmfe_descriptor_init===>tp->buf_pool_ptr:%x,tp->buf_pool_dma_ptr:%x\n",tp->buf_pool_ptr,tp->buf_pool_dma_ptr);
#endif
	tmp_buf = tp->buf_pool_start;
	tmp_buf_dma = tp->buf_pool_dma_start;
	tx = tp->tx_desc_head;
	tx_dma = tp->tx_desc_dma_head;
	for (i = 0; i < TX_DESC_CNT; i++) {
		tx->tx_buf_ptr = tmp_buf;
		tx->skb = NULL;
		tx->tdes0 = cpu_to_le32(0);
		tx->tdes1 = cpu_to_le32(0x81000000);
		tx->tdes2 = cpu_to_le32(tmp_buf_dma);
		tx_dma += sizeof(struct tx_desc);
		tx->tdes3 = cpu_to_le32(tx_dma);   // point to next descriptor
		tx->next_desc = tx + 1;
		//dma_cache_wback((unsigned long)tx, sizeof(struct tx_desc));

		tmp_buf = tmp_buf + TX_BUF_ALLOC;
		tmp_buf_dma = tmp_buf_dma + TX_BUF_ALLOC;
		tx++;
		tp->tx_avail_cnt++;
	}
	// set the tailer point back to the header
	(--tx)->tdes3 = cpu_to_le32(tp->tx_desc_dma_head);
	tx->next_desc = tp->tx_desc_head;

	//dma_cache_wback((unsigned long)tx, sizeof(struct tx_desc));
	rx = tp->rx_desc_head;
	rx_dma = tp->rx_desc_dma_head;
	for (i = 0; i < RX_DESC_CNT; i++) {
		rx->skb = NULL;
		rx->rdes0 = cpu_to_le32(0);
		rx->rdes1 = cpu_to_le32(0x01000600);
		//rx->rdes1 = cpu_to_le32(0x010007f0);
		rx_dma += sizeof(struct rx_desc);  // point to next descriptor
		rx->rdes3 = cpu_to_le32(rx_dma);

		//dma_cache_wback((unsigned long)rx, sizeof(struct rx_desc));
		rx->next_desc = rx + 1;
		rx++;
	}
	// set the tailer point back to the header
	(--rx)->rdes3 = cpu_to_le32(tp->rx_desc_dma_head);
	rx->next_desc = tp->rx_desc_head;
	//dma_cache_wback((unsigned long)rx, sizeof(struct rx_desc));
	rx = tp->rx_desc_head;
	// Allocate recv data buffer.
	while (tp->rx_avail_cnt < RX_DESC_CNT) {
		skb = dev_alloc_skb(RX_BUF_SIZE);
		if (skb == NULL) {
			goto err_rx_buffers;
		}
		rx->skb = skb;
		rx_buf_dma = dma_map_single(tp->dma_dev, skb->data, RX_BUF_SIZE,
					    DMA_FROM_DEVICE);
		if (dma_mapping_error(tp->dma_dev, rx_buf_dma)) {
			dev_kfree_skb(skb);
			rx->skb = NULL;
			goto err_rx_buffers;
		}
		rx->rdes2 = cpu_to_le32(rx_buf_dma);
#ifdef DBG_FLAG3
		printk("dmfe_descriptor_init===>this rx->rdes2:%x\n", rx->rdes2);
#endif
		// set the owner bit for MAC.
		dma_wmb();
		rx->rdes0 = cpu_to_le32(0x80000000);
		//dma_cache_wback((unsigned long)rx, sizeof(struct rx_desc));
		rx = rx->next_desc;
		tp->rx_avail_cnt++;
	}
	return 0;

err_rx_buffers:
	rx = tp->rx_desc_head;
	for (i = 0; i < tp->rx_avail_cnt; i++) {
		dma_unmap_single(tp->dma_dev, le32_to_cpu(rx->rdes2),
				 RX_BUF_SIZE, DMA_FROM_DEVICE);
		dev_kfree_skb(rx->skb);
		rx->skb = NULL;
		rx = rx->next_desc;
	}
	tp->rx_avail_cnt = 0;
	dma_free_coherent(tp->dma_dev, buf_pool_size, tp->buf_pool_ptr,
			  tp->buf_pool_dma_ptr);
	dma_free_coherent(tp->dma_dev, desc_pool_size, tp->tx_desc_head,
			  tp->tx_desc_dma_head);
	tp->buf_pool_ptr = NULL;
	tp->tx_desc_head = NULL;
	tp->rx_desc_head = NULL;
	return -ENOMEM;
}

static void dmfe_descriptor_free(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	struct rx_desc		*rx = tp->rx_desc_head;
	struct tx_desc		*tx = tp->tx_desc_head;
	size_t			desc_pool_size;
	size_t			buf_pool_size;
	int			i;

	if (!tp->tx_desc_head)
		return;

	desc_pool_size = sizeof(struct tx_desc) * TX_DESC_CNT +
			 sizeof(struct rx_desc) * RX_DESC_CNT;
	buf_pool_size = TX_BUF_ALLOC * TX_DESC_CNT;

	for (i = 0; i < RX_DESC_CNT; i++) {
		if (rx->skb) {
			dma_unmap_single(tp->dma_dev, le32_to_cpu(rx->rdes2),
					 RX_BUF_SIZE, DMA_FROM_DEVICE);
			dev_kfree_skb(rx->skb);
			rx->skb = NULL;
		}
		rx = rx->next_desc;
	}

	for (i = 0; i < TX_DESC_CNT; i++) {
		if (tx->skb) {
			dev_kfree_skb_any(tx->skb);
			tx->skb = NULL;
		}
		tx = tx->next_desc;
	}

	dma_free_coherent(tp->dma_dev, buf_pool_size, tp->buf_pool_ptr,
			  tp->buf_pool_dma_ptr);
	dma_free_coherent(tp->dma_dev, desc_pool_size, tp->tx_desc_head,
			  tp->tx_desc_dma_head);
	tp->buf_pool_ptr = NULL;
	tp->tx_desc_head = NULL;
	tp->rx_desc_head = NULL;
	tp->rx_avail_cnt = 0;
	tp->tx_avail_cnt = 0;
	tp->tx_packets = 0;
}

#if 0
static void allocate_rx_buffer(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	struct rx_desc *rx;
	struct sk_buff *skb;

	rx = tp->rx_desc_head;
	// Allocate recv data buffer.
	while (tp->rx_avail_cnt < RX_DESC_CNT) {
		skb = dev_alloc_skb(RX_BUF_SIZE);
		if (skb == NULL) {
			break;
		}
		rx->skb = skb;
		rx->rdes2 = cpu_to_le32(dma_map_single(&dev->dev, skb->data, RX_BUF_SIZE, DMA_FROM_DEVICE));
	#ifdef DBG_FLAG3
		printk("allocate_rx_buffer===>this rx->rdes2:%x\n", rx->rdes2);
	#endif

		wmb();
		rx->rdes0 = cpu_to_le32(0x80000000);

	//dma_cache_wback((unsigned long)rx, sizeof(struct rx_desc));

	rx = rx->next_desc;
		tp->rx_avail_cnt++;
	}
}
#endif

static void dmfe_hw_init(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	void		*ioaddr = tp->ioaddr;
	int mc_count = netdev_mc_count(dev);

#ifdef DBG_FLAG
	printk("dmfe_hw_init===============================================>begin\n");
#endif
	//tmp = readl(tp->ioaddr+CSR0);
	/* RESET MAC */
	//writel(DMFE_RESET | tmp, ioaddr + CSR0);
	//udelay(1000);

	/* Reset DM910x MAC controller */
	tp->cr0_data = CR0_DEFAULT;
	dw32(CSR0, DM910X_RESET);	/* RESET MAC */
	udelay(1000);


#ifdef CONFIG_SOC_MAC_HARDWARE_ACCELERATE
	writel(1, tp->ioaddr + CSR10);
#endif
	dw32(CSR0, tp->cr0_data);
	udelay(5);

	//writel(0, tp->ioaddr + CSR0);
	//udelay(5);

	writel(tp->rx_desc_dma_head, tp->ioaddr+CSR3);
	writel(tp->tx_desc_dma_head, tp->ioaddr+CSR4);

	tp->media_mode = DMFE_100MFD;  // DMFE_AUTO;
	//tp->media_mode = DMFE_10MFD;  // DMFE_AUTO;

	if (dev->irq == DMFE1_IRQ) {
		dmfe_set_phyxcer(dev);
	}
	/* Media Mode Process */
	if (!(tp->media_mode & DMFE_AUTO))
		tp->op_mode = tp->media_mode; 	/* Force Mode */

	tp->cr5_data = readl(ioaddr + CSR5);
	writel(tp->cr5_data, ioaddr + CSR5);

	/* Init CR6 to program DM910x operation */
	update_csr6(tp->cr6_data, tp->ioaddr+CSR6);

	send_filter_frame(dev, mc_count);	/* DM9102/DM9102A */

	/* Init CR7, interrupt active bit */
	tp->cr7_data = CR7_DEFAULT;
	writel(tp->cr7_data, tp->ioaddr + CSR7);

	/* Init CR15, Tx jabber and Rx watchdog timer */
	//	writel(tp->cr15_data, ioaddr + CSR15);

	/* Enable DM910X Tx/Rx function */
	tp->cr6_data |= CR6_RXSC | CR6_TXSC | 0x40000 | CR6_PM;  // | CR6_PBF;
	//tp->cr6_data |= CR6_RXSC | CR6_TXSC | 0x40000;
	update_csr6(tp->cr6_data, tp->ioaddr+CSR6);
#ifdef DBG_FLAG
	printk("dmfe_hw_init===============================================>end\n");
#endif
}

static int dmfe_open(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
//    struct dmfe_board_info *db = netdev_priv(dev);
	unsigned long 		flags;
	int ret;

	tp->cr6_data = 0x32003002;
	tp->cr0_data = 0;
	tp->PHY_reg4 = 0x1E0;
	tp->link_failed = 1;

#ifdef DBG_FLAG
    printk("dmfe_open===============================================>\n");
#endif

	ret = request_irq(dev->irq, &dmfe_interrupt, IRQF_SHARED, dev->name, dev);
	if (ret) {
		printk("dmfe request_irq for %s failed\n", dev->name);
		goto no_irq;
	}

	/* Initiliaze Transmit/Receive decriptor and CR3/4 */
	tp->rx_avail_cnt = 0;
	tp->tx_avail_cnt = 0;
	tp->tx_packets = 0;
	ret = dmfe_descriptor_init(dev);
	if (ret < 0) {
		printk("dmfe allocted descriptor memory failed\n");
		goto no_desc;
	}
	tp->cpu_cur_tx = tp->tx_desc_head;
	tp->mac_cur_tx = tp->tx_desc_head;
	tp->cpu_cur_rx = tp->rx_desc_head;
	tp->mac_cur_rx = tp->rx_desc_head;

	tp->cr6_data |= CR6_SFT;	/* Store & Forward mode */
	tp->cr0_data = 0;
	tp->dm910x_chk_mode = 1;

	spin_lock_irqsave(&tp->lock, flags);
	dmfe_hw_init(dev);
	netif_wake_queue(dev);
	timer_setup(&tp->timer, dmfe_timer, 0);
	mod_timer(&tp->timer, jiffies + TIMEOUT);

	spin_unlock_irqrestore(&tp->lock, flags);

#ifdef DBG_FLAG
	printk("dmfe_open===============================================>test1\n");
#endif
	return ret;

no_desc:
	free_irq(dev->irq, dev);
no_irq:
	return ret;
}

static int dmfe_close(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	unsigned long 		flags;

#ifdef DBG_FLAG
	printk("dmfe_close===============================================>begin\n");
#endif

	netif_stop_queue(dev);

	spin_lock_irqsave(&tp->lock, flags);
	writel(DMFE_RESET, tp->ioaddr+CSR0);
	// disable all interrupt
	writel(0, tp->ioaddr+CSR7);
	phy_write(tp->ioaddr, tp->phy_addr, 0, 0x8000, tp->chip_id);
	spin_unlock_irqrestore(&tp->lock, flags);

	timer_shutdown_sync(&tp->timer);
	free_irq(dev->irq, dev);
	dmfe_descriptor_free(dev);

#ifdef DBG_FLAG
	printk("dmfe_close===============================================>end\n");
#endif
	return 0;
}

static void dmfe_set_filter_mode(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	unsigned long		flags;
	int mc_count = netdev_mc_count(dev);

#ifdef DBG_FLAG
	printk("dmfe_set_filter_mode===============================================>begin,dev->flags:%d\n",dev->flags);
#endif

	spin_lock_irqsave(&tp->lock, flags);

	if (dev->flags & IFF_PROMISC) {
		printk("Enable PROM Mode\n");
		tp->cr6_data |= CR6_PM | CR6_PBF;
		update_csr6(tp->cr6_data, tp->ioaddr+CSR6);
		goto out;
	}

	if (dev->flags & IFF_ALLMULTI || mc_count > DMFE_MAX_MULTICAST) {
		printk("Pass all multicast address\n");
		tp->cr6_data &= ~(CR6_PM | CR6_PBF);
		tp->cr6_data |= CR6_PAM;
		goto out;
	}

	send_filter_frame(dev, mc_count);
out:
	spin_unlock_irqrestore(&tp->lock, flags);

#ifdef DBG_FLAG
	printk("dmfe_set_filter_mode===============================================>end\n");
#endif
}

#if 0
/* Superseded by dmfe_start_xmit(); retained for reference to the vendor RTL. */
static netdev_tx_t dmfe_start_xmit2(struct sk_buff *skb,
					 struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	void __iomem *ioaddr = tp->ioaddr;
	struct tx_desc *tx;
	unsigned long flags;

	printk("dmfe_start_xmit========================>\n");

	/* Too large packet check */
	if (skb->len > MAX_PACKET_SIZE) {
		printk("big packet = %d\n", (u16)skb->len);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Resource flag check */
	netif_stop_queue(dev);

	spin_lock_irqsave(&tp->lock, flags);

	/* No Tx resource check, it never happen nromally */
	if (tp->tx_queue_cnt >= TX_FREE_DESC_CNT) {
		spin_unlock_irqrestore(&tp->lock, flags);
		printk("No Tx resource %ld\n", tp->tx_queue_cnt);
		return NETDEV_TX_BUSY;
	}

	/* Disable NIC interrupt */
	dw32(CSR7, 0);

	/* transmit this packet */
	tx = tp->cpu_cur_tx;
	//skb_copy_from_linear_data(skb, tx->tx_buf_ptr, skb->len);
	tx->tdes1 = cpu_to_le32(0xe1000000 | skb->len);

	/* Point to next transmit free descriptor */
	tp->cpu_cur_tx = tx->next_desc;

	/* Transmit Packet Process */
	if ( (!tp->tx_queue_cnt) && (tp->tx_packet_cnt < TX_MAX_SEND_CNT) ) {
		tx->tdes0 = cpu_to_le32(0x80000000);	/* Set owner bit */
		tp->tx_packet_cnt++;			/* Ready to send */

		//dma_cache_wback((unsigned long)tx, sizeof(struct tx_desc));

		dw32(CSR1, 0x1);			/* Issue Tx polling */
		netif_trans_update(dev);		/* saved time stamp */
	} else {

		//dma_cache_wback((unsigned long)tx, sizeof(struct tx_desc));

		tp->tx_queue_cnt++;			/* queue TX packet */
		dw32(CSR1, 0x1);			/* Issue Tx polling */
	}

	/* Tx resource check */
	if ( tp->tx_queue_cnt < TX_FREE_DESC_CNT )
		netif_wake_queue(dev);

	/* Restore CR7 to enable interrupt */
	spin_unlock_irqrestore(&tp->lock, flags);
	dw32(CSR7, tp->cr7_data);

	/* free this SKB */
	dev_consume_skb_any(skb);

	return NETDEV_TX_OK;
}
#endif

static netdev_tx_t dmfe_start_xmit(struct sk_buff *skb,
				   struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	struct tx_desc		*tx;
	unsigned long		flags;

#ifdef DBG_FLAG
	printk("dmfe_start_xmit=============================================>begin\n");
#endif

	if (skb->len > MAX_PACKET_SIZE) {
		printk("error send packet too big length = %d\n", (u16)skb->len);
		dev_kfree_skb(skb);
		tp->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	if (tp->tx_avail_cnt <= 0) {
		printk("no tx descriptor\n");
		dev_kfree_skb(skb);
		tp->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&tp->lock, flags);
	writel(tp->cr7_data | 0x01, tp->ioaddr + CSR7);
	tx = tp->cpu_cur_tx;
	tp->cpu_cur_tx = tx->next_desc;
	tp->tx_avail_cnt--;
	tp->tx_packets++;
	tx->skb = skb;
	skb_copy_from_linear_data(skb, tx->tx_buf_ptr, skb->len);
#ifdef DBG_FLAG3
	printk("dmfe_start_xmit===>this tx->tdes2:%x\n", tx->tdes2);
#endif
	tx->tdes1 = cpu_to_le32(0xE1000000 | skb->len);
	dma_wmb();
	tx->tdes0 = cpu_to_le32(0x80000000);
	//tx->tdes0 = cpu_to_le32(DES0_BASE);

	//	printk("%s===>this tx->tdes0:%x\n", __func__, tx->tdes0);
	//	printk("%s===>this tx->tdes1:%x\n", __func__, tx->tdes1);
	//	printk("%s===>this tx->tdes2:%x\n", __func__, tx->tdes2);
	//dma_cache_wback((unsigned long)tx, sizeof(struct tx_desc));

	writel(0x01, tp->ioaddr+CSR1);
	netif_trans_update(dev);
	udelay(1000);

	if (!tp->tx_avail_cnt)
		netif_stop_queue(dev);

	spin_unlock_irqrestore(&tp->lock, flags);

	/* free this SKB */
	//dev_consume_skb_any(skb);

#ifdef DBG_FLAG
	printk("dmfe_start_xmit=============================================>end\n");
#endif
	return NETDEV_TX_OK;
}

#if 0
static void dmfe_reuse_skb(struct net_device *dev, struct sk_buff * skb)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	struct rx_desc *rx = tp->cpu_cur_rx;

	if (!(rx->rdes0 & cpu_to_le32(0x80000000))) {
		rx->skb = skb;
		rx->rdes2 = cpu_to_le32(dma_map_single(&dev->dev, skb->data, RX_BUF_SIZE, DMA_FROM_DEVICE));
#ifdef DBG_FLAG3
		printk("dmfe_reuse_skb===>this rx->rdes2:%x\n", rx->rdes2);
#endif
		wmb();
		rx->rdes0 = cpu_to_le32(0x80000000);

		//dma_cache_wback((unsigned long)rx, sizeof(struct rx_desc));

		tp->rx_avail_cnt++;
		tp->cpu_cur_rx = rx->next_desc;
	} else
		printk("SK Buffer reuse method error:%d\n", tp->rx_avail_cnt);
}
#endif


#if 0
/* Superseded by dmfe_rx_clean(); retained for reference to the vendor RTL. */
static void dmfe_rx_clean2(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	struct rx_desc *rx;
	struct sk_buff *skb,*newskb;
	u32	rdes0;
	u32	rxlen;

#ifdef DBG_FLAG
    printk("dmfe_rx_clean===================================>start\n");
#endif
	rx = tp->mac_cur_rx;
	while(tp->rx_avail_cnt) {
		rdes0 = le32_to_cpu(rx->rdes0);
		if (rdes0 & 0x80000000)	/* packet owner check */
			break;

		tp->rx_avail_cnt--;
		//tp->interval_rx_cnt++;

		dma_unmap_single(&dev->dev, le32_to_cpu(rx->rdes2),
			 RX_BUF_SIZE, DMA_FROM_DEVICE);

		if((rdes0 & 0x300) != 0x300) {
			/* A packet without First/Last flag */
			/* reuse this SKB */
			printk("Reuse SK buffer, rdes0:%x\n", rdes0);
			dmfe_reuse_skb(dev, rx->skb);
		} else {
			/* A packet with First/Last flag */
			rxlen = ( (rdes0 >> 16) & 0x3fff) - 4;

			/* error summary bit check */
			if (rdes0 & 0x8000) {
				/* This is a error packet */
				dev->stats.rx_errors++;
				if (rdes0 & 1)
					dev->stats.rx_fifo_errors++;
				if (rdes0 & 2)
					dev->stats.rx_crc_errors++;
				if (rdes0 & 0x80)
					dev->stats.rx_length_errors++;
			}

			if(!(rdes0 & 0x8000) || ((tp->cr6_data & CR6_PM) && (rxlen>6)) ) {
				skb = rx->skb;

				/* Received Packet CRC check need or not */
				if ( (tp->dm910x_chk_mode & 1) && (cal_CRC(skb->data, rxlen, 1) !=(*(u32 *) (skb->data+rxlen) ))) { /* FIXME (?) */
					//if (cal_CRC(skb->data, rxlen, 1) !=(*(u32 *) (skb->data+rxlen))) {
					/* Found a error received packet */
					dmfe_reuse_skb(dev, rx->skb);
					tp->dm910x_chk_mode = 3;
					printk("found a error packet!\n");
				} else {
					/* Good packet, send to upper layer */
					/* Shorst packet used new SKB */
					if ((rxlen < RX_BUF_SIZE) && ((newskb = netdev_alloc_skb(dev, rxlen + 2))	!= NULL)) {
						skb = newskb;
						/* size less than COPY_SIZE, allocate a rxlen SKB */
						skb_reserve(skb, 2); /* 16byte align */
						skb_copy_from_linear_data(rx->skb, skb_put(skb, rxlen), rxlen);
						dmfe_reuse_skb(dev, rx->skb);
					} else
						skb_put(skb, rxlen);

					skb->protocol = eth_type_trans(skb, dev);
					netif_rx(skb);
					dev->stats.rx_packets++;
					dev->stats.rx_bytes += rxlen;
				}
			} else {
				/* Reuse SKB buffer when the packet is error */
				printk("Reuse SK buffer, rdes0:%x\n",rdes0);
				dmfe_reuse_skb(dev,rx->skb);
			}
		}
		rx = rx->next_desc;
	}
	tp->mac_cur_rx = rx;

#ifdef DBG_FLAG
	printk("dmfe_rx_clean===================================>end\n");
#endif
}
#endif


static void dmfe_rx_clean(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	struct rx_desc *rx;
	struct sk_buff *skb, *new_skb;
	dma_addr_t new_dma;
	u32	rdes0;
	u32	rxlen;

#ifdef DBG_FLAG
printk("dmfe_rx_clean===================================>start\n");
#endif
	//rx = tp->cpu_cur_rx;
	rx = tp->mac_cur_rx;
	rdes0 = le32_to_cpu(rx->rdes0);
#ifdef DBG_FLAG2
	printk("dmfe_rx_clean===================================>rdes0:%x\n",rdes0);
#endif
	while (!(rdes0 & 0x80000000)) {
		if (unlikely(!rx->skb))
			break;

		dma_unmap_single(tp->dma_dev, le32_to_cpu(rx->rdes2),
				 RX_BUF_SIZE, DMA_FROM_DEVICE);
		rxlen = ((rdes0 >> 16) & 0x3FFF) - 4;
#ifdef DBG_FLAG2
		printk("dmfe_rx_clean===================================>rxlen:%d,rdes2:%x,rdes3:%x\n",
		       rxlen, le32_to_cpu(rx->rdes2), le32_to_cpu(rx->rdes3));

#endif
		skb = rx->skb;
		new_skb = NULL;

		if ((rdes0 & 0x300) != 0x300 || rxlen > MAX_PACKET_SIZE) {
			/* A packet without First/Last flag */
			printk("frame too long: %d bytes\n", rxlen);
			tp->stats.rx_length_errors++;
			tp->stats.rx_errors++;
		} else {
			if (rdes0 & 0x8000) {
				tp->stats.rx_errors++;
				if (rdes0 & 0x01) {
					tp->stats.rx_fifo_errors++;
				}
				if (rdes0 & 0x02) {
					tp->stats.rx_crc_errors++;
				}
				if (rdes0 & 0x80) {
					tp->stats.rx_length_errors++;
				}
			}
			if (!(rdes0 & 0x8000)) {
				new_skb = dev_alloc_skb(RX_BUF_SIZE);
				if (new_skb) {
					new_dma = dma_map_single(tp->dma_dev, new_skb->data,
							 RX_BUF_SIZE,
							 DMA_FROM_DEVICE);
					if (dma_mapping_error(tp->dma_dev, new_dma)) {
						dev_kfree_skb(new_skb);
						new_skb = NULL;
					}
				}

				if (new_skb) {
					rx->skb = new_skb;
					rx->rdes2 = cpu_to_le32(new_dma);
					skb->dev = dev;
#ifdef CONFIG_SOC_MAC_HARDWARE_ACCELERATE
					skb_reserve(skb, 2);
#endif
					skb_put(skb, rxlen);
					skb->protocol = eth_type_trans(skb, dev);
					netif_rx(skb);
					tp->stats.rx_packets++;
					tp->stats.rx_bytes += rxlen;
				} else {
					tp->stats.rx_dropped++;
				}
			}
		}

		if (!new_skb) {
			new_dma = dma_map_single(tp->dma_dev, skb->data,
						 RX_BUF_SIZE, DMA_FROM_DEVICE);
			if (dma_mapping_error(tp->dma_dev, new_dma)) {
				netdev_err(dev, "failed to remap an RX buffer\n");
			dev_kfree_skb(skb);
			rx->skb = NULL;
			rx->rdes2 = cpu_to_le32(0);
			rx->rdes0 = cpu_to_le32(0);
			if (tp->rx_avail_cnt)
				tp->rx_avail_cnt--;
			break;
		}
			rx->skb = skb;
			rx->rdes2 = cpu_to_le32(new_dma);
		}

		dma_wmb();
		rx->rdes0 = cpu_to_le32(0x80000000);

	//dma_cache_wback((unsigned long)rx, sizeof(struct rx_desc));

	rx = rx->next_desc;
		rdes0 = le32_to_cpu(rx->rdes0);
	}
	//tp->cpu_cur_rx = rx;
	tp->mac_cur_rx = rx;
#ifdef DBG_FLAG
	printk("dmfe_rx_clean===================================>end\n");
#endif
}


static void dmfe_tx_clean(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	struct tx_desc *tx;
	u32 tdes0;

	tx = tp->mac_cur_tx;

	while (tp->tx_packets) {
		tdes0 = le32_to_cpu(tx->tdes0);
		if (tdes0 & 0x80000000) {
			break;
		}

		tp->stats.tx_packets++;
		if (tdes0 != 0x7FFFFFFF) {
			tp->stats.collisions += (tdes0 >> 3) & 0xF;
			tp->stats.tx_bytes += le32_to_cpu(tx->tdes1) & 0x7FF;
			if (tdes0 & TDES0_ERR_MASK) {
				tp->stats.tx_errors++;
				if (tdes0 & 0x0002) {
					tp->stats.tx_errors++;
					if (!(tp->cr6_data & CR6_SFT)) {
						tp->cr6_data = tp->cr6_data | CR6_SFT;
						update_csr6(tp->cr6_data, tp->ioaddr+CSR6);
					}
				}
				if (tdes0 & 0x0100) {
				// need do something ?
					;
				}
				if (tdes0 & 0x200) {
				// need do something ?
					;
				}
				if (tdes0 & 0x0800) {
				// need do something ?
					;
				}
				if (tdes0 & 0x4000) {
				// need do something ?
					;
				}
			}
		}
		if (tx->skb != NULL) {
			dev_kfree_skb_irq(tx->skb);
			tx->skb = NULL;
		}
		tx = tx->next_desc;
		tp->tx_packets--;
		tp->tx_avail_cnt++;
	}
	tp->mac_cur_tx = tx;
	if (netif_queue_stopped(dev) && tp->tx_avail_cnt)
		netif_wake_queue(dev);
	writel(0x01, tp->ioaddr+CSR1);
	//udelay(1000);
	netif_trans_update(dev);

}

static irqreturn_t dmfe_interrupt (int irq, void *dev_instance)
{
	struct net_device 	*dev = (struct net_device *)dev_instance;
	struct dmfe_private 	*tp = netdev_priv(dev);
	unsigned long 	flags;
	int handle = IRQ_NONE;

	(void)irq;

	spin_lock_irqsave(&tp->lock, flags);

	tp->cr5_data =  dr32(CSR5); //readl(tp->ioaddr+CSR5);
	dw32(CSR5, tp->cr5_data);   //writel(tp->cr5_data, tp->ioaddr+CSR5);

	if (! (tp->cr5_data & 0xC1)) {
		//spin_unlock_irqrestore(&tp->lock, flags);
		//return IRQ_HANDLED;
	}

	//writel(0, ioaddr + CSR7);
	//dw32(CSR7, 0);
	iowrite32(0,tp->ioaddr + CSR7);
	if (tp->cr5_data & 0x2000) {
		printk("dmfe_interrupt--->System bus error happen.cr5 = %d\n",tp->cr5_data);
		spin_unlock_irqrestore(&tp->lock, flags);
		return IRQ_HANDLED;
	}

	if (tp->cr5_data & 0x40) {
		dmfe_rx_clean(dev);
	}

	if (tp->cr5_data & 0x01) {
		dmfe_tx_clean(dev);
	}

	/* Mode Check */
	if (tp->dm910x_chk_mode & 0x2) {
		tp->dm910x_chk_mode = 0x4;
		tp->cr6_data |= 0x100;
		update_csr6(tp->cr6_data, tp->ioaddr + CSR6);
	}

	handle = IRQ_HANDLED;
	//writel(tp->cr7_data, ioaddr + CSR7);
	//dw32(CSR7, tp->cr7_data);
	iowrite32(tp->cr7_data,tp->ioaddr + CSR7);

#ifdef DBG_FLAG
	printk("dmfe_interrupt===================================>end\n");
#endif
	spin_unlock_irqrestore(&tp->lock, flags);

	return handle;
}


static struct net_device_stats *dmfe_get_stats(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	return &tp->stats;
}


static void send_filter_frame(struct net_device *dev,int mc_cnt)
{
	struct dmfe_private *tp = netdev_priv(dev);
	struct netdev_hw_addr *ha;
	struct tx_desc *tx;
	u16 * addrptr;
	u32 * suptr;
	int i;

	if (!tp->tx_avail_cnt) {
		netdev_warn(dev, "no descriptor available for setup frame\n");
		return;
	}

	tx = tp->cpu_cur_tx;
	suptr = (u32 *) tx->tx_buf_ptr;

	/* Node address */
	addrptr = (u16 *) dev->dev_addr;
	*suptr++ = addrptr[0];
	*suptr++ = addrptr[1];
	*suptr++ = addrptr[2];

	/* broadcast address */
	*suptr++ = 0xffff;
	*suptr++ = 0xffff;
	*suptr++ = 0xffff;

	/* fit the multicast address */
	netdev_for_each_mc_addr(ha, dev) {
		addrptr = (u16 *) ha->addr;
		*suptr++ = addrptr[0];
		*suptr++ = addrptr[1];
		*suptr++ = addrptr[2];
	}

	for (i = netdev_mc_count(dev); i < 14; i++) {
		*suptr++ = 0xffff;
		*suptr++ = 0xffff;
		*suptr++ = 0xffff;
	}

	/* prepare the setup frame */
	tp->cpu_cur_tx = tx->next_desc;
	tp->tx_avail_cnt--;
	tx->tdes1 = cpu_to_le32(0x890000c0);

	/* Resource Empty */
	tp->tx_packets++;
	dma_wmb();
	tx->tdes0 = cpu_to_le32(0x80000000);

	//dma_cache_wback((unsigned long)tx, sizeof(struct tx_desc));

	update_csr6(tp->cr6_data | 0x2000, tp->ioaddr + CSR6);
	dw32(CSR1, 0x1);	/* Issue Tx polling */
	update_csr6(tp->cr6_data, tp->ioaddr + CSR6);
	netif_trans_update(dev);
	for (i = 0; i < TOUT_LOOP; i++) {
		if (!(le32_to_cpu(tx->tdes0) & 0x80000000))
			return;
		udelay(1);
	}

	netdev_err(dev, "setup frame transmission timed out\n");
}

#if 0
/* Superseded by send_filter_frame(); retained for reference to the vendor RTL. */
static void send_filter_frame2(struct net_device *dev, int mc_cnt)
{
	struct dmfe_private *tp = netdev_priv(dev);
	struct sk_buff *skb;
	//struct dev_mc_list *mc;
	struct netdev_hw_addr *ha;
	struct tx_desc	*tx;
	u8 *ptr;
	int i;

#ifdef DBG_FLAG
	printk("send_filter_frame=================================>,tp addr:%x\n",tp);
#endif
	skb = dev_alloc_skb(MAX_PACKET_SIZE);
	if (skb == NULL) {
		printk("send filter frame failed\n");
		return;
	}
	ptr = skb->data;
	memcpy(ptr, dev->dev_addr, ETH_ALEN);
	ptr += ETH_ALEN;
	memset(ptr, 0xFF, ETH_ALEN);
	ptr += ETH_ALEN;

	netdev_for_each_mc_addr(ha, dev){
		memcpy(ptr, ha->addr, ETH_ALEN);
		ptr += ETH_ALEN;
	}

	/*for (mc = dev->mc_list, i = 0; i < mc_cnt; i++, mc = mc->next) {
		memcpy(ptr, mc->dmi_addr, ETH_ALEN);
		ptr += ETH_ALEN;
	}*/
	memset(ptr, 0xFF, 28);

	tx = tp->cpu_cur_tx;
	tp->cpu_cur_tx = tx->next_desc;
	tx->tdes2 = cpu_to_le32(dma_map_single(&dev->dev, skb->data, RX_BUF_SIZE, DMA_TO_DEVICE));
	tx->tdes1 = cpu_to_le32(0x890000C0);
	tx->tdes0 = cpu_to_le32(0x80000000);
	update_csr6(tp->cr6_data | 0x2000, tp->ioaddr);
	dw32(CSR1, 0x1);	/* Issue Tx polling */
	update_csr6(tp->cr6_data, tp->ioaddr);
	netif_trans_update(dev);
	udelay(1000);

	/* wait for sending */
	/*
	i = 0;
	while ((tx->tdes0 & 0x80000000) && (i<TOUT_LOOP)) {
		udelay(1000);
		i++;
	printk("tx->tdes0 value is %x", tx->tdes0);
	}
	*/
	dev_kfree_skb(skb);
}
#endif


static void dmfe_timer(struct timer_list *t)
{
	struct dmfe_private *tp = timer_container_of(tp, t, timer);
	struct net_device *dev = tp->ndev;
	unsigned char 		tmp_cr12;
	unsigned long 		flags;
	int			link_status;
#ifdef DBG_FLAG
	u32			csr5_val, csr6_val, csr7_val, csr8_val;
#endif

	spin_lock_irqsave(&tp->lock, flags);
	phy_read(tp->ioaddr, tp->phy_addr, 0x01, tp->chip_id);
	link_status = phy_read(tp->ioaddr, tp->phy_addr, 0x01, tp->chip_id) & 0x4;
	tmp_cr12 = link_status ? 0x3 : 0;
#ifdef DBG_FLAG
	csr5_val = dr32(CSR5);
	csr6_val = dr32(CSR6);
	csr7_val = dr32(CSR7);
	csr8_val = dr32(CSR8);
	printk("dmfe_timer===>start,CRS5:%x,CRS6:%x,CRS7:%x,CRS8:%x,tp->rx_avail_cnt:%d\n",csr5_val,csr6_val,csr7_val,csr8_val,tp->rx_avail_cnt);
	printk("dmfe_timer===>start,link_status:%x,tmp_cr12:%x,tp->phy_addr:%x,tp->link_failed:%d\n",link_status,tmp_cr12,tp->phy_addr,tp->link_failed);
#endif

	/* Operating Mode Check */
	if ( (tp->dm910x_chk_mode & 0x1) && (dev->stats.rx_packets > MAX_CHECK_PACKET) )
		tp->dm910x_chk_mode = 0x4;

	if ( (!(tmp_cr12 & 0x3)) && (!tp->link_failed) ) {
		/* Link Failed */
		printk("dev %x:Link Failed %x\n", tp->phy_addr, link_status);
		tp->link_failed = 1;

		/* For Force 10/100M Half/Full mode: Enable Auto-Nego mode */
		/* AUTO or force 1M Homerun/Longrun don't need */
		if ( !(tp->media_mode & 0x38) )
			phy_write(tp->ioaddr, tp->phy_addr, 0, 0x1000, tp->chip_id);

		/* AUTO mode */
		if (tp->media_mode & DMFE_AUTO) {
			/* 10/100M link failed */
			tp->cr6_data&=~0x00000200;      /* bit9=0, HD mode */
			update_csr6(tp->cr6_data, tp->ioaddr);
		}
	} else if ((tmp_cr12 & 0x3) && tp->link_failed) {
		printk("dev %x:Link OK %x", tp->phy_addr,link_status);
		tp->link_failed = 0;

		/* Auto Sense Speed */
		if ((tp->media_mode & DMFE_AUTO) && dmfe_sense_speed(dev) )
			tp->link_failed = 1;
		dmfe_process_mode(dev);
		SHOW_MEDIA_TYPE(tp->op_mode);
	}

	spin_unlock_irqrestore(&tp->lock, flags);
	mod_timer(&tp->timer, DMFE_TIMER_WUT + HZ * 2);

#ifdef DBG_FLAG
    printk("dmfe_timer===================================>end\n");
#endif
}

/*
 * update CSR6, firstly stop it then write new value and start.
 */
static void update_csr6(u32 val, void *ioaddr)
{
	writel((val & (~0x2002)), ioaddr);
	udelay(5);
	//writel((val | 0x2002), ioaddr);
	writel((val | 0x602002), ioaddr);

	udelay(5);
}

static u8 dmfe_sense_speed(struct net_device *dev)
{
	struct dmfe_private *tp = netdev_priv(dev);
	u8 ErrFlag = 0;
	u16 phy_mode0,phy_mode1,phy_mode25;

	phy_mode0 = phy_read(tp->ioaddr, tp->phy_addr, 0, tp->chip_id);
	phy_mode1 = phy_read(tp->ioaddr, tp->phy_addr, 1, tp->chip_id);
	phy_mode25 = phy_read(tp->ioaddr, tp->phy_addr, 25, tp->chip_id);

	if ( (phy_mode0 & 0x1000)&& (phy_mode1&0x0020)) {
		switch ((phy_mode25&3)|(phy_mode0&0x100)) {
		case 0x002:
			tp->op_mode = DMFE_10MHF;
			break;
		case 0x102:
			tp->op_mode = DMFE_10MFD;
			break;
		case 0x001:
			tp->op_mode = DMFE_100MHF;
			break;
		case 0x101:
			tp->op_mode = DMFE_100MFD;
			break;
		default:
			tp->op_mode = DMFE_100MHF;
			ErrFlag = 1;
			break;
		}
	} else {
		tp->op_mode = DMFE_100MHF;
		ErrFlag = 1;
	}
	return ErrFlag;
}
/*
 *  Set 10/100 phyxcer capability
 *  AUTO mode : phyxcer register4 is NIC capability
 *  Force mode: phyxcer register4 is the force media
 **/
static void dmfe_set_phyxcer(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);
	u16 	phy_reg;
	int 	i = 0;
	/* restart auto negotion */
	phy_reg = phy_read(tp->ioaddr, tp->phy_addr, 0, tp->chip_id);
	phy_write(tp->ioaddr, tp->phy_addr, 0, 0x200|phy_reg, tp->chip_id);

	/* Phyxcer capability setting */
	do {
		i++;
		phy_reg = phy_read(tp->ioaddr, tp->phy_addr, 1, tp->chip_id);
	} while (((phy_reg & 0x20) == 0) && (i < 10000));

	phy_reg = phy_read(tp->ioaddr, tp->phy_addr, 4, tp->chip_id);
	phy_reg = phy_read(tp->ioaddr, tp->phy_addr, 4, tp->chip_id) & ~0x01e0;

	if (tp->media_mode & DMFE_AUTO) {
		/* AUTO Mode */
		phy_reg |= tp->PHY_reg4;
	} else {
		/* Force Mode */
		switch(tp->media_mode) {
		case DMFE_10MHF:
			phy_reg |= 0x20;
			break;
		case DMFE_10MFD:
			phy_reg |= 0x40;
			break;
		case DMFE_100MHF:
			phy_reg |= 0x80;
			break;
		case DMFE_100MFD:
			phy_reg |= 0x100;
			break;
		}
	}

	/* Write new capability to Phyxcer Reg4 */
	if ( !(phy_reg & 0x01e0)) {
		phy_reg|=tp->PHY_reg4;
		tp->media_mode|=DMFE_AUTO;
	}
	phy_write(tp->ioaddr, tp->phy_addr, 4, phy_reg, tp->chip_id);

#if 0
	phy_reg = phy_read(tp->ioaddr, tp->phy_addr, 0, tp->chip_id);
	phy_reg &= ~(1 << 12); /* to close th auto negotiation */
	phy_reg |= 1 << 8; /* Full duplex mode */
	phy_reg &= ~(1 << 13); /* 10Mbps mode */
#endif
	/* Restart Auto-Negotiation */
	phy_write(tp->ioaddr, tp->phy_addr, 0, 0x1200, tp->chip_id);

}

/*
 *	Process op-mode
 *	AUTO mode : PHY controller in Auto-negotiation Mode
 *	Force mode: PHY controller in force mode with HUB
 *			N-way force capability with SWITCH
 */

static void dmfe_process_mode(struct net_device *dev)
{
	struct dmfe_private 	*tp = netdev_priv(dev);

	/* Full Duplex Mode Check */
	if (tp->op_mode & 0x4)
		tp->cr6_data |= CR6_FDM;	/* Set Full Duplex Bit */
	else
		tp->cr6_data &= ~CR6_FDM;	/* Clear Full Duplex Bit */

	update_csr6(tp->cr6_data, tp->ioaddr+CSR6);
}


/*
 *	Write a word to Phy register
 */

static void phy_write(void *iobase, u8 phy_addr, u8 offset, u16 phy_data, u32 chip_id)
{
	u16 i;
	void *ioaddr;

	if (chip_id == PCI_DM9132_ID) {
		ioaddr = iobase + 0x80 + offset * 4;
		writew(phy_data, ioaddr);
	} else {
		/* DM9102/DM9102A Chip */
		ioaddr = iobase + CSR9;

		/* Send 33 synchronization clock to Phy controller */
		for (i = 0; i < 35; i++)
			phy_write_1bit(ioaddr, PHY_DATA_1);

		/* Send start command(01) to Phy */
		phy_write_1bit(ioaddr, PHY_DATA_0);
		phy_write_1bit(ioaddr, PHY_DATA_1);

		/* Send write command(01) to Phy */
		phy_write_1bit(ioaddr, PHY_DATA_0);
		phy_write_1bit(ioaddr, PHY_DATA_1);

		/* Send Phy addres */
		for (i = 0x10; i > 0; i = i >> 1)
			phy_write_1bit(ioaddr, phy_addr & i ? PHY_DATA_1 : PHY_DATA_0);

		/* Send register addres */
		for (i = 0x10; i > 0; i = i >> 1)
			phy_write_1bit(ioaddr, offset & i ? PHY_DATA_1 : PHY_DATA_0);

		/* written trasnition */
		phy_write_1bit(ioaddr, PHY_DATA_1);
		phy_write_1bit(ioaddr, PHY_DATA_0);

		/* Write a word data to PHY controller */
		for ( i = 0x8000; i > 0; i >>= 1)
			phy_write_1bit(ioaddr, phy_data & i ? PHY_DATA_1 : PHY_DATA_0);
	}
}


/*
 *	Read a word data from phy register
 */

static u16 phy_read(void *iobase, u8 phy_addr, u8 offset, u32 chip_id)
{
	int i;
	u16 phy_data;
	void *ioaddr;

	if (chip_id == PCI_DM9132_ID) {
		/* DM9132 Chip */
		ioaddr = iobase + 0x80 + offset * 4;
		phy_data = readw(ioaddr);
	} else {
		/* DM9102/DM9102A Chip */
		ioaddr = iobase + CSR9;

		/* Send 33 synchronization clock to Phy controller */
		for (i = 0; i < 35; i++)
			phy_write_1bit(ioaddr, PHY_DATA_1);

		/* Send start command(01) to Phy */
		phy_write_1bit(ioaddr, PHY_DATA_0);
		phy_write_1bit(ioaddr, PHY_DATA_1);

		/* Send read command(10) to Phy */
		phy_write_1bit(ioaddr, PHY_DATA_1);
		phy_write_1bit(ioaddr, PHY_DATA_0);

		/* Send Phy addres */
		for (i = 0x10; i > 0; i = i >> 1)
			phy_write_1bit(ioaddr, phy_addr & i ? PHY_DATA_1 : PHY_DATA_0);

		/* Send register addres */
		for (i = 0x10; i > 0; i = i >> 1)
			phy_write_1bit(ioaddr, offset & i ? PHY_DATA_1 : PHY_DATA_0);

		/* Skip transition state */
		phy_read_1bit(ioaddr);

		/* read 16bit data */
		for (phy_data = 0, i = 0; i < 16; i++) {
			phy_data <<= 1;
			phy_data |= phy_read_1bit(ioaddr);
		}
	}

	return phy_data;
}

static void phy_write_1bit(void *ioaddr, u32 phy_data)
{
	phy_data |=1<<18;
	writel(phy_data, ioaddr);			/* MII Clock Low */
	readl(ioaddr);
	udelay(1);
	writel(phy_data | MDCLKH, ioaddr);	/* MII Clock High */
	readl(ioaddr);
	udelay(1);
	writel(phy_data, ioaddr);			/* MII Clock Low */
	readl(ioaddr);
	udelay(1);
}


/*
 *	Read one bit phy data from PHY controller
 */

static u16 phy_read_1bit(void *ioaddr)
{
	u16 phy_data;

	writel(0x10000, ioaddr);
	readl(ioaddr);
	udelay(1);
	phy_data = ( readl(ioaddr) >> 19 ) & 0x1;
	writel(0x00000, ioaddr);
	readl(ioaddr);
	udelay(1);

	return phy_data;
}

static void dmfe_remove_one(struct net_device 	*dev)
{
	unregister_netdev(dev);
	free_netdev(dev);
}

static int dmfe_pltfr_probe(struct platform_device *pdev)
{
	void __iomem *addr;
	int irq;
	struct net_device *ndev;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "32-bit DMA is unavailable\n");

	addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(addr))
		return PTR_ERR(addr);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ndev = dmfe_init_one(&pdev->dev, addr, irq);
	if (!ndev)
		return -ENOMEM;

	platform_set_drvdata(pdev, ndev);
	return 0;
}

static void dmfe_pltfr_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);

	dmfe_remove_one(ndev);
}

#ifdef CONFIG_OF
static const struct of_device_id ls_dmfe_dt_match[] = {
	{ .compatible = "loongson,chiplab-dmfe", },
	{ .compatible = "dmfe", },
	{},
};
MODULE_DEVICE_TABLE(of, ls_dmfe_dt_match);
#endif

static struct platform_driver dmfe_driver = {
	.probe = dmfe_pltfr_probe,
	.remove = dmfe_pltfr_remove,
	.driver = {
		.name = "dmfe",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(ls_dmfe_dt_match),
#endif
	},
};

static int __init dmfe_init_module(void)
{
	printk("ITC MAC 10/100M Fast Ethernet Adapter driver 1.0 init\n");

	return platform_driver_register(&dmfe_driver);
}

static void __exit dmfe_cleanup_module(void)
{
	printk("ITC MAC 10/100M Fast Ethernet Adapter driver 1.0 init cleanup\n");
	platform_driver_unregister(&dmfe_driver);
}

module_init(dmfe_init_module);
module_exit(dmfe_cleanup_module);

MODULE_DESCRIPTION("Chiplab platform DMFE Ethernet driver");
MODULE_LICENSE("GPL");
