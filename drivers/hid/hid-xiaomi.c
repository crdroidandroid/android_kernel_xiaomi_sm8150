// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for the Xiaomi Mi Pad 5 keyboard.
 *
 *  Copyright (c) 2024 Sebastiano Barezzi <seba@sebaubuntu.dev>
 */

/**
 * This driver is needed to support the Xiaomi Mi Pad 5 keyboard due to their engineering mess.
 * The keyboard controller is in the tablet, and the keyboard connects through pogo pins.
 * The controllers appears as a USB HID device, but without being aware of its presence.
 * The driver, thanks to the original Xiaomi driver needed for power juice, will add and remove the
 * Linux input device when the keyboard is connected and disconnected.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "hid-ids.h"

/**
 * Wait interval for the connection check to let the IRQ handler to settle down.
 */
#define CONNECTION_WAIT_INTERVAL 1000 /* msec */

struct xiaomi_priv {
	struct list_head list;

	struct hid_device *hdev;

	bool enabled;
	struct mutex enabled_lock;

	struct workqueue_struct *connection_wq;
	struct delayed_work connection_work;
};

// List of all instances
static struct list_head instances = LIST_HEAD_INIT(instances);
static DEFINE_MUTEX(instances_lock);

// Keyboard connection status, true by default
static bool connected = true;

// Called by vendor driver
void xiaomi_keyboard_connection_change(bool _connected)
{
	struct xiaomi_priv *instance;

	connected = _connected;

	mutex_lock(&instances_lock);
	list_for_each_entry(instance, &instances, list) {
		// Schedule the work, wait a sec before toggling the keyboard
		mod_delayed_work(instance->connection_wq,
				 &instance->connection_work,
				 msecs_to_jiffies(CONNECTION_WAIT_INTERVAL));
	}
	mutex_unlock(&instances_lock);
}

/**
 * Updates the HID device status in a safe way.
 * Needed to avoid a double-free in the HID core.
 */
static int xiaomi_safe_toggle(struct hid_device *hdev, bool enable)
{
	struct xiaomi_priv *priv = hid_get_drvdata(hdev);
	int ret = 0;

	mutex_lock(&priv->enabled_lock);

	if (priv->enabled == enable) {
		goto done;
	}

	if (enable) {
		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		if (ret) {
			hid_err(hdev, "hid_hw_start failed\n");
			goto done;
		}
	} else {
		hid_hw_stop(hdev);
	}

	priv->enabled = enable;

done:
	mutex_unlock(&priv->enabled_lock);

	return ret;
}

static void xiaomi_connection_work(struct work_struct *work)
{
	struct xiaomi_priv *priv = container_of(
		to_delayed_work(work), struct xiaomi_priv, connection_work);
	struct hid_device *hdev = priv->hdev;

	xiaomi_safe_toggle(hdev, connected);
}

static int xiaomi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct xiaomi_priv *priv;
	int ret;

	if (!hid_is_usb(hdev)) {
		ret = -EINVAL;
		goto err_1;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_1;
	}

	// Setup workqueue
	priv->connection_wq = create_singlethread_workqueue("hid-xiaomi");
	if (!priv->connection_wq) {
		ret = -ENOMEM;
		goto err_2;
	}
	INIT_DELAYED_WORK(&priv->connection_work, xiaomi_connection_work);

	priv->hdev = hdev;
	priv->enabled = false;

	// Setup the mutex
	mutex_init(&priv->enabled_lock);

	hid_set_drvdata(hdev, priv);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid_parse failed\n");
		goto err_3;
	}

	ret = xiaomi_safe_toggle(hdev, true);
	if (ret) {
		hid_err(hdev, "hid_hw_start failed\n");
		goto err_3;
	}

	// Schedule an update to keep the keyboard in sync
	mod_delayed_work(priv->connection_wq, &priv->connection_work,
			 msecs_to_jiffies(CONNECTION_WAIT_INTERVAL));

	// Add the instance to the list
	mutex_lock(&instances_lock);
	list_add(&priv->list, &instances);
	mutex_unlock(&instances_lock);

	return 0;

err_3:
	mutex_destroy(&priv->enabled_lock);
	destroy_workqueue(priv->connection_wq);
err_2:
	kfree(priv);
err_1:
	return ret;
}

static void xiaomi_remove(struct hid_device *hdev)
{
	struct xiaomi_priv *priv = hid_get_drvdata(hdev);

	// Remove the instance from the list
	mutex_lock(&instances_lock);
	list_del(&priv->list);
	mutex_unlock(&instances_lock);

	// Wait for the work to finish
	cancel_delayed_work_sync(&priv->connection_work);

	// Stop the HID device
	xiaomi_safe_toggle(hdev, false);

	// Destroy the mutex
	mutex_destroy(&priv->enabled_lock);

	// Destroy the workqueue
	destroy_workqueue(priv->connection_wq);

	kfree(priv);
}

static const struct hid_device_id xiaomi_devices[] = {
	{
		HID_USB_DEVICE(USB_VENDOR_ID_NANO_IC, 0x3FFC),
	},
	{}
};
MODULE_DEVICE_TABLE(hid, xiaomi_devices);

static struct hid_driver hid_xiaomi = {
	.name = "hid-xiaomi",
	.id_table = xiaomi_devices,
	.probe = xiaomi_probe,
	.remove = xiaomi_remove,
};
module_hid_driver(hid_xiaomi);

MODULE_AUTHOR("Sebastiano Barezzi <seba@sebaubuntu.dev>");
MODULE_DESCRIPTION("Xiaomi Mi Pad 5 keyboard HID driver");
MODULE_LICENSE("GPL");
