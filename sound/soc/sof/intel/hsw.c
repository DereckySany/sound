// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

/*
 * Hardwre interface for audio DSP on Haswell
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>

#include <trace/events/hswadsp.h>
#include <sound/sof.h>

#include "../sof-priv.h"
#include "../ops.h"
#include "shim.h"

/* BARs */
#define HSW_DSP_BAR 0
#define HSW_PCI_BAR 1

/*
 * Debug
 */

/* DSP memories for HSW */
#define IRAM_OFFSET     0x80000
#define HSW_IRAM_SIZE       (10 * 32 * 1024)
#define DRAM_OFFSET     0x00000
#define HSW_DRAM_SIZE       (16 * 32 * 1024)
#define SHIM_OFFSET     0xE7000
#define SHIM_SIZE       0x100
#define MBOX_OFFSET     0x7E000
#define MBOX_SIZE       0x1000
#define MBOX_DUMP_SIZE 0x30
#define EXCEPT_OFFSET	0x800

/* DSP peripherals */
#define DMAC0_OFFSET    0xFE000
#define DMAC1_OFFSET    0xFF000
#define DMAC_SIZE       0x420
#define SSP0_OFFSET     0xFC000
#define SSP1_OFFSET     0xFD000
#define SSP_SIZE	0x100

#define HSW_STACK_DUMP_SIZE	32

#define HSW_PANIC_OFFSET(x)	((x) & 0xFFFF)

static const struct snd_sof_debugfs_map hsw_debugfs[] = {
	{"dmac0", HSW_DSP_BAR, DMAC0_OFFSET, DMAC_SIZE},
	{"dmac1", HSW_DSP_BAR, DMAC1_OFFSET, DMAC_SIZE},
	{"ssp0", HSW_DSP_BAR, SSP0_OFFSET, SSP_SIZE},
	{"ssp1", HSW_DSP_BAR, SSP1_OFFSET, SSP_SIZE},
	{"iram", HSW_DSP_BAR, IRAM_OFFSET, HSW_IRAM_SIZE},
	{"dram", HSW_DSP_BAR, DRAM_OFFSET, HSW_DRAM_SIZE},
	{"shim", HSW_DSP_BAR, SHIM_OFFSET, SHIM_SIZE},
};

/*
 * Memory copy.
 */

/* write has to deal with copying non 32 bit sized data */
static void hsw_block_write(struct snd_sof_dev *sdev, u32 offset, void *src,
			    size_t size)
{
	void __iomem *dest = sdev->bar[sdev->mmio_bar] + offset;
	u32 tmp = 0;
	int i, m, n;
	const u8 *src_byte = src;

	m = size / 4;
	n = size % 4;

	/* __iowrite32_copy use 32bit size values so divide by 4 */
	__iowrite32_copy((void *)dest, src, m);

	if (n) {
		for (i = 0; i < n; i++)
			tmp |= (u32)*(src_byte + m * 4 + i) << (i * 8);
		__iowrite32_copy((void *)(dest + m * 4), &tmp, 1);
	}
}

static void hsw_block_read(struct snd_sof_dev *sdev, u32 offset, void *dest,
			   size_t size)
{
	void __iomem *src = sdev->bar[sdev->mmio_bar] + offset;

	memcpy_fromio(dest, src, size);
}

static void hsw_mailbox_write(struct snd_sof_dev *sdev, u32 offset,
			      void *message, size_t bytes)
{
	void __iomem *dest = sdev->bar[sdev->mailbox_bar] + offset;

	memcpy_toio(dest, message, bytes);
}

static void hsw_mailbox_read(struct snd_sof_dev *sdev, u32 offset,
			     void *message, size_t bytes)
{
	void __iomem *src = sdev->bar[sdev->mailbox_bar] + offset;

	memcpy_fromio(message, src, bytes);
}

/*
 * Register IO
 */

static void hsw_write(struct snd_sof_dev *sdev, void __iomem *addr,
		      u32 value)
{
	writel(value, addr);
}

static u32 hsw_read(struct snd_sof_dev *sdev, void __iomem *addr)
{
	return readl(addr);
}

static void hsw_write64(struct snd_sof_dev *sdev, void __iomem *addr,
			u64 value)
{
	memcpy_toio(addr, &value, sizeof(value));
}

static u64 hsw_read64(struct snd_sof_dev *sdev, void __iomem *addr)
{
	u64 val;

	memcpy_fromio(&val, addr, sizeof(val));
	return val;
}

/*
 * DSP Control.
 */

static int hsw_run(struct snd_sof_dev *sdev)
{
	/* set oportunistic mode on engine 0,1 for all channels */
	snd_sof_dsp_update_bits(sdev, HSW_DSP_BAR, SHIM_HMDC,
				SHIM_HMDC_HDDA_E0_ALLCH |
				SHIM_HMDC_HDDA_E1_ALLCH, 0);

	/* set DSP to RUN */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_CSR,
					 SHIM_CSR_STALL, 0x0);

	return 0; //TODO: Fix return value
}

static int hsw_reset(struct snd_sof_dev *sdev)
{
	/* put DSP into reset and stall */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_CSR,
					 SHIM_CSR_RST | SHIM_CSR_STALL,
					 SHIM_CSR_RST | SHIM_CSR_STALL);

	/* keep in reset for 10ms */
	mdelay(10);

	/* take DSP out of reset and keep stalled for FW loading */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_CSR,
					 SHIM_CSR_RST | SHIM_CSR_STALL,
					 SHIM_CSR_STALL);

	return 0; //TODO: Fix return value
}

static int hsw_set_dsp_D0(struct snd_sof_dev *sdev)
{
	int tries = 10;
	u32 reg;

	/* Disable core clock gating (VDRTCTL2.DCLCGE = 0) */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_PCI_BAR, PCI_VDRTCTL2,
					 PCI_VDRTCL2_DCLCGE | PCI_VDRTCL2_DTCGE,
					 0);

	/* Disable D3PG (VDRTCTL0.D3PGD = 1) */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_PCI_BAR, PCI_VDRTCTL0,
					 PCI_VDRTCL0_D3PGD, PCI_VDRTCL0_D3PGD);

	/* Set D0 state */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_PCI_BAR, PCI_PMCS,
					 PCI_PMCS_PS_MASK, 0);

	/* check that ADSP shim is enabled */
	while (tries--) {
		reg = readl(sdev->bar[HSW_PCI_BAR] + PCI_PMCS)
			& PCI_PMCS_PS_MASK;
		if (reg == 0)
			goto finish;

		msleep(20);
	}

	return -ENODEV;

finish:
	/*
	 * select SSP1 19.2MHz base clock, SSP clock 0,
	 * turn off Low Power Clock
	 */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_CSR,
					 SHIM_CSR_S1IOCS | SHIM_CSR_SBCS1 |
					 SHIM_CSR_LPCS, 0x0);

	/* stall DSP core, set clk to 192/96Mhz */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR,
					 SHIM_CSR, SHIM_CSR_STALL |
					 SHIM_CSR_DCS_MASK,
					 SHIM_CSR_STALL | SHIM_CSR_DCS(4));

	/* Set 24MHz MCLK, prevent local clock gating, enable SSP0 clock */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_CLKCTL,
					 SHIM_CLKCTL_MASK | SHIM_CLKCTL_DCPLCG |
					 SHIM_CLKCTL_SCOE0,
					 SHIM_CLKCTL_MASK | SHIM_CLKCTL_DCPLCG |
					 SHIM_CLKCTL_SCOE0);

	/* Stall and reset core, set CSR */
	hsw_reset(sdev);

	/* Enable core clock gating (VDRTCTL2.DCLCGE = 1), delay 50 us */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_PCI_BAR, PCI_VDRTCTL2,
					 PCI_VDRTCL2_DCLCGE |
					 PCI_VDRTCL2_DTCGE,
					 PCI_VDRTCL2_DCLCGE |
					 PCI_VDRTCL2_DTCGE);

	usleep_range(50, 55);

	/* switch on audio PLL */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_PCI_BAR, PCI_VDRTCTL2,
					 PCI_VDRTCL2_APLLSE_MASK, 0);

	/*
	 * set default power gating control, enable power gating control for
	 * all blocks. that is, can't be accessed, please enable each block
	 * before accessing.
	 */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_PCI_BAR, PCI_VDRTCTL0,
					 PCI_VDRTCL0_DSRAMPGE_MASK |
					 PCI_VDRTCL0_ISRAMPGE_MASK, 0);

	/* disable DMA finish function for SSP0 & SSP1 */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR,  SHIM_CSR2,
					 SHIM_CSR2_SDFD_SSP1,
					 SHIM_CSR2_SDFD_SSP1);

	/* set on-demond mode on engine 0,1 for all channels */
	snd_sof_dsp_update_bits(sdev, HSW_DSP_BAR, SHIM_HMDC,
				SHIM_HMDC_HDDA_E0_ALLCH |
				SHIM_HMDC_HDDA_E1_ALLCH,
				SHIM_HMDC_HDDA_E0_ALLCH |
				SHIM_HMDC_HDDA_E1_ALLCH);

	/* Enable Interrupt from both sides */
	snd_sof_dsp_update_bits(sdev, HSW_DSP_BAR, SHIM_IMRX,
				(SHIM_IMRX_BUSY | SHIM_IMRX_DONE), 0x0);
	snd_sof_dsp_update_bits(sdev, HSW_DSP_BAR, SHIM_IMRD,
				(SHIM_IMRD_DONE | SHIM_IMRD_BUSY |
				SHIM_IMRD_SSP0 | SHIM_IMRD_DMAC), 0x0);

	/* clear IPC registers */
	snd_sof_dsp_write(sdev, HSW_DSP_BAR, SHIM_IPCX, 0x0);
	snd_sof_dsp_write(sdev, HSW_DSP_BAR, SHIM_IPCD, 0x0);
	snd_sof_dsp_write(sdev, HSW_DSP_BAR, 0x80, 0x6);
	snd_sof_dsp_write(sdev, HSW_DSP_BAR, 0xe0, 0x300a);

	return 0;
}

static void hsw_get_registers(struct snd_sof_dev *sdev,
			      struct sof_ipc_dsp_oops_xtensa *xoops,
			      u32 *stack, size_t stack_words)
{
	/* first read regsisters */
	hsw_mailbox_read(sdev, sdev->dsp_oops_offset, xoops, sizeof(*xoops));

	/* the get the stack */
	hsw_mailbox_read(sdev, sdev->dsp_oops_offset + sizeof(*xoops), stack,
			 stack_words * sizeof(u32));
}

static void hsw_dump(struct snd_sof_dev *sdev, u32 flags)
{
	struct sof_ipc_dsp_oops_xtensa xoops;
	u32 stack[HSW_STACK_DUMP_SIZE];
	u32 status, panic;

	/* now try generic SOF status messages */
	status = snd_sof_dsp_read(sdev, HSW_DSP_BAR, SHIM_IPCD);
	panic = snd_sof_dsp_read(sdev, HSW_DSP_BAR, SHIM_IPCX);
	hsw_get_registers(sdev, &xoops, stack, HSW_STACK_DUMP_SIZE);
	snd_sof_get_status(sdev, status, panic, &xoops, stack,
			   HSW_STACK_DUMP_SIZE);
}

/*
 * IPC Doorbell IRQ handler and thread.
 */

static irqreturn_t hsw_irq_handler(int irq, void *context)
{
	struct snd_sof_dev *sdev = (struct snd_sof_dev *)context;
	u32 isr;
	int ret = IRQ_NONE;

	spin_lock(&sdev->hw_lock);

	/* Interrupt arrived, check src */
	isr = snd_sof_dsp_read(sdev, HSW_DSP_BAR, SHIM_ISRX);
	if (isr & SHIM_ISRX_DONE) {
		/* Mask Done interrupt before return */
		snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_IMRX,
						 SHIM_IMRX_DONE,
						 SHIM_IMRX_DONE);
		ret = IRQ_WAKE_THREAD;
	}

	if (isr & SHIM_ISRX_BUSY) {
		/* Mask Busy interrupt before return */
		snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_IMRX,
						 SHIM_IMRX_BUSY,
						 SHIM_IMRX_BUSY);
		ret = IRQ_WAKE_THREAD;
	}

	spin_unlock(&sdev->hw_lock);
	return ret;
}

static irqreturn_t hsw_irq_thread(int irq, void *context)
{
	struct snd_sof_dev *sdev = (struct snd_sof_dev *)context;
	u32 ipcx, ipcd, hdr;

	ipcx = snd_sof_dsp_read(sdev, HSW_DSP_BAR, SHIM_IPCX);

	/* reply message from DSP */
	if (ipcx & SHIM_IPCX_DONE) {
		/* Handle Immediate reply from DSP Core */
		hsw_mailbox_read(sdev, sdev->host_box.offset, &hdr,
				 sizeof(hdr));
		snd_sof_ipc_reply(sdev, hdr);

		/* clear DONE bit - tell DSP we have completed */
		snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_IPCX,
						 SHIM_IPCX_DONE, 0);

		/* unmask Done interrupt */
		snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_IMRX,
						 SHIM_IMRX_DONE, 0);
	}

	ipcd = snd_sof_dsp_read(sdev, HSW_DSP_BAR, SHIM_IPCD);

	/* new message from DSP */
	if (ipcd & SHIM_IPCD_BUSY) {
		/* Handle messages from DSP Core */
		if ((ipcd & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
			snd_sof_dsp_panic(sdev, HSW_PANIC_OFFSET(ipcx) +
					  MBOX_OFFSET);
		} else {
			snd_sof_ipc_msgs_rx(sdev);
		}
	}

	return IRQ_HANDLED;
}

/*
 * IPC Firmware ready.
 */
static void hsw_get_windows(struct snd_sof_dev *sdev)
{
	struct sof_ipc_window_elem *elem;
	u32 outbox_offset = 0;
	u32 stream_offset = 0;
	u32 inbox_offset = 0;
	u32 outbox_size = 0;
	u32 stream_size = 0;
	u32 inbox_size = 0;
	int i;

	if (!sdev->info_window) {
		dev_err(sdev->dev, "error: have no window info\n");
		return;
	}

	for (i = 0; i < sdev->info_window->num_windows; i++) {
		elem = &sdev->info_window->window[i];

		switch (elem->type) {
		case SOF_IPC_REGION_UPBOX:
			inbox_offset = elem->offset + MBOX_OFFSET;
			inbox_size = elem->size;
			snd_sof_debugfs_create_item(sdev,
						    sdev->bar[HSW_DSP_BAR] +
						    inbox_offset,
						    elem->size, "inbox");
			break;
		case SOF_IPC_REGION_DOWNBOX:
			outbox_offset = elem->offset + MBOX_OFFSET;
			outbox_size = elem->size;
			snd_sof_debugfs_create_item(sdev,
						    sdev->bar[HSW_DSP_BAR] +
						    outbox_offset,
						    elem->size, "outbox");
			break;
		case SOF_IPC_REGION_TRACE:
			snd_sof_debugfs_create_item(sdev,
						    sdev->bar[HSW_DSP_BAR] +
						    elem->offset + MBOX_OFFSET,
						    elem->size, "etrace");
			break;
		case SOF_IPC_REGION_DEBUG:
			snd_sof_debugfs_create_item(sdev,
						    sdev->bar[HSW_DSP_BAR] +
						    elem->offset + MBOX_OFFSET,
						    elem->size, "debug");
			break;
		case SOF_IPC_REGION_STREAM:
			stream_offset = elem->offset + MBOX_OFFSET;
			stream_size = elem->size;
			snd_sof_debugfs_create_item(sdev,
						    sdev->bar[HSW_DSP_BAR] +
						    stream_offset,
						    elem->size, "stream");
			break;
		case SOF_IPC_REGION_REGS:
			snd_sof_debugfs_create_item(sdev,
						    sdev->bar[HSW_DSP_BAR] +
						    elem->offset + MBOX_OFFSET,
						    elem->size, "regs");
			break;
		case SOF_IPC_REGION_EXCEPTION:
			sdev->dsp_oops_offset = elem->offset + MBOX_OFFSET;
			snd_sof_debugfs_create_item(sdev,
						    sdev->bar[HSW_DSP_BAR] +
						    elem->offset + MBOX_OFFSET,
						    elem->size, "exception");
			break;
		default:
			dev_err(sdev->dev, "error: get illegal window info\n");
			return;
		}
	}

	if (outbox_size == 0 || inbox_size == 0) {
		dev_err(sdev->dev, "error: get illegal mailbox window\n");
		return;
	}

	snd_sof_dsp_mailbox_init(sdev, inbox_offset, inbox_size,
				 outbox_offset, outbox_size);
	sdev->stream_box.offset = stream_offset;
	sdev->stream_box.size = stream_size;

	dev_dbg(sdev->dev, " mailbox upstream 0x%x - size 0x%x\n",
		inbox_offset, inbox_size);
	dev_dbg(sdev->dev, " mailbox downstream 0x%x - size 0x%x\n",
		outbox_offset, outbox_size);
	dev_dbg(sdev->dev, " stream region 0x%x - size 0x%x\n",
		stream_offset, stream_size);
}

static int hsw_fw_ready(struct snd_sof_dev *sdev, u32 msg_id)
{
	struct sof_ipc_fw_ready *fw_ready = &sdev->fw_ready;
	struct sof_ipc_fw_version *v = &fw_ready->version;
	u32 offset;

	/* mailbox must be on 4k boundary */
	offset = MBOX_OFFSET;

	dev_dbg(sdev->dev, "ipc: DSP is ready 0x%8.8x offset %d\n",
		msg_id, offset);

	/* copy data from the DSP FW ready offset */
	hsw_block_read(sdev, offset, fw_ready, sizeof(*fw_ready));

	snd_sof_dsp_mailbox_init(sdev, fw_ready->dspbox_offset,
				 fw_ready->dspbox_size,
				 fw_ready->hostbox_offset,
				 fw_ready->hostbox_size);

	dev_info(sdev->dev,
		 " Firmware info: version %d:%d-%s build %d on %s:%s\n",
		 v->major, v->minor, v->tag, v->build, v->date, v->time);

	/* now check for extended data */
	snd_sof_fw_parse_ext_data(sdev, MBOX_OFFSET +
				  sizeof(struct sof_ipc_fw_ready));

	hsw_get_windows(sdev);

	return 0;
}

/*
 * IPC Mailbox IO
 */

static int hsw_is_ready(struct snd_sof_dev *sdev)
{
	u32 val;

	val = snd_sof_dsp_read(sdev, HSW_DSP_BAR, SHIM_IPCX);
	if (val & SHIM_IPCX_BUSY)
		return 0;

	return 1;
}

static int hsw_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	/* send the message */
	hsw_mailbox_write(sdev, sdev->host_box.offset, msg->msg_data,
			  msg->msg_size);
	snd_sof_dsp_write(sdev, HSW_DSP_BAR, SHIM_IPCX, SHIM_IPCX_BUSY);

	return 0;
}

static int hsw_get_reply(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct sof_ipc_reply reply;
	int ret = 0;
	u32 size;

	/* get reply */
	hsw_mailbox_read(sdev, sdev->host_box.offset, &reply, sizeof(reply));
	if (reply.error < 0) {
		size = sizeof(reply);
		ret = reply.error;
	} else {
		/* reply correct size ? */
		if (reply.hdr.size != msg->reply_size) {
			dev_err(sdev->dev, "error: reply expected 0x%zx got 0x%x bytes\n",
				msg->reply_size, reply.hdr.size);
			size = msg->reply_size;
			ret = -EINVAL;
		} else {
			size = reply.hdr.size;
		}
	}

	/* read the message */
	if (msg->msg_data && size > 0)
		hsw_mailbox_read(sdev, sdev->host_box.offset, msg->reply_data,
				 size);
	return ret;
}

static int hsw_cmd_done(struct snd_sof_dev *sdev)
{
	/* clear BUSY bit and set DONE bit - accept new messages */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_IPCD,
					 SHIM_IPCD_BUSY | SHIM_IPCD_DONE,
					 SHIM_IPCD_DONE);

	/* unmask busy interrupt */
	snd_sof_dsp_update_bits_unlocked(sdev, HSW_DSP_BAR, SHIM_IMRX,
					 SHIM_IMRX_BUSY, 0);

	return 0;
}

/*
 * Probe and remove.
 */
static int hsw_probe(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *pdata = sdev->pdata;
	const struct sof_dev_desc *desc = pdata->desc;
	struct platform_device *pdev =
		container_of(sdev->parent, struct platform_device, dev);
	struct resource *mmio;
	u32 base, size;
	int ret = 0;

	/* LPE base */
	mmio = platform_get_resource(pdev, IORESOURCE_MEM,
				     desc->resindex_lpe_base);
	if (mmio) {
		base = mmio->start;
		size = resource_size(mmio);
	} else {
		dev_err(sdev->dev, "error: failed to get LPE base at idx %d\n",
			desc->resindex_lpe_base);
		return -EINVAL;
	}

	dev_dbg(sdev->dev, "LPE PHY base at 0x%x size 0x%x", base, size);
	sdev->bar[HSW_DSP_BAR] = ioremap(base, size);
	if (!sdev->bar[HSW_DSP_BAR]) {
		dev_err(sdev->dev,
			"error: failed to ioremap LPE base 0x%x size 0x%x\n",
			base, size);
		return -ENODEV;
	}
	dev_dbg(sdev->dev, "LPE VADDR %p\n", sdev->bar[HSW_DSP_BAR]);

	/* TODO: add offsets */
	sdev->mmio_bar = HSW_DSP_BAR;
	sdev->mailbox_bar = HSW_DSP_BAR;

	/* PCI base */
	mmio = platform_get_resource(pdev, IORESOURCE_MEM,
				     desc->resindex_pcicfg_base);
	if (mmio) {
		base = mmio->start;
		size = resource_size(mmio);
	} else {
		dev_err(sdev->dev, "error: failed to get PCI base at idx %d\n",
			desc->resindex_pcicfg_base);
		ret = -ENODEV;
		goto pci_err;
	}

	dev_dbg(sdev->dev, "PCI base at 0x%x size 0x%x", base, size);
	sdev->bar[HSW_PCI_BAR] = ioremap(base, size);
	if (!sdev->bar[HSW_PCI_BAR]) {
		dev_err(sdev->dev,
			"error: failed to ioremap PCI base 0x%x size 0x%x\n",
			base, size);
		ret = -ENODEV;
		goto pci_err;
	}
	dev_dbg(sdev->dev, "PCI VADDR %p\n", sdev->bar[HSW_PCI_BAR]);

	/* register our IRQ */
	sdev->ipc_irq = platform_get_irq(pdev, desc->irqindex_host_ipc);
	if (sdev->ipc_irq < 0) {
		dev_err(sdev->dev, "error: failed to get IRQ at index %d\n",
			desc->irqindex_host_ipc);
		ret = sdev->ipc_irq;
		goto irq_err;
	}

	dev_dbg(sdev->dev, "using IRQ %d\n", sdev->ipc_irq);
	ret = request_threaded_irq(sdev->ipc_irq, hsw_irq_handler,
				   hsw_irq_thread, 0, "AudioDSP", sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to register IRQ %d\n",
			sdev->ipc_irq);
		goto irq_err;
	}

	/* enable the DSP SHIM */
	ret = hsw_set_dsp_D0(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to set DSP D0\n");
		return ret;
	}

	/* DSP DMA can only access low 31 bits of host memory */
	ret = dma_coerce_mask_and_coherent(sdev->dev, DMA_BIT_MASK(31));
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to set DMA mask %d\n", ret);
		return ret;
	}

	/* set BARS */
	sdev->cl_bar = HSW_DSP_BAR;

	/* set default mailbox */
	snd_sof_dsp_mailbox_init(sdev, MBOX_OFFSET, MBOX_SIZE, 0, 0);

	return ret;

irq_err:
	iounmap(sdev->bar[HSW_DSP_BAR]);
pci_err:
	iounmap(sdev->bar[HSW_PCI_BAR]);
	return ret;
}

static int hsw_remove(struct snd_sof_dev *sdev)
{
	iounmap(sdev->bar[HSW_DSP_BAR]);
	iounmap(sdev->bar[HSW_PCI_BAR]);
	free_irq(sdev->ipc_irq, sdev);
	return 0;
}

/* haswell ops */
struct snd_sof_dsp_ops sof_hsw_ops = {
	/*Device init */
	.probe          = hsw_probe,
	.remove         = hsw_remove,

	/* DSP Core Control */
	.run            = hsw_run,
	.reset          = hsw_reset,

	/* Register IO */
	.read           = hsw_read,
	.write          = hsw_write,
	.read64         = hsw_read64,
	.write64        = hsw_write64,

	/* Block IO */
	.block_read     = hsw_block_read,
	.block_write    = hsw_block_write,

	/* mailbox */
	.mailbox_read   = hsw_mailbox_read,
	.mailbox_write  = hsw_mailbox_write,

	/* ipc */
	.send_msg	= hsw_send_msg,
	.get_reply	= hsw_get_reply,
	.fw_ready	= hsw_fw_ready,
	.is_ready	= hsw_is_ready,
	.cmd_done	= hsw_cmd_done,

	/* debug */
	.debug_map  = hsw_debugfs,
	.debug_map_count    = ARRAY_SIZE(hsw_debugfs),
	.dbg_dump   = hsw_dump,

	/* Module loading */
	.load_module    = snd_sof_parse_module_memcpy,

	/*Firmware loading */
	.load_firmware	= snd_sof_load_firmware_memcpy,

};
EXPORT_SYMBOL(sof_hsw_ops);

MODULE_LICENSE("Dual BSD/GPL");
