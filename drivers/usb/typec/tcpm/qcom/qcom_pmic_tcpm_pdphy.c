// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021, Linaro Ltd. All rights reserved.
 */
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/usb/pd.h>
#include <linux/usb/tcpm.h>
#include <dt-bindings/usb/typec/tcpm/qcom,pmic-usb-pdphy.h>
#include "qcom_pmic_tcpm_pdphy.h"

#define PMIC_PDPHY_MAX_IRQS		0x08

struct pmic_pdphy_irq_params {
	int				virq;
	char				*irq_name;
};

struct pmic_pdphy_resources {
	unsigned int			nr_irqs;
	struct pmic_pdphy_irq_params	irq_params[PMIC_PDPHY_MAX_IRQS];
};

struct pmic_pdphy_irq_data {
	int				virq;
	int				irq;
	struct pmic_pdphy		*pmic_pdphy;
};

struct pmic_pdphy {
	struct device			*dev;
	struct tcpm_port		*tcpm_port;
	struct regmap			*regmap;
	u32				base;

	unsigned int			nr_irqs;
	struct pmic_pdphy_irq_data	*irq_data;

	struct work_struct		reset_work;
	struct work_struct		receive_work;
	struct regulator		*vdd_pdphy;
	spinlock_t			lock;		/* Register atomicity */
};

static void qcom_pmic_tcpm_pdphy_reset_on(struct pmic_pdphy *pmic_pdphy)
{
	struct device *dev = pmic_pdphy->dev;
	int ret;

	/* Terminate TX */
	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_TX_CONTROL_REG, 0);
	if (ret)
		goto err;

	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_FRAME_FILTER_REG, 0);
	if (ret)
		goto err;

	return;
err:
	dev_err(dev, "pd_reset_on error\n");
}

static void qcom_pmic_tcpm_pdphy_reset_off(struct pmic_pdphy *pmic_pdphy)
{
	struct device *dev = pmic_pdphy->dev;
	int ret;

	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_FRAME_FILTER_REG,
			   FRAME_FILTER_EN_SOP | FRAME_FILTER_EN_HARD_RESET);
	if (ret)
		dev_err(dev, "pd_reset_off error\n");
}

static void qcom_pmic_tcpm_pdphy_sig_reset_work(struct work_struct *work)
{
	struct pmic_pdphy *pmic_pdphy = container_of(work, struct pmic_pdphy,
						     reset_work);
	unsigned long flags;

	spin_lock_irqsave(&pmic_pdphy->lock, flags);

	qcom_pmic_tcpm_pdphy_reset_on(pmic_pdphy);
	qcom_pmic_tcpm_pdphy_reset_off(pmic_pdphy);

	spin_unlock_irqrestore(&pmic_pdphy->lock, flags);

	tcpm_pd_hard_reset(pmic_pdphy->tcpm_port);
}

static int
qcom_pmic_tcpm_pdphy_clear_tx_control_reg(struct pmic_pdphy *pmic_pdphy)
{
	struct device *dev = pmic_pdphy->dev;
	unsigned int val;
	int ret;

	/* Clear TX control register */
	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_TX_CONTROL_REG, 0);
	if (ret)
		goto done;

	/* Perform readback to ensure sufficient delay for command to latch */
	ret = regmap_read(pmic_pdphy->regmap,
			  pmic_pdphy->base + USB_PDPHY_TX_CONTROL_REG, &val);

done:
	if (ret)
		dev_err(dev, "pd_clear_tx_control_reg: clear tx flag\n");

	return ret;
}

static int
qcom_pmic_tcpm_pdphy_pd_transmit_signal(struct pmic_pdphy *pmic_pdphy,
					enum tcpm_transmit_type type,
					unsigned int negotiated_rev)
{
	struct device *dev = pmic_pdphy->dev;
	unsigned int val;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_pdphy->lock, flags);

	/* Clear TX control register */
	ret = qcom_pmic_tcpm_pdphy_clear_tx_control_reg(pmic_pdphy);
	if (ret)
		goto done;

	val = TX_CONTROL_SEND_SIGNAL;
	if (negotiated_rev == PD_REV30)
		val |= TX_CONTROL_RETRY_COUNT(2);
	else
		val |= TX_CONTROL_RETRY_COUNT(3);

	if (type == TCPC_TX_CABLE_RESET || type == TCPC_TX_HARD_RESET)
		val |= TX_CONTROL_FRAME_TYPE(1);

	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_TX_CONTROL_REG, val);

done:
	spin_unlock_irqrestore(&pmic_pdphy->lock, flags);

	dev_vdbg(dev, "pd_transmit_signal: type %d negotiate_rev %d send %d\n",
		 type, negotiated_rev, ret);

	return ret;
}

static int
qcom_pmic_tcpm_pdphy_pd_transmit_payload(struct pmic_pdphy *pmic_pdphy,
					 enum tcpm_transmit_type type,
					 const struct pd_message *msg,
					 unsigned int negotiated_rev)
{
	struct device *dev = pmic_pdphy->dev;
	unsigned int val, hdr_len, txbuf_len, txsize_len;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_pdphy->lock, flags);

	ret = regmap_read(pmic_pdphy->regmap,
			  pmic_pdphy->base + USB_PDPHY_RX_ACKNOWLEDGE_REG,
			  &val);
	if (ret)
		goto done;

	if (val) {
		dev_err(dev, "pd_transmit_payload: RX message pending\n");
		ret = -EBUSY;
		goto done;
	}

	/* Clear TX control register */
	ret = qcom_pmic_tcpm_pdphy_clear_tx_control_reg(pmic_pdphy);
	if (ret)
		goto done;

	hdr_len = sizeof(msg->header);
	txbuf_len = pd_header_cnt_le(msg->header) * 4;
	txsize_len = hdr_len + txbuf_len - 1;

	/* Write message header sizeof(u16) to USB_PDPHY_TX_BUFFER_HDR_REG */
	ret = regmap_bulk_write(pmic_pdphy->regmap,
				pmic_pdphy->base + USB_PDPHY_TX_BUFFER_HDR_REG,
				&msg->header, hdr_len);
	if (ret)
		goto done;

	/* Write payload to USB_PDPHY_TX_BUFFER_DATA_REG for txbuf_len */
	if (txbuf_len) {
		ret = regmap_bulk_write(pmic_pdphy->regmap,
					pmic_pdphy->base + USB_PDPHY_TX_BUFFER_DATA_REG,
					&msg->payload, txbuf_len);
		if (ret)
			goto done;
	}

	/* Write total length ((header + data) - 1) to USB_PDPHY_TX_SIZE_REG */
	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_TX_SIZE_REG,
			   txsize_len);
	if (ret)
		goto done;

	/* Clear TX control register */
	ret = qcom_pmic_tcpm_pdphy_clear_tx_control_reg(pmic_pdphy);
	if (ret)
		goto done;

	/* Initiate transmit with retry count as indicated by PD revision */
	val = TX_CONTROL_FRAME_TYPE(type) | TX_CONTROL_SEND_MSG;
	if (pd_header_rev(msg->header) == PD_REV30)
		val |= TX_CONTROL_RETRY_COUNT(2);
	else
		val |= TX_CONTROL_RETRY_COUNT(3);

	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_TX_CONTROL_REG, val);

done:
	spin_unlock_irqrestore(&pmic_pdphy->lock, flags);

	if (ret) {
		dev_err(dev, "pd_transmit_payload: %d hdr %*ph data %*ph ret %d\n",
			ret, hdr_len, &msg->header, txbuf_len, &msg->payload, ret);
	}

	return ret;
}

int qcom_pmic_tcpm_pdphy_pd_transmit(struct pmic_pdphy *pmic_pdphy,
				     enum tcpm_transmit_type type,
				     const struct pd_message *msg,
				     unsigned int negotiated_rev)
{
	struct device *dev = pmic_pdphy->dev;
	int ret;

	if (msg) {
		ret = qcom_pmic_tcpm_pdphy_pd_transmit_payload(pmic_pdphy, type,
							       msg,
							       negotiated_rev);
	} else {
		ret = qcom_pmic_tcpm_pdphy_pd_transmit_signal(pmic_pdphy, type,
							      negotiated_rev);
	}

	if (ret)
		dev_dbg(dev, "pd_transmit: type %x result %d\n", type, ret);

	return ret;
}

static void qcom_pmic_tcpm_pdphy_pd_receive(struct pmic_pdphy *pmic_pdphy)
{
	struct device *dev = pmic_pdphy->dev;
	struct pd_message msg;
	unsigned int size, rx_status;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_pdphy->lock, flags);

	ret = regmap_read(pmic_pdphy->regmap,
			  pmic_pdphy->base + USB_PDPHY_RX_SIZE_REG, &size);
	if (ret)
		goto done;

	/* If we received a subsequent RX sig this value can be zero */
	if ((size < 1 || size > sizeof(msg.payload))) {
		dev_dbg(dev, "pd_receive: invalid size %d\n", size);
		goto done;
	}

	size += 1;
	ret = regmap_read(pmic_pdphy->regmap,
			  pmic_pdphy->base + USB_PDPHY_RX_STATUS_REG,
			  &rx_status);

	if (ret)
		goto done;

	ret = regmap_bulk_read(pmic_pdphy->regmap,
			       pmic_pdphy->base + USB_PDPHY_RX_BUFFER_REG,
			       (u8 *)&msg, size);
	if (ret)
		goto done;

	/* Return ownership of RX buffer to hardware */
	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_RX_ACKNOWLEDGE_REG, 0);

done:
	spin_unlock_irqrestore(&pmic_pdphy->lock, flags);

	if (!ret) {
		dev_vdbg(dev, "pd_receive: handing %d bytes to tcpm\n", size);
		tcpm_pd_receive(pmic_pdphy->tcpm_port, &msg);
	}
}

static irqreturn_t qcom_pmic_tcpm_pdphy_isr(int irq, void *dev_id)
{
	struct pmic_pdphy_irq_data *irq_data = dev_id;
	struct pmic_pdphy *pmic_pdphy = irq_data->pmic_pdphy;
	struct device *dev = pmic_pdphy->dev;

	switch (irq_data->virq) {
	case PMIC_PDPHY_SIG_TX_IRQ:
		dev_err(dev, "isr: tx_sig\n");
		break;
	case PMIC_PDPHY_SIG_RX_IRQ:
		schedule_work(&pmic_pdphy->reset_work);
		break;
	case PMIC_PDPHY_MSG_TX_IRQ:
		tcpm_pd_transmit_complete(pmic_pdphy->tcpm_port,
					  TCPC_TX_SUCCESS);
		break;
	case PMIC_PDPHY_MSG_RX_IRQ:
		qcom_pmic_tcpm_pdphy_pd_receive(pmic_pdphy);
		break;
	case PMIC_PDPHY_MSG_TX_FAIL_IRQ:
		tcpm_pd_transmit_complete(pmic_pdphy->tcpm_port,
					  TCPC_TX_FAILED);
		break;
	case PMIC_PDPHY_MSG_TX_DISCARD_IRQ:
		tcpm_pd_transmit_complete(pmic_pdphy->tcpm_port,
					  TCPC_TX_DISCARDED);
		break;
	}

	return IRQ_HANDLED;
}

int qcom_pmic_tcpm_pdphy_set_pd_rx(struct pmic_pdphy *pmic_pdphy, bool on)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_pdphy->lock, flags);

	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_RX_ACKNOWLEDGE_REG, !on);

	spin_unlock_irqrestore(&pmic_pdphy->lock, flags);

	dev_dbg(pmic_pdphy->dev, "set_pd_rx: %s\n", on ? "on" : "off");

	return ret;
}

int qcom_pmic_tcpm_pdphy_set_roles(struct pmic_pdphy *pmic_pdphy,
				   bool data_role_host, bool power_role_src)
{
	struct device *dev = pmic_pdphy->dev;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_pdphy->lock, flags);

	ret = regmap_update_bits(pmic_pdphy->regmap,
				 pmic_pdphy->base + USB_PDPHY_MSG_CONFIG_REG,
				 MSG_CONFIG_PORT_DATA_ROLE |
				 MSG_CONFIG_PORT_POWER_ROLE,
				 data_role_host << 3 | power_role_src << 2);

	spin_unlock_irqrestore(&pmic_pdphy->lock, flags);

	dev_dbg(dev, "pdphy_set_roles: data_role_host=%d power_role_src=%d\n",
		data_role_host, power_role_src);

	return ret;
}

static int qcom_pmic_tcpm_pdphy_enable(struct pmic_pdphy *pmic_pdphy)
{
	struct device *dev = pmic_pdphy->dev;
	int ret;

	ret = regulator_enable(pmic_pdphy->vdd_pdphy);
	if (ret)
		return ret;

	/* PD 2.0, DR=TYPEC_DEVICE, PR=TYPEC_SINK */
	ret = regmap_update_bits(pmic_pdphy->regmap,
				 pmic_pdphy->base + USB_PDPHY_MSG_CONFIG_REG,
				 MSG_CONFIG_SPEC_REV_MASK, PD_REV20);
	if (ret)
		goto done;

	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_EN_CONTROL_REG, 0);
	if (ret)
		goto done;

	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_EN_CONTROL_REG,
			   CONTROL_ENABLE);
	if (ret)
		goto done;

	qcom_pmic_tcpm_pdphy_reset_off(pmic_pdphy);
done:
	if (ret) {
		regulator_disable(pmic_pdphy->vdd_pdphy);
		dev_err(dev, "pdphy_enable fail %d\n", ret);
	}

	return ret;
}

static int qcom_pmic_tcpm_pdphy_disable(struct pmic_pdphy *pmic_pdphy)
{
	int ret;

	qcom_pmic_tcpm_pdphy_reset_on(pmic_pdphy);

	ret = regmap_write(pmic_pdphy->regmap,
			   pmic_pdphy->base + USB_PDPHY_EN_CONTROL_REG, 0);

	regulator_disable(pmic_pdphy->vdd_pdphy);

	return ret;
}

static int pmic_pdphy_reset(struct pmic_pdphy *pmic_pdphy)
{
	int ret;

	ret = qcom_pmic_tcpm_pdphy_disable(pmic_pdphy);
	if (ret)
		goto done;

	usleep_range(400, 500);
	ret = qcom_pmic_tcpm_pdphy_enable(pmic_pdphy);
done:
	return ret;
}

int qcom_pmic_tcpm_pdphy_init(struct pmic_pdphy *pmic_pdphy,
			      struct tcpm_port *tcpm_port)
{
	int i;
	int ret;

	pmic_pdphy->tcpm_port = tcpm_port;

	ret = pmic_pdphy_reset(pmic_pdphy);
	if (ret)
		return ret;

	for (i = 0; i < pmic_pdphy->nr_irqs; i++)
		enable_irq(pmic_pdphy->irq_data[i].irq);

	return 0;
}

void qcom_pmic_tcpm_pdphy_put(struct pmic_pdphy *pmic_pdphy)
{
	put_device(pmic_pdphy->dev);
}

static int qcom_pmic_tcpm_pdphy_probe(struct platform_device *pdev)
{
	struct pmic_pdphy *pmic_pdphy;
	struct device *dev = &pdev->dev;
	const struct pmic_pdphy_resources *res;
	struct pmic_pdphy_irq_data *irq_data;
	int i, ret, irq;
	u32 reg;

	ret = device_property_read_u32(dev, "reg", &reg);
	if (ret < 0) {
		dev_err(dev, "missing base address\n");
		return ret;
	}

	res = of_device_get_match_data(dev);
	if (!res)
		return -ENODEV;

	if (!res->nr_irqs || res->nr_irqs > PMIC_PDPHY_MAX_IRQS)
		return -EINVAL;

	pmic_pdphy = devm_kzalloc(dev, sizeof(*pmic_pdphy), GFP_KERNEL);
	if (!pmic_pdphy)
		return -ENOMEM;

	irq_data = devm_kzalloc(dev, sizeof(*irq_data) * res->nr_irqs,
				GFP_KERNEL);
	if (!irq_data)
		return -ENOMEM;

	pmic_pdphy->vdd_pdphy = devm_regulator_get(dev, "vdd-pdphy");
	if (IS_ERR(pmic_pdphy->vdd_pdphy))
		return PTR_ERR(pmic_pdphy->vdd_pdphy);

	pmic_pdphy->dev = dev;
	pmic_pdphy->base = reg;
	pmic_pdphy->nr_irqs = res->nr_irqs;
	pmic_pdphy->irq_data = irq_data;
	spin_lock_init(&pmic_pdphy->lock);
	INIT_WORK(&pmic_pdphy->reset_work, qcom_pmic_tcpm_pdphy_sig_reset_work);

	pmic_pdphy->regmap = dev_get_regmap(dev->parent, NULL);
	if (!pmic_pdphy->regmap) {
		dev_err(dev, "Failed to get regmap\n");
		return -ENODEV;
	}

	platform_set_drvdata(pdev, pmic_pdphy);
	for (i = 0; i < res->nr_irqs; i++, irq_data++) {
		irq = platform_get_irq_byname(pdev, res->irq_params[i].irq_name);
		if (irq < 0)
			return irq;

		irq_data->pmic_pdphy = pmic_pdphy;
		irq_data->irq = irq;
		irq_data->virq = res->irq_params[i].virq;

		ret = devm_request_threaded_irq(dev, irq, NULL,
						qcom_pmic_tcpm_pdphy_isr,
						IRQF_ONESHOT | IRQF_NO_AUTOEN,
						res->irq_params[i].irq_name,
						irq_data);
		if (ret)
			return ret;
	}
	return 0;
}

static int qcom_pmic_tcpm_pdphy_remove(struct platform_device *pdev)
{
	struct pmic_pdphy *pmic_pdphy = platform_get_drvdata(pdev);

	qcom_pmic_tcpm_pdphy_reset_on(pmic_pdphy);

	return 0;
}

static struct pmic_pdphy_resources pm8150b_pdphy_res = {
	.irq_params = {
		{
			.virq = PMIC_PDPHY_SIG_TX_IRQ,
			.irq_name = "sig-tx",
		},
		{
			.virq = PMIC_PDPHY_SIG_RX_IRQ,
			.irq_name = "sig-rx",
		},
		{
			.virq = PMIC_PDPHY_MSG_TX_IRQ,
			.irq_name = "msg-tx",
		},
		{
			.virq = PMIC_PDPHY_MSG_RX_IRQ,
			.irq_name = "msg-rx",
		},
		{
			.virq = PMIC_PDPHY_MSG_TX_FAIL_IRQ,
			.irq_name = "msg-tx-failed",
		},
		{
			.virq = PMIC_PDPHY_MSG_TX_DISCARD_IRQ,
			.irq_name = "msg-tx-discarded",
		},
		{
			.virq = PMIC_PDPHY_MSG_RX_DISCARD_IRQ,
			.irq_name = "msg-rx-discarded",
		},
	},
	.nr_irqs = 7,
};

static const struct of_device_id qcom_pmic_tcpm_pdphy_table[] = {
	{ .compatible = "qcom,pm8150b-pdphy", .data = &pm8150b_pdphy_res },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_pmic_tcpm_pdphy_table);

struct platform_driver qcom_pmic_tcpm_pdphy_platform_driver = {
	.driver = {
		.name = "qcom,pmic-usb-pdphy",
		.of_match_table = qcom_pmic_tcpm_pdphy_table,
	},
	.probe = qcom_pmic_tcpm_pdphy_probe,
	.remove = qcom_pmic_tcpm_pdphy_remove,
};
