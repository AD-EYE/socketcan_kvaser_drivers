// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* Copyright (C) 2018 KVASER AB, Sweden. All rights reserved.
 * Parts of this driver are based on the following:
 *  - Kvaser linux pciefd driver (version 5.41)
 *  - PEAK linux canfd driver
 */

#include <linux/version.h>
#include <linux/can/dev.h>
#include <linux/device.h>
#include <linux/ethtool.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
#include <linux/minmax.h>
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION 5.10.0 */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/timer.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))
#define netdev_info_once(dev, fmt, ...) netdev_info(dev, fmt, ##__VA_ARGS__)
#endif /* LINUX_VERSION_CODE < 4.15 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0))
/* drop skb if it does not contain a valid CAN frame for sending */
static inline bool can_dev_dropped_skb(struct net_device *dev, struct sk_buff *skb)
{
	struct can_priv *priv = netdev_priv(dev);

	if (priv->ctrlmode & CAN_CTRLMODE_LISTENONLY) {
		netdev_info_once(dev,
				 "interface in listen only mode, dropping skb\n");
		kfree_skb(skb);
		dev->stats.tx_dropped++;
		return true;
	}

	return can_dropped_invalid_skb(dev, skb);
}
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION 6.1.0 */
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Kvaser AB <support@kvaser.com>");
MODULE_DESCRIPTION("CAN driver for Kvaser CAN/PCIe devices");

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0))
#define CAN_ERR_CNT 0
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION 6.0.0 */
#define KVASER_PCIEFD_DRV_NAME "kvaser_pciefd"

#define KVASER_PCIEFD_WAIT_TIMEOUT msecs_to_jiffies(1000)
#define KVASER_PCIEFD_BEC_POLL_FREQ (jiffies + msecs_to_jiffies(200))
#define KVASER_PCIEFD_MAX_ERR_REP 256U
#define KVASER_PCIEFD_CAN_TX_MAX_COUNT 17U
#define KVASER_PCIEFD_MAX_CAN_CHANNELS 4U
#define KVASER_PCIEFD_DMA_COUNT 2U

#define KVASER_PCIEFD_DMA_SIZE (4U * 1024U)
#define KVASER_PCIEFD_64BIT_DMA_BIT BIT(0)

#define KVASER_PCIEFD_VENDOR 0x1a07
/* Altera based devices */
#define KVASER_PCIEFD_4HS_DEVICE_ID 0x000d
#define KVASER_PCIEFD_2HS_V2_DEVICE_ID 0x000e
#define KVASER_PCIEFD_HS_V2_DEVICE_ID 0x000f
#define KVASER_PCIEFD_MINIPCIE_HS_V2_DEVICE_ID 0x0010
#define KVASER_PCIEFD_MINIPCIE_2HS_V2_DEVICE_ID 0x0011

/* SmartFusion2 based devices */
#define KVASER_PCIEFD_2CAN_V3_DEVICE_ID 0x0012
#define KVASER_PCIEFD_1CAN_V3_DEVICE_ID 0x0013
#define KVASER_PCIEFD_4CAN_V2_DEVICE_ID 0x0014
#define KVASER_PCIEFD_MINIPCIE_2CAN_V3_DEVICE_ID 0x0015
#define KVASER_PCIEFD_MINIPCIE_1CAN_V3_DEVICE_ID 0x0016

/* Kvaser KCAN CAN controller registers */
#define KVASER_PCIEFD_KCAN_FIFO_REG 0x100
#define KVASER_PCIEFD_KCAN_FIFO_LAST_REG 0x180
#define KVASER_PCIEFD_KCAN_CTRL_REG 0x2c0
#define KVASER_PCIEFD_KCAN_CMD_REG 0x400
#define KVASER_PCIEFD_KCAN_IEN_REG 0x408
#define KVASER_PCIEFD_KCAN_IRQ_REG 0x410
#define KVASER_PCIEFD_KCAN_TX_NR_PACKETS_REG 0x414
#define KVASER_PCIEFD_KCAN_STAT_REG 0x418
#define KVASER_PCIEFD_KCAN_MODE_REG 0x41c
#define KVASER_PCIEFD_KCAN_BTRN_REG 0x420
#define KVASER_PCIEFD_KCAN_BUS_LOAD_REG 0x424
#define KVASER_PCIEFD_KCAN_BTRD_REG 0x428
#define KVASER_PCIEFD_KCAN_PWM_REG 0x430
/* System identification and information registers */
#define KVASER_PCIEFD_SYSID_VERSION_REG 0x8
#define KVASER_PCIEFD_SYSID_CANFREQ_REG 0xc
#define KVASER_PCIEFD_SYSID_BUSFREQ_REG 0x10
#define KVASER_PCIEFD_SYSID_BUILD_REG 0x14
/* Shared receive buffer FIFO registers */
#define KVASER_PCIEFD_SRB_FIFO_LAST_REG 0x1f4
/* Shared receive buffer registers */
#define KVASER_PCIEFD_SRB_CMD_REG 0x0
#define KVASER_PCIEFD_SRB_IEN_REG 0x04
#define KVASER_PCIEFD_SRB_IRQ_REG 0x0c
#define KVASER_PCIEFD_SRB_STAT_REG 0x10
#define KVASER_PCIEFD_SRB_RX_NR_PACKETS_REG 0x14
#define KVASER_PCIEFD_SRB_CTRL_REG 0x18

#define KVASER_PCIEFD_SYSID_VERSION_NRCHAN_SHIFT 24
#define KVASER_PCIEFD_SYSID_VERSION_MAJOR_SHIFT 16
#define KVASER_PCIEFD_SYSID_BUILD_SHIFT 1

/* Reset DMA buffer 0, 1 and FIFO offset */
#define KVASER_PCIEFD_SRB_CMD_RDB0 BIT(4)
#define KVASER_PCIEFD_SRB_CMD_RDB1 BIT(5)
#define KVASER_PCIEFD_SRB_CMD_FOR BIT(0)

/* DMA packet done, buffer 0 and 1 */
#define KVASER_PCIEFD_SRB_IRQ_DPD0 BIT(8)
#define KVASER_PCIEFD_SRB_IRQ_DPD1 BIT(9)
/* DMA overflow, buffer 0 and 1 */
#define KVASER_PCIEFD_SRB_IRQ_DOF0 BIT(10)
#define KVASER_PCIEFD_SRB_IRQ_DOF1 BIT(11)
/* DMA underflow, buffer 0 and 1 */
#define KVASER_PCIEFD_SRB_IRQ_DUF0 BIT(12)
#define KVASER_PCIEFD_SRB_IRQ_DUF1 BIT(13)

/* DMA idle */
#define KVASER_PCIEFD_SRB_STAT_DI BIT(15)
/* DMA support */
#define KVASER_PCIEFD_SRB_STAT_DMA BIT(24)

/* SRB current packet level */
#define KVASER_PCIEFD_SRB_RX_NR_PACKETS_CURRENT_MASK 0xff

/* DMA Enable */
#define KVASER_PCIEFD_SRB_CTRL_DMA_ENABLE BIT(0)

/* Kvaser KCAN definitions */
#define KVASER_PCIEFD_KCAN_CTRL_EFLUSH (4 << 29)
#define KVASER_PCIEFD_KCAN_CTRL_EFRAME (5 << 29)

#define KVASER_PCIEFD_KCAN_CMD_SEQ_SHIFT 16
/* Request status packet */
#define KVASER_PCIEFD_KCAN_CMD_SRQ BIT(0)
/* Abort, flush and reset */
#define KVASER_PCIEFD_KCAN_CMD_AT BIT(1)

/* Tx FIFO unaligned read */
#define KVASER_PCIEFD_KCAN_IRQ_TAR BIT(0)
/* Tx FIFO unaligned end */
#define KVASER_PCIEFD_KCAN_IRQ_TAE BIT(1)
/* Bus parameter protection error */
#define KVASER_PCIEFD_KCAN_IRQ_BPP BIT(2)
/* FDF bit when controller is in classic mode */
#define KVASER_PCIEFD_KCAN_IRQ_FDIC BIT(3)
/* Rx FIFO overflow */
#define KVASER_PCIEFD_KCAN_IRQ_ROF BIT(5)
/* Abort done */
#define KVASER_PCIEFD_KCAN_IRQ_ABD BIT(13)
/* Tx buffer flush done */
#define KVASER_PCIEFD_KCAN_IRQ_TFD BIT(14)
/* Tx FIFO overflow */
#define KVASER_PCIEFD_KCAN_IRQ_TOF BIT(15)
/* Tx FIFO empty */
#define KVASER_PCIEFD_KCAN_IRQ_TE BIT(16)
/* Transmitter unaligned */
#define KVASER_PCIEFD_KCAN_IRQ_TAL BIT(17)

#define KVASER_PCIEFD_KCAN_TX_NR_PACKETS_MAX_SHIFT 16

#define KVASER_PCIEFD_KCAN_STAT_SEQNO_SHIFT 24
/* Abort request */
#define KVASER_PCIEFD_KCAN_STAT_AR BIT(7)
/* Idle state. Controller in reset mode and no abort or flush pending */
#define KVASER_PCIEFD_KCAN_STAT_IDLE BIT(10)
/* Bus off */
#define KVASER_PCIEFD_KCAN_STAT_BOFF BIT(11)
/* Reset mode request */
#define KVASER_PCIEFD_KCAN_STAT_RMR BIT(14)
/* Controller in reset mode */
#define KVASER_PCIEFD_KCAN_STAT_IRM BIT(15)
/* Controller got one-shot capability */
#define KVASER_PCIEFD_KCAN_STAT_CAP BIT(16)
/* Controller got CAN FD capability */
#define KVASER_PCIEFD_KCAN_STAT_FD BIT(19)
#define KVASER_PCIEFD_KCAN_STAT_BUS_OFF_MASK                         \
	(KVASER_PCIEFD_KCAN_STAT_AR | KVASER_PCIEFD_KCAN_STAT_BOFF | \
	 KVASER_PCIEFD_KCAN_STAT_RMR | KVASER_PCIEFD_KCAN_STAT_IRM)

/* Reset mode */
#define KVASER_PCIEFD_KCAN_MODE_RM BIT(8)
/* Listen only mode */
#define KVASER_PCIEFD_KCAN_MODE_LOM BIT(9)
/* Error packet enable */
#define KVASER_PCIEFD_KCAN_MODE_EPEN BIT(12)
/* CAN FD non-ISO */
#define KVASER_PCIEFD_KCAN_MODE_NIFDEN BIT(15)
/* Acknowledgment packet type */
#define KVASER_PCIEFD_KCAN_MODE_APT BIT(20)
/* Active error flag enable. Clear to force error passive */
#define KVASER_PCIEFD_KCAN_MODE_EEN BIT(23)
/* Classic CAN mode */
#define KVASER_PCIEFD_KCAN_MODE_CCM BIT(31)

#define KVASER_PCIEFD_KCAN_BTRN_SJW_SHIFT 13
#define KVASER_PCIEFD_KCAN_BTRN_TSEG1_SHIFT 17
#define KVASER_PCIEFD_KCAN_BTRN_TSEG2_SHIFT 26

#define KVASER_PCIEFD_KCAN_PWM_TOP_SHIFT 16

/* Kvaser KCAN packet types */
#define KVASER_PCIEFD_PACK_TYPE_DATA 0
#define KVASER_PCIEFD_PACK_TYPE_ACK 1
#define KVASER_PCIEFD_PACK_TYPE_TXRQ 2
#define KVASER_PCIEFD_PACK_TYPE_ERROR 3
#define KVASER_PCIEFD_PACK_TYPE_EFLUSH_ACK 4
#define KVASER_PCIEFD_PACK_TYPE_EFRAME_ACK 5
#define KVASER_PCIEFD_PACK_TYPE_ACK_DATA 6
#define KVASER_PCIEFD_PACK_TYPE_STATUS 8
#define KVASER_PCIEFD_PACK_TYPE_BUS_LOAD 9

/* Kvaser KCAN packet common definitions */
#define KVASER_PCIEFD_PACKET_SEQ_MASK 0xff
#define KVASER_PCIEFD_PACKET_CHID_SHIFT 25
#define KVASER_PCIEFD_PACKET_TYPE_SHIFT 28

/* Kvaser KCAN TDATA and RDATA first word */
#define KVASER_PCIEFD_RPACKET_IDE BIT(30)
#define KVASER_PCIEFD_RPACKET_RTR BIT(29)
/* Kvaser KCAN TDATA and RDATA second word */
#define KVASER_PCIEFD_RPACKET_ESI BIT(13)
#define KVASER_PCIEFD_RPACKET_BRS BIT(14)
#define KVASER_PCIEFD_RPACKET_FDF BIT(15)
#define KVASER_PCIEFD_RPACKET_DLC_SHIFT 8
/* Kvaser KCAN TDATA second word */
#define KVASER_PCIEFD_TPACKET_SMS BIT(16)
#define KVASER_PCIEFD_TPACKET_AREQ BIT(31)

/* Kvaser KCAN APACKET */
#define KVASER_PCIEFD_APACKET_FLU BIT(8)
#define KVASER_PCIEFD_APACKET_CT BIT(9)
#define KVASER_PCIEFD_APACKET_ABL BIT(10)
#define KVASER_PCIEFD_APACKET_NACK BIT(11)

/* Kvaser KCAN SPACK first word */
#define KVASER_PCIEFD_SPACK_RXERR_SHIFT 8
#define KVASER_PCIEFD_SPACK_BOFF BIT(16)
#define KVASER_PCIEFD_SPACK_IDET BIT(20)
#define KVASER_PCIEFD_SPACK_IRM BIT(21)
#define KVASER_PCIEFD_SPACK_RMCD BIT(22)
/* Kvaser KCAN SPACK second word */
#define KVASER_PCIEFD_SPACK_AUTO BIT(21)
#define KVASER_PCIEFD_SPACK_EWLR BIT(23)
#define KVASER_PCIEFD_SPACK_EPLR BIT(24)

/* Kvaser KCAN_EPACK second word */
#define KVASER_PCIEFD_EPACK_DIR_TX BIT(0)

/* Macros for calculating addresses of registers */
#define KVASER_PCIEFD_GET_BLOCK_ADDR(pcie, block) \
	((pcie)->reg_base + (pcie)->driver_data->address_offset->block)
#define KVASER_PCIEFD_PCI_IEN_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), pci_ien))
#define KVASER_PCIEFD_PCI_IRQ_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), pci_irq))
#define KVASER_PCIEFD_SERDES_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), serdes))
#define KVASER_PCIEFD_SYSID_VERSION_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), sysid) + KVASER_PCIEFD_SYSID_VERSION_REG)
#define KVASER_PCIEFD_SYSID_CANFREQ_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), sysid) + KVASER_PCIEFD_SYSID_CANFREQ_REG)
#define KVASER_PCIEFD_SYSID_BUSFREQ_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), sysid) + KVASER_PCIEFD_SYSID_BUSFREQ_REG)
#define KVASER_PCIEFD_SYSID_BUILD_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), sysid) + KVASER_PCIEFD_SYSID_BUILD_REG)
#define KVASER_PCIEFD_SRB_FIFO_LAST_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_srb_fifo) + KVASER_PCIEFD_SRB_FIFO_LAST_REG)
#define KVASER_PCIEFD_SRB_CMD_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_srb) + KVASER_PCIEFD_SRB_CMD_REG)
#define KVASER_PCIEFD_SRB_IEN_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_srb) + KVASER_PCIEFD_SRB_IEN_REG)
#define KVASER_PCIEFD_SRB_IRQ_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_srb) + KVASER_PCIEFD_SRB_IRQ_REG)
#define KVASER_PCIEFD_SRB_STAT_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_srb) + KVASER_PCIEFD_SRB_STAT_REG)
#define KVASER_PCIEFD_SRB_RX_NR_PACKETS_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_srb) + KVASER_PCIEFD_SRB_RX_NR_PACKETS_REG)
#define KVASER_PCIEFD_SRB_CTRL_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_srb) + KVASER_PCIEFD_SRB_CTRL_REG)
#define KVASER_PCIEFD_KCAN_CH0_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_ch0))
#define KVASER_PCIEFD_KCAN_CH1_ADDR(pcie) \
	(KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), kcan_ch1))

/* Macros for calculating addresses of Kvaser KCAN registers */
#define KVASER_PCIEFD_KCAN_FIFO_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_FIFO_REG)
#define KVASER_PCIEFD_KCAN_FIFO_LAST_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_FIFO_LAST_REG)
#define KVASER_PCIEFD_KCAN_CTRL_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_CTRL_REG)
#define KVASER_PCIEFD_KCAN_CMD_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_CMD_REG)
#define KVASER_PCIEFD_KCAN_IEN_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_IEN_REG)
#define KVASER_PCIEFD_KCAN_IRQ_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_IRQ_REG)
#define KVASER_PCIEFD_KCAN_TX_NR_PACKETS_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_TX_NR_PACKETS_REG)
#define KVASER_PCIEFD_KCAN_STAT_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_STAT_REG)
#define KVASER_PCIEFD_KCAN_MODE_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_MODE_REG)
#define KVASER_PCIEFD_KCAN_BTRN_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_BTRN_REG)
#define KVASER_PCIEFD_KCAN_BUS_LOAD_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_BUS_LOAD_REG)
#define KVASER_PCIEFD_KCAN_BTRD_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_BTRD_REG)
#define KVASER_PCIEFD_KCAN_PWM_ADDR(can) \
	((can)->reg_base + KVASER_PCIEFD_KCAN_PWM_REG)

/* Macros for reading and writing registers */
#define KVASER_PCIEFD_PCI_IEN_SET(pcie, value) \
	(iowrite32((value), KVASER_PCIEFD_PCI_IEN_ADDR((pcie))))
#define KVASER_PCIEFD_PCI_IRQ_GET(pcie) \
	(ioread32(KVASER_PCIEFD_PCI_IRQ_ADDR((pcie))))
#define KVASER_PCIEFD_SYSID_VERSION_NUM_CHANNELS_GET(pcie)      \
	((ioread32(KVASER_PCIEFD_SYSID_VERSION_ADDR((pcie))) >> \
	  KVASER_PCIEFD_SYSID_VERSION_NRCHAN_SHIFT) &           \
	 0xff)
#define KVASER_PCIEFD_SYSID_VERSION_MINOR_GET(pcie) \
	(ioread32(KVASER_PCIEFD_SYSID_VERSION_ADDR((pcie))) & 0xff)
#define KVASER_PCIEFD_SYSID_VERSION_MAJOR_GET(pcie)             \
	((ioread32(KVASER_PCIEFD_SYSID_VERSION_ADDR((pcie))) >> \
	  KVASER_PCIEFD_SYSID_VERSION_MAJOR_SHIFT) &            \
	 0xff)
#define KVASER_PCIEFD_SYSID_CANFREQ_GET(pcie) \
	(ioread32(KVASER_PCIEFD_SYSID_CANFREQ_ADDR((pcie))))
#define KVASER_PCIEFD_SYSID_BUSFREQ_GET(pcie) \
	(ioread32(KVASER_PCIEFD_SYSID_BUSFREQ_ADDR((pcie))))
#define KVASER_PCIEFD_SYSID_BUILD_GET(pcie)                   \
	((ioread32(KVASER_PCIEFD_SYSID_BUILD_ADDR((pcie))) >> \
	  KVASER_PCIEFD_SYSID_BUILD_SHIFT) &                  \
	 0x7fff)
#define KVASER_PCIEFD_SRB_FIFO_LAST_GET(pcie) \
	(ioread32(KVASER_PCIEFD_SRB_FIFO_LAST_ADDR((pcie))))
#define KVASER_PCIEFD_SRB_CMD_SET(pcie, value) \
	(iowrite32((value), KVASER_PCIEFD_SRB_CMD_ADDR((pcie))))
#define KVASER_PCIEFD_SRB_IEN_SET(pcie, value) \
	(iowrite32((value), KVASER_PCIEFD_SRB_IEN_ADDR((pcie))))
#define KVASER_PCIEFD_SRB_IRQ_GET(pcie) \
	(ioread32(KVASER_PCIEFD_SRB_IRQ_ADDR((pcie))))
#define KVASER_PCIEFD_SRB_IRQ_SET(pcie, value) \
	(iowrite32((value), KVASER_PCIEFD_SRB_IRQ_ADDR((pcie))))
#define KVASER_PCIEFD_SRB_STAT_GET(pcie) \
	(ioread32(KVASER_PCIEFD_SRB_STAT_ADDR((pcie))))
#define KVASER_PCIEFD_SRB_RX_NR_PACKETS_CURRENT_GET(pcie)         \
	(ioread32(KVASER_PCIEFD_SRB_RX_NR_PACKETS_ADDR((pcie))) & \
	 KVASER_PCIEFD_SRB_RX_NR_PACKETS_CURRENT_MASK)
#define KVASER_PCIEFD_SRB_CTRL_SET(pcie, value) \
	(iowrite32((value), KVASER_PCIEFD_SRB_CTRL_ADDR((pcie))))
#define KVASER_PCIEFD_KCAN_FIFO_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_FIFO_ADDR((can))))
#define KVASER_PCIEFD_KCAN_CTRL_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_CTRL_ADDR((can))))
#define KVASER_PCIEFD_KCAN_CMD_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_CMD_ADDR((can))))
#define KVASER_PCIEFD_KCAN_IEN_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_IEN_ADDR((can))))
#define KVASER_PCIEFD_KCAN_IRQ_GET(can) \
	(ioread32(KVASER_PCIEFD_KCAN_IRQ_ADDR((can))))
#define KVASER_PCIEFD_KCAN_IRQ_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_IRQ_ADDR((can))))
#define KVASER_PCIEFD_KCAN_TX_NR_PACKETS_CURRENT_GET(can) \
	(ioread32(KVASER_PCIEFD_KCAN_TX_NR_PACKETS_ADDR((can))) & 0xff)
#define KVASER_PCIEFD_KCAN_TX_NR_PACKETS_MAX_GET(can)               \
	((ioread32(KVASER_PCIEFD_KCAN_TX_NR_PACKETS_ADDR((can))) >> \
	  KVASER_PCIEFD_KCAN_TX_NR_PACKETS_MAX_SHIFT) &             \
	 0xff)
#define KVASER_PCIEFD_KCAN_STAT_GET(can) \
	(ioread32(KVASER_PCIEFD_KCAN_STAT_ADDR((can))))
#define KVASER_PCIEFD_KCAN_MODE_GET(can) \
	(ioread32(KVASER_PCIEFD_KCAN_MODE_ADDR((can))))
#define KVASER_PCIEFD_KCAN_MODE_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_MODE_ADDR((can))))
#define KVASER_PCIEFD_KCAN_BTRN_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_BTRN_ADDR((can))))
#define KVASER_PCIEFD_KCAN_BTRD_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_BTRD_ADDR((can))))
#define KVASER_PCIEFD_KCAN_PWM_GET(can) \
	(ioread32(KVASER_PCIEFD_KCAN_PWM_ADDR((can))))
#define KVASER_PCIEFD_KCAN_PWM_SET(can, value) \
	(iowrite32((value), KVASER_PCIEFD_KCAN_PWM_ADDR((can))))

#define KVASER_PCIEFD_PCI_IEN_DISABLE_ALL(pcie) \
	(KVASER_PCIEFD_PCI_IEN_SET((pcie), 0))
#define KVASER_PCIEFD_PCI_IEN_ENABLE_ALL(pcie) \
	(KVASER_PCIEFD_PCI_IEN_SET((pcie), (pcie)->driver_data->irq_mask->all))

#define KVASER_PCIEFD_SRB_DMA_DISABLE(pcie) (KVASER_PCIEFD_SRB_CTRL_SET((pcie), 0))
#define KVASER_PCIEFD_SRB_DMA_ENABLE(pcie) \
	(KVASER_PCIEFD_SRB_CTRL_SET((pcie), KVASER_PCIEFD_SRB_CTRL_DMA_ENABLE))

#define KVASER_PCIEFD_SRB_IEN_ENABLE_ALL(pcie)                          \
	(KVASER_PCIEFD_SRB_IEN_SET((pcie),                              \
				   KVASER_PCIEFD_SRB_IRQ_DPD0 |         \
					   KVASER_PCIEFD_SRB_IRQ_DPD1 | \
					   KVASER_PCIEFD_SRB_IRQ_DOF0 | \
					   KVASER_PCIEFD_SRB_IRQ_DOF1 | \
					   KVASER_PCIEFD_SRB_IRQ_DUF0 | \
					   KVASER_PCIEFD_SRB_IRQ_DUF1))

#define KVASER_PCIEFD_KCAN_IEN_DISABLE_ALL(can) \
	(KVASER_PCIEFD_KCAN_IEN_SET((can), 0))
#define KVASER_PCIEFD_KCAN_IEN_ENABLE_ALL(can)                            \
	(KVASER_PCIEFD_KCAN_IEN_SET((can),                                \
				    KVASER_PCIEFD_KCAN_IRQ_TOF |          \
					    KVASER_PCIEFD_KCAN_IRQ_ABD |  \
					    KVASER_PCIEFD_KCAN_IRQ_TAE |  \
					    KVASER_PCIEFD_KCAN_IRQ_TAL |  \
					    KVASER_PCIEFD_KCAN_IRQ_FDIC | \
					    KVASER_PCIEFD_KCAN_IRQ_BPP |  \
					    KVASER_PCIEFD_KCAN_IRQ_TAR))
#define KVASER_PCIEFD_KCAN_IEN_ENABLE_ABD(can) \
	(KVASER_PCIEFD_KCAN_IEN_SET((can), KVASER_PCIEFD_KCAN_IRQ_ABD))
#define KVASER_PCIEFD_KCAN_IRQ_CLEAR_ALL(can) \
	(KVASER_PCIEFD_KCAN_IRQ_SET((can), GENMASK(31, 0)))
#define KVASER_PCIEFD_KCAN_BUS_LOAD_DISABLE(can) \
	(iowrite32(0, KVASER_PCIEFD_KCAN_BUS_LOAD_ADDR((can))))
#define KVASER_PCIEFD_KCAN_CHANNEL_SPAN(pcie) \
	(KVASER_PCIEFD_KCAN_CH1_ADDR((pcie)) - KVASER_PCIEFD_KCAN_CH0_ADDR((pcie)))
#define KVASER_PCIEFD_KCAN_CHX_ADDR(pcie, i) \
	(KVASER_PCIEFD_KCAN_CH0_ADDR((pcie)) + (i) * KVASER_PCIEFD_KCAN_CHANNEL_SPAN((pcie)))

#define KVASER_PCIEFD_LOOPBACK_DISABLE(pcie) \
	(iowrite32(0, KVASER_PCIEFD_GET_BLOCK_ADDR((pcie), loopback)))

#define KVASER_PCIEFD_WRITE_DMA_MAP(pcie, addr, index) \
	((pcie)->driver_data->ops->kvaser_pciefd_write_dma_map((pcie), (addr), (index)))

#define KVASER_PCIEFD_PACKET_CHID(packet) \
	(((packet)->header[1] >> KVASER_PCIEFD_PACKET_CHID_SHIFT) & 0x7)

#define KVASER_PCIEFD_PACKET_TYPE(packet) \
	(((packet)->header[1] >> KVASER_PCIEFD_PACKET_TYPE_SHIFT) & 0xf)

#define KVASER_PCIEFD_SPACKET_TXERR_COUNT(packet) ((packet)->header[0] & 0xff)

#define KVASER_PCIEFD_SPACKET_RXERR_COUNT(packet) \
	(((packet)->header[0] >> KVASER_PCIEFD_SPACK_RXERR_SHIFT) & 0xff)

struct kvaser_pciefd;
static void kvaser_pciefd_write_dma_map_altera(struct kvaser_pciefd *pcie,
					       dma_addr_t addr, int index);
static void kvaser_pciefd_write_dma_map_sf2(struct kvaser_pciefd *pcie,
					    dma_addr_t addr, int index);

struct kvaser_pciefd_address_offset {
	u32 serdes;
	u32 pci_ien;
	u32 pci_irq;
	u32 sysid;
	u32 loopback;
	u32 kcan_srb_fifo;
	u32 kcan_srb;
	u32 kcan_ch0;
	u32 kcan_ch1;
};

struct kvaser_pciefd_dev_ops {
	void (*kvaser_pciefd_write_dma_map)(struct kvaser_pciefd *pcie,
					    dma_addr_t addr, int index);
};

struct kvaser_pciefd_irq_mask {
	u32 kcan_rx0;
	u32 kcan_tx[KVASER_PCIEFD_MAX_CAN_CHANNELS];
	u32 all;
};

struct kvaser_pciefd_driver_data {
	const struct kvaser_pciefd_address_offset *address_offset;
	const struct kvaser_pciefd_irq_mask *irq_mask;
	const struct kvaser_pciefd_dev_ops *ops;
};

const struct kvaser_pciefd_address_offset kvaser_pciefd_altera_address_offset = {
	.serdes = 0x1000,
	.pci_ien = 0x50,
	.pci_irq = 0x40,
	.sysid = 0x1f020,
	.loopback = 0x1f000,
	.kcan_srb_fifo = 0x1f200,
	.kcan_srb = 0x1f400,
	.kcan_ch0 = 0x10000,
	.kcan_ch1 = 0x11000,
};

const struct kvaser_pciefd_address_offset kvaser_pciefd_sf2_address_offset = {
	.serdes = 0x280c8,
	.pci_ien = 0x102004,
	.pci_irq = 0x102008,
	.sysid = 0x100000,
	.loopback = 0x103000,
	.kcan_srb_fifo = 0x120000,
	.kcan_srb = 0x121000,
	.kcan_ch0 = 0x140000,
	.kcan_ch1 = 0x142000,
};

const struct kvaser_pciefd_irq_mask kvaser_pciefd_altera_irq_mask = {
	.kcan_rx0 = BIT(4),
	.kcan_tx = { BIT(0), BIT(1), BIT(2), BIT(3) },
	.all = 0x0000001f,
};

const struct kvaser_pciefd_irq_mask kvaser_pciefd_sf2_irq_mask = {
	.kcan_rx0 = BIT(4),
	.kcan_tx = { BIT(16), BIT(17), BIT(18), BIT(19) },
	.all = 0x000f0010,
};

const struct kvaser_pciefd_dev_ops kvaser_pciefd_altera_dev_ops = {
	.kvaser_pciefd_write_dma_map = kvaser_pciefd_write_dma_map_altera,
};

const struct kvaser_pciefd_dev_ops kvaser_pciefd_sf2_dev_ops = {
	.kvaser_pciefd_write_dma_map = kvaser_pciefd_write_dma_map_sf2,
};

const struct kvaser_pciefd_driver_data kvaser_pciefd_altera_driver_data = {
	.address_offset = &kvaser_pciefd_altera_address_offset,
	.irq_mask = &kvaser_pciefd_altera_irq_mask,
	.ops = &kvaser_pciefd_altera_dev_ops,
};

const struct kvaser_pciefd_driver_data kvaser_pciefd_sf2_driver_data = {
	.address_offset = &kvaser_pciefd_sf2_address_offset,
	.irq_mask = &kvaser_pciefd_sf2_irq_mask,
	.ops = &kvaser_pciefd_sf2_dev_ops,
};

struct kvaser_pciefd_can {
	struct can_priv can;
	struct kvaser_pciefd *kv_pcie;
	void __iomem *reg_base;
	struct can_berr_counter bec;
	u8 cmd_seq;
	int err_rep_cnt;
	int echo_idx;
	spinlock_t lock; /* Locks sensitive registers (e.g. MODE) */
	spinlock_t echo_lock; /* Locks the message echo buffer */
	struct timer_list bec_poll_timer;
	struct completion start_comp, flush_comp;
};

struct kvaser_pciefd {
	struct pci_dev *pci;
	void __iomem *reg_base;
	struct kvaser_pciefd_can *can[KVASER_PCIEFD_MAX_CAN_CHANNELS];
	const struct kvaser_pciefd_driver_data *driver_data;
	void *dma_data[KVASER_PCIEFD_DMA_COUNT];
	u8 nr_channels;
	u32 bus_freq;
	u32 freq;
	u32 freq_to_ticks_div;
};

struct kvaser_pciefd_rx_packet {
	u32 header[2];
	u64 timestamp;
};

struct kvaser_pciefd_tx_packet {
	u32 header[2];
	u8 data[64];
};

static const struct can_bittiming_const kvaser_pciefd_bittiming_const = {
	.name = KVASER_PCIEFD_DRV_NAME,
	.tseg1_min = 1,
	.tseg1_max = 512,
	.tseg2_min = 1,
	.tseg2_max = 32,
	.sjw_max = 16,
	.brp_min = 1,
	.brp_max = 8192,
	.brp_inc = 1,
};

static struct pci_device_id kvaser_pciefd_id_table[] = {
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_4HS_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_altera_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_2HS_V2_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_altera_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_HS_V2_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_altera_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_MINIPCIE_HS_V2_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_altera_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_MINIPCIE_2HS_V2_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_altera_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_2CAN_V3_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_sf2_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_1CAN_V3_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_sf2_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_4CAN_V2_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_sf2_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_MINIPCIE_2CAN_V3_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_sf2_driver_data,
	},
	{
		PCI_DEVICE(KVASER_PCIEFD_VENDOR, KVASER_PCIEFD_MINIPCIE_1CAN_V3_DEVICE_ID),
		.driver_data = (kernel_ulong_t)&kvaser_pciefd_sf2_driver_data,
	},
	{
		0,
	},
};
MODULE_DEVICE_TABLE(pci, kvaser_pciefd_id_table);

static void kvaser_pciefd_send_kcan_cmd(struct kvaser_pciefd_can *can, u32 cmd)
{
	KVASER_PCIEFD_KCAN_CMD_SET(can,
				   cmd | (++can->cmd_seq
					  << KVASER_PCIEFD_KCAN_CMD_SEQ_SHIFT));
}

static void kvaser_pciefd_kcan_abort_flush_reset(struct kvaser_pciefd_can *can)
{
	kvaser_pciefd_send_kcan_cmd(can, KVASER_PCIEFD_KCAN_CMD_AT);
}

static void kvaser_pciefd_request_status(struct kvaser_pciefd_can *can)
{
	kvaser_pciefd_send_kcan_cmd(can, KVASER_PCIEFD_KCAN_CMD_SRQ);
}

static void kvaser_pciefd_enable_err_gen(struct kvaser_pciefd_can *can)
{
	u32 mode;
	unsigned long irq;

	spin_lock_irqsave(&can->lock, irq);
	mode = KVASER_PCIEFD_KCAN_MODE_GET(can);
	if (!(mode & KVASER_PCIEFD_KCAN_MODE_EPEN)) {
		mode |= KVASER_PCIEFD_KCAN_MODE_EPEN;
		KVASER_PCIEFD_KCAN_MODE_SET(can, mode);
	}
	spin_unlock_irqrestore(&can->lock, irq);
}

static void kvaser_pciefd_disable_err_gen(struct kvaser_pciefd_can *can)
{
	u32 mode;
	unsigned long irq;

	spin_lock_irqsave(&can->lock, irq);
	mode = KVASER_PCIEFD_KCAN_MODE_GET(can);
	mode &= ~KVASER_PCIEFD_KCAN_MODE_EPEN;
	KVASER_PCIEFD_KCAN_MODE_SET(can, mode);
	spin_unlock_irqrestore(&can->lock, irq);
}

static void kvaser_pciefd_set_skb_timestamp(const struct kvaser_pciefd *pcie,
					    struct sk_buff *skb, u64 timestamp)
{
	struct skb_shared_hwtstamps *hwtstamps = skb_hwtstamps(skb);

	hwtstamps->hwtstamp =
		ns_to_ktime(div_u64(timestamp * 1000, pcie->freq_to_ticks_div));
}

static void kvaser_pciefd_setup_controller(struct kvaser_pciefd_can *can)
{
	u32 mode;
	unsigned long irq;

	spin_lock_irqsave(&can->lock, irq);

	mode = KVASER_PCIEFD_KCAN_MODE_GET(can);
	if (can->can.ctrlmode & CAN_CTRLMODE_FD) {
		mode &= ~KVASER_PCIEFD_KCAN_MODE_CCM;
		if (can->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO)
			mode |= KVASER_PCIEFD_KCAN_MODE_NIFDEN;
		else
			mode &= ~KVASER_PCIEFD_KCAN_MODE_NIFDEN;
	} else {
		mode |= KVASER_PCIEFD_KCAN_MODE_CCM;
		mode &= ~KVASER_PCIEFD_KCAN_MODE_NIFDEN;
	}

	if (can->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		mode |= KVASER_PCIEFD_KCAN_MODE_LOM;
	else
		mode &= ~KVASER_PCIEFD_KCAN_MODE_LOM;

	mode |= KVASER_PCIEFD_KCAN_MODE_EEN;
	mode |= KVASER_PCIEFD_KCAN_MODE_EPEN;
	/* Use ACK packet type */
	mode &= ~KVASER_PCIEFD_KCAN_MODE_APT;
	mode &= ~KVASER_PCIEFD_KCAN_MODE_RM;
	KVASER_PCIEFD_KCAN_MODE_SET(can, mode);

	spin_unlock_irqrestore(&can->lock, irq);
}

static void kvaser_pciefd_start_controller_flush(struct kvaser_pciefd_can *can)
{
	u32 status;
	unsigned long irq;

	spin_lock_irqsave(&can->lock, irq);
	KVASER_PCIEFD_KCAN_IRQ_CLEAR_ALL(can);
	KVASER_PCIEFD_KCAN_IEN_ENABLE_ABD(can);

	status = KVASER_PCIEFD_KCAN_STAT_GET(can);
	if (status & KVASER_PCIEFD_KCAN_STAT_IDLE) {
		/* If controller is already idle, run abort, flush and reset */
		kvaser_pciefd_kcan_abort_flush_reset(can);
	} else if (!(status & KVASER_PCIEFD_KCAN_STAT_RMR)) {
		u32 mode;

		/* Put controller in reset mode */
		mode = KVASER_PCIEFD_KCAN_MODE_GET(can);
		mode |= KVASER_PCIEFD_KCAN_MODE_RM;
		KVASER_PCIEFD_KCAN_MODE_SET(can, mode);
	}

	spin_unlock_irqrestore(&can->lock, irq);
}

static int kvaser_pciefd_bus_on(struct kvaser_pciefd_can *can)
{
	u32 mode;
	unsigned long irq;

	del_timer(&can->bec_poll_timer);

	if (!completion_done(&can->flush_comp))
		kvaser_pciefd_start_controller_flush(can);

	if (!wait_for_completion_timeout(&can->flush_comp,
					 KVASER_PCIEFD_WAIT_TIMEOUT)) {
		netdev_err(can->can.dev, "Timeout during bus on flush\n");
		return -ETIMEDOUT;
	}

	spin_lock_irqsave(&can->lock, irq);
	KVASER_PCIEFD_KCAN_IEN_DISABLE_ALL(can);
	KVASER_PCIEFD_KCAN_IRQ_CLEAR_ALL(can);

	KVASER_PCIEFD_KCAN_IEN_ENABLE_ABD(can);

	mode = KVASER_PCIEFD_KCAN_MODE_GET(can);
	mode &= ~KVASER_PCIEFD_KCAN_MODE_RM;
	KVASER_PCIEFD_KCAN_MODE_SET(can, mode);
	spin_unlock_irqrestore(&can->lock, irq);

	if (!wait_for_completion_timeout(&can->start_comp,
					 KVASER_PCIEFD_WAIT_TIMEOUT)) {
		netdev_err(can->can.dev, "Timeout during bus on reset\n");
		return -ETIMEDOUT;
	}
	/* Reset interrupt handling */
	KVASER_PCIEFD_KCAN_IEN_DISABLE_ALL(can);
	KVASER_PCIEFD_KCAN_IRQ_CLEAR_ALL(can);

	KVASER_PCIEFD_KCAN_IEN_ENABLE_ALL(can);
	kvaser_pciefd_setup_controller(can);

	can->can.state = CAN_STATE_ERROR_ACTIVE;
	netif_wake_queue(can->can.dev);
	can->bec.txerr = 0;
	can->bec.rxerr = 0;
	can->err_rep_cnt = 0;

	return 0;
}

static void kvaser_pciefd_pwm_stop(struct kvaser_pciefd_can *can)
{
	u8 top;
	u32 pwm_ctrl;
	unsigned long irq;

	spin_lock_irqsave(&can->lock, irq);
	pwm_ctrl = KVASER_PCIEFD_KCAN_PWM_GET(can);
	top = (pwm_ctrl >> KVASER_PCIEFD_KCAN_PWM_TOP_SHIFT) & 0xff;

	/* Set duty cycle to zero */
	pwm_ctrl |= top;
	KVASER_PCIEFD_KCAN_PWM_SET(can, pwm_ctrl);
	spin_unlock_irqrestore(&can->lock, irq);
}

static void kvaser_pciefd_pwm_start(struct kvaser_pciefd_can *can)
{
	int top, trigger;
	u32 pwm_ctrl;
	unsigned long irq;

	kvaser_pciefd_pwm_stop(can);
	spin_lock_irqsave(&can->lock, irq);

	/* Set frequency to 500 KHz*/
	top = can->kv_pcie->bus_freq / (2 * 500000) - 1;

	pwm_ctrl = top & 0xff;
	pwm_ctrl |= (top & 0xff) << KVASER_PCIEFD_KCAN_PWM_TOP_SHIFT;
	KVASER_PCIEFD_KCAN_PWM_SET(can, pwm_ctrl);

	/* Set duty cycle to 95 */
	trigger = (100 * top - 95 * (top + 1) + 50) / 100;
	pwm_ctrl = trigger & 0xff;
	pwm_ctrl |= (top & 0xff) << KVASER_PCIEFD_KCAN_PWM_TOP_SHIFT;
	KVASER_PCIEFD_KCAN_PWM_SET(can, pwm_ctrl);
	spin_unlock_irqrestore(&can->lock, irq);
}

static int kvaser_pciefd_open(struct net_device *netdev)
{
	int err;
	struct kvaser_pciefd_can *can = netdev_priv(netdev);

	err = open_candev(netdev);
	if (err)
		return err;

	err = kvaser_pciefd_bus_on(can);
	if (err) {
		close_candev(netdev);
		return err;
	}

	return 0;
}

static int kvaser_pciefd_stop(struct net_device *netdev)
{
	struct kvaser_pciefd_can *can = netdev_priv(netdev);
	int ret = 0;

	/* Don't interrupt ongoing flush */
	if (!completion_done(&can->flush_comp))
		kvaser_pciefd_start_controller_flush(can);

	if (!wait_for_completion_timeout(&can->flush_comp,
					 KVASER_PCIEFD_WAIT_TIMEOUT)) {
		netdev_err(can->can.dev, "Timeout during stop\n");
		ret = -ETIMEDOUT;
	} else {
		KVASER_PCIEFD_KCAN_IEN_DISABLE_ALL(can);
		del_timer(&can->bec_poll_timer);
	}

	can->can.state = CAN_STATE_STOPPED;
	close_candev(netdev);

	return ret;
}

static int kvaser_pciefd_prepare_tx_packet(struct kvaser_pciefd_tx_packet *p,
					   struct kvaser_pciefd_can *can,
					   struct sk_buff *skb)
{
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	int packet_size;
	int seq = can->echo_idx;

	memset(p, 0, sizeof(*p));

	if (can->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT)
		p->header[1] |= KVASER_PCIEFD_TPACKET_SMS;

	if (cf->can_id & CAN_RTR_FLAG)
		p->header[0] |= KVASER_PCIEFD_RPACKET_RTR;

	if (cf->can_id & CAN_EFF_FLAG)
		p->header[0] |= KVASER_PCIEFD_RPACKET_IDE;

	p->header[0] |= cf->can_id & CAN_EFF_MASK;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0))
	p->header[1] |= can_fd_len2dlc(cf->len) << KVASER_PCIEFD_RPACKET_DLC_SHIFT;
#else
	p->header[1] |= can_len2dlc(cf->len) << KVASER_PCIEFD_RPACKET_DLC_SHIFT;
#endif /* LINUX_VERSION_CODE >= 5.11.0) */
	p->header[1] |= KVASER_PCIEFD_TPACKET_AREQ;

	if (can_is_canfd_skb(skb)) {
		p->header[1] |= KVASER_PCIEFD_RPACKET_FDF;
		if (cf->flags & CANFD_BRS)
			p->header[1] |= KVASER_PCIEFD_RPACKET_BRS;
		if (cf->flags & CANFD_ESI)
			p->header[1] |= KVASER_PCIEFD_RPACKET_ESI;
	}

	p->header[1] |= seq & KVASER_PCIEFD_PACKET_SEQ_MASK;

	packet_size = cf->len;
	memcpy(p->data, cf->data, packet_size);

	return DIV_ROUND_UP(packet_size, 4);
}

static netdev_tx_t kvaser_pciefd_start_xmit(struct sk_buff *skb,
					    struct net_device *netdev)
{
	struct kvaser_pciefd_can *can = netdev_priv(netdev);
	unsigned long irq_flags;
	struct kvaser_pciefd_tx_packet packet;
	int nwords;
	u8 count;

	if (can_dev_dropped_skb(netdev, skb))
		return NETDEV_TX_OK;

	nwords = kvaser_pciefd_prepare_tx_packet(&packet, can, skb);

	spin_lock_irqsave(&can->echo_lock, irq_flags);

	/* Prepare and save echo skb in internal slot */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0))
	can_put_echo_skb(skb, netdev, can->echo_idx, 0);
#else
	can_put_echo_skb(skb, netdev, can->echo_idx);
#endif /* LINUX_VERSION_CODE >= 5.12.0) */

	/* Move echo index to the next slot */
	can->echo_idx = (can->echo_idx + 1) % can->can.echo_skb_max;

	/* Write header to fifo */
	KVASER_PCIEFD_KCAN_FIFO_SET(can, packet.header[0]);
	KVASER_PCIEFD_KCAN_FIFO_SET(can, packet.header[1]);

	if (nwords) {
		u32 data_last = ((u32 *)packet.data)[nwords - 1];

		/* Write data to fifo, except last word */
		iowrite32_rep(KVASER_PCIEFD_KCAN_FIFO_ADDR(can), packet.data,
			      nwords - 1);
		/* Write last word to end of fifo */
		__raw_writel(data_last, KVASER_PCIEFD_KCAN_FIFO_LAST_ADDR(can));
	} else {
		/* Complete write to fifo */
		__raw_writel(0, KVASER_PCIEFD_KCAN_FIFO_LAST_ADDR(can));
	}

	count = KVASER_PCIEFD_KCAN_TX_NR_PACKETS_CURRENT_GET(can);
	/* No room for a new message, stop the queue until at least one
	 * successful transmit
	 */
	if (count >= can->can.echo_skb_max || can->can.echo_skb[can->echo_idx])
		netif_stop_queue(netdev);

	spin_unlock_irqrestore(&can->echo_lock, irq_flags);

	return NETDEV_TX_OK;
}

static int kvaser_pciefd_set_bittiming(struct kvaser_pciefd_can *can, bool data)
{
	u32 mode, test, btrn;
	unsigned long irq_flags;
	int ret;
	struct can_bittiming *bt;

	if (data)
		bt = &can->can.data_bittiming;
	else
		bt = &can->can.bittiming;

	btrn = ((bt->phase_seg2 - 1) & 0x1f)
		       << KVASER_PCIEFD_KCAN_BTRN_TSEG2_SHIFT |
	       (((bt->prop_seg + bt->phase_seg1) - 1) & 0x1ff)
		       << KVASER_PCIEFD_KCAN_BTRN_TSEG1_SHIFT |
	       ((bt->sjw - 1) & 0xf) << KVASER_PCIEFD_KCAN_BTRN_SJW_SHIFT |
	       ((bt->brp - 1) & 0x1fff);

	spin_lock_irqsave(&can->lock, irq_flags);
	mode = KVASER_PCIEFD_KCAN_MODE_GET(can);
	/* Put the circuit in reset mode */
	KVASER_PCIEFD_KCAN_MODE_SET(can, mode | KVASER_PCIEFD_KCAN_MODE_RM);

	/* Can only set bittiming if in reset mode */
	ret = readl_poll_timeout(KVASER_PCIEFD_KCAN_MODE_ADDR(can), test,
				 test & KVASER_PCIEFD_KCAN_MODE_RM, 0, 10);

	if (ret) {
		spin_unlock_irqrestore(&can->lock, irq_flags);
		return -EBUSY;
	}

	if (data)
		KVASER_PCIEFD_KCAN_BTRD_SET(can, btrn);
	else
		KVASER_PCIEFD_KCAN_BTRN_SET(can, btrn);

	/* Restore previous reset mode status */
	KVASER_PCIEFD_KCAN_MODE_SET(can, mode);

	spin_unlock_irqrestore(&can->lock, irq_flags);
	return 0;
}

static int kvaser_pciefd_set_nominal_bittiming(struct net_device *ndev)
{
	return kvaser_pciefd_set_bittiming(netdev_priv(ndev), false);
}

static int kvaser_pciefd_set_data_bittiming(struct net_device *ndev)
{
	return kvaser_pciefd_set_bittiming(netdev_priv(ndev), true);
}

static int kvaser_pciefd_set_mode(struct net_device *ndev, enum can_mode mode)
{
	struct kvaser_pciefd_can *can = netdev_priv(ndev);
	int ret = 0;

	switch (mode) {
	case CAN_MODE_START:
		if (!can->can.restart_ms)
			ret = kvaser_pciefd_bus_on(can);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return ret;
}

static int kvaser_pciefd_get_berr_counter(const struct net_device *ndev,
					  struct can_berr_counter *bec)
{
	struct kvaser_pciefd_can *can = netdev_priv(ndev);

	bec->rxerr = can->bec.rxerr;
	bec->txerr = can->bec.txerr;
	return 0;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))
static void kvaser_pciefd_bec_poll_timer(unsigned long data)
{
	struct kvaser_pciefd_can *can = (struct kvaser_pciefd_can *)data;
#else
static void kvaser_pciefd_bec_poll_timer(struct timer_list *data)
{
	struct kvaser_pciefd_can *can = from_timer(can, data, bec_poll_timer);
#endif /* LINUX_VERSION_CODE < 4.15.0 */

	kvaser_pciefd_enable_err_gen(can);
	kvaser_pciefd_request_status(can);
	can->err_rep_cnt = 0;
}

static const struct net_device_ops kvaser_pciefd_netdev_ops = {
	.ndo_open = kvaser_pciefd_open,
	.ndo_stop = kvaser_pciefd_stop,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0))
	.ndo_eth_ioctl = can_eth_ioctl_hwts,
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION 6.0.0 */
	.ndo_start_xmit = kvaser_pciefd_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0))
static const struct ethtool_ops kvaser_pciefd_ethtool_ops = {
	.get_ts_info = can_ethtool_op_get_ts_info_hwts,
};
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION 6.0.0 */

static int kvaser_pciefd_setup_can_ctrls(struct kvaser_pciefd *pcie)
{
	int i;

	for (i = 0; i < pcie->nr_channels; i++) {
		struct net_device *netdev;
		struct kvaser_pciefd_can *can;
		u32 status;

		netdev = alloc_candev(sizeof(struct kvaser_pciefd_can),
				      KVASER_PCIEFD_CAN_TX_MAX_COUNT);
		if (!netdev)
			return -ENOMEM;

		can = netdev_priv(netdev);
		netdev->netdev_ops = &kvaser_pciefd_netdev_ops;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0))
		netdev->ethtool_ops = &kvaser_pciefd_ethtool_ops;
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION 6.0.0 */
		can->reg_base = KVASER_PCIEFD_KCAN_CHX_ADDR(pcie, i);

		can->kv_pcie = pcie;
		can->cmd_seq = 0;
		can->err_rep_cnt = 0;
		can->bec.txerr = 0;
		can->bec.rxerr = 0;

		init_completion(&can->start_comp);
		init_completion(&can->flush_comp);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))
		setup_timer(&can->bec_poll_timer, kvaser_pciefd_bec_poll_timer,
			    (unsigned long)can);
#else
		timer_setup(&can->bec_poll_timer, kvaser_pciefd_bec_poll_timer,
			    0);
#endif /* LINUX_VERSION_CODE < 4.15.0 */

		/* Disable Bus load reporting */
		KVASER_PCIEFD_KCAN_BUS_LOAD_DISABLE(can);

		can->can.clock.freq = pcie->freq;
		can->can.echo_skb_max =
			min(KVASER_PCIEFD_CAN_TX_MAX_COUNT,
			    KVASER_PCIEFD_KCAN_TX_NR_PACKETS_MAX_GET(can) - 1);
		can->echo_idx = 0;
		spin_lock_init(&can->echo_lock);
		spin_lock_init(&can->lock);
		can->can.bittiming_const = &kvaser_pciefd_bittiming_const;
		can->can.data_bittiming_const = &kvaser_pciefd_bittiming_const;

		can->can.do_set_bittiming = kvaser_pciefd_set_nominal_bittiming;
		can->can.do_set_data_bittiming = kvaser_pciefd_set_data_bittiming;

		can->can.do_set_mode = kvaser_pciefd_set_mode;
		can->can.do_get_berr_counter = kvaser_pciefd_get_berr_counter;

		can->can.ctrlmode_supported = CAN_CTRLMODE_LISTENONLY |
					      CAN_CTRLMODE_FD |
					      CAN_CTRLMODE_FD_NON_ISO;

		status = KVASER_PCIEFD_KCAN_STAT_GET(can);
		if (!(status & KVASER_PCIEFD_KCAN_STAT_FD)) {
			dev_err(&pcie->pci->dev,
				"CAN FD not supported as expected %d\n", i);

			free_candev(netdev);
			return -ENODEV;
		}

		if (status & KVASER_PCIEFD_KCAN_STAT_CAP)
			can->can.ctrlmode_supported |= CAN_CTRLMODE_ONE_SHOT;

		netdev->flags |= IFF_ECHO;

		SET_NETDEV_DEV(netdev, &pcie->pci->dev);

		KVASER_PCIEFD_KCAN_IRQ_CLEAR_ALL(can);
		KVASER_PCIEFD_KCAN_IEN_ENABLE_ABD(can);

		pcie->can[i] = can;
		kvaser_pciefd_pwm_start(can);
	}

	return 0;
}

static int kvaser_pciefd_reg_candev(struct kvaser_pciefd *pcie)
{
	int i;

	for (i = 0; i < pcie->nr_channels; i++) {
		int err = register_candev(pcie->can[i]->can.dev);

		if (err) {
			int j;

			/* Unregister all successfully registered devices. */
			for (j = 0; j < i; j++)
				unregister_candev(pcie->can[j]->can.dev);
			return err;
		}
	}

	return 0;
}

static void kvaser_pciefd_write_dma_map_altera(struct kvaser_pciefd *pcie,
					       dma_addr_t addr, int index)
{
	void __iomem *serdes_base;
	u32 word1, word2;

#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	word1 = addr | KVASER_PCIEFD_64BIT_DMA_BIT;
	word2 = addr >> 32;
#else
	word1 = addr;
	word2 = 0;
#endif
	serdes_base = KVASER_PCIEFD_SERDES_ADDR(pcie) + 0x8 * index;
	iowrite32(word1, serdes_base);
	iowrite32(word2, serdes_base + 0x4);
}

static void kvaser_pciefd_write_dma_map_sf2(struct kvaser_pciefd *pcie,
					    dma_addr_t addr, int index)
{
	void __iomem *serdes_base;
	u32 lsb = addr & 0xfffff000;
	u32 msb = 0x0;

#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	msb = addr >> 32;
#endif
	serdes_base = KVASER_PCIEFD_SERDES_ADDR(pcie) + 0x10 * index;
	iowrite32(lsb, serdes_base);
	iowrite32(msb, serdes_base + 0x4);
}

static int kvaser_pciefd_setup_dma(struct kvaser_pciefd *pcie)
{
	int i;
	u32 srb_packet_count;
	dma_addr_t dma_addr[KVASER_PCIEFD_DMA_COUNT];

	/* Disable the DMA */
	KVASER_PCIEFD_SRB_DMA_DISABLE(pcie);
	for (i = 0; i < KVASER_PCIEFD_DMA_COUNT; i++) {
		pcie->dma_data[i] = dmam_alloc_coherent(&pcie->pci->dev,
							KVASER_PCIEFD_DMA_SIZE,
							&dma_addr[i],
							GFP_KERNEL);

		if (!pcie->dma_data[i] || !dma_addr[i]) {
			dev_err(&pcie->pci->dev, "Rx dma_alloc(%u) failure\n",
				KVASER_PCIEFD_DMA_SIZE);
			return -ENOMEM;
		}

		KVASER_PCIEFD_WRITE_DMA_MAP(pcie, dma_addr[i], i);
	}

	/* Reset Rx FIFO, and both DMA buffers */
	KVASER_PCIEFD_SRB_CMD_SET(pcie, KVASER_PCIEFD_SRB_CMD_FOR |
						KVASER_PCIEFD_SRB_CMD_RDB0 |
						KVASER_PCIEFD_SRB_CMD_RDB1);

	/* Empty Rx FIFO */
	srb_packet_count = KVASER_PCIEFD_SRB_RX_NR_PACKETS_CURRENT_GET(pcie);
	while (srb_packet_count) {
		/* Drop current packet in FIFO */
		KVASER_PCIEFD_SRB_FIFO_LAST_GET(pcie);
		srb_packet_count--;
	}

	if (!(KVASER_PCIEFD_SRB_STAT_GET(pcie) & KVASER_PCIEFD_SRB_STAT_DI)) {
		dev_err(&pcie->pci->dev, "DMA not idle before enabling\n");
		return -EIO;
	}

	/* Enable the DMA */
	KVASER_PCIEFD_SRB_DMA_ENABLE(pcie);

	return 0;
}

static int kvaser_pciefd_setup_board(struct kvaser_pciefd *pcie)
{
	pcie->nr_channels = min(KVASER_PCIEFD_MAX_CAN_CHANNELS,
				KVASER_PCIEFD_SYSID_VERSION_NUM_CHANNELS_GET(pcie));

	dev_dbg(&pcie->pci->dev, "Version %u.%u.%u\n",
		KVASER_PCIEFD_SYSID_VERSION_MAJOR_GET(pcie),
		KVASER_PCIEFD_SYSID_VERSION_MINOR_GET(pcie),
		KVASER_PCIEFD_SYSID_BUILD_GET(pcie));

	if (!(KVASER_PCIEFD_SRB_STAT_GET(pcie) & KVASER_PCIEFD_SRB_STAT_DMA)) {
		dev_err(&pcie->pci->dev, "Hardware without DMA is not supported\n");
		return -ENODEV;
	}

	pcie->bus_freq = KVASER_PCIEFD_SYSID_BUSFREQ_GET(pcie);
	pcie->freq = KVASER_PCIEFD_SYSID_CANFREQ_GET(pcie);
	pcie->freq_to_ticks_div = pcie->freq / 1000000;
	if (pcie->freq_to_ticks_div == 0)
		pcie->freq_to_ticks_div = 1;

	/* Turn off all loopback functionality */
	KVASER_PCIEFD_LOOPBACK_DISABLE(pcie);
	return 0;
}

static int kvaser_pciefd_handle_data_packet(struct kvaser_pciefd *pcie,
					    struct kvaser_pciefd_rx_packet *p,
					    __le32 *data)
{
	struct sk_buff *skb;
	struct canfd_frame *cf;
	struct can_priv *priv;
	struct net_device_stats *stats;
	u8 ch_id = KVASER_PCIEFD_PACKET_CHID(p);

	if (ch_id >= pcie->nr_channels)
		return -EIO;

	priv = &pcie->can[ch_id]->can;
	stats = &priv->dev->stats;

	if (p->header[1] & KVASER_PCIEFD_RPACKET_FDF) {
		skb = alloc_canfd_skb(priv->dev, &cf);
		if (!skb) {
			stats->rx_dropped++;
			return -ENOMEM;
		}

		if (p->header[1] & KVASER_PCIEFD_RPACKET_BRS)
			cf->flags |= CANFD_BRS;

		if (p->header[1] & KVASER_PCIEFD_RPACKET_ESI)
			cf->flags |= CANFD_ESI;
	} else {
		skb = alloc_can_skb(priv->dev, (struct can_frame **)&cf);
		if (!skb) {
			stats->rx_dropped++;
			return -ENOMEM;
		}
	}

	cf->can_id = p->header[0] & CAN_EFF_MASK;
	if (p->header[0] & KVASER_PCIEFD_RPACKET_IDE)
		cf->can_id |= CAN_EFF_FLAG;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0))
	cf->len = can_fd_dlc2len(p->header[1] >> KVASER_PCIEFD_RPACKET_DLC_SHIFT);
#else
	cf->len = can_dlc2len(p->header[1] >> KVASER_PCIEFD_RPACKET_DLC_SHIFT);
#endif /* LINUX_VERSION_CODE >= 5.11.0) */

	if (p->header[0] & KVASER_PCIEFD_RPACKET_RTR) {
		cf->can_id |= CAN_RTR_FLAG;
	} else {
		memcpy(cf->data, data, cf->len);

		stats->rx_bytes += cf->len;
	}
	stats->rx_packets++;
	kvaser_pciefd_set_skb_timestamp(pcie, skb, p->timestamp);

	return netif_rx(skb);
}

static void kvaser_pciefd_change_state(struct kvaser_pciefd_can *can,
				       struct can_frame *cf,
				       enum can_state new_state,
				       enum can_state tx_state,
				       enum can_state rx_state)
{
	can_change_state(can->can.dev, cf, tx_state, rx_state);

	if (new_state == CAN_STATE_BUS_OFF) {
		struct net_device *ndev = can->can.dev;
		unsigned long irq_flags;

		spin_lock_irqsave(&can->lock, irq_flags);
		netif_stop_queue(can->can.dev);
		spin_unlock_irqrestore(&can->lock, irq_flags);

		/* Prevent CAN controller from auto recover from bus off */
		if (!can->can.restart_ms) {
			kvaser_pciefd_start_controller_flush(can);
			can_bus_off(ndev);
		}
	}
}

static void kvaser_pciefd_packet_to_state(struct kvaser_pciefd_rx_packet *p,
					  struct can_berr_counter *bec,
					  enum can_state *new_state,
					  enum can_state *tx_state,
					  enum can_state *rx_state)
{
	if (p->header[0] & KVASER_PCIEFD_SPACK_BOFF ||
	    p->header[0] & KVASER_PCIEFD_SPACK_IRM)
		*new_state = CAN_STATE_BUS_OFF;
	else if (bec->txerr >= 255 || bec->rxerr >= 255)
		*new_state = CAN_STATE_BUS_OFF;
	else if (p->header[1] & KVASER_PCIEFD_SPACK_EPLR)
		*new_state = CAN_STATE_ERROR_PASSIVE;
	else if (bec->txerr >= 128 || bec->rxerr >= 128)
		*new_state = CAN_STATE_ERROR_PASSIVE;
	else if (p->header[1] & KVASER_PCIEFD_SPACK_EWLR)
		*new_state = CAN_STATE_ERROR_WARNING;
	else if (bec->txerr >= 96 || bec->rxerr >= 96)
		*new_state = CAN_STATE_ERROR_WARNING;
	else
		*new_state = CAN_STATE_ERROR_ACTIVE;

	*tx_state = bec->txerr >= bec->rxerr ? *new_state : 0;
	*rx_state = bec->txerr <= bec->rxerr ? *new_state : 0;
}

static int kvaser_pciefd_rx_error_frame(struct kvaser_pciefd_can *can,
					struct kvaser_pciefd_rx_packet *p)
{
	struct can_berr_counter bec;
	enum can_state old_state, new_state, tx_state, rx_state;
	struct net_device *ndev = can->can.dev;
	struct sk_buff *skb;
	struct can_frame *cf = NULL;
	struct net_device_stats *stats = &ndev->stats;

	old_state = can->can.state;

	bec.txerr = KVASER_PCIEFD_SPACKET_TXERR_COUNT(p);
	bec.rxerr = KVASER_PCIEFD_SPACKET_RXERR_COUNT(p);

	kvaser_pciefd_packet_to_state(p, &bec, &new_state, &tx_state, &rx_state);

	skb = alloc_can_err_skb(ndev, &cf);

	if (new_state != old_state) {
		kvaser_pciefd_change_state(can, cf, new_state, tx_state, rx_state);

		if (old_state == CAN_STATE_BUS_OFF &&
		    new_state == CAN_STATE_ERROR_ACTIVE &&
		    can->can.restart_ms) {
			can->can.can_stats.restarts++;
			if (skb)
				cf->can_id |= CAN_ERR_RESTARTED;
		}
	}

	can->err_rep_cnt++;
	can->can.can_stats.bus_error++;
	if (p->header[1] & KVASER_PCIEFD_EPACK_DIR_TX)
		stats->tx_errors++;
	else
		stats->rx_errors++;

	can->bec.txerr = bec.txerr;
	can->bec.rxerr = bec.rxerr;

	if (!skb) {
		stats->rx_dropped++;
		return -ENOMEM;
	}

	kvaser_pciefd_set_skb_timestamp(can->kv_pcie, skb, p->timestamp);
	cf->can_id |= CAN_ERR_BUSERROR | CAN_ERR_CNT;

	cf->data[6] = bec.txerr;
	cf->data[7] = bec.rxerr;

	netif_rx(skb);
	return 0;
}

static int kvaser_pciefd_handle_error_packet(struct kvaser_pciefd *pcie,
					     struct kvaser_pciefd_rx_packet *p)
{
	struct kvaser_pciefd_can *can;
	u8 ch_id = KVASER_PCIEFD_PACKET_CHID(p);

	if (ch_id >= pcie->nr_channels)
		return -EIO;

	can = pcie->can[ch_id];

	kvaser_pciefd_rx_error_frame(can, p);
	if (can->err_rep_cnt >= KVASER_PCIEFD_MAX_ERR_REP)
		/* Do not report more errors, until bec_poll_timer expires */
		kvaser_pciefd_disable_err_gen(can);
	/* Start polling the error counters */
	mod_timer(&can->bec_poll_timer, KVASER_PCIEFD_BEC_POLL_FREQ);
	return 0;
}

static int kvaser_pciefd_handle_status_resp(struct kvaser_pciefd_can *can,
					    struct kvaser_pciefd_rx_packet *p)
{
	struct can_berr_counter bec;
	enum can_state old_state, new_state, tx_state, rx_state;

	old_state = can->can.state;

	bec.txerr = KVASER_PCIEFD_SPACKET_TXERR_COUNT(p);
	bec.rxerr = KVASER_PCIEFD_SPACKET_RXERR_COUNT(p);

	kvaser_pciefd_packet_to_state(p, &bec, &new_state, &tx_state, &rx_state);

	if (new_state != old_state) {
		struct net_device *ndev = can->can.dev;
		struct sk_buff *skb;
		struct can_frame *cf;

		skb = alloc_can_err_skb(ndev, &cf);
		if (!skb) {
			struct net_device_stats *stats = &ndev->stats;

			stats->rx_dropped++;
			return -ENOMEM;
		}

		kvaser_pciefd_change_state(can, cf, new_state, tx_state, rx_state);

		if (old_state == CAN_STATE_BUS_OFF &&
		    new_state == CAN_STATE_ERROR_ACTIVE &&
		    can->can.restart_ms) {
			can->can.can_stats.restarts++;
			cf->can_id |= CAN_ERR_RESTARTED;
		}

		kvaser_pciefd_set_skb_timestamp(can->kv_pcie, skb, p->timestamp);

		cf->data[6] = bec.txerr;
		cf->data[7] = bec.rxerr;

		netif_rx(skb);
	}
	can->bec.txerr = bec.txerr;
	can->bec.rxerr = bec.rxerr;
	/* Check if we need to poll the error counters */
	if (bec.txerr || bec.rxerr)
		mod_timer(&can->bec_poll_timer, KVASER_PCIEFD_BEC_POLL_FREQ);

	return 0;
}

static int kvaser_pciefd_handle_status_packet(struct kvaser_pciefd *pcie,
					      struct kvaser_pciefd_rx_packet *p)
{
	struct kvaser_pciefd_can *can;
	u8 cmdseq;
	u32 status;
	u8 ch_id = KVASER_PCIEFD_PACKET_CHID(p);

	if (ch_id >= pcie->nr_channels)
		return -EIO;

	can = pcie->can[ch_id];

	status = KVASER_PCIEFD_KCAN_STAT_GET(can);
	cmdseq = (status >> KVASER_PCIEFD_KCAN_STAT_SEQNO_SHIFT) & KVASER_PCIEFD_PACKET_SEQ_MASK;

	/* Reset done, start abort and flush */
	if ((p->header[0] & KVASER_PCIEFD_SPACK_IRM) &&
	    (p->header[0] & KVASER_PCIEFD_SPACK_RMCD) &&
	    (p->header[1] & KVASER_PCIEFD_SPACK_AUTO) &&
	    (cmdseq == (p->header[1] & KVASER_PCIEFD_PACKET_SEQ_MASK)) &&
	    (status & KVASER_PCIEFD_KCAN_STAT_IDLE)) {
		KVASER_PCIEFD_KCAN_IRQ_SET(can, KVASER_PCIEFD_KCAN_IRQ_ABD);
		kvaser_pciefd_kcan_abort_flush_reset(can);
	} else if ((p->header[0] & KVASER_PCIEFD_SPACK_IDET) &&
		   (p->header[0] & KVASER_PCIEFD_SPACK_IRM) &&
		   (cmdseq == (p->header[1] & KVASER_PCIEFD_PACKET_SEQ_MASK)) &&
		   (status & KVASER_PCIEFD_KCAN_STAT_IDLE)) {
		/* Reset detected, send end of flush if no packet are in FIFO */
		u8 count = KVASER_PCIEFD_KCAN_TX_NR_PACKETS_CURRENT_GET(can);

		if (!count)
			KVASER_PCIEFD_KCAN_CTRL_SET(can, KVASER_PCIEFD_KCAN_CTRL_EFLUSH);
	} else if (!(p->header[1] & KVASER_PCIEFD_SPACK_AUTO) &&
		   (cmdseq == (p->header[1] & KVASER_PCIEFD_PACKET_SEQ_MASK))) {
		/* Response to status request received */
		kvaser_pciefd_handle_status_resp(can, p);
		if (can->can.state != CAN_STATE_BUS_OFF &&
		    can->can.state != CAN_STATE_ERROR_ACTIVE) {
			mod_timer(&can->bec_poll_timer,
				  KVASER_PCIEFD_BEC_POLL_FREQ);
		}
	} else if ((p->header[0] & KVASER_PCIEFD_SPACK_RMCD) &&
		   !(status & KVASER_PCIEFD_KCAN_STAT_BUS_OFF_MASK)) {
		/* Reset to bus on detected */
		if (!completion_done(&can->start_comp))
			complete(&can->start_comp);
	}

	return 0;
}

static void kvaser_pciefd_handle_nack_packet(struct kvaser_pciefd_can *can,
					     struct kvaser_pciefd_rx_packet *p)
{
	struct sk_buff *skb;
	struct net_device_stats *stats = &can->can.dev->stats;
	struct can_frame *cf;

	skb = alloc_can_err_skb(can->can.dev, &cf);

	stats->tx_errors++;
	if (p->header[0] & KVASER_PCIEFD_APACKET_ABL) {
		if (skb)
			cf->can_id |= CAN_ERR_LOSTARB;
		can->can.can_stats.arbitration_lost++;
	} else if (skb) {
		cf->can_id |= CAN_ERR_ACK;
	}

	if (skb) {
		cf->can_id |= CAN_ERR_BUSERROR;
		kvaser_pciefd_set_skb_timestamp(can->kv_pcie, skb, p->timestamp);
		netif_rx(skb);
	} else {
		stats->rx_dropped++;
		netdev_warn(can->can.dev, "No memory left for err_skb\n");
	}
}

static int kvaser_pciefd_handle_ack_packet(struct kvaser_pciefd *pcie,
					   struct kvaser_pciefd_rx_packet *p)
{
	struct kvaser_pciefd_can *can;
	bool one_shot_fail = false;
	u8 ch_id = KVASER_PCIEFD_PACKET_CHID(p);

	if (ch_id >= pcie->nr_channels)
		return -EIO;

	can = pcie->can[ch_id];
	/* Ignore control packet ACK */
	if (p->header[0] & KVASER_PCIEFD_APACKET_CT)
		return 0;

	if (p->header[0] & KVASER_PCIEFD_APACKET_NACK) {
		kvaser_pciefd_handle_nack_packet(can, p);
		one_shot_fail = true;
	}

	if (p->header[0] & KVASER_PCIEFD_APACKET_FLU) {
		netdev_dbg(can->can.dev, "Packet was flushed\n");
	} else {
		int echo_idx = p->header[0] & KVASER_PCIEFD_PACKET_SEQ_MASK;
		int dlc;
		u8 count;
		struct sk_buff *skb;

		skb = can->can.echo_skb[echo_idx];
		if (skb)
			kvaser_pciefd_set_skb_timestamp(pcie, skb, p->timestamp);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0))
		dlc = can_get_echo_skb(can->can.dev, echo_idx, NULL);
#else
		dlc = can_get_echo_skb(can->can.dev, echo_idx);
#endif /* LINUX_VERSION_CODE >= 5.12.0) */
		count = KVASER_PCIEFD_KCAN_TX_NR_PACKETS_CURRENT_GET(can);

		if (count < can->can.echo_skb_max &&
		    netif_queue_stopped(can->can.dev))
			netif_wake_queue(can->can.dev);

		if (!one_shot_fail) {
			struct net_device_stats *stats = &can->can.dev->stats;

			stats->tx_bytes += dlc;
			stats->tx_packets++;
		}
	}

	return 0;
}

static int kvaser_pciefd_handle_eflush_packet(struct kvaser_pciefd *pcie,
					      struct kvaser_pciefd_rx_packet *p)
{
	struct kvaser_pciefd_can *can;
	u8 ch_id = KVASER_PCIEFD_PACKET_CHID(p);

	if (ch_id >= pcie->nr_channels)
		return -EIO;

	can = pcie->can[ch_id];

	if (!completion_done(&can->flush_comp))
		complete(&can->flush_comp);

	return 0;
}

static int kvaser_pciefd_read_packet(struct kvaser_pciefd *pcie, int *start_pos,
				     int dma_buf)
{
	__le32 *buffer = pcie->dma_data[dma_buf];
	__le64 timestamp;
	struct kvaser_pciefd_rx_packet packet;
	struct kvaser_pciefd_rx_packet *p = &packet;
	u8 type;
	int pos = *start_pos;
	int size;
	int ret = 0;

	size = le32_to_cpu(buffer[pos++]);
	if (!size) {
		*start_pos = 0;
		return 0;
	}

	p->header[0] = le32_to_cpu(buffer[pos++]);
	p->header[1] = le32_to_cpu(buffer[pos++]);

	/* Read 64-bit timestamp */
	memcpy(&timestamp, &buffer[pos], sizeof(__le64));
	pos += 2;
	p->timestamp = le64_to_cpu(timestamp);

	type = KVASER_PCIEFD_PACKET_TYPE(p);
	switch (type) {
	case KVASER_PCIEFD_PACK_TYPE_DATA:
		ret = kvaser_pciefd_handle_data_packet(pcie, p, &buffer[pos]);
		if (!(p->header[0] & KVASER_PCIEFD_RPACKET_RTR)) {
			u8 data_len;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0))
			data_len = can_fd_dlc2len(p->header[1] >>
#else
			data_len = can_dlc2len(p->header[1] >>
#endif /* LINUX_VERSION_CODE >= 5.11.0) */
					       KVASER_PCIEFD_RPACKET_DLC_SHIFT);
			pos += DIV_ROUND_UP(data_len, 4);
		}
		break;

	case KVASER_PCIEFD_PACK_TYPE_ACK:
		ret = kvaser_pciefd_handle_ack_packet(pcie, p);
		break;

	case KVASER_PCIEFD_PACK_TYPE_STATUS:
		ret = kvaser_pciefd_handle_status_packet(pcie, p);
		break;

	case KVASER_PCIEFD_PACK_TYPE_ERROR:
		ret = kvaser_pciefd_handle_error_packet(pcie, p);
		break;

	case KVASER_PCIEFD_PACK_TYPE_EFLUSH_ACK:
		ret = kvaser_pciefd_handle_eflush_packet(pcie, p);
		break;

	case KVASER_PCIEFD_PACK_TYPE_ACK_DATA:
	case KVASER_PCIEFD_PACK_TYPE_BUS_LOAD:
	case KVASER_PCIEFD_PACK_TYPE_EFRAME_ACK:
	case KVASER_PCIEFD_PACK_TYPE_TXRQ:
		dev_info(&pcie->pci->dev,
			 "Received unexpected packet type 0x%08X\n", type);
		break;

	default:
		dev_err(&pcie->pci->dev, "Unknown packet type 0x%08X\n", type);
		ret = -EIO;
		break;
	}

	if (ret)
		return ret;

	/* Position does not point to the end of the package,
	 * corrupted packet size?
	 */
	if ((*start_pos + size) != pos)
		return -EIO;

	/* Point to the next packet header, if any */
	*start_pos = pos;

	return ret;
}

static int kvaser_pciefd_read_buffer(struct kvaser_pciefd *pcie, int dma_buf)
{
	int pos = 0;
	int res = 0;

	do {
		res = kvaser_pciefd_read_packet(pcie, &pos, dma_buf);
	} while (!res && pos > 0 && pos < KVASER_PCIEFD_DMA_SIZE);

	return res;
}

static void kvaser_pciefd_receive_irq(struct kvaser_pciefd *pcie)
{
	u32 irq = KVASER_PCIEFD_SRB_IRQ_GET(pcie);

	if (irq & KVASER_PCIEFD_SRB_IRQ_DPD0) {
		kvaser_pciefd_read_buffer(pcie, 0);
		/* Reset DMA buffer 0 */
		KVASER_PCIEFD_SRB_CMD_SET(pcie, KVASER_PCIEFD_SRB_CMD_RDB0);
	}

	if (irq & KVASER_PCIEFD_SRB_IRQ_DPD1) {
		kvaser_pciefd_read_buffer(pcie, 1);
		/* Reset DMA buffer 1 */
		KVASER_PCIEFD_SRB_CMD_SET(pcie, KVASER_PCIEFD_SRB_CMD_RDB1);
	}

	if (irq & KVASER_PCIEFD_SRB_IRQ_DOF0 ||
	    irq & KVASER_PCIEFD_SRB_IRQ_DOF1 ||
	    irq & KVASER_PCIEFD_SRB_IRQ_DUF0 ||
	    irq & KVASER_PCIEFD_SRB_IRQ_DUF1)
		dev_err(&pcie->pci->dev, "DMA IRQ error 0x%08X\n", irq);

	KVASER_PCIEFD_SRB_IRQ_SET(pcie, irq);
}

static void kvaser_pciefd_transmit_irq(struct kvaser_pciefd_can *can)
{
	u32 irq = KVASER_PCIEFD_KCAN_IRQ_GET(can);

	if (irq & KVASER_PCIEFD_KCAN_IRQ_TOF)
		netdev_err(can->can.dev, "Tx FIFO overflow\n");

	if (irq & KVASER_PCIEFD_KCAN_IRQ_BPP)
		netdev_err(can->can.dev,
			   "Fail to change bittiming, when not in reset mode\n");

	if (irq & KVASER_PCIEFD_KCAN_IRQ_FDIC)
		netdev_err(can->can.dev, "CAN FD frame in CAN mode\n");

	if (irq & KVASER_PCIEFD_KCAN_IRQ_ROF)
		netdev_err(can->can.dev, "Rx FIFO overflow\n");

	KVASER_PCIEFD_KCAN_IRQ_SET(can, irq);
}

static irqreturn_t kvaser_pciefd_irq_handler(int irq, void *dev)
{
	struct kvaser_pciefd *pcie = (struct kvaser_pciefd *)dev;
	const struct kvaser_pciefd_irq_mask *irq_mask = pcie->driver_data->irq_mask;
	u32 board_irq = KVASER_PCIEFD_PCI_IRQ_GET(pcie);
	int i;

	if (!(board_irq & irq_mask->all))
		return IRQ_NONE;

	if (board_irq & irq_mask->kcan_rx0)
		kvaser_pciefd_receive_irq(pcie);

	for (i = 0; i < pcie->nr_channels; i++) {
		if (!pcie->can[i]) {
			dev_err(&pcie->pci->dev,
				"IRQ mask points to unallocated controller\n");
			break;
		}

		/* Check that mask matches channel (i) IRQ mask */
		if (board_irq & irq_mask->kcan_tx[i])
			kvaser_pciefd_transmit_irq(pcie->can[i]);
	}

	return IRQ_HANDLED;
}

static void kvaser_pciefd_teardown_can_ctrls(struct kvaser_pciefd *pcie)
{
	int i;

	for (i = 0; i < pcie->nr_channels; i++) {
		struct kvaser_pciefd_can *can = pcie->can[i];

		if (can) {
			KVASER_PCIEFD_KCAN_IEN_DISABLE_ALL(can);
			kvaser_pciefd_pwm_stop(can);
			free_candev(can->can.dev);
		}
	}
}

static int kvaser_pciefd_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	int err;
	struct kvaser_pciefd *pcie;
	const struct kvaser_pciefd_irq_mask *irq_mask;

	pcie = devm_kzalloc(&pdev->dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci_set_drvdata(pdev, pcie);
	pcie->pci = pdev;
	pcie->driver_data = (const struct kvaser_pciefd_driver_data *)id->driver_data;
	irq_mask = pcie->driver_data->irq_mask;

	err = pci_enable_device(pdev);
	if (err)
		return err;

	err = pci_request_regions(pdev, KVASER_PCIEFD_DRV_NAME);
	if (err)
		goto err_disable_pci;

	pcie->reg_base = pci_iomap(pdev, 0, 0);
	if (!pcie->reg_base) {
		err = -ENOMEM;
		goto err_release_regions;
	}

	err = kvaser_pciefd_setup_board(pcie);
	if (err)
		goto err_pci_iounmap;

	err = kvaser_pciefd_setup_dma(pcie);
	if (err)
		goto err_pci_iounmap;

	pci_set_master(pdev);

	err = kvaser_pciefd_setup_can_ctrls(pcie);
	if (err)
		goto err_teardown_can_ctrls;

	err = request_irq(pcie->pci->irq, kvaser_pciefd_irq_handler,
			  IRQF_SHARED, KVASER_PCIEFD_DRV_NAME, pcie);
	if (err)
		goto err_teardown_can_ctrls;

	/* Enable shared receive buffer interrupts */
	KVASER_PCIEFD_SRB_IRQ_SET(pcie, KVASER_PCIEFD_SRB_IRQ_DPD0 |
						KVASER_PCIEFD_SRB_IRQ_DPD1);
	KVASER_PCIEFD_SRB_IEN_ENABLE_ALL(pcie);

	/* Enable PCI interrupts */
	KVASER_PCIEFD_PCI_IEN_ENABLE_ALL(pcie);
	/* Ready the DMA buffers */
	KVASER_PCIEFD_SRB_CMD_SET(pcie, KVASER_PCIEFD_SRB_CMD_RDB0);
	KVASER_PCIEFD_SRB_CMD_SET(pcie, KVASER_PCIEFD_SRB_CMD_RDB1);

	err = kvaser_pciefd_reg_candev(pcie);
	if (err)
		goto err_free_irq;

	return 0;

err_free_irq:
	/* Disable PCI interrupts */
	KVASER_PCIEFD_PCI_IEN_DISABLE_ALL(pcie);
	free_irq(pcie->pci->irq, pcie);

err_teardown_can_ctrls:
	kvaser_pciefd_teardown_can_ctrls(pcie);
	KVASER_PCIEFD_SRB_DMA_DISABLE(pcie);
	pci_clear_master(pdev);

err_pci_iounmap:
	pci_iounmap(pdev, pcie->reg_base);

err_release_regions:
	pci_release_regions(pdev);

err_disable_pci:
	pci_disable_device(pdev);

	return err;
}

static void kvaser_pciefd_remove_all_ctrls(struct kvaser_pciefd *pcie)
{
	int i;

	for (i = 0; i < pcie->nr_channels; i++) {
		struct kvaser_pciefd_can *can = pcie->can[i];

		if (can) {
			KVASER_PCIEFD_KCAN_IEN_DISABLE_ALL(can);
			unregister_candev(can->can.dev);
			del_timer(&can->bec_poll_timer);
			kvaser_pciefd_pwm_stop(can);
			free_candev(can->can.dev);
		}
	}
}

static void kvaser_pciefd_remove(struct pci_dev *pdev)
{
	struct kvaser_pciefd *pcie = pci_get_drvdata(pdev);

	kvaser_pciefd_remove_all_ctrls(pcie);

	/* Disable interrupts */
	KVASER_PCIEFD_SRB_DMA_DISABLE(pcie);
	KVASER_PCIEFD_PCI_IEN_DISABLE_ALL(pcie);

	free_irq(pcie->pci->irq, pcie);

	pci_clear_master(pdev);
	pci_iounmap(pdev, pcie->reg_base);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static struct pci_driver kvaser_pciefd = {
	.name = KVASER_PCIEFD_DRV_NAME,
	.id_table = kvaser_pciefd_id_table,
	.probe = kvaser_pciefd_probe,
	.remove = kvaser_pciefd_remove,
};

module_pci_driver(kvaser_pciefd)
