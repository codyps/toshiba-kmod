/* *********************** BEGIN LICENSE BLOCK *******************************\
 *
 * toshiba-tos1900.c - Driver for the Toshiba TOS1900 ACPI device.
 * Copyright (C) 2013 Isaac Lenton (aka ilent2)
 *
 * This driver is targeted specifically at certain features of the
 * TOS1900 ACPI device.  Parts where based on components in the
 * toshiba_acpi.c driver.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
\* *********************** END LICENSE BLOCK *********************************/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define TOSHIBA_TOS1900_VERSION "0.1"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/i8042.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/platform_device.h>
#include <acpi/acpi_drivers.h>

MODULE_ALIAS("platform:toshiba-tos1900");
MODULE_DESCRIPTION("Driver for the Toshiba Laptop TOS1900 Device");
MODULE_AUTHOR("Isaac Lenton (aka ilent2) <isaac@isuniversal.com>");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(TOSHIBA_TOS1900_VERSION);

#define SPFC_PATH "SPFC"
#define PIDC_PATH "\\PIDC"

#define SPFC_PARAMS     6
#define SPFC_RESULTS    7

#define PIDC_ID_ILLUMIN         0x06
#define PIDC_ID_KBD_BL          0x12
#define PIDC_ID_BOOT_SPEED      0x13
#define PIDC_ID_SLEEP_MUSIC     0x14
#define PIDC_ID_ALT_KBD_BL      0x15
#define PIDC_ID_ILLUMIN_FLASH   0x17
#define PIDC_ID_0A              0x0A

#define SPFC_LOWER_SET          0xFF00
#define SPFC_LOWER_GET          0xFE00
#define SPFC_UPPER_SET          0xF400
#define SPFC_UPPER_GET          0xF300

#define SPFC_NOT_SUPPORTED      0x8000

#define SPFC_ILLUMINATION       0x014E
#define SPFC_KBD_BACKLIGHT      0x015C
#define SPFC_BOOT_SPEED         0x015D
#define SPFC_SLEEP_MUSIC        0x015E
#define SPFC_TRACKPAD           0x050E
#define SPFC_WIRELESS           0x56
#define SPFC_CPU_MODE           0x7F
#define SPFC_ALT_KBD_BL         0x95
#define SPFC_ILLUMIN_FLASH      0x97

#define SPFC_HOTKEYS            0x1E
#define SPFC_HOTKEY_ENABLE      0x08
#define SPFC_HOTKEY_DISABLE     0x0A

static int tos1900_add(struct acpi_device *device);
static int tos1900_remove(struct acpi_device *device);
static void tos1900_notify(struct acpi_device *device, u32 event);

#ifdef CONFIG_PM_SLEEP
static int tos1900_suspend(struct device *device);
static int tos1900_resume(struct device *device);
#endif

static SIMPLE_DEV_PM_OPS(tos1900_pm, tos1900_suspend, tos1900_resume);

static const struct acpi_device_id tos1900_device_ids[] = {
    { "TOS1900", 0 },
    { "", 0 },
};
MODULE_DEVICE_TABLE(acpi, tos1900_device_ids);

static struct acpi_driver tos1900_acpi_driver = {
    .name   = "Toshiba TOS1900 Device Driver",
    .class  = "Toshiba",
    .ids    = tos1900_device_ids,
    .ops    = {
              .add    = tos1900_add,
              .remove = tos1900_remove,
              .notify = tos1900_notify,
              },
    .owner  = THIS_MODULE,
    .drv.pm = &tos1900_pm,
};
module_acpi_driver(tos1900_acpi_driver);

struct tos1900_device {
    struct acpi_device  *acpi_dev;
    struct input_dev    *hotkey_dev;

    unsigned int lumin_flash_mode:2;

    struct device_attribute *lumin_mode_attr;
    struct device_attribute *lumin_flash_attr;
    struct device_attribute *kbdbl_mode_attr;
    struct device_attribute *kbdbl_time_attr;
    struct device_attribute *alt_kbdbl_attr;
    struct device_attribute *boot_speed_attr;
    struct device_attribute *sleep_music_attr;
    struct device_attribute *trackpad_attr;
    struct device_attribute *cpu_mode_attr;
};
static struct tos1900_device *tos1900_dev;

/*********** Platform Device ***********/

static struct platform_driver tos1900_pf_driver = {
    .driver = {
            .name = "toshiba-tos1900",
            .owner = THIS_MODULE,
            }
};
static struct platform_device *tos1900_pf_device;

static int tos1900_pf_add(void)
{
    int result;

    result = platform_driver_register(&tos1900_pf_driver);
    if (result)
        goto out;

    tos1900_pf_device = platform_device_alloc("toshiba-tos1900", -1);
    if (!tos1900_pf_device) {
        result = -ENOMEM;
        goto outdriver;
    }

    result = platform_device_add(tos1900_pf_device);
    if (result)
        goto outalloc;

    return 0;

outalloc:
    platform_device_put(tos1900_pf_device);
    tos1900_pf_device = NULL;
outdriver:
    platform_driver_unregister(&tos1900_pf_driver);
out:
    return result;
}

static void tos1900_pf_remove(void)
{
    platform_device_unregister(tos1900_pf_device);
    platform_driver_unregister(&tos1900_pf_driver);
}

/*********** Toshiba Hotkeys ***********/

static const struct key_entry tos1900_keymap[] = {
    { KE_KEY, 0x101, { KEY_MUTE } },
    { KE_KEY, 0x102, { KEY_ZOOMOUT } },
    { KE_KEY, 0x103, { KEY_ZOOMIN } },
    { KE_IGNORE, 0x10f, { KEY_RESERVED } },     /* Unknown: Fn+Tab */
    { KE_KEY, 0x12c, { KEY_KBDILLUMTOGGLE } },
    { KE_KEY, 0x139, { KEY_ZOOMRESET } },
    { KE_KEY, 0x13b, { KEY_COFFEE } },
    { KE_KEY, 0x13c, { KEY_BATTERY } },
    { KE_KEY, 0x13d, { KEY_SLEEP } },
    { KE_KEY, 0x13e, { KEY_SUSPEND } },
    { KE_KEY, 0x13f, { KEY_SWITCHVIDEOMODE } },
    { KE_KEY, 0x140, { KEY_BRIGHTNESSDOWN } },
    { KE_KEY, 0x141, { KEY_BRIGHTNESSUP } },
    { KE_KEY, 0x142, { KEY_WLAN } },
    { KE_KEY, 0x143, { KEY_TOUCHPAD_TOGGLE } },

    /* Following keys are untested from toshiba_acpi.c */
    { KE_KEY, 0x17f, { KEY_FN } },            
    { KE_KEY, 0xb05, { KEY_PROG2 } },
    { KE_KEY, 0xb06, { KEY_WWW } },
    { KE_KEY, 0xb07, { KEY_MAIL } },
    { KE_KEY, 0xb30, { KEY_STOP } },
    { KE_KEY, 0xb31, { KEY_PREVIOUSSONG } },
    { KE_KEY, 0xb32, { KEY_NEXTSONG } },
    { KE_KEY, 0xb33, { KEY_PLAYPAUSE } },
    { KE_KEY, 0xb5a, { KEY_MEDIA } },
    { KE_IGNORE, 0x1430, { KEY_RESERVED } },

    { KE_END, 0 },
};

/*********** Hardware Communication Functions ***********/

/** Communicate with the SPFC Device (Based on toshiba_acpi.h : hci_raw)
 *  
 *  @param in : SPFC method arguments.
 *  @param out : SPFC return results (can be NULL).
 */
static acpi_status toshiba_spfc_communicate(const u32 *in, u32 *out)
{
    struct acpi_object_list params;
    union acpi_object in_objs[SPFC_PARAMS];
    struct acpi_buffer results;
    union acpi_object out_objs[SPFC_RESULTS];
    acpi_status status;
    int i;

    params.count = SPFC_PARAMS;
    params.pointer = in_objs;
    for (i = 0; i < SPFC_PARAMS; ++i) {
        in_objs[i].type = ACPI_TYPE_INTEGER;
        in_objs[i].integer.value = in[i];
    }

    results.length = sizeof(out_objs);
    results.pointer = out_objs;

    status = acpi_evaluate_object(tos1900_dev->acpi_dev->handle,
            SPFC_PATH, &params, &results);
    if (out && (status == AE_OK) && (out_objs->package.count < SPFC_RESULTS)) {
        for (i = 0; i < out_objs->package.count; ++i) {
            out[i] = out_objs->package.elements[i].integer.value;
        }
    }

    return status;
}

/** Asks the PIDC device if the system has device with id.
 *  
 *  @param handle : ACPI device handle.
 *  @param id : Device id to check.
 */
static int toshiba_acpi_is_device(u32 id)
{
    struct acpi_object_list params;
    union acpi_object in_obj;
    struct acpi_buffer results;
    union acpi_object out_obj;
    acpi_status status;

    params.count = 1;
    params.pointer = &in_obj;
    in_obj.type = ACPI_TYPE_INTEGER;
    in_obj.integer.value = id;
    results.length = sizeof(out_obj);
    results.pointer = &out_obj;

    status = acpi_evaluate_object(tos1900_dev->acpi_dev->handle, PIDC_PATH,
                                  &params, &results);
    if (ACPI_FAILURE(status))
        return 0;

    return out_obj.integer.value != -1;
}

static void tos1900_enable_hotkeys(void)
{
    u32 in[SPFC_PARAMS] = { SPFC_LOWER_SET, SPFC_HOTKEYS,
                            SPFC_HOTKEY_ENABLE, 0, 0, 0 };
    toshiba_spfc_communicate(in, NULL);
}

static void tos1900_disable_hotkeys(void)
{
    u32 in[SPFC_PARAMS] = { SPFC_LOWER_SET, SPFC_HOTKEYS,
                            SPFC_HOTKEY_DISABLE, 0, 0, 0 };
    toshiba_spfc_communicate(in, NULL);
}

/*********** Toshiba Hotkey Functions ***********/

/** Results in the invocation of the tos1900_notify method.
 *
 *  @see tos1900_notify
 */
static void tos1900_send_key(struct work_struct *work)
{
    acpi_handle ec_handle = ec_get_handle();
    acpi_status status;

    if (!ec_handle) {
        pr_err("Could not execute hotkey notify method.\n");
        return;
    }

    status = acpi_evaluate_object(ec_handle, "NTFY", NULL, NULL);
    if (ACPI_FAILURE(status)) {
        pr_err("Could not execute ACPI NTFY notify method.\n");
        return;
    }
}
static DECLARE_WORK(tos1900_work, tos1900_send_key);

/** Stop those pesky unmapped key warnings in dmesg (and schedule work). */
static bool tos1900_i8042_filter(unsigned char data, unsigned char str,
        struct serio *port)
{
    static bool btn_strip;

    if (str & 0x20)
        return false;

    /* Hide the keycode 0x60 = 0xe0 & 0x7f (doesn't work with multiple keys) */
    if (unlikely(btn_strip && data == 0xe0)) {
        btn_strip = false;
        return true;
    }

    if (unlikely(data == 0xe0)) {
        return false;
    }

    /* Function Keys */
    if (unlikely((data & 0x7f) == 0x6e)) {
        schedule_work(&tos1900_work);
        return true;
    }

    /* Button Strip */
    if (unlikely((data & 0x7f) == 0x42)) {
        btn_strip = true;
        schedule_work(&tos1900_work);
        return true;
    }

    return false;
}

static int toshiba_acpi_keyboard_setup(void)
{
    int result;

    tos1900_dev->hotkey_dev = input_allocate_device();
    if (!tos1900_dev->hotkey_dev)
        return -ENOMEM;

    tos1900_dev->hotkey_dev->name = "Toshiba input device";
    tos1900_dev->hotkey_dev->phys = "toshiba-tos1900/input0";
    tos1900_dev->hotkey_dev->id.bustype = BUS_HOST;

    result = sparse_keymap_setup(tos1900_dev->hotkey_dev,
            tos1900_keymap, NULL);
    if (result)
        goto outalloc;

    result = i8042_install_filter(tos1900_i8042_filter);
    if (result)
        goto outkeymap;

    result = input_register_device(tos1900_dev->hotkey_dev);
    if (result)
        goto outi8042;

    tos1900_enable_hotkeys();

    return 0;

outi8042:
    i8042_remove_filter(tos1900_i8042_filter);
outkeymap:
    sparse_keymap_free(tos1900_dev->hotkey_dev);
outalloc:
    input_free_device(tos1900_dev->hotkey_dev);
    tos1900_dev->hotkey_dev = NULL;
    return result;
}

static void toshiba_acpi_keyboard_cleanup(void)
{
    if (tos1900_dev->hotkey_dev) {
        input_unregister_device(tos1900_dev->hotkey_dev);
        i8042_remove_filter(tos1900_i8042_filter);
        sparse_keymap_free(tos1900_dev->hotkey_dev);
        tos1900_dev->hotkey_dev = NULL;
    }
}

/*********** Toshiba Illumination ***********/

static int __toshiba_illumination_mode_show(u32 *value)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_GET, SPFC_ILLUMINATION, 0, 0, 0, 0};
    u32 out[SPFC_PARAMS];

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, out)))
        return -EIO;

    *value = out[2];

    return 0;
}

static ssize_t toshiba_illumination_mode_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    int result, value;
    ssize_t count = 0;

    result = __toshiba_illumination_mode_show(&value);
    if (result)
        return result;

    count = snprintf(buffer, PAGE_SIZE, "%d\n", value);
    return count;
}

static int __toshiba_illumination_mode_store(u32 value)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_SET, SPFC_ILLUMINATION,
                           value ? 1 : 0, 0, 0, 0};

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, NULL)))
        return -EIO;

    return 0;
}

static ssize_t toshiba_illumination_mode_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    int result;
    unsigned long value;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    result = __toshiba_illumination_mode_store(value);
    if (result)
        return result;

    return count;
}

static int toshiba_illumination_setup(void)
{
    int result;

    tos1900_dev->lumin_mode_attr = kzalloc(
            sizeof(*tos1900_dev->lumin_mode_attr), GFP_KERNEL);
    if (!tos1900_dev->lumin_mode_attr)
        return -ENOMEM;

    sysfs_attr_init(tos1900_dev->lumin_mode_attr->attr);
    tos1900_dev->lumin_mode_attr->attr.name = "illumination";
    tos1900_dev->lumin_mode_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->lumin_mode_attr->show = toshiba_illumination_mode_show;
    tos1900_dev->lumin_mode_attr->store = toshiba_illumination_mode_store;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->lumin_mode_attr);
    if (result)
        goto outkzalloc;

    return 0;

outkzalloc:
    kfree(tos1900_dev->lumin_mode_attr);
    tos1900_dev->lumin_mode_attr = NULL;
    return result;
}

static void toshiba_illumination_cleanup(void)
{
    if (tos1900_dev->lumin_mode_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->lumin_mode_attr);
        kfree(tos1900_dev->lumin_mode_attr);
        tos1900_dev->lumin_mode_attr = NULL;
    }
}

/*********** Illumination Flash ***********/

static ssize_t toshiba_illumination_flash_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    int count = 0;
    count = snprintf(buffer, PAGE_SIZE, "%d\n",
            tos1900_dev->lumin_flash_mode);
    return count;
}

static int __toshiba_illumination_flash_store(u32 value)
{
    u32 in[SPFC_PARAMS] = {SPFC_LOWER_SET, SPFC_ILLUMIN_FLASH, value,
                           0, 0, 0};

    if (value >= 3)
        return -EINVAL;

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, NULL)))
        return -EIO;

    tos1900_dev->lumin_flash_mode = value;

    return 0;
}

static ssize_t toshiba_illumination_flash_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    unsigned long value;
    int result;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    result = __toshiba_illumination_flash_store(value);
    if (result)
        return result;

    return count;
}

static int toshiba_illumination_flash_setup(void)
{
    int result;

    tos1900_dev->lumin_flash_attr = kzalloc(
            sizeof(*tos1900_dev->lumin_flash_attr), GFP_KERNEL);
    if (!tos1900_dev->lumin_flash_attr)
        return -ENOMEM;

    sysfs_attr_init(tos1900_dev->lumin_flash_attr->attr);
    tos1900_dev->lumin_flash_attr->attr.name = "illumination_flash";
    tos1900_dev->lumin_flash_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->lumin_flash_attr->show = toshiba_illumination_flash_show;
    tos1900_dev->lumin_flash_attr->store = toshiba_illumination_flash_store;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->lumin_flash_attr);
    if (result)
        goto outkzalloc;

    __toshiba_illumination_flash_store(0);

    return 0;

outkzalloc:
    kfree(tos1900_dev->lumin_flash_attr);
    tos1900_dev->lumin_flash_attr = NULL;
    return result;
}

static void toshiba_illumination_flash_cleanup(void)
{
    if (tos1900_dev->lumin_flash_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->lumin_flash_attr);
        kfree(tos1900_dev->lumin_flash_attr);
        tos1900_dev->lumin_flash_attr = NULL;
    }
}

/*********** Keyboard Back-light ***********/

static int __toshiba_kbd_backlight_mode_show(u32 *mode)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_GET, SPFC_KBD_BACKLIGHT, 0, 0, 0, 0};
    u32 out[SPFC_PARAMS];

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, out)))
        return -EIO;

    *mode = out[2] & 0xFFFF;

    return 0;
}

static int __toshiba_kbd_backlight_time_show(u32 *time)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_GET, SPFC_KBD_BACKLIGHT, 0, 0, 0, 0};
    u32 out[SPFC_PARAMS];

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, out)))
        return -EIO;

    *time = (out[2] & 0x00FF0000) >> 16;

    return 0;
}

static ssize_t toshiba_kbd_backlight_mode_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    int mode, result;
    ssize_t count = 0;

    result = __toshiba_kbd_backlight_mode_show(&mode);
    if (result)
        return result;

    switch (mode) {
        case 0x10: { mode = 0; break; }
        case 0x08: { mode = 1; break; }
        case 0x02: { mode = 2; break; }
    }

    count = snprintf(buffer, PAGE_SIZE, "%d\n", mode);
    return count;
}

static ssize_t toshiba_kbd_backlight_time_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    int time, result;
    ssize_t count = 0;

    result = __toshiba_kbd_backlight_time_show(&time);
    if (result)
        return result;

    count = snprintf(buffer, PAGE_SIZE, "%d\n", time);
    return count;
}

static int __toshiba_kbd_backlight_store(u32 mode, u32 time)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_SET, SPFC_KBD_BACKLIGHT, 0, 0, 0, 0};

    if (time > 60)
        return -EINVAL;

    if (mode != 0x10 && mode != 0x08 && mode != 0x02)
        return -EINVAL;

    in[2] = (time << 16) | mode;
    if (ACPI_FAILURE(toshiba_spfc_communicate(in, NULL)))
        return -EIO;

    return 0;
}

static ssize_t toshiba_kbd_backlight_mode_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    unsigned long value;
    int mode=0x10, time;
    int result;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    switch (value) {
        case 0: { mode = 0x10; break; }
        case 1: { mode = 0x08; break; }
        case 2: { mode = 0x02; break; }
    }

    result = __toshiba_kbd_backlight_time_show(&time);
    if (result)
        return result;

    result = __toshiba_kbd_backlight_store(mode, time);
    if (result)
        return result;

    return count;
}

static ssize_t toshiba_kbd_backlight_time_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    unsigned long value;
    int mode, result;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    result = __toshiba_kbd_backlight_mode_show(&mode);
    if (result)
        return result;

    result = __toshiba_kbd_backlight_store(mode, value);
    if (result)
        return result;

    return count;
}

static int toshiba_kbd_backlight_setup(void)
{
    int result;

    tos1900_dev->kbdbl_mode_attr = kzalloc(
            sizeof(*tos1900_dev->kbdbl_mode_attr), GFP_KERNEL);
    if (!tos1900_dev->kbdbl_mode_attr)
        return -ENOMEM;

    tos1900_dev->kbdbl_time_attr = kzalloc(
            sizeof(*tos1900_dev->kbdbl_time_attr), GFP_KERNEL);
    if (!tos1900_dev->kbdbl_time_attr) {
        result = -ENOMEM;
        goto outkzalloc0;
    }

    sysfs_attr_init(tos1900_dev->kbdbl_mode_attr->attr);
    tos1900_dev->kbdbl_mode_attr->attr.name = "kbd_backlight";
    tos1900_dev->kbdbl_mode_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->kbdbl_mode_attr->show = toshiba_kbd_backlight_mode_show;
    tos1900_dev->kbdbl_mode_attr->store = toshiba_kbd_backlight_mode_store;

    sysfs_attr_init(tos1900_dev->kbdbl_time_attr->attr);
    tos1900_dev->kbdbl_time_attr->attr.name = "kbd_backlight_timeout";
    tos1900_dev->kbdbl_time_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->kbdbl_time_attr->show = toshiba_kbd_backlight_time_show;
    tos1900_dev->kbdbl_time_attr->store = toshiba_kbd_backlight_time_store;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->kbdbl_mode_attr);
    if (result)
        goto outkzalloc1;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->kbdbl_time_attr);
    if (result)
        goto outmode;

    return 0;

outmode:
    device_remove_file(&tos1900_pf_device->dev,
            tos1900_dev->kbdbl_mode_attr);
outkzalloc1:
    kfree(tos1900_dev->kbdbl_time_attr);
    tos1900_dev->kbdbl_time_attr = NULL;
outkzalloc0:
    kfree(tos1900_dev->kbdbl_mode_attr);
    tos1900_dev->kbdbl_mode_attr = NULL;
    return result;
}

static void toshiba_kbd_backlight_cleanup(void)
{
    if (tos1900_dev->kbdbl_mode_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->kbdbl_mode_attr);
        kfree(tos1900_dev->kbdbl_mode_attr);
        tos1900_dev->kbdbl_mode_attr = NULL;
    }

    if (tos1900_dev->kbdbl_time_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->kbdbl_time_attr);
        kfree(tos1900_dev->kbdbl_time_attr);
        tos1900_dev->kbdbl_time_attr = NULL;
    }
}

/*********** Alt. Keyboard Back-light***********/

static ssize_t toshiba_alt_kbdbl_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    u32 in[SPFC_PARAMS] = {SPFC_LOWER_GET, SPFC_ALT_KBD_BL, 0, 0, 0, 0};
    u32 out[SPFC_PARAMS];
    ssize_t count = 0;

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, out)))
        return -EIO;

    count = snprintf(buffer, PAGE_SIZE, "%d\n", out[2]);
    return count;
}

static ssize_t toshiba_alt_kbdbl_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    u32 in[SPFC_PARAMS] = {SPFC_LOWER_SET, SPFC_ALT_KBD_BL, 0, 0, 0, 0};
    unsigned long value;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    in[2] = value ? 1 : 0;
    if (ACPI_FAILURE(toshiba_spfc_communicate(in, NULL)))
        return -EIO;

    return count;
}

static int toshiba_alt_kbd_backlight_setup(void)
{
    int result;

    tos1900_dev->alt_kbdbl_attr = kzalloc(
            sizeof(*tos1900_dev->alt_kbdbl_attr), GFP_KERNEL);
    if (!tos1900_dev->alt_kbdbl_attr)
        return -ENOMEM;

    sysfs_attr_init(tos1900_dev->alt_kbdbl_attr->attr);
    tos1900_dev->alt_kbdbl_attr->attr.name = "kbd_backlight";
    tos1900_dev->alt_kbdbl_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->alt_kbdbl_attr->show = toshiba_alt_kbdbl_show;
    tos1900_dev->alt_kbdbl_attr->store = toshiba_alt_kbdbl_store;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->alt_kbdbl_attr);
    if (result)
        goto outkzalloc;

    return 0;

outkzalloc:
    kfree(tos1900_dev->alt_kbdbl_attr);
    tos1900_dev->alt_kbdbl_attr = NULL;
    return result;
}

static void toshiba_alt_kbd_backlight_cleanup(void)
{
    if (tos1900_dev->alt_kbdbl_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->alt_kbdbl_attr);
        kfree(tos1900_dev->alt_kbdbl_attr);
        tos1900_dev->alt_kbdbl_attr = NULL;
    }
}

/*********** Boot Speed ***********/

static ssize_t toshiba_boot_speed_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_GET, SPFC_BOOT_SPEED, 0, 0, 0, 0};
    u32 out[SPFC_PARAMS];
    ssize_t count = 0;

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, out)))
        return -EIO;

    count = snprintf(buffer, PAGE_SIZE, "%d\n", out[2]);
    return count;
}

static ssize_t toshiba_boot_speed_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_SET, SPFC_BOOT_SPEED, 0, 0, 0, 0};
    unsigned long value;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    in[2] = value ? 1 : 0;
    if (ACPI_FAILURE(toshiba_spfc_communicate(in, NULL)))
        return -EIO;

    return count;
}

static int toshiba_boot_speed_setup(void)
{
    int result;

    tos1900_dev->boot_speed_attr = kzalloc(
            sizeof(*tos1900_dev->boot_speed_attr), GFP_KERNEL);
    if (!tos1900_dev->boot_speed_attr)
        return -ENOMEM;

    sysfs_attr_init(tos1900_dev->boot_speed_attr->attr);
    tos1900_dev->boot_speed_attr->attr.name = "fast_boot";
    tos1900_dev->boot_speed_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->boot_speed_attr->show = toshiba_boot_speed_show;
    tos1900_dev->boot_speed_attr->store = toshiba_boot_speed_store;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->boot_speed_attr);
    if (result)
        goto outkzalloc;

    return 0;

outkzalloc:
    kfree(tos1900_dev->boot_speed_attr);
    tos1900_dev->boot_speed_attr = NULL;
    return result;
}

static void toshiba_boot_speed_cleanup(void)
{
    if (tos1900_dev->boot_speed_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->boot_speed_attr);
        kfree(tos1900_dev->boot_speed_attr);
        tos1900_dev->boot_speed_attr = NULL;
    }
}

/*********** Sleep and Music ***********/

static ssize_t toshiba_sleep_music_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_GET, SPFC_SLEEP_MUSIC, 0, 0, 0, 0};
    u32 out[SPFC_PARAMS];
    ssize_t count = 0;

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, out)))
        return -EIO;

    count = snprintf(buffer, PAGE_SIZE, "%d\n", out[2]);
    return count;
}

static ssize_t toshiba_sleep_music_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_SET, SPFC_SLEEP_MUSIC, 0, 0, 0, 0};
    unsigned long value;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    in[2] = value ? 1 : 0;
    if (ACPI_FAILURE(toshiba_spfc_communicate(in, NULL)))
        return -EIO;

    return count;
}

static int toshiba_sleep_music_setup(void)
{
    int result;

    tos1900_dev->sleep_music_attr = kzalloc(
            sizeof(*tos1900_dev->sleep_music_attr), GFP_KERNEL);
    if (!tos1900_dev->sleep_music_attr)
        return -ENOMEM;

    sysfs_attr_init(tos1900_dev->sleep_music_attr->attr);
    tos1900_dev->sleep_music_attr->attr.name = "sleep_and_music";
    tos1900_dev->sleep_music_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->sleep_music_attr->show = toshiba_sleep_music_show;
    tos1900_dev->sleep_music_attr->store = toshiba_sleep_music_store;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->sleep_music_attr);
    if (result)
        goto outkzalloc;

    return 0;

outkzalloc:
    kfree(tos1900_dev->sleep_music_attr);
    tos1900_dev->sleep_music_attr = NULL;
    return result;
}

static void toshiba_sleep_music_cleanup(void)
{
    if (tos1900_dev->sleep_music_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->sleep_music_attr);
        kfree(tos1900_dev->sleep_music_attr);
        tos1900_dev->sleep_music_attr = NULL;
    }
}

/*********** Trackpad ***********/

static ssize_t toshiba_trackpad_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_GET, SPFC_TRACKPAD, 0, 0, 0, 0};
    u32 out[SPFC_PARAMS];
    ssize_t count = 0;

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, out)))
        return -EIO;

    count = snprintf(buffer, PAGE_SIZE, "%d\n", out[2]);
    return count;
}

static ssize_t toshiba_trackpad_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    u32 in[SPFC_PARAMS] = {SPFC_UPPER_SET, SPFC_TRACKPAD, 0, 0, 0, 0};
    unsigned long value;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    in[2] = value ? 1 : 0;
    if (ACPI_FAILURE(toshiba_spfc_communicate(in, NULL)))
        return -EIO;

    return count;
}

static int toshiba_trackpad_setup(void)
{
    int result;

    tos1900_dev->trackpad_attr = kzalloc(
            sizeof(*tos1900_dev->trackpad_attr), GFP_KERNEL);
    if (!tos1900_dev->trackpad_attr)
        return -ENOMEM;

    sysfs_attr_init(tos1900_dev->trackpad_attr->attr);
    tos1900_dev->trackpad_attr->attr.name = "trackpad";
    tos1900_dev->trackpad_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->trackpad_attr->show = toshiba_trackpad_show;
    tos1900_dev->trackpad_attr->store = toshiba_trackpad_store;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->trackpad_attr);
    if (result)
        goto outkzalloc;

    return 0;

outkzalloc:
    kfree(tos1900_dev->trackpad_attr);
    tos1900_dev->trackpad_attr = NULL;
    return result;
}

static void toshiba_trackpad_cleanup(void)
{
    if (tos1900_dev->trackpad_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->trackpad_attr);
        kfree(tos1900_dev->trackpad_attr);
        tos1900_dev->trackpad_attr = NULL;
    }
}

/*********** CPU Mode ***********/

static ssize_t toshiba_cpu_mode_show(struct device *dev,
        struct device_attribute *attr, char* buffer)
{
    u32 in[SPFC_PARAMS] = {SPFC_LOWER_GET, SPFC_CPU_MODE, 0, 0, 0, 0};
    u32 out[SPFC_PARAMS];
    ssize_t count = 0;

    if (ACPI_FAILURE(toshiba_spfc_communicate(in, out)))
        return -EIO;

    count = snprintf(buffer, PAGE_SIZE, "%d\n", out[2] & 0x1);
    return count;
}

static ssize_t toshiba_cpu_mode_store(struct device *dev,
        struct device_attribute *attr, const char* buffer, size_t count)
{
    u32 in[SPFC_PARAMS] = {SPFC_LOWER_SET, SPFC_CPU_MODE, 0, 0, 0, 0};
    unsigned long value;

    if (count > 31)
        return -EINVAL;

    if (kstrtoul(buffer, 10, &value))
        return -EINVAL;

    in[2] = value ? 1 : 0;
    if (ACPI_FAILURE(toshiba_spfc_communicate(in, NULL)))
        return -EIO;

    return count;
}

static int toshiba_cpu_mode_setup(void)
{
    int result;

    tos1900_dev->cpu_mode_attr = kzalloc(
            sizeof(*tos1900_dev->cpu_mode_attr), GFP_KERNEL);
    if (!tos1900_dev->cpu_mode_attr)
        return -ENOMEM;

    sysfs_attr_init(tos1900_dev->cpu_mode_attr->attr);
    tos1900_dev->cpu_mode_attr->attr.name = "cpu_mode";
    tos1900_dev->cpu_mode_attr->attr.mode = S_IRUGO | S_IWUSR;
    tos1900_dev->cpu_mode_attr->show = toshiba_cpu_mode_show;
    tos1900_dev->cpu_mode_attr->store = toshiba_cpu_mode_store;

    result = device_create_file(&tos1900_pf_device->dev,
            tos1900_dev->cpu_mode_attr);
    if (result)
        goto outkzalloc;

    return 0;

outkzalloc:
    kfree(tos1900_dev->cpu_mode_attr);
    tos1900_dev->cpu_mode_attr = NULL;
    return result;
}

static void toshiba_cpu_mode_cleanup(void)
{
    if (tos1900_dev->cpu_mode_attr) {
        device_remove_file(&tos1900_pf_device->dev,
                tos1900_dev->cpu_mode_attr);
        kfree(tos1900_dev->cpu_mode_attr);
        tos1900_dev->cpu_mode_attr = NULL;
    }
}

/*********** TODO: Wireless ***********/

static int toshiba_acpi_wireless_setup(void)
{
    return 0;
}

static void toshiba_acpi_wireless_cleanup(void)
{

}

/*********** TOS1900 ACPI Setup/Cleanup ***********/

static int tos1900_acpi_setup(void)
{
    int result;

    if (toshiba_acpi_is_device(PIDC_ID_ILLUMIN) ||
            toshiba_acpi_is_device(PIDC_ID_0A)) {
        result = toshiba_illumination_setup();
        if (result)
            goto out;
    }

    if (toshiba_acpi_is_device(PIDC_ID_BOOT_SPEED)) {
        result = toshiba_boot_speed_setup();
        if (result)
            goto outillumination;
    }

    if (toshiba_acpi_is_device(PIDC_ID_SLEEP_MUSIC)) {
        result = toshiba_sleep_music_setup();
        if (result)
            goto outbootspeed;
    }

    if (toshiba_acpi_is_device(PIDC_ID_ILLUMIN_FLASH)) {
        result = toshiba_illumination_flash_setup();
        if (result)
            goto outsleepmusic;
    }

    result = toshiba_trackpad_setup();
    if (result)
        goto outluminflash;

    result = toshiba_cpu_mode_setup();
    if (result)
        goto outtrackpad;

    if (toshiba_acpi_is_device(PIDC_ID_KBD_BL)) {
        result = toshiba_kbd_backlight_setup();
        if (result)
            goto outcpumode;
    } else if (toshiba_acpi_is_device(PIDC_ID_ALT_KBD_BL)) {
        result = toshiba_alt_kbd_backlight_setup();
        if (result)
            goto outcpumode;
    }

    return 0;

outcpumode:
    toshiba_cpu_mode_cleanup();
outtrackpad:
    toshiba_trackpad_cleanup();
outluminflash:
    toshiba_illumination_flash_cleanup();
outsleepmusic:
    toshiba_sleep_music_cleanup();
outbootspeed:
    toshiba_boot_speed_cleanup();
outillumination:
    toshiba_illumination_cleanup();
out:
    return result;
}

static void tos1900_acpi_cleanup(void)
{
    toshiba_cpu_mode_cleanup();
    toshiba_trackpad_cleanup();
    toshiba_illumination_flash_cleanup();
    toshiba_alt_kbd_backlight_cleanup();
    toshiba_sleep_music_cleanup();
    toshiba_boot_speed_cleanup();
    toshiba_kbd_backlight_cleanup();
    toshiba_illumination_cleanup();
}

/*********** ACPI Driver Functions ***********/

static int tos1900_add(struct acpi_device *device)
{
    acpi_status status;
    acpi_handle handle;
    int result;

    if (tos1900_dev)
        return -EBUSY;

    pr_info("Toshiba TOS1900 Device Found\n");

    tos1900_dev = kzalloc(sizeof(struct tos1900_device), GFP_KERNEL);
    if (!tos1900_dev)
        return -ENOMEM;

    device->driver_data = tos1900_dev;
    tos1900_dev->acpi_dev = device;

    result = tos1900_pf_add();
    if (result)
        goto out;

    status = acpi_get_handle(device->handle, SPFC_PATH, &handle);
    if (ACPI_SUCCESS(status)) {
        result = tos1900_acpi_setup();
        if (result)
            goto outpf;
    }

    result = toshiba_acpi_keyboard_setup();
    if (result)
        goto outacpi;

    result = toshiba_acpi_wireless_setup();
    if (result)
        goto outkeyboard;

    return 0;

outkeyboard:
    toshiba_acpi_keyboard_cleanup();
outacpi:
    tos1900_acpi_cleanup();
outpf:
    tos1900_pf_remove();
out:
    kfree(tos1900_dev);
    return result;
}

static int tos1900_remove(struct acpi_device *device)
{
    toshiba_acpi_wireless_cleanup();
    toshiba_acpi_keyboard_cleanup();
    tos1900_acpi_cleanup();

    tos1900_pf_remove();

    kfree(tos1900_dev);
    tos1900_dev = NULL;

    return 0;
}

static void tos1900_notify(struct acpi_device *device, u32 event)
{
    struct tos1900_device *tdev = acpi_driver_data(device);
    unsigned long long hotkey;
    acpi_status status;

    if (event != 0x80)
        return;

    status = acpi_evaluate_integer(device->handle, "INFO", NULL, &hotkey);
    if (ACPI_FAILURE(status))
        return;

    /* Ignore FN Release and No Hotkey Events. */
    if (hotkey == 0x000 || hotkey == 0x100)
        return;

    /* Act only on key press events, ignore key release */
    if (hotkey & 0x80)
        return;

    if (!sparse_keymap_report_event(tdev->hotkey_dev, hotkey, 1, true))
        pr_info("Unknown Hotkey: 0x%04X\n", (unsigned int) hotkey);
}

#ifdef CONFIG_PM_SLEEP
static int tos1900_suspend(struct device *device)
{
    tos1900_disable_hotkeys();
    return 0;
}

static int tos1900_resume(struct device *device)
{
    tos1900_enable_hotkeys();
    return 0;
}
#endif

