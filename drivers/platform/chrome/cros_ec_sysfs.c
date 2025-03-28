// SPDX-License-Identifier: GPL-2.0+
// Expose the ChromeOS EC through sysfs
//
// Copyright (C) 2014 Google, Inc.

#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#define DRV_NAME "cros-ec-sysfs"

/* Accessor functions */

static ssize_t reboot_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	int count = 0;

	count += sysfs_emit_at(buf, count,
			       "ro|rw|cancel|cold|disable-jump|hibernate|cold-ap-off");
	count += sysfs_emit_at(buf, count, " [at-shutdown]\n");
	return count;
}

static ssize_t reboot_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	static const struct {
		const char * const str;
		uint8_t cmd;
		uint8_t flags;
	} words[] = {
		{"cancel",       EC_REBOOT_CANCEL, 0},
		{"ro",           EC_REBOOT_JUMP_RO, 0},
		{"rw",           EC_REBOOT_JUMP_RW, 0},
		{"cold-ap-off",  EC_REBOOT_COLD_AP_OFF, 0},
		{"cold",         EC_REBOOT_COLD, 0},
		{"disable-jump", EC_REBOOT_DISABLE_JUMP, 0},
		{"hibernate",    EC_REBOOT_HIBERNATE, 0},
		{"at-shutdown",  -1, EC_REBOOT_FLAG_ON_AP_SHUTDOWN},
	};
	struct cros_ec_command *msg;
	struct ec_params_reboot_ec *param;
	int got_cmd = 0, offset = 0;
	int i;
	int ret;
	struct cros_ec_dev *ec = to_cros_ec_dev(dev);

	msg = kmalloc(sizeof(*msg) + sizeof(*param), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	param = (struct ec_params_reboot_ec *)msg->data;

	param->flags = 0;
	while (1) {
		/* Find word to start scanning */
		while (buf[offset] && isspace(buf[offset]))
			offset++;
		if (!buf[offset])
			break;

		for (i = 0; i < ARRAY_SIZE(words); i++) {
			if (!strncasecmp(words[i].str, buf+offset,
					 strlen(words[i].str))) {
				if (words[i].flags) {
					param->flags |= words[i].flags;
				} else {
					param->cmd = words[i].cmd;
					got_cmd = 1;
				}
				break;
			}
		}

		/* On to the next word, if any */
		while (buf[offset] && !isspace(buf[offset]))
			offset++;
	}

	if (!got_cmd) {
		count = -EINVAL;
		goto exit;
	}

	msg->version = 0;
	msg->command = EC_CMD_REBOOT_EC + ec->cmd_offset;
	msg->outsize = sizeof(*param);
	msg->insize = 0;
	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	if (ret < 0)
		count = ret;
exit:
	kfree(msg);
	return count;
}

static ssize_t version_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	static const char * const image_names[] = {"unknown", "RO", "RW"};
	struct ec_response_get_version *r_ver;
	struct ec_response_get_chip_info *r_chip;
	struct ec_response_board_version *r_board;
	struct cros_ec_command *msg;
	int ret;
	int count = 0;
	struct cros_ec_dev *ec = to_cros_ec_dev(dev);

	msg = kmalloc(sizeof(*msg) + EC_HOST_PARAM_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	/* Get versions. RW may change. */
	msg->version = 0;
	msg->command = EC_CMD_GET_VERSION + ec->cmd_offset;
	msg->insize = sizeof(*r_ver);
	msg->outsize = 0;
	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	if (ret < 0) {
		count = ret;
		goto exit;
	}
	r_ver = (struct ec_response_get_version *)msg->data;
	/* Strings should be null-terminated, but let's be sure. */
	r_ver->version_string_ro[sizeof(r_ver->version_string_ro) - 1] = '\0';
	r_ver->version_string_rw[sizeof(r_ver->version_string_rw) - 1] = '\0';
	count += sysfs_emit_at(buf, count, "RO version:    %s\n", r_ver->version_string_ro);
	count += sysfs_emit_at(buf, count, "RW version:    %s\n", r_ver->version_string_rw);
	count += sysfs_emit_at(buf, count, "Firmware copy: %s\n",
			   (r_ver->current_image < ARRAY_SIZE(image_names) ?
			    image_names[r_ver->current_image] : "?"));

	/* Get build info. */
	msg->command = EC_CMD_GET_BUILD_INFO + ec->cmd_offset;
	msg->insize = EC_HOST_PARAM_SIZE;
	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	if (ret < 0) {
		count += sysfs_emit_at(buf, count,
				   "Build info:    XFER / EC ERROR %d / %d\n",
				   ret, msg->result);
	} else {
		msg->data[EC_HOST_PARAM_SIZE - 1] = '\0';
		count += sysfs_emit_at(buf, count, "Build info:    %s\n", msg->data);
	}

	/* Get chip info. */
	msg->command = EC_CMD_GET_CHIP_INFO + ec->cmd_offset;
	msg->insize = sizeof(*r_chip);
	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	if (ret < 0) {
		count += sysfs_emit_at(buf, count,
				   "Chip info:     XFER / EC ERROR %d / %d\n",
				   ret, msg->result);
	} else {
		r_chip = (struct ec_response_get_chip_info *)msg->data;

		r_chip->vendor[sizeof(r_chip->vendor) - 1] = '\0';
		r_chip->name[sizeof(r_chip->name) - 1] = '\0';
		r_chip->revision[sizeof(r_chip->revision) - 1] = '\0';
		count += sysfs_emit_at(buf, count, "Chip vendor:   %s\n", r_chip->vendor);
		count += sysfs_emit_at(buf, count, "Chip name:     %s\n", r_chip->name);
		count += sysfs_emit_at(buf, count, "Chip revision: %s\n", r_chip->revision);
	}

	/* Get board version */
	msg->command = EC_CMD_GET_BOARD_VERSION + ec->cmd_offset;
	msg->insize = sizeof(*r_board);
	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	if (ret < 0) {
		count += sysfs_emit_at(buf, count,
				   "Board version: XFER / EC ERROR %d / %d\n",
				   ret, msg->result);
	} else {
		r_board = (struct ec_response_board_version *)msg->data;

		count += sysfs_emit_at(buf, count,
				   "Board version: %d\n",
				   r_board->board_version);
	}

exit:
	kfree(msg);
	return count;
}

static ssize_t flashinfo_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct ec_response_flash_info *resp;
	struct cros_ec_command *msg;
	int ret;
	struct cros_ec_dev *ec = to_cros_ec_dev(dev);

	msg = kmalloc(sizeof(*msg) + sizeof(*resp), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	/* The flash info shouldn't ever change, but ask each time anyway. */
	msg->version = 0;
	msg->command = EC_CMD_FLASH_INFO + ec->cmd_offset;
	msg->insize = sizeof(*resp);
	msg->outsize = 0;
	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	if (ret < 0)
		goto exit;

	resp = (struct ec_response_flash_info *)msg->data;

	ret = sysfs_emit(buf,
			"FlashSize %d\nWriteSize %d\n"
			"EraseSize %d\nProtectSize %d\n",
			resp->flash_size, resp->write_block_size,
			resp->erase_block_size, resp->protect_block_size);
exit:
	kfree(msg);
	return ret;
}

/* Keyboard wake angle control */
static ssize_t kb_wake_angle_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cros_ec_dev *ec = to_cros_ec_dev(dev);
	struct ec_response_motion_sense *resp;
	struct ec_params_motion_sense *param;
	struct cros_ec_command *msg;
	int ret;

	msg = kmalloc(sizeof(*msg) + EC_HOST_PARAM_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	param = (struct ec_params_motion_sense *)msg->data;
	msg->command = EC_CMD_MOTION_SENSE_CMD + ec->cmd_offset;
	msg->version = 2;
	param->cmd = MOTIONSENSE_CMD_KB_WAKE_ANGLE;
	param->kb_wake_angle.data = EC_MOTION_SENSE_NO_VALUE;
	msg->outsize = sizeof(*param);
	msg->insize = sizeof(*resp);

	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	if (ret < 0)
		goto exit;

	resp = (struct ec_response_motion_sense *)msg->data;
	ret = sysfs_emit(buf, "%d\n", resp->kb_wake_angle.ret);
exit:
	kfree(msg);
	return ret;
}

static ssize_t kb_wake_angle_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cros_ec_dev *ec = to_cros_ec_dev(dev);
	struct ec_params_motion_sense *param;
	struct cros_ec_command *msg;
	u16 angle;
	int ret;

	ret = kstrtou16(buf, 0, &angle);
	if (ret)
		return ret;

	msg = kmalloc(sizeof(*msg) + EC_HOST_PARAM_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	param = (struct ec_params_motion_sense *)msg->data;
	msg->command = EC_CMD_MOTION_SENSE_CMD + ec->cmd_offset;
	msg->version = 2;
	param->cmd = MOTIONSENSE_CMD_KB_WAKE_ANGLE;
	param->kb_wake_angle.data = angle;
	msg->outsize = sizeof(*param);
	msg->insize = sizeof(struct ec_response_motion_sense);

	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	kfree(msg);
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t usbpdmuxinfo_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct cros_ec_dev *ec = to_cros_ec_dev(dev);
	ssize_t count = 0;
	struct ec_response_usb_pd_ports resp_pd_ports;
	int ret;
	int i;

	ret = cros_ec_cmd(ec->ec_dev, 0, EC_CMD_USB_PD_PORTS, NULL, 0,
			  &resp_pd_ports, sizeof(resp_pd_ports));
	if (ret < 0)
		return -EIO;

	for (i = 0; i < resp_pd_ports.num_ports; i++) {
		struct ec_response_usb_pd_mux_info resp_mux;
		struct ec_params_usb_pd_mux_info req = {
			.port = i,
		};

		ret = cros_ec_cmd(ec->ec_dev, 0, EC_CMD_USB_PD_MUX_INFO,
			  &req, sizeof(req), &resp_mux, sizeof(resp_mux));

		if (ret >= 0) {
			count += sysfs_emit_at(buf, count, "Port %d:", i);
			count += sysfs_emit_at(buf, count, " USB=%d",
					!!(resp_mux.flags & USB_PD_MUX_USB_ENABLED));
			count += sysfs_emit_at(buf, count, " DP=%d",
					!!(resp_mux.flags & USB_PD_MUX_DP_ENABLED));
			count += sysfs_emit_at(buf, count, " POLARITY=%s",
					(resp_mux.flags & USB_PD_MUX_POLARITY_INVERTED) ?
					"INVERTED" : "NORMAL");
			count += sysfs_emit_at(buf, count, " HPD_IRQ=%d",
					!!(resp_mux.flags & USB_PD_MUX_HPD_IRQ));
			count += sysfs_emit_at(buf, count, " HPD_LVL=%d",
					!!(resp_mux.flags & USB_PD_MUX_HPD_LVL));
			count += sysfs_emit_at(buf, count, " SAFE=%d",
					!!(resp_mux.flags & USB_PD_MUX_SAFE_MODE));
			count += sysfs_emit_at(buf, count, " TBT=%d",
					!!(resp_mux.flags & USB_PD_MUX_TBT_COMPAT_ENABLED));
			count += sysfs_emit_at(buf, count, " USB4=%d\n",
					!!(resp_mux.flags & USB_PD_MUX_USB4_ENABLED));
		}
	}

	return count ? : -EIO;
}

static ssize_t ap_mode_entry_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct cros_ec_dev *ec = to_cros_ec_dev(dev);
	const bool ap_driven_altmode = cros_ec_check_features(
		ec, EC_FEATURE_TYPEC_REQUIRE_AP_MODE_ENTRY);

	return sysfs_emit(buf, "%s\n", ap_driven_altmode ? "yes" : "no");
}

/* Module initialization */

static DEVICE_ATTR_RW(reboot);
static DEVICE_ATTR_RO(version);
static DEVICE_ATTR_RO(flashinfo);
static DEVICE_ATTR_RW(kb_wake_angle);
static DEVICE_ATTR_RO(usbpdmuxinfo);
static DEVICE_ATTR_RO(ap_mode_entry);

static struct attribute *__ec_attrs[] = {
	&dev_attr_kb_wake_angle.attr,
	&dev_attr_reboot.attr,
	&dev_attr_version.attr,
	&dev_attr_flashinfo.attr,
	&dev_attr_usbpdmuxinfo.attr,
	&dev_attr_ap_mode_entry.attr,
	NULL,
};

static umode_t cros_ec_ctrl_visible(struct kobject *kobj,
				    struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct cros_ec_dev *ec = to_cros_ec_dev(dev);

	if (a == &dev_attr_kb_wake_angle.attr && !ec->has_kb_wake_angle)
		return 0;

	if (a == &dev_attr_usbpdmuxinfo.attr ||
		a == &dev_attr_ap_mode_entry.attr) {
		struct cros_ec_platform *ec_platform = dev_get_platdata(ec->dev);

		if (strcmp(ec_platform->ec_name, CROS_EC_DEV_NAME))
			return 0;
	}

	return a->mode;
}

static const struct attribute_group cros_ec_attr_group = {
	.attrs = __ec_attrs,
	.is_visible = cros_ec_ctrl_visible,
};

static int cros_ec_sysfs_probe(struct platform_device *pd)
{
	struct cros_ec_dev *ec_dev = dev_get_drvdata(pd->dev.parent);
	struct device *dev = &pd->dev;
	int ret;

	ret = sysfs_create_group(&ec_dev->class_dev.kobj, &cros_ec_attr_group);
	if (ret < 0)
		dev_err(dev, "failed to create attributes. err=%d\n", ret);

	return ret;
}

static void cros_ec_sysfs_remove(struct platform_device *pd)
{
	struct cros_ec_dev *ec_dev = dev_get_drvdata(pd->dev.parent);

	sysfs_remove_group(&ec_dev->class_dev.kobj, &cros_ec_attr_group);
}

static const struct platform_device_id cros_ec_sysfs_id[] = {
	{ DRV_NAME, 0 },
	{}
};
MODULE_DEVICE_TABLE(platform, cros_ec_sysfs_id);

static struct platform_driver cros_ec_sysfs_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = cros_ec_sysfs_probe,
	.remove = cros_ec_sysfs_remove,
	.id_table = cros_ec_sysfs_id,
};

module_platform_driver(cros_ec_sysfs_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Expose the ChromeOS EC through sysfs");
