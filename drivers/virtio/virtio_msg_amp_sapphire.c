// SPDX-License-Identifier: GPL-2.0+
/*
 * Sapphire implementation for Virtio message AMP bus.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>

#include "virtio_msg_amp.h"

struct vmamp_sap_regs {
	u32 int_status;
};

struct vmamp_sap {
	struct virtio_msg_amp vmamp;
	u32 __iomem *cfg_bram;
	struct vmamp_sap_regs __iomem *regs;
	struct hrtimer poll; /* Broken MSI ? */
	dma_addr_t shmem_dma;
	int vectors;
};

static irqreturn_t irq_handler(int irq, void *dev_id)
{
	struct vmamp_sap *vmamp_sap = (struct vmamp_sap *)dev_id;

	virtio_msg_amp_event(&vmamp_sap->vmamp);
	return IRQ_HANDLED;
}

static void tx_notify(struct virtio_msg_amp *vmamp)
{
	struct vmamp_sap *vmamp_sap =
		container_of(vmamp, struct vmamp_sap, vmamp);

	smp_wmb();
	writel(1, &vmamp_sap->regs->int_status);
	readl(&vmamp_sap->regs->int_status);
}

static struct virtio_msg_amp_ops vmamp_sap_ops = {
	.tx_notify = tx_notify,
};

static enum hrtimer_restart poll_timer_expired(struct hrtimer *hrtimer)
{
	struct vmamp_sap *vmamp_sap =
		container_of(hrtimer, struct vmamp_sap, poll);

	virtio_msg_amp_event(&vmamp_sap->vmamp);
	hrtimer_forward_now(hrtimer, ms_to_ktime(50));
	return HRTIMER_RESTART;
}

static int vmamp_sap_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct vmamp_sap *vmamp_sap;
	resource_size_t	size;
	void __iomem * const *bar;
	phys_addr_t addr;
	int ret, i;

	vmamp_sap = devm_kzalloc(&pdev->dev, sizeof(struct vmamp_sap),
				 GFP_KERNEL);
	if (!vmamp_sap) {
		ret = -ENOMEM;
		goto error;
	}

	ret = pcim_enable_device(pdev);
	if (ret)
		goto error;

	ret = pcim_iomap_regions(pdev, BIT(0) | BIT(1), dev_name(&pdev->dev));
	if (ret)
		goto error;

	for (i = 0; i < 2; i++) {
		addr = pci_resource_start(pdev, i);
		size = pci_resource_len(pdev, i);
		dev_dbg(&pdev->dev, "msix (BAR%d) at %pa, size %pr\n", i, &addr,
			&size);
	}

	bar = pcim_iomap_table(pdev);
	if (!bar) {
		ret = -ENOMEM;
		goto error;
	}

	vmamp_sap->cfg_bram = bar[1];
	vmamp_sap->regs = (void __iomem *)((u8 __iomem *)bar[1] + 0x50000);

	/*
	 * Grab all vectors although we can only coalesce them into a single
	 * notifier. This avoids missing any event.
	 */
	vmamp_sap->vectors = pci_msix_vec_count(pdev);
	if (vmamp_sap->vectors < 0)
		vmamp_sap->vectors = 1;

	ret = pci_alloc_irq_vectors(pdev, vmamp_sap->vectors,
				    vmamp_sap->vectors,
				    PCI_IRQ_INTX | PCI_IRQ_MSIX);
	if (ret < 0)
		goto error;

	for (i = 0; i < vmamp_sap->vectors; i++) {
		ret = request_irq(pci_irq_vector(pdev, i), irq_handler,
				  IRQF_SHARED, dev_name(&pdev->dev), vmamp_sap);
		if (ret)
			goto free_irq;
	}

	vmamp_sap->vmamp.dev = &pdev->dev;
	vmamp_sap->vmamp.ops = &vmamp_sap_ops;
	pci_set_drvdata(pdev, vmamp_sap);
	pci_set_master(pdev);

	vmamp_sap->vmamp.shmem_size = 8 * 1024;
	vmamp_sap->vmamp.shmem = dma_alloc_coherent(&pdev->dev,
						    vmamp_sap->vmamp.shmem_size,
						    &vmamp_sap->shmem_dma,
						    GFP_KERNEL);
	if (!vmamp_sap->vmamp.shmem) {
		ret = -ENOMEM;
		goto clear_master;
	}

	memset(vmamp_sap->vmamp.shmem, 0, vmamp_sap->vmamp.shmem_size);

	hrtimer_setup(&vmamp_sap->poll, &poll_timer_expired, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	// hrtimer_start(&vmamp_sap->poll, ms_to_ktime(50), HRTIMER_MODE_REL);

	ret = virtio_msg_amp_register(&vmamp_sap->vmamp);
	if (ret)
		goto free_shmem;

	// hrtimer_cancel(&vmamp_sap->poll);

	writel(vmamp_sap->shmem_dma, &vmamp_sap->cfg_bram[0x4000 / 4 + 1]);
	writel(vmamp_sap->shmem_dma >> 32, &vmamp_sap->cfg_bram[0x4000 / 4 + 2]);
	writel(1, &vmamp_sap->cfg_bram[0x4000 / 4 + 0]);
	return 0;

free_shmem:
	dma_free_coherent(&pdev->dev, vmamp_sap->vmamp.shmem_size,
			  vmamp_sap->vmamp.shmem, vmamp_sap->shmem_dma);
clear_master:
	pci_clear_master(pdev);
free_irq:
	while (--i >= 0)
		free_irq(pci_irq_vector(pdev, i), vmamp_sap);
	pci_free_irq_vectors(pdev);

error:
	dev_info(&pdev->dev, "probe failed: %d\n", ret);
	return ret;
}

static void vmamp_sap_remove(struct pci_dev *pdev)
{
	struct vmamp_sap *vmamp_sap = pci_get_drvdata(pdev);
	int i;

	virtio_msg_amp_unregister(&vmamp_sap->vmamp);
	dma_free_coherent(&pdev->dev, vmamp_sap->vmamp.shmem_size,
			  vmamp_sap->vmamp.shmem, vmamp_sap->shmem_dma);
	writel(0, &vmamp_sap->regs->int_status);
	pci_clear_master(pdev);

	for (i = vmamp_sap->vectors - 1; i >= 0; i--)
		free_irq(pci_irq_vector(pdev, i), vmamp_sap);

	pci_free_irq_vectors(pdev);
}

static void vmamp_sap_shutdown(struct pci_dev *pdev)
{
	struct vmamp_sap *vmamp_sap = pci_get_drvdata(pdev);

	/*
	 * Do the minimal to make device harmless. Need to tell our virtio-msg
	 * peer we're going down.
	 */
	virtio_msg_amp_unregister(&vmamp_sap->vmamp);
}

static const struct pci_device_id vmamp_sap_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, 0x9038) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, vmamp_sap_id_table);

static struct pci_driver vmamp_sap_driver = {
	.name = "virtio_msg_amp_sapphire",
	.id_table = vmamp_sap_id_table,
	.probe = vmamp_sap_probe,
	.remove = vmamp_sap_remove,
	.shutdown = vmamp_sap_shutdown,
};
module_pci_driver(vmamp_sap_driver);

MODULE_AUTHOR("Edgar E. Iglesias <edgar.iglesiass@amd.com>");
MODULE_DESCRIPTION("Virtio-message AMP Sapphire bus");
MODULE_LICENSE("GPL");
