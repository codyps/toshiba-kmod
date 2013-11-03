/*
 *  toshiba_acpi.c - Toshiba Laptop ACPI Extras
 *
 *
 *  Copyright (C) 2002-2004 John Belmonte
 *  Copyright (C) 2008 Philip Langdale
 *  Copyright (C) 2010 Pierre Ducroquet
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  The devolpment page for this driver is located at
 *  http://memebeam.org/toys/ToshibaAcpiDriver.
 *
 *  Credits:
 *	Jonathan A. Buzzard - Toshiba HCI info, and critical tips on reverse
 *		engineering the Windows drivers
 *	Yasushi Nagato - changes for linux kernel 2.4 -> 2.5
 *	Rob Miller - TV out and hotkeys help
 *
 *
 *  TODO
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/backlight.h>
#include <linux/rfkill.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/i8042.h>

#include <asm/uaccess.h>

#include <acpi/acpi_drivers.h>

MODULE_AUTHOR("John Belmonte");
MODULE_DESCRIPTION("Toshiba Laptop ACPI Extras Driver");
MODULE_LICENSE("GPL");

#define TOSHIBA_WMI_EVENT_GUID "59142400-C6A3-40FA-BADB-8A2652834100"

/* Scan code for Fn key on TOS1900 models */
#define TOS1900_FN_SCAN		0x6e

/* Toshiba ACPI method paths */
#define METHOD_VIDEO_OUT	"\\_SB_.VALX.DSSX"

/* Toshiba HCI interface definitions
 *
 * HCI is Toshiba's "Hardware Control Interface" which is supposed to
 * be uniform across all their models.  Ideally we would just call
 * dedicated ACPI methods instead of using this primitive interface.
 * However the ACPI methods seem to be incomplete in some areas (for
 * example they allow setting, but not reading, the LCD brightness value),
 * so this is still useful.
 */

#define HCI_WORDS			6

/* operations */
#define HCI_SET				0xff00
#define HCI_GET				0xfe00
#define HCI_TPAD_GET			0xf300
#define HCI_TPAD_SET			0xf400

/* return codes */
#define HCI_SUCCESS			0x0000
#define HCI_FAILURE			0x1000
#define HCI_NOT_SUPPORTED		0x8000
#define HCI_EMPTY			0x8c00

/* registers */
#define HCI_FAN				0x0004
#define HCI_TR_BACKLIGHT		0x0005
#define HCI_SYSTEM_EVENT		0x0016
#define HCI_VIDEO_OUT			0x001c
#define HCI_HOTKEY_EVENT		0x001e
#define HCI_LCD_BRIGHTNESS		0x002a
#define HCI_WIRELESS			0x0056
#define HCI_TOUCHPAD			0x050e

/* field definitions */

#define HCI_HOTKEY_S1 0x02 /* 0b0010,  HKEV &&  HKHS, unknown */
#define HCI_HOTKEY_S2 0x03 /* 0b0011, !HKEV &&  HKHS, unknown  */
#define HCI_HOTKEY_S3 0x09 /* 0b1001,  HKEV && !HKHS, "ENABLE" */
#define HCI_HOTKEY_S4 0x0b /* 0b1011, !HKEV && !HKHS, "DISABLE" */
#define HCI_HOTKEY_S5 0x0a /* 0b1010, ???, "DISABLE" from tos1900 driver */

#define HCI_HOTKEY_DISABLE		0x0b
#define HCI_HOTKEY_ENABLE		0x09
#define HCI_LCD_BRIGHTNESS_BITS		3
#define HCI_LCD_BRIGHTNESS_SHIFT	(16-HCI_LCD_BRIGHTNESS_BITS)
#define HCI_LCD_BRIGHTNESS_LEVELS	(1 << HCI_LCD_BRIGHTNESS_BITS)
#define HCI_VIDEO_OUT_LCD		0x1
#define HCI_VIDEO_OUT_CRT		0x2
#define HCI_VIDEO_OUT_TV		0x4
#define HCI_WIRELESS_KILL_SWITCH	0x01
#define HCI_WIRELESS_BT_PRESENT		0x0f
#define HCI_WIRELESS_BT_ATTACH		0x40
#define HCI_WIRELESS_BT_POWER		0x80


static DEVICE_ATTR_RW(touchpad);
static DEVICE_ATTR_RW(cpu_mode);

struct toshiba_dev {
	struct acpi_device *acpi_dev;
	const char *method_hci;
};

static const struct acpi_device_id toshiba_device_ids[] = {
	{"TOS6200", 0},
	{"TOS6208", 0},
	{"TOS1900", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, toshiba_device_ids);

/* utility
 */

static __inline__ void _set_bit(u32 * word, u32 mask, int value)
{
	*word = (*word & ~mask) | (mask * value);
}

/* acpi interface wrappers
 */

static int write_acpi_int(const char *methodName, int val)
{
	struct acpi_object_list params;
	union acpi_object in_objs[1];
	acpi_status status;

	params.count = ARRAY_SIZE(in_objs);
	params.pointer = in_objs;
	in_objs[0].type = ACPI_TYPE_INTEGER;
	in_objs[0].integer.value = val;

	status = acpi_evaluate_object(NULL, (char *)methodName, &params, NULL);
	return (status == AE_OK) ? 0 : -EIO;
}

/* Perform a raw HCI call.  Here we don't care about input or output buffer
 * format.
 */
static acpi_status hci_raw(struct toshiba_acpi_dev *dev,
			   const u32 in[HCI_WORDS], u32 out[HCI_WORDS])
{
	struct acpi_object_list params;
	union acpi_object in_objs[HCI_WORDS];
	struct acpi_buffer results;
	union acpi_object out_objs[HCI_WORDS + 1];
	acpi_status status;
	int i;

	params.count = HCI_WORDS;
	params.pointer = in_objs;
	for (i = 0; i < HCI_WORDS; ++i) {
		in_objs[i].type = ACPI_TYPE_INTEGER;
		in_objs[i].integer.value = in[i];
	}

	results.length = sizeof(out_objs);
	results.pointer = out_objs;

	status = acpi_evaluate_object(dev->acpi_dev->handle,
				      (char *)dev->method_hci, &params,
				      &results);
	if ((status == AE_OK) && (out_objs->package.count <= HCI_WORDS)) {
		for (i = 0; i < out_objs->package.count; ++i) {
			out[i] = out_objs->package.elements[i].integer.value;
		}
	}

	return status;
}

/* common hci tasks (get or set one or two value)
 *
 * In addition to the ACPI status, the HCI system returns a result which
 * may be useful (such as "not supported").
 */

static acpi_status hci_write1(struct toshiba_acpi_dev *dev, u32 reg,
			      u32 in1, u32 *result)
{
	u32 in[HCI_WORDS] = { HCI_SET, reg, in1, 0, 0, 0 };
	u32 out[HCI_WORDS];
	acpi_status status = hci_raw(dev, in, out);
	*result = (status == AE_OK) ? out[0] : HCI_FAILURE;
	return status;
}

static acpi_status hci_read1(struct toshiba_acpi_dev *dev, u32 reg,
			     u32 *out1, u32 *result)
{
	u32 in[HCI_WORDS] = { HCI_GET, reg, 0, 0, 0, 0 };
	u32 out[HCI_WORDS];
	acpi_status status = hci_raw(dev, in, out);
	*out1 = out[2];
	*result = (status == AE_OK) ? out[0] : HCI_FAILURE;
	return status;
}

static acpi_status hci_write2(struct toshiba_acpi_dev *dev, u32 reg,
			      u32 in1, u32 in2, u32 *result)
{
	u32 in[HCI_WORDS] = { HCI_SET, reg, in1, in2, 0, 0 };
	u32 out[HCI_WORDS];
	acpi_status status = hci_raw(dev, in, out);
	*result = (status == AE_OK) ? out[0] : HCI_FAILURE;
	return status;
}

static acpi_status hci_read2(struct toshiba_acpi_dev *dev, u32 reg,
			     u32 *out1, u32 *out2, u32 *result)
{
	u32 in[HCI_WORDS] = { HCI_GET, reg, *out1, *out2, 0, 0 };
	u32 out[HCI_WORDS];
	acpi_status status = hci_raw(dev, in, out);
	*out1 = out[2];
	*out2 = out[3];
	*result = (status == AE_OK) ? out[0] : HCI_FAILURE;
	return status;
}

/*** Driver ***/

static int toshiba_acpi_remove(struct acpi_device *acpi_dev)
{
	struct toshiba_acpi_dev *dev = acpi_driver_data(acpi_dev);
	kfree(dev);
	return 0;
}

static const char *find_hci_method(acpi_handle handle)
{
	acpi_status status;
	acpi_handle hci_handle;

	status = acpi_get_handle(handle, "GHCI", &hci_handle);
	if (ACPI_SUCCESS(status))
		return "GHCI";

	status = acpi_get_handle(handle, "SPFC", &hci_handle);
	if (ACPI_SUCCESS(status))
		return "SPFC";

	return NULL;
}

static int toshiba_acpi_add(struct acpi_device *acpi_dev)
{
	struct toshiba_acpi_dev *dev;
	const char *hci_method;

	/*
	 * Machines with this WMI guid aren't supported due to bugs in
	 * their AML. This check relies on wmi initializing before
	 * toshiba_acpi to guarantee guids have been identified.
	 */
	if (wmi_has_guid(TOSHIBA_WMI_EVENT_GUID))
		return -ENODEV;

	hci_method = find_hci_method(acpi_dev->handle);
	if (!hci_method) {
		pr_err("HCI interface not found\n");
		return -ENODEV;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->acpi_dev = acpi_dev;
	dev->method_hci = hci_method;
	acpi_dev->driver_data = dev;

	pr_info("loaded %s\n", acpi_dev->driver->name);

	return ret;
}

static void toshiba_acpi_notify(struct acpi_device *acpi_dev, u32 event)
{
	struct toshiba_acpi_dev *dev = acpi_driver_data(acpi_dev);

	pr_info("event: 0x%02x\n", event);
}

static struct acpi_driver toshiba_acpi_driver = {
	.name	= "toshiba_nb205",
	.owner	= THIS_MODULE,
	.ids	= toshiba_device_ids,
	.flags	= ACPI_DRIVER_ALL_NOTIFY_EVENTS,
	.ops	= {
		.add		= toshiba_acpi_add,
		.remove		= toshiba_acpi_remove,
		.notify		= toshiba_acpi_notify,
	},
};
module_acpi_driver(toshiba_acpi_driver);
