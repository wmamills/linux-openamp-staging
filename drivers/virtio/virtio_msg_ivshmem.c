// SPDX-License-Identifier: GPL-2.0
/*
 * Virtio-msg-amp driver for Inter-VM shared memory PCI device
 *
 * Copyright (c) Linaro Ltd, 2024
 *
 * Based partially on a uio driver for ivshmem PCI driver by:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 */

#include <linux/ivshmem.h>
#include <linux/module.h>
#include <linux/pci.h>

#define DRV_NAME "virtio_msg_ivshmem"

struct my_info_t {
	int num;
};

struct ivshm_dev {
	struct my_info_t info;
	struct pci_dev *pdev;
	struct ivshm_regs __iomem *regs;
	void *shmem;
	size_t shmem_size;
	int vectors;
};

static irqreturn_t ivshm_irq_handler(int irq, void *dev_id)
{
	struct ivshm_dev *ivshm_dev = (struct ivshm_dev *)dev_id;

	dev_info(&ivshm_dev->pdev->dev, "ivshmem IRQ fired");

	return IRQ_HANDLED;
}

static int ivshm_release(struct my_info_t *info, struct inode *inode)
{
	struct ivshm_dev *ivshm_dev =
		container_of(info, struct ivshm_dev, info);

	writel(0, &ivshm_dev->regs->int_status);
	return 0;
}

#include <linux/delay.h>

// do the "uio" test with Zephyr peer
static int uio_test(struct ivshm_dev *ivshm_dev, u32 peer_vmid)
{
	int i;
	const struct device *pdev = &ivshm_dev->pdev->dev;
	volatile u32 *mmr = (u32 *) ivshm_dev->regs;
	volatile u32 *shm = (u32 *) ivshm_dev->shmem;
	u32 val;

	for (i = 0; i < 4; ++i) {
		val = mmr[i];
		dev_info(pdev, "mmr%d: %d %0x\n", i, val, val);
	}

	/*
	* Save our peer ID taken from IVPOSITION register to shm[0] so
	* the Zephyr peer knows which peer it should notify back.
	*/
	shm[0] = mmr[2];

	/* invalidate the memory pattern */
	for (i = 1 ; i < 32; i++) {
		shm[i] = 0;
	}

	dev_info(pdev, "SHMEM Before: %32ph \n", ivshm_dev->shmem);

	/* Notify peer by writting its peer ID to the DOORBELL register */
	mmr[3] = peer_vmid << 16;

	/*
	 * Wait notification. read() will block until Zephyr finishes writting
	 * the whole shmem region with value 0xb5b5b5b5.
	 */
	// n = read(fd, (uint8_t *)&buf, 4);
	msleep(1000);

	dev_info(pdev, "SHMEM After: %32ph \n", ivshm_dev->shmem);

	/* Check shmem region: 4 MiB */
	for (i = 1 /* skip peer id */; i < ((4 * 1024 * 1024) / 4) ; i++) {
		val = shm[i];
		if ( val != 0xb5b5b5b5 ) {
			dev_info(pdev, "Data mismatch at %d: %x\n",
				i, val);
			return 1;
		}
	}

	dev_info(pdev, "Data ok. %d byte(s) checked.\n", i * 4);
	return 0;
}

static int ivshm_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ivshm_dev *ivshm_dev;
	int err, i;
	char *device_name;
	char *name;
	phys_addr_t addr;
	resource_size_t	size;
	u32 vmid;

	ivshm_dev = devm_kzalloc(&pdev->dev, sizeof(struct ivshm_dev),
				 GFP_KERNEL);
	if (!ivshm_dev) {
		err = -ENOMEM;
		goto error;
	}

	err = pcim_enable_device(pdev);
	if (err) {
		goto error;
	}

	device_name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s[%s]", DRV_NAME,
				     dev_name(&pdev->dev));
	if (!device_name) {
		err = -ENOMEM;
		goto error;
	}

	//ivshm_dev->info.name = device_name;
	//ivshm_dev->info.version = "1";
	//ivshm_dev->info.release = ivshm_release;

	err = pcim_iomap_regions(pdev, BIT(2) | BIT(0), device_name);
	if (err) {
		goto error;
	}

	name = "ivshmem-mmr";
	addr = pci_resource_start(pdev, 0);
	size = pci_resource_len(pdev, 0);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	name = "ivshmem-msix";
	addr = pci_resource_start(pdev, 1);
	size = pci_resource_len(pdev, 1);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	name = "ivshmem-shmem";
	addr = pci_resource_start(pdev, 2);
	size = pci_resource_len(pdev, 2);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);
	ivshm_dev->shmem_size = size;

	ivshm_dev->regs  = pcim_iomap_table(pdev)[0];
	ivshm_dev->shmem = pcim_iomap_table(pdev)[2];

	vmid = readl(&ivshm_dev->regs->ivposition);
	dev_info(&pdev->dev, "VMID=%x\n", vmid);

	dev_info(&pdev->dev, "SHMEM @ 0: %32ph \n", ivshm_dev->shmem);

	/*
	 * Grab all vectors although we can only coalesce them into a single
	 * notifier. This avoids missing any event.
	 */
	ivshm_dev->vectors = pci_msix_vec_count(pdev);
	if (ivshm_dev->vectors < 0)
		ivshm_dev->vectors = 1;

	err = pci_alloc_irq_vectors(pdev, ivshm_dev->vectors,
				    ivshm_dev->vectors,
				    PCI_IRQ_INTX | PCI_IRQ_MSIX);
	if (err < 0)
		goto error;

	for (i = 0; i < ivshm_dev->vectors; i++) {
		err = request_irq(pci_irq_vector(pdev, i), ivshm_irq_handler,
				  IRQF_SHARED, device_name, ivshm_dev);
		if (err)
			goto error_irq;
	}

	//ivshm_dev->info.irq = UIO_IRQ_CUSTOM;

	//err = uio_register_device(&pdev->dev, &ivshm_dev->info);
	if (err)
		goto error_irq;

	pci_set_master(pdev);

	pci_set_drvdata(pdev, ivshm_dev);
	ivshm_dev->pdev = pdev;

	// run the test
	uio_test(ivshm_dev, 0);

	dev_info(&pdev->dev, "probe successful\n");

	return 0;

error_irq:
	while (--i > 0)
		free_irq(pci_irq_vector(pdev, i), ivshm_dev);
	pci_free_irq_vectors(pdev);

error:
	dev_info(&pdev->dev, "probe failed!\n");

	return err;
}

static void ivshm_remove(struct pci_dev *pdev)
{
	struct ivshm_dev *ivshm_dev = pci_get_drvdata(pdev);
	int i;

	writel(0, &ivshm_dev->regs->int_mask);
	pci_clear_master(pdev);

	//uio_unregister_device(&ivshm_dev->info);

	for (i = 0; i < ivshm_dev->vectors; i++)
		free_irq(pci_irq_vector(pdev, i), ivshm_dev);

	pci_free_irq_vectors(pdev);
	dev_info(&pdev->dev, "device removed\n");
}

static const struct pci_device_id ivshm_device_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_IVSHMEM, PCI_DEVICE_ID_IVSHMEM) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, ivshm_device_id_table);

static struct pci_driver virtio_msg_ivshm_driver = {
	.name = DRV_NAME,
	.id_table = ivshm_device_id_table,
	.probe = ivshm_probe,
	.remove = ivshm_remove,
};
module_pci_driver(virtio_msg_ivshm_driver);

MODULE_AUTHOR("Bill Mills <bill.mills@linaro.org>");
MODULE_LICENSE("GPL v2");
