// SPDX-License-Identifier: GPL-2.0+
/*
 * Generic PCI implementation for Virtio message AMP bus.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>

#include "virtio_msg_amp.h"

struct vmamp_pci_regs {
	u32 version;
	u32 features;
	u64 reserved[3];
	u32 notify[4];
};

struct vmamp_pci {
	struct virtio_msg_amp vmamp;
	void __iomem *ram;
	struct vmamp_pci_regs __iomem *regs;
	int vectors;
};

static irqreturn_t irq_handler(int irq, void *dev_id)
{
	struct vmamp_pci *vmamp_pci = (struct vmamp_pci *)dev_id;

	virtio_msg_amp_event(&vmamp_pci->vmamp);
	return IRQ_HANDLED;
}

static void tx_notify(struct virtio_msg_amp *vmamp)
{
	struct vmamp_pci *vmamp_pci =
		container_of(vmamp, struct vmamp_pci, vmamp);

	smp_wmb();
	writel(1, &vmamp_pci->regs->notify[0]);
}

static struct virtio_msg_amp_ops vmamp_pci_ops = {
	.tx_notify = tx_notify,
};

static int vmamp_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct vmamp_pci *vmamp_pci;
	resource_size_t	size;
	phys_addr_t addr;
	int ret, i;

	vmamp_pci = devm_kzalloc(&pdev->dev, sizeof(*vmamp_pci), GFP_KERNEL);
	if (!vmamp_pci) {
		ret = -ENOMEM;
		goto error;
	}

	ret = pcim_enable_device(pdev);
	if (ret)
		goto error;

	/* Request 3 BARs */
	for (i = 0; i < 3; i++) {
		ret = pcim_request_region(pdev, i, dev_name(&pdev->dev));
		if (ret)
			goto error;

		addr = pci_resource_start(pdev, i);
		size = pci_resource_len(pdev, i);
		dev_dbg(&pdev->dev, "mmr (BAR%d) at %pa, size %pr\n", i, &addr,
			&size);
	}

	vmamp_pci->regs = pcim_iomap(pdev, 0, 0);
	if (!vmamp_pci->regs) {
		ret = -ENOMEM;
		goto error;
	}

	vmamp_pci->ram = pci_iomap_wc(pdev, 1, 0);
	if (!vmamp_pci->ram) {
		ret = -ENOMEM;
		goto error;
	}

	/*
	 * Grab all vectors although we can only coalesce them into a single
	 * notifier. This avoids missing any event.
	 */
	vmamp_pci->vectors = pci_msix_vec_count(pdev);
	if (vmamp_pci->vectors < 0)
		vmamp_pci->vectors = 1;

	ret = pci_alloc_irq_vectors(pdev, vmamp_pci->vectors,
				    vmamp_pci->vectors, PCI_IRQ_MSIX);
	if (ret < 0)
		goto unmap_ram;

	for (i = 0; i < vmamp_pci->vectors; i++) {
		ret = request_irq(pci_irq_vector(pdev, i), irq_handler,
				  IRQF_SHARED, dev_name(&pdev->dev), vmamp_pci);
		if (ret)
			goto free_irq;
	}

	vmamp_pci->vmamp.dev = &pdev->dev;
	vmamp_pci->vmamp.ops = &vmamp_pci_ops;
	pci_set_drvdata(pdev, vmamp_pci);
	pci_set_master(pdev);

	/* First 4KB are reserved */
	vmamp_pci->vmamp.shmem = vmamp_pci->ram + 4 * 1024;
	vmamp_pci->vmamp.shmem_size = 8 * 1024;
	memset(vmamp_pci->vmamp.shmem, 0, vmamp_pci->vmamp.shmem_size);

	ret = virtio_msg_amp_register(&vmamp_pci->vmamp);
	if (ret)
		goto clear_master;

	return 0;

clear_master:
	pci_clear_master(pdev);
free_irq:
	while (--i >= 0)
		free_irq(pci_irq_vector(pdev, i), vmamp_pci);
	pci_free_irq_vectors(pdev);
unmap_ram:
	pci_iounmap(pdev, vmamp_pci->ram);
error:
	dev_err(&pdev->dev, "probe failed: %d\n", ret);
	return ret;
}

static void vmamp_pci_remove(struct pci_dev *pdev)
{
	struct vmamp_pci *vmamp_pci = pci_get_drvdata(pdev);
	int i;

	virtio_msg_amp_unregister(&vmamp_pci->vmamp);
	pci_clear_master(pdev);

	for (i = vmamp_pci->vectors - 1; i >= 0; i--)
		free_irq(pci_irq_vector(pdev, i), vmamp_pci);

	pci_free_irq_vectors(pdev);
	pci_iounmap(pdev, vmamp_pci->ram);
}

static void vmamp_pci_shutdown(struct pci_dev *pdev)
{
	struct vmamp_pci *vmamp_pci = pci_get_drvdata(pdev);

	/*
	 * Do the minimal to make device harmless. Need to tell our virtio-msg
	 * peer we're going down.
	 */
	virtio_msg_amp_unregister(&vmamp_pci->vmamp);
}

static const struct pci_device_id vmamp_pci_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, 0x9039) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, vmamp_pci_id_table);

static struct pci_driver vmamp_pci_driver = {
	.name = "virtio_msg_amp_pci",
	.id_table = vmamp_pci_id_table,
	.probe = vmamp_pci_probe,
	.remove = vmamp_pci_remove,
	.shutdown = vmamp_pci_shutdown,
};
module_pci_driver(vmamp_pci_driver);

MODULE_AUTHOR("Edgar E. Iglesias <edgar.iglesiass@amd.com>");
MODULE_DESCRIPTION("Virtio-message generic AMP PCI bus");
MODULE_LICENSE("GPL");
