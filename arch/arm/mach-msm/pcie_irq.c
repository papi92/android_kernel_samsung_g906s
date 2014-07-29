/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * MSM PCIe controller IRQ driver.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <mach/irqs.h>
#include <linux/irqdomain.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include "pcie.h"

/* Any address will do here, as it won't be dereferenced */
#define MSM_PCIE_MSI_PHY 0xa0000000

#define PCIE20_MSI_CTRL_ADDR            (0x820)
#define PCIE20_MSI_CTRL_UPPER_ADDR      (0x824)
#define PCIE20_MSI_CTRL_INTR_EN         (0x828)
#define PCIE20_MSI_CTRL_INTR_MASK       (0x82C)
#define PCIE20_MSI_CTRL_INTR_STATUS     (0x830)

#define PCIE20_MSI_CTRL_MAX 8

#define LINKDOWN_INIT_WAITING_US_MIN    995
#define LINKDOWN_INIT_WAITING_US_MAX    1005
#define LINKDOWN_WAITING_US_MIN         4900
#define LINKDOWN_WAITING_US_MAX         5100
#define LINKDOWN_WAITING_COUNT          200

static int msm_pcie_recover_link(struct msm_pcie_dev_t *dev)
{
	int ret;

	ret = msm_pcie_enable(dev, PM_PIPE_CLK | PM_CLK | PM_VREG);

	if (!ret) {
		PCIE_DBG(dev, "Recover config space of RC%d and its EP\n",
				dev->rc_idx);
		PCIE_DBG(dev, "Recover RC%d\n", dev->rc_idx);
		msm_pcie_cfg_recover(dev, true);
		PCIE_DBG(dev, "Recover EP of RC%d\n", dev->rc_idx);
		msm_pcie_cfg_recover(dev, false);
		dev->shadow_en = true;

		if ((dev->link_status == MSM_PCIE_LINK_ENABLED) &&
			dev->event_reg && dev->event_reg->callback &&
			(dev->event_reg->events & MSM_PCIE_EVENT_LINKUP)) {
			struct msm_pcie_notify *notify =
					&dev->event_reg->notify;
			notify->event = MSM_PCIE_EVENT_LINKUP;
			notify->user = dev->event_reg->user;
			PCIE_DBG(dev, "Linkup callback for RC%d\n",
				dev->rc_idx);
			dev->event_reg->callback(notify);
		}
	}

	return ret;
}

static void msm_pcie_notify_linkdown(struct msm_pcie_dev_t *dev)
{
	if (dev->event_reg && dev->event_reg->callback &&
		(dev->event_reg->events & MSM_PCIE_EVENT_LINKDOWN)) {
		struct msm_pcie_notify *notify = &dev->event_reg->notify;
		notify->event = MSM_PCIE_EVENT_LINKDOWN;
		notify->user = dev->event_reg->user;
		PCIE_DBG(dev, "PCIe: Linkdown callback for RC%d\n",
			dev->rc_idx);
		dev->event_reg->callback(notify);

		if (dev->event_reg->options & MSM_PCIE_CONFIG_NO_RECOVERY) {
			dev->user_suspend = true;
			PCIE_DBG(dev,
				"PCIe: Client of RC%d will recover the link later.\n",
				dev->rc_idx);
			return;
		}

		if (dev->link_status == MSM_PCIE_LINK_DISABLED) {
			PCIE_DBG(dev,
				"PCIe: Client of RC%d does not enable link in callback; so disable the link\n",
				dev->rc_idx);
			dev->recovery_pending = true;
			msm_pcie_disable(dev,
				PM_EXPT | PM_PIPE_CLK | PM_CLK | PM_VREG);
		} else {
			dev->recovery_pending = false;
			PCIE_DBG(dev,
				"PCIe: Client of RC%d has enabled link in callback; so recover config space\n",
				dev->rc_idx);
			PCIE_DBG(dev, "PCIe: Recover RC%d\n", dev->rc_idx);
			msm_pcie_cfg_recover(dev, true);
			PCIE_DBG(dev, "PCIe: Recover EP of RC%d\n",
				dev->rc_idx);
			msm_pcie_cfg_recover(dev, false);
			dev->shadow_en = true;

			if ((dev->link_status == MSM_PCIE_LINK_ENABLED) &&
				dev->event_reg && dev->event_reg->callback &&
				(dev->event_reg->events &
					MSM_PCIE_EVENT_LINKUP)) {
				struct msm_pcie_notify *notify =
						&dev->event_reg->notify;
				notify->event = MSM_PCIE_EVENT_LINKUP;
				notify->user = dev->event_reg->user;
				PCIE_DBG(dev,
					"PCIe: Linkup callback for RC%d\n",
					dev->rc_idx);
				dev->event_reg->callback(notify);
			}
		}
	} else {
		PCIE_ERR(dev,
			"PCIe: Client driver does not have registration and this linkdown of RC%d should never happen.\n",
			dev->rc_idx);
	}
}

static void handle_wake_func(struct work_struct *work)
{
	int ret;
	struct msm_pcie_dev_t *dev = container_of(work, struct msm_pcie_dev_t,
					handle_wake_work);


	PCIE_DBG(dev, "PCIe: Wake work for RC%d\n", dev->rc_idx);


	if (!dev->enumerated) {
		mutex_lock(&dev->recovery_lock);
		ret = msm_pcie_enumerate(dev->rc_idx);
		mutex_unlock(&dev->recovery_lock);
		if (ret) {
			PCIE_ERR(dev,
				"PCIe: failed to enable RC%d upon wake request from the device.\n",
				dev->rc_idx);
			return;
		}

		if ((dev->link_status == MSM_PCIE_LINK_ENABLED) &&
			dev->event_reg && dev->event_reg->callback &&
			(dev->event_reg->events & MSM_PCIE_EVENT_LINKUP)) {
			struct msm_pcie_notify *notify =
					&dev->event_reg->notify;
			notify->event = MSM_PCIE_EVENT_LINKUP;
			notify->user = dev->event_reg->user;
			PCIE_DBG(dev,
				"PCIe: Linkup callback for RC%d after enumeration is successful in wake IRQ handling\n",
				dev->rc_idx);
			dev->event_reg->callback(notify);
		}
		return;
	} else {
		int waiting_cycle = 0;
		usleep_range(LINKDOWN_INIT_WAITING_US_MIN,
				LINKDOWN_INIT_WAITING_US_MAX);
		while ((dev->handling_linkdown > 0) &&
			(waiting_cycle++ < LINKDOWN_WAITING_COUNT)) {
			usleep_range(LINKDOWN_WAITING_US_MIN,
				LINKDOWN_WAITING_US_MAX);
		}

		if (waiting_cycle == LINKDOWN_WAITING_COUNT)
			PCIE_ERR(dev,
				"PCIe: Linkdown handling for RC%d is not finished after max waiting time.\n",
				dev->rc_idx);

		mutex_lock(&dev->recovery_lock);
		if (dev->link_status == MSM_PCIE_LINK_ENABLED) {
			PCIE_DBG(dev,
				"PCIe: The link status of RC%d is up. Check if it is really up.\n",
					dev->rc_idx);

			if (msm_pcie_confirm_linkup(dev, false, true)) {
				PCIE_DBG(dev,
					"PCIe: The link status of RC%d is really up; so ignore wake IRQ.\n",
					dev->rc_idx);
				goto out;
			} else {
				dev->link_status = MSM_PCIE_LINK_DISABLED;
				dev->shadow_en = false;
				/* assert PERST */
				gpio_set_value(
					dev->gpio[MSM_PCIE_GPIO_PERST].num,
					dev->gpio[MSM_PCIE_GPIO_PERST].on);
				PCIE_ERR(dev,
					"PCIe: The link of RC%d is actually down; notify the client.\n",
					dev->rc_idx);

				msm_pcie_notify_linkdown(dev);
			}
		} else {
			PCIE_DBG(dev,
				"PCIe: The link status of RC%d is down.\n",
				dev->rc_idx);

			if (dev->recovery_pending) {
				static u32 retries = 1;
				PCIE_DBG(dev,
					"PCIe: Start recovering link for RC%d after receive wake IRQ.\n",
					dev->rc_idx);
				ret = msm_pcie_recover_link(dev);
				if (ret) {
					PCIE_ERR(dev,
						"PCIe:failed to enable link for RC%d in No. %d try after receive wake IRQ.\n",
						dev->rc_idx, retries++);
					goto out;
				} else {
					dev->recovery_pending = false;
					PCIE_DBG(dev,
						"PCIe: Successful recovery for RC%d in No. %d try.\n",
						dev->rc_idx, retries);
					retries = 1;
				}
			} else if (dev->user_suspend) {
				PCIE_DBG(dev,
					"PCIe: wake IRQ for RC%d for a user-suspended link.\n",
					dev->rc_idx);
				if (dev->event_reg &&
					dev->event_reg->callback &&
					(dev->event_reg->events &
					MSM_PCIE_EVENT_WAKEUP)) {
					struct msm_pcie_notify *nfy =
						&dev->event_reg->notify;
					nfy->event = MSM_PCIE_EVENT_WAKEUP;
					nfy->user = dev->event_reg->user;
					PCIE_DBG(dev,
						"PCIe: wakeup callback for RC%d\n",
						dev->rc_idx);
					dev->event_reg->callback(nfy);
					if (dev->link_status ==
						MSM_PCIE_LINK_ENABLED)
						PCIE_DBG(dev,
							"PCIe: link is enabled after wakeup callback for RC%d\n",
							dev->rc_idx);
					else
						PCIE_DBG(dev,
							"PCIe: link is NOT enabled after wakeup callback for RC%d\n",
							dev->rc_idx);
				} else {
					PCIE_ERR(dev,
						"PCIe: client of RC%d does not register callback for wake IRQ for a user-suspended link.\n",
						dev->rc_idx);
				}
				goto out;
			} else {
				PCIE_DBG(dev,
					"PCIe: No pending recovery or user-issued suspend for RC%d; so ignore wake IRQ.\n",
					dev->rc_idx);
				goto out;
			}
		}
	}

out:
	mutex_unlock(&dev->recovery_lock);
}

static irqreturn_t handle_wake_irq(int irq, void *data)
{
	struct msm_pcie_dev_t *dev = data;

	dev->wake_counter++;
	PCIE_DBG(dev, "PCIe: No. %ld wake IRQ for RC%d\n",
			dev->wake_counter, dev->rc_idx);

	PCIE_DBG(dev, "PCIe WAKE is asserted by Endpoint of RC%d\n",
		dev->rc_idx);

	if (!dev->enumerated) {
		PCIE_DBG(dev, "Start enumeating RC%d\n", dev->rc_idx);
		schedule_work(&dev->handle_wake_work);
	} else {
		PCIE_DBG(dev, "Wake up RC%d\n", dev->rc_idx);
		__pm_stay_awake(&dev->ws);
		__pm_relax(&dev->ws);

		schedule_work(&dev->handle_wake_work);
	}

	return IRQ_HANDLED;
}

static void handle_linkdown_func(struct work_struct *work)
{
	struct msm_pcie_dev_t *dev = container_of(work, struct msm_pcie_dev_t,
					handle_linkdown_work);

	PCIE_DBG(dev, "PCIe: Linkdown work for RC%d\n", dev->rc_idx);

	mutex_lock(&dev->recovery_lock);

	if (msm_pcie_confirm_linkup(dev, true, true))
		PCIE_DBG(dev,
			"PCIe: The link status of RC%d is up now, indicating recovery has been done.\n",
 			dev->rc_idx);
	else
		msm_pcie_notify_linkdown(dev);

	dev->handling_linkdown--;
	if (dev->handling_linkdown < 0)
		PCIE_ERR(dev, "PCIe:handling_linkdown for RC%d is %d\n",
			dev->rc_idx, dev->handling_linkdown);
	mutex_unlock(&dev->recovery_lock);
}

static irqreturn_t handle_linkdown_irq(int irq, void *data)
{
	struct msm_pcie_dev_t *dev = data;

	dev->linkdown_counter++;
	dev->handling_linkdown++;
	PCIE_DBG(dev,
		"PCIe: No. %ld linkdown IRQ for RC%d: handling_linkdown:%d\n",
		dev->linkdown_counter, dev->rc_idx, dev->handling_linkdown);

	if (!dev->enumerated || dev->link_status != MSM_PCIE_LINK_ENABLED) {
		PCIE_DBG(dev,
			"PCIe:Linkdown IRQ for RC%d when the link is not enabled\n",
			dev->rc_idx);
	} else if (dev->suspending) {
		PCIE_DBG(dev,
			"PCIe:the link of RC%d is suspending.\n",
			dev->rc_idx);
	} else {
		dev->link_status = MSM_PCIE_LINK_DISABLED;
		dev->shadow_en = false;
		/* assert PERST */
		gpio_set_value(dev->gpio[MSM_PCIE_GPIO_PERST].num,
				dev->gpio[MSM_PCIE_GPIO_PERST].on);
		PCIE_ERR(dev, "PCIe link is down for RC%d\n", dev->rc_idx);
		schedule_work(&dev->handle_linkdown_work);
	}

	return IRQ_HANDLED;
}

static irqreturn_t handle_msi_irq(int irq, void *data)
{
	int i, j;
	unsigned long val;
	struct msm_pcie_dev_t *dev = data;
	void __iomem *ctrl_status;

	PCIE_DBG(dev, "irq=%d\n", irq);

	/* check for set bits, clear it by setting that bit
	   and trigger corresponding irq */
	for (i = 0; i < PCIE20_MSI_CTRL_MAX; i++) {
		ctrl_status = dev->dm_core +
				PCIE20_MSI_CTRL_INTR_STATUS + (i * 12);

		val = readl_relaxed(ctrl_status);
		while (val) {
			j = find_first_bit(&val, 32);
			writel_relaxed(BIT(j), ctrl_status);
			/* ensure that interrupt is cleared (acked) */
			wmb();
			generic_handle_irq(
			   irq_find_mapping(dev->irq_domain, (j + (32*i)))
			   );
			val = readl_relaxed(ctrl_status);
		}
	}

	return IRQ_HANDLED;
}

void msm_pcie_config_msi_controller(struct msm_pcie_dev_t *dev)
{
	int i;

	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

	/* program MSI controller and enable all interrupts */
	writel_relaxed(MSM_PCIE_MSI_PHY, dev->dm_core + PCIE20_MSI_CTRL_ADDR);
	writel_relaxed(0, dev->dm_core + PCIE20_MSI_CTRL_UPPER_ADDR);

	for (i = 0; i < PCIE20_MSI_CTRL_MAX; i++)
		writel_relaxed(~0, dev->dm_core +
			       PCIE20_MSI_CTRL_INTR_EN + (i * 12));

	/* ensure that hardware is configured before proceeding */
	wmb();
}

void msm_pcie_destroy_irq(unsigned int irq, struct msm_pcie_dev_t *pcie_dev)
{
	int pos;
	struct msm_pcie_dev_t *dev;

	if (pcie_dev)
		dev = pcie_dev;
	else
		dev = irq_get_chip_data(irq);

	if (dev->msi_gicm_addr) {
		PCIE_DBG(dev, "destroy QGIC based irq %d\n", irq);
		pos = irq - dev->msi_gicm_base;
	} else {
		PCIE_DBG(dev, "destroy default MSI irq %d\n", irq);
		pos = irq - irq_find_mapping(dev->irq_domain, 0);
	}

	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

	if (!dev->msi_gicm_addr)
		dynamic_irq_cleanup(irq);

	PCIE_DBG(dev, "Before clear_bit pos:%d msi_irq_in_use:%ld\n",
		pos, *dev->msi_irq_in_use);
	clear_bit(pos, dev->msi_irq_in_use);
	PCIE_DBG(dev, "After clear_bit pos:%d msi_irq_in_use:%ld\n",
		pos, *dev->msi_irq_in_use);
}

/* hookup to linux pci msi framework */
void arch_teardown_msi_irq(unsigned int irq)
{
	pr_debug("irq %d deallocated\n", irq);
	msm_pcie_destroy_irq(irq, NULL);
}

void arch_teardown_msi_irqs(struct pci_dev *dev)
{
	struct msi_desc *entry;
	struct msm_pcie_dev_t *pcie_dev = PCIE_BUS_PRIV_DATA(dev);

	PCIE_DBG(pcie_dev, "RC:%d EP: vendor_id:0x%x device_id:0x%x\n",
		pcie_dev->rc_idx, dev->vendor, dev->device);

	pcie_dev->use_msi = false;

	list_for_each_entry(entry, &dev->msi_list, list) {
		int i, nvec;
		if (entry->irq == 0)
			continue;
		nvec = 1 << entry->msi_attrib.multiple;
		for (i = 0; i < nvec; i++)
			msm_pcie_destroy_irq(entry->irq + i, pcie_dev);
	}
}

static void msm_pcie_msi_nop(struct irq_data *d)
{
	return;
}

static struct irq_chip pcie_msi_chip = {
	.name = "msm-pcie-msi",
	.irq_ack = msm_pcie_msi_nop,
	.irq_enable = unmask_msi_irq,
	.irq_disable = mask_msi_irq,
	.irq_mask = mask_msi_irq,
	.irq_unmask = unmask_msi_irq,
};

static int msm_pcie_create_irq(struct msm_pcie_dev_t *dev)
{
	int irq, pos;

	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

again:
	pos = find_first_zero_bit(dev->msi_irq_in_use, PCIE_MSI_NR_IRQS);

	if (pos >= PCIE_MSI_NR_IRQS)
		return -ENOSPC;

	PCIE_DBG(dev, "pos:%d msi_irq_in_use:%ld\n", pos, *dev->msi_irq_in_use);

	if (test_and_set_bit(pos, dev->msi_irq_in_use))
		goto again;
	else
		PCIE_DBG(dev, "test_and_set_bit is successful pos=%d\n", pos);

	irq = irq_create_mapping(dev->irq_domain, pos);
	if (!irq)
		return -EINVAL;

	return irq;
}

static int arch_setup_msi_irq_default(struct pci_dev *pdev,
		struct msi_desc *desc, int nvec)
{
	int irq;
	struct msi_msg msg;
	struct msm_pcie_dev_t *dev = PCIE_BUS_PRIV_DATA(pdev);

	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

	irq = msm_pcie_create_irq(dev);

	PCIE_DBG(dev, "IRQ %d is allocated.\n", irq);

	if (irq < 0)
		return irq;

	PCIE_DBG(dev, "irq %d allocated\n", irq);

	irq_set_msi_desc(irq, desc);

	/* write msi vector and data */
	msg.address_hi = 0;
	msg.address_lo = MSM_PCIE_MSI_PHY;
	msg.data = irq - irq_find_mapping(dev->irq_domain, 0);
	write_msi_msg(irq, &msg);

	return 0;
}

static int msm_pcie_create_irq_qgic(struct msm_pcie_dev_t *dev)
{
	int irq, pos;

	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

again:
	pos = find_first_zero_bit(dev->msi_irq_in_use, PCIE_MSI_NR_IRQS);

	if (pos >= PCIE_MSI_NR_IRQS)
		return -ENOSPC;

	PCIE_DBG(dev, "pos:%d msi_irq_in_use:%ld\n", pos, *dev->msi_irq_in_use);

	if (test_and_set_bit(pos, dev->msi_irq_in_use))
		goto again;
	else
		PCIE_DBG(dev, "test_and_set_bit is successful pos=%d\n", pos);

	irq = dev->msi_gicm_base + pos;
	if (!irq) {
		PCIE_ERR(dev, "PCIe: RC%d failed to create QGIC MSI IRQ.\n",
			dev->rc_idx);
		return -EINVAL;
	}

	return irq;
}

static int arch_setup_msi_irq_qgic(struct pci_dev *pdev,
		struct msi_desc *desc, int nvec)
{
	int irq, index, firstirq = 0;
	struct msi_msg msg;
	struct msm_pcie_dev_t *dev = PCIE_BUS_PRIV_DATA(pdev);

	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

	for (index = 0; index < nvec; index++) {
		irq = msm_pcie_create_irq_qgic(dev);
		PCIE_DBG(dev, "irq %d is allocated\n", irq);

		if (irq < 0)
			return irq;

		if (index == 0)
			firstirq = irq;

		irq_set_irq_type(irq, IRQ_TYPE_EDGE_RISING);
	}

	/* write msi vector and data */
	irq_set_msi_desc(firstirq, desc);
	msg.address_hi = 0;
	msg.address_lo = dev->msi_gicm_addr;
	msg.data = firstirq;
	write_msi_msg(firstirq, &msg);

	return 0;
}

int arch_setup_msi_irq(struct pci_dev *pdev, struct msi_desc *desc)
{
	struct msm_pcie_dev_t *dev = PCIE_BUS_PRIV_DATA(pdev);

	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

	if (dev->msi_gicm_addr)
		return arch_setup_msi_irq_qgic(pdev, desc, 1);
	else
		return arch_setup_msi_irq_default(pdev, desc, 1);
}

static int msm_pcie_get_msi_multiple(int nvec)
{
	int msi_multiple = 0;

	while (nvec) {
		nvec = nvec >> 1;
		msi_multiple++;
	}
	pr_debug("log2 number of MSI multiple:%d\n",
		msi_multiple - 1);

	return msi_multiple - 1;
}

int arch_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	struct msi_desc *entry;
	int ret;
	struct msm_pcie_dev_t *pcie_dev = PCIE_BUS_PRIV_DATA(dev);

	PCIE_DBG(pcie_dev, "RC%d\n", pcie_dev->rc_idx);

	if (type != PCI_CAP_ID_MSI || nvec > 32)
		return -ENOSPC;

	PCIE_DBG(pcie_dev, "nvec = %d\n", nvec);

	list_for_each_entry(entry, &dev->msi_list, list) {
		entry->msi_attrib.multiple =
				msm_pcie_get_msi_multiple(nvec);

		if (pcie_dev->msi_gicm_addr)
			ret = arch_setup_msi_irq_qgic(dev, entry, nvec);
		else
			ret = arch_setup_msi_irq_default(dev, entry, nvec);

		PCIE_DBG(pcie_dev, "ret from msi_irq: %d\n", ret);

		if (ret < 0)
			return ret;
		if (ret > 0)
			return -ENOSPC;
	}

	pcie_dev->use_msi = true;

	return 0;
}

static int msm_pcie_msi_map(struct irq_domain *domain, unsigned int irq,
	   irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler (irq, &pcie_msi_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);
	set_irq_flags(irq, IRQF_VALID);
	return 0;
}

static const struct irq_domain_ops msm_pcie_msi_ops = {
	.map = msm_pcie_msi_map,
};

int32_t msm_pcie_irq_init(struct msm_pcie_dev_t *dev)
{
	int rc;
	int msi_start =  0;
	struct device *pdev = &dev->pdev->dev;

	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

	wakeup_source_init(&dev->ws, "pcie_wakeup_source");

	/* register handler for linkdown interrupt */
	rc = devm_request_irq(pdev,
		dev->irq[MSM_PCIE_INT_LINK_DOWN].num, handle_linkdown_irq,
		IRQF_TRIGGER_RISING, dev->irq[MSM_PCIE_INT_LINK_DOWN].name,
		dev);
	if (rc) {
		PCIE_ERR(dev, "PCIe: Unable to request linkdown interrupt:%d\n",
			dev->irq[MSM_PCIE_INT_LINK_DOWN].num);
		return rc;
	}

	INIT_WORK(&dev->handle_linkdown_work, handle_linkdown_func);

	/* register handler for physical MSI interrupt line */
	rc = devm_request_irq(pdev,
		dev->irq[MSM_PCIE_INT_MSI].num, handle_msi_irq,
		IRQF_TRIGGER_RISING, dev->irq[MSM_PCIE_INT_MSI].name, dev);
	if (rc) {
		PCIE_ERR(dev, "PCIe: RC%d: Unable to request MSI interrupt\n",
			dev->rc_idx);
		return rc;
	}

	/* register handler for PCIE_WAKE_N interrupt line */
	rc = devm_request_irq(pdev,
			dev->wake_n, handle_wake_irq, IRQF_TRIGGER_FALLING,
			 "msm_pcie_wake", dev);
	if (rc) {
		PCIE_ERR(dev, "PCIe: RC%d: Unable to request wake interrupt\n",
			dev->rc_idx);
		return rc;
	}

	INIT_WORK(&dev->handle_wake_work, handle_wake_func);

	rc = enable_irq_wake(dev->wake_n);
	if (rc) {
		PCIE_ERR(dev, "PCIe: RC%d: Unable to enable wake interrupt\n",
			dev->rc_idx);
		return rc;
	}

	/* Create a virtual domain of interrupts */
	if (!dev->msi_gicm_addr) {
		dev->irq_domain = irq_domain_add_linear(dev->pdev->dev.of_node,
			PCIE_MSI_NR_IRQS, &msm_pcie_msi_ops, dev);

		if (!dev->irq_domain) {
			PCIE_ERR(dev,
				"PCIe: RC%d: Unable to initialize irq domain\n",
				dev->rc_idx);
			disable_irq(dev->wake_n);
			return PTR_ERR(dev->irq_domain);
		}

		msi_start = irq_create_mapping(dev->irq_domain, 0);
	}

	return 0;
}

void msm_pcie_irq_deinit(struct msm_pcie_dev_t *dev)
{
	PCIE_DBG(dev, "RC%d\n", dev->rc_idx);

	wakeup_source_trash(&dev->ws);
	disable_irq(dev->wake_n);
}
