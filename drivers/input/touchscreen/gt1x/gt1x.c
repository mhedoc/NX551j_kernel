/* drivers/input/touchscreen/gt1x.c
 *
 * 2010 - 2014 Goodix Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Version: 1.4
 * Release Date:  2015/07/10
 */

#include <linux/irq.h>
#include <linux/power_supply.h>
#include "gt1x.h"
#if GTP_ICS_SLOT_REPORT
#include <linux/input/mt.h>
#endif

static struct work_struct gt1x_work;
static struct work_struct gt1x_resume_work; //nubia add resume in workqueue
static struct input_dev *input_dev;
static struct workqueue_struct *gt1x_wq;
static const char *gt1x_ts_name = "goodix-ts";
static const char *input_dev_phys = "input/ts";
#ifdef GTP_CONFIG_OF
int gt1x_rst_gpio;
int gt1x_int_gpio;
#endif
extern u8 gt1x_wakeup_gesture;
extern bool gt1x_dev_halt;
extern bool gt1x_dev_suspended;
static int gt1x_register_powermanger(void);
static int gt1x_unregister_powermanger(void);

/*open/short test*/
extern s32 gtp_test_sysfs_init(void);
extern void gtp_test_sysfs_deinit(void);

#if GTP_CHARGER_SWITCH
static struct notifier_block	gt1_power_notifier;
extern int gt1_power_notifier_cb(struct notifier_block *self,unsigned long val, void *v);
#endif

/**
 * gt1x_i2c_write - i2c write.
 * @addr: register address.
 * @buffer: data buffer.
 * @len: the bytes of data to write.
 *Return: 0: success, otherwise: failed
 */
s32 gt1x_i2c_write(u16 addr, u8 * buffer, s32 len)
{
	struct i2c_msg msg = {
		.flags = 0,
		.addr = gt1x_i2c_client->addr,
	};
	return _do_i2c_write(&msg, addr, buffer, len);
}

/**
 * gt1x_i2c_read - i2c read.
 * @addr: register address.
 * @buffer: data buffer.
 * @len: the bytes of data to write.
 *Return: 0: success, otherwise: failed
 */
s32 gt1x_i2c_read(u16 addr, u8 * buffer, s32 len)
{
	u8 addr_buf[GTP_ADDR_LENGTH] = { (addr >> 8) & 0xFF, addr & 0xFF };
	struct i2c_msg msgs[2] = {
		{
		 .addr = gt1x_i2c_client->addr,
		 .flags = 0,
		 .buf = addr_buf,
		 .len = GTP_ADDR_LENGTH},
		{
		 .addr = gt1x_i2c_client->addr,
		 .flags = I2C_M_RD}
	};
	return _do_i2c_read(msgs, addr, buffer, len);
}

static spinlock_t irq_lock;
static s32 irq_is_disable = 0;
static bool irq_wakeup = false;

/**
 * gt1x_irq_wakeup_enable() - enable irq wakeup
 *
 */
void gt1x_irq_wakeup_enable(void)
{
	unsigned long irqflags = 0;
	spin_lock_irqsave(&irq_lock, irqflags);
	if(irq_wakeup == false) {
		irq_wakeup = true;
		enable_irq_wake(gt1x_i2c_client->irq);
	}
	spin_unlock_irqrestore(&irq_lock, irqflags);
}
/**
 * gt1x_irq_wakeup_disable() - disable irq wakeup
 *
 */
void gt1x_irq_wakeup_disable(void)
{
	unsigned long irqflags = 0;
	spin_lock_irqsave(&irq_lock, irqflags);
	if(irq_wakeup == true) {
		irq_wakeup = false;
		disable_irq_wake(gt1x_i2c_client->irq);
	}
	spin_unlock_irqrestore(&irq_lock, irqflags);
}

/**
 * gt1x_irq_enable - enable irq function.
 *
 */
void gt1x_irq_enable(void)
{
	unsigned long irqflags = 0;

	GTP_DEBUG_FUNC();

	spin_lock_irqsave(&irq_lock, irqflags);
	if (irq_is_disable) {
		enable_irq(gt1x_i2c_client->irq);
		irq_is_disable = 0;
	}
	spin_unlock_irqrestore(&irq_lock, irqflags);
}

/**
 * gt1x_irq_enable - disable irq function.
 *
 */
void gt1x_irq_disable(void)
{
	unsigned long irqflags;

	GTP_DEBUG_FUNC();

	spin_lock_irqsave(&irq_lock, irqflags);
	if (!irq_is_disable) {
		irq_is_disable = 1;
		disable_irq_nosync(gt1x_i2c_client->irq);
	}
	spin_unlock_irqrestore(&irq_lock, irqflags);
}

/**
 * gt1x_cancle_irq_work - cancle irq work when suspend
 *
 */
 void gt1x_cancle_irq_work()
{
    cancel_work_sync(&gt1x_work);
}

#ifndef GTP_CONFIG_OF
int gt1x_power_switch(s32 state)
{
    return 0;
}
#endif

int gt1x_debug_proc(u8 * buf, int count)
{
	return -1;
}


/**
 * gt1x_ts_irq_handler - External interrupt service routine for interrupt mode.
 * @irq:  interrupt number.
 * @dev_id: private data pointer.
 * Return: Handle Result.
 *  		IRQ_HANDLED: interrupt handled successfully
 */
static irqreturn_t gt1x_ts_irq_handler(int irq, void *dev_id)
{
	GTP_DEBUG_FUNC();
      gt1x_irq_disable();
	queue_work(gt1x_wq, &gt1x_work);
	return IRQ_HANDLED;
}

void gt1x_release_fingers()
{
#if GTP_ICS_SLOT_REPORT
    int i;
//nubia clear point
    for (i = 0; i < GTP_MAX_TOUCH; i++) {
        input_mt_slot(input_dev, i);
        input_mt_report_slot_state(input_dev,
            MT_TOOL_FINGER, false);
    }
    input_sync(input_dev);
#else
    input_report_key(input_dev, BTN_TOUCH, 0);
    input_mt_sync(input_dev);
    input_sync(input_dev);
#endif
}
/**
 * gt1x_touch_down - Report touch point event .
 * @id: trackId
 * @x:  input x coordinate
 * @y:  input y coordinate
 * @w:  input pressure
 * Return: none.
 */
void gt1x_touch_down(s32 x, s32 y, s32 size, s32 id)
{
#if GTP_CHANGE_X2Y
	GTP_SWAP(x, y);
#endif

#if GTP_ICS_SLOT_REPORT
	input_mt_slot(input_dev, id);
	input_report_abs(input_dev, ABS_MT_PRESSURE, size);
	input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, size);
	input_report_abs(input_dev, ABS_MT_TRACKING_ID, id);
	input_report_abs(input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
#else
	input_report_key(input_dev, BTN_TOUCH, 1);
	if ((!size) && (!id)) {
		/* for virtual button */
		input_report_abs(input_dev, ABS_MT_PRESSURE, 100);
		input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, 100);
	} else {
		input_report_abs(input_dev, ABS_MT_PRESSURE, size);
		input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, size);
		input_report_abs(input_dev, ABS_MT_TRACKING_ID, id);
	}
	input_report_abs(input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
	input_mt_sync(input_dev);
#endif
}

/**
 * gt1x_touch_up -  Report touch release event.
 * @id: trackId
 * Return: none.
 */
void gt1x_touch_up(s32 id)
{
#if GTP_ICS_SLOT_REPORT
	input_mt_slot(input_dev, id);
	input_report_abs(input_dev, ABS_MT_TRACKING_ID, -1);
#else
	input_report_key(input_dev, BTN_TOUCH, 0);
	input_mt_sync(input_dev);
#endif
}

/**
 * gt1x_ts_work_func - Goodix touchscreen work function.
 * @iwork: work struct of gt1x_workqueue.
 * Return: none.
 */
static void gt1x_ts_work_func(struct work_struct *work)
{
	u8 end_cmd = 0;
	u8 finger = 0;
	s32 ret = 0;
	u8 point_data[11] = { 0 };

    if (update_info.status) {
        GTP_INFO("Ignore interrupts during fw update.");
        return;
    }
#if GTP_GESTURE_WAKEUP
	if (gt1x_wakeup_gesture) {
		ret = gesture_event_handler(input_dev);
		if (ret >= 0) {
			goto exit_work_func;
		}
	}
#endif

	if (gt1x_halt) {
		GTP_INFO("Ignore interrupts after suspend...");
        return;
	}

	ret = gt1x_i2c_read(GTP_READ_COOR_ADDR, point_data, sizeof(point_data));
	if (ret < 0) {
		GTP_ERROR("I2C transfer error!");
#if !GTP_ESD_PROTECT
		gt1x_power_reset();
#endif
		goto exit_work_func;
	}

	finger = point_data[0];
	if (finger == 0x00) {
		gt1x_request_event_handler();
	}

	if ((finger & 0x80) == 0) {
#if HOTKNOT_BLOCK_RW
		if (!hotknot_paired_flag)
#endif
		{
			//GTP_ERROR("buffer not ready:0x%02x", finger);
			goto exit_eint;
		}
	}
#if HOTKNOT_BLOCK_RW
	ret = hotknot_event_handler(point_data);
	if (!ret) {
		goto exit_work_func;
	}
#endif

#if GTP_PROXIMITY
	ret = gt1x_prox_event_handler(point_data);
	if (ret > 0) {
		goto exit_work_func;
	}
#endif

#if GTP_WITH_STYLUS
	ret = gt1x_touch_event_handler(point_data, input_dev, pen_dev);
#else
	ret = gt1x_touch_event_handler(point_data, input_dev, NULL);
#endif

exit_work_func:
	if (!gt1x_rawdiff_mode && (ret >= 0 || ret == ERROR_VALUE)) {
		ret = gt1x_i2c_write(GTP_READ_COOR_ADDR, &end_cmd, 1);
		if (ret < 0) {
			GTP_ERROR("I2C write end_cmd  error!");
		}
	}
exit_eint:
    gt1x_irq_enable();

}

/*
 * Devices resume work func
 * nubia add resume in workqueue
 */
static void gt1x_resume_work_func(struct work_struct *work)
{
	gt1x_resume();
}

/*
 * Devices Tree support,
*/
#ifdef GTP_CONFIG_OF

static struct regulator *vdd_ana;
static struct regulator *vdd_io;
static struct regulator *vcc_i2c;

/**
 * gt1x_parse_dt - parse platform infomation form devices tree.
 */
static int gt1x_parse_dt(struct device *dev)
{
	struct device_node *np;
    int ret = 0;

    if (!dev)
        return -ENODEV;

    np = dev->of_node;
	gt1x_int_gpio = of_get_named_gpio(np, "goodix,irq-gpio", 0);
	gt1x_rst_gpio = of_get_named_gpio(np, "goodix,reset-gpio", 0);

    if (!gpio_is_valid(gt1x_int_gpio) || !gpio_is_valid(gt1x_rst_gpio)) {
        GTP_ERROR("Invalid GPIO, irq-gpio:%d, rst-gpio:%d",
            gt1x_int_gpio, gt1x_rst_gpio);
        return -EINVAL;
    }

    vdd_ana = regulator_get(dev, "vdd_ana");
    if (IS_ERR(vdd_ana)) {
	    GTP_ERROR("regulator get of vdd_ana failed");
	    ret = PTR_ERR(vdd_ana);
	    vdd_ana = NULL;
	    return ret;
    }

    if (regulator_count_voltages(vdd_ana) > 0) {
	    ret = regulator_set_voltage(vdd_ana,2800000, 3300000);
	    if (ret) {
		    GTP_ERROR("vdd set_vtg failed \n");
		    goto ERR_GET_VCC;
	    }
     }

	vcc_i2c = regulator_get(dev, "vcc_i2c");
	if (IS_ERR(vcc_i2c)) {
		GTP_ERROR("regulator get of vcc_i2c failed");
		ret = PTR_ERR(vcc_i2c);
		vcc_i2c = NULL;
		goto ERR_GET_VCC;
	}

    if (regulator_count_voltages(vcc_i2c) > 0) {
	    ret = regulator_set_voltage(vcc_i2c,1700000, 1900000);
	    if (ret) {
		    GTP_ERROR("vdd set_vtg failed \n");
		    goto ERR_GET_I2C_VCC;
	    }
    }

	vdd_io = regulator_get(dev, "vdd_io");
	if (IS_ERR(vdd_io)) {
		GTP_ERROR("regulator get of vdd_io failed");
		ret = PTR_ERR(vdd_io);
		vdd_io = NULL;
	}

    return 0;
ERR_GET_I2C_VCC:
    regulator_put(vcc_i2c);
    vcc_i2c = NULL;
ERR_GET_VCC:
    regulator_put(vdd_ana);
    vdd_ana = NULL;
    return ret;

}

/**
 * gt1x_power_switch - power switch .
 * @on: 1-switch on, 0-switch off.
 * return: 0-succeed, -1-faileds
 */
int gt1x_power_switch(int on)
{
	int ret;
	struct i2c_client *client = gt1x_i2c_client;

    if (!client || !vdd_ana || !vcc_i2c)
    	{
        return -1;
    	}
	if (on) {
		GTP_DEBUG("GTP power on.");
		ret = regulator_enable(vdd_ana);
		udelay(2);
		ret = regulator_enable(vcc_i2c);
		udelay(2);
		if(vdd_io)
			ret = regulator_enable(vdd_io);
	} else {
		GTP_DEBUG("GTP power off.");
		ret = regulator_disable(vcc_i2c);
		udelay(2);
		ret = regulator_disable(vdd_ana);
		udelay(2);
		if(vdd_io)
			ret = regulator_disable(vdd_io);
	}
	return ret;

}

#define NUBIA_GT1_PINCTRL

#ifdef NUBIA_GT1_PINCTRL
#define GT1_PINCTRL_STATE_SUSPEND "gt1x_pin_suspend"
#define GT1_PINCTRL_STATE_ACTIVE "gt1x_pin_active"
#define GT1_PINCTRL_STATE_RELEASE "gt1x_pin_release"
struct gt1_pinctrl {
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_active;
	struct pinctrl_state *pin_suspend;
	struct pinctrl_state *pin_release;
}gt1_pinctrl_info;

static int gt1_pinctrl_init(struct device *dev)
{
	gt1_pinctrl_info.pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gt1_pinctrl_info.pinctrl)) {
		GTP_ERROR(" get pinctrl info error.\n");
		return -EINVAL;
	}

	gt1_pinctrl_info.pin_active = pinctrl_lookup_state(gt1_pinctrl_info.pinctrl,GT1_PINCTRL_STATE_ACTIVE);
	if (IS_ERR_OR_NULL(gt1_pinctrl_info.pin_active)) {
		GTP_ERROR(" get pin_active info error.\n");
		return -EINVAL;
	}

	gt1_pinctrl_info.pin_suspend = pinctrl_lookup_state(gt1_pinctrl_info.pinctrl,GT1_PINCTRL_STATE_SUSPEND);
	if (IS_ERR_OR_NULL(gt1_pinctrl_info.pin_suspend)) {
		GTP_ERROR("get pin_suspend info error.\n");
		return -EINVAL;
	}

	gt1_pinctrl_info.pin_release = pinctrl_lookup_state(gt1_pinctrl_info.pinctrl,GT1_PINCTRL_STATE_RELEASE);
	if (IS_ERR_OR_NULL(gt1_pinctrl_info.pin_release)) {
		GTP_ERROR("get pin_release info error.\n");
		return -EINVAL;
	}

	return 0;
}

//nubia for pinctrl
static int gt1_pinctrl_set_state(bool active)
{
	int ret = -1;

	if (!gt1_pinctrl_info.pinctrl ||
			!gt1_pinctrl_info.pin_active ||
			!gt1_pinctrl_info.pin_suspend ||
			!gt1_pinctrl_info.pin_release) {
		GTP_ERROR("pinctrl is invalid, skip.\n");
		return ret;
	}
	if (active) {
		ret = pinctrl_select_state(gt1_pinctrl_info.pinctrl,gt1_pinctrl_info.pin_active);
	} else {
		ret = pinctrl_select_state(gt1_pinctrl_info.pinctrl,gt1_pinctrl_info.pin_suspend);
	}
	GTP_DEBUG("set pinctrl to [%s], ret = %d.\n",active ? "active" : "suspend", ret);

	return ret;
}
#endif

#endif

static void gt1x_remove_gpio_and_power(void)
{
    if (gpio_is_valid(gt1x_int_gpio))
        gpio_free(gt1x_int_gpio);

    if (gpio_is_valid(gt1x_rst_gpio))
        gpio_free(gt1x_rst_gpio);

#ifdef GTP_CONFIG_OF
    if (vcc_i2c)
        regulator_put(vcc_i2c);

    if (vdd_ana)
        regulator_put(vdd_ana);
#endif

    if (gt1x_i2c_client && gt1x_i2c_client->irq)
        free_irq(gt1x_i2c_client->irq, gt1x_i2c_client);

}


/**
 * gt1x_request_io_port - Request gpio(INT & RST) ports.
 */
static s32 gt1x_request_io_port(void)
{
	s32 ret = 0;

	GTP_DEBUG_FUNC();
	ret = gpio_request(GTP_INT_PORT, "GTP_INT_IRQ");
	if (ret < 0) {
		GTP_ERROR("Failed to request GPIO:%d, ERRNO:%d", (s32) GTP_INT_PORT, ret);
		ret = -ENODEV;
	} else {
		GTP_GPIO_AS_INT(GTP_INT_PORT);
		gt1x_i2c_client->irq = GTP_INT_IRQ;
	}

	ret = gpio_request(GTP_RST_PORT, "GTP_RST_PORT");
	if (ret < 0) {
		GTP_ERROR("Failed to request GPIO:%d, ERRNO:%d", (s32) GTP_RST_PORT, ret);
		ret = -ENODEV;
	}

	GTP_GPIO_AS_INPUT(GTP_RST_PORT);
	if (ret < 0) {
		gpio_free(GTP_RST_PORT);
		gpio_free(GTP_INT_PORT);
	}

	return ret;
}

/**
 * gt1x_request_irq - Request interrupt.
 * Return
 *      0: succeed, -1: failed.
 */
static s32 gt1x_request_irq(void)
{
	s32 ret = -1;
	const u8 irq_table[] = GTP_IRQ_TAB;

	GTP_DEBUG_FUNC();
	GTP_DEBUG("INT trigger type:%x", gt1x_int_type);

	ret = request_irq(gt1x_i2c_client->irq, gt1x_ts_irq_handler, irq_table[gt1x_int_type], gt1x_i2c_client->name, gt1x_i2c_client);
	if (ret) {
		GTP_ERROR("Request IRQ failed!ERRNO:%d.", ret);
		GTP_GPIO_AS_INPUT(GTP_INT_PORT);
		gpio_free(GTP_INT_PORT);

		return -1;
	} else {
		gt1x_irq_disable();
		return 0;
	}
}

/**
 * gt1x_request_input_dev -  Request input device Function.
 * Return
 *      0: succeed, -1: failed.
 */
static s8 gt1x_request_input_dev(void)
{
	s8 ret = -1;
#if GTP_HAVE_TOUCH_KEY
	u8 index = 0;
#endif

	GTP_DEBUG_FUNC();

	input_dev = input_allocate_device();
	if (input_dev == NULL) {
		GTP_ERROR("Failed to allocate input device.");
		return -ENOMEM;
	}

	input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
#if GTP_ICS_SLOT_REPORT
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 7, 0))
    input_mt_init_slots(input_dev, 16, INPUT_MT_DIRECT);
#else
    input_mt_init_slots(input_dev, 16);
#endif
#else
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
#endif
	set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

#if GTP_HAVE_TOUCH_KEY
	for (index = 0; index < GTP_MAX_KEY_NUM; index++) {
		input_set_capability(input_dev, EV_KEY, gt1x_touch_key_array[index]);
	}
#endif

#if GTP_GESTURE_WAKEUP
	input_set_capability(input_dev, EV_KEY, KEY_GES_REGULAR);
    input_set_capability(input_dev, EV_KEY, KEY_GES_CUSTOM);
#endif

    input_set_capability(input_dev, EV_KEY, KEY_PALM_LOCK);


#if GTP_CHANGE_X2Y
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, gt1x_abs_y_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, gt1x_abs_x_max, 0, 0);
#else
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, gt1x_abs_x_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, gt1x_abs_y_max, 0, 0);
#endif
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TRACKING_ID, 0, 255, 0, 0);

	input_dev->name = gt1x_ts_name;
	input_dev->phys = input_dev_phys;
	input_dev->id.bustype = BUS_I2C;
	input_dev->id.vendor = 0xDEAD;
	input_dev->id.product = 0xBEEF;
	input_dev->id.version = 10427;

	ret = input_register_device(input_dev);
	if (ret) {
		GTP_ERROR("Register %s input device failed", input_dev->name);
		return -ENODEV;
	}

	return 0;
}

/**
 * gt1x_ts_probe -   I2c probe.
 * @client: i2c device struct.
 * @id: device id.
 * Return  0: succeed, -1: failed.
 */
static int gt1x_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	s32 ret = -1;
#if GTP_AUTO_UPDATE
	struct task_struct *thread = NULL;
#endif
	//do NOT remove these logs
	GTP_DEBUG("GTP Driver Version: %s", GTP_DRIVER_VERSION);
	GTP_DEBUG("GTP I2C Address: 0x%02x", client->addr);

	gt1x_i2c_client = client;
	spin_lock_init(&irq_lock);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		GTP_ERROR("I2C check functionality failed.");
		return -ENODEV;
	}

#ifdef GTP_CONFIG_OF	/* device tree support */
	if (client->dev.of_node) {
		gt1x_parse_dt(&client->dev);
	}
#endif


//nubia for synaptics pinctrl
#ifdef NUBIA_GT1_PINCTRL
	ret = gt1_pinctrl_init(&client->dev);
	if (ret < 0) {
		GTP_ERROR("pinctrl init failed.\n");
		return ret;
	}
	ret = gt1_pinctrl_set_state(true);
	if (ret) {
		GTP_ERROR("pinctrl set state failed.\n");
		return ret;
	}
	GTP_DEBUG("pinctrl inited, set state to active successfully.\n");
#endif


	ret = gt1x_request_io_port();
	 if (ret < 0) {
		GTP_ERROR("GTP request IO port failed.");
		return ret;
	}

	gt1x_init();

	INIT_WORK(&gt1x_work, gt1x_ts_work_func);

	// nubia add resume in workqueue
	INIT_WORK(&gt1x_resume_work, gt1x_resume_work_func);

	ret = gt1x_request_input_dev();
	if (ret < 0) {
		GTP_ERROR("GTP request input dev failed");
	}

	ret = gt1x_request_irq();
	if (ret < 0) {
		GTP_DEBUG("GTP works in polling mode.");
	} else {
		GTP_DEBUG("GTP works in interrupt mode.");
	}

#if GTP_GESTURE_WAKEUP
	//enable_irq_wake(client->irq);
#endif

	gt1x_irq_enable();

#if GTP_ESD_PROTECT
	// must before auto update
	gt1x_init_esd_protect();
	gt1x_esd_switch(SWITCH_ON);
#endif

#if GTP_CHARGER_SWITCH
	gt1_power_notifier.notifier_call = gt1_power_notifier_cb;
	ret = power_supply_reg_notifier(&gt1_power_notifier);
	if (ret < 0) {
		GTP_ERROR("Failed to register power notifier client\n");
	}
#endif

#if GTP_AUTO_UPDATE
	thread = kthread_run(gt1x_auto_update_proc, (void *)NULL, "gt1x_auto_update");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		GTP_ERROR("Failed to create auto-update thread: %d.", ret);
	}
#endif
	gt1x_create_sysfs_interfaces(&client->dev);
	gt1x_register_powermanger();
	/*open/short test*/
	gtp_test_sysfs_init();
	return 0;
}

/**
 * gt1x_ts_remove -  Goodix touchscreen driver release function.
 * @client: i2c device struct.
 * Return  0: succeed, -1: failed.
 */
 static int gt1x_ts_remove(struct i2c_client *client)
{
	GTP_DEBUG_FUNC();
	GTP_INFO("GTP driver removing...");
	gtp_test_sysfs_deinit();
	gt1x_unregister_powermanger();
	gt1x_remove_sysfs_interfaces(&client->dev);
#if GTP_GESTURE_WAKEUP
	//disable_irq_wake(client->irq);
#endif
    gt1x_deinit();
	input_unregister_device(input_dev);
    gt1x_remove_gpio_and_power();

    return 0;
}

#if   defined(CONFIG_FB)
/* frame buffer notifier block control the suspend/resume procedure */
static struct notifier_block gt1x_fb_notifier;

static int gtp_fb_notifier_callback(struct notifier_block *noti, unsigned long event, void *data)
{
	struct fb_event *ev_data = data;
	int *blank;

#if GTP_INCELL_PANEL
    #ifndef FB_EARLY_EVENT_BLANK
        #error Need add FB_EARLY_EVENT_BLANK to fbmem.c
    #endif

    if(ev_data && ev_data->data) {
        blank = ev_data->data;
        if(event == FB_EARLY_EVENT_BLANK && *blank == FB_BLANK_UNBLANK) {
            schedule_work(&gt1x_resume_work);
        } else if(event == FB_EVENT_BLANK && *blank == FB_BLANK_POWERDOWN) {
            flush_work(&gt1x_resume_work);
            gt1x_suspend();
        }
    }
#else
    if(ev_data && ev_data->data) {
        blank = ev_data->data;
        if(event == FB_EVENT_BLANK && *blank == FB_BLANK_UNBLANK) {
            GTP_INFO("Resume by fb notifier.");
            schedule_work(&gt1x_resume_work);
        } else if(event == FB_EVENT_BLANK && *blank == FB_BLANK_POWERDOWN) {
            flush_work(&gt1x_resume_work);
            GTP_INFO("Suspend by fb notifier.");
            gt1x_suspend();
        }
    }
#endif

    return 0;
}
#elif defined(CONFIG_PM)
/**
 * gt1x_ts_suspend - i2c suspend callback function.
 * @dev: i2c device.
 * Return  0: succeed, -1: failed.
 */
static int gt1x_pm_suspend(struct device *dev)
{
    return gt1x_suspend();
}

/**
 * gt1x_ts_resume - i2c resume callback function.
 * @dev: i2c device.
 * Return  0: succeed, -1: failed.
 */
static int gt1x_pm_resume(struct device *dev)
{
	return gt1x_resume();
}

/* bus control the suspend/resume procedure */
static const struct dev_pm_ops gt1x_ts_pm_ops = {
	.suspend = gt1x_pm_suspend,
	.resume = gt1x_pm_resume,
};

#elif defined(CONFIG_HAS_EARLYSUSPEND)
/* earlysuspend module the suspend/resume procedure */
static void gt1x_ts_early_suspend(struct early_suspend *h)
{
	gt1x_suspend();
}

static void gt1x_ts_late_resume(struct early_suspend *h)
{
	gt1x_resume();
}

static struct early_suspend gt1x_early_suspend = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = gt1x_ts_early_suspend,
	.resume = gt1x_ts_late_resume,
};
#endif


static int gt1x_register_powermanger(void)
{
#if   defined(CONFIG_FB)
	gt1x_fb_notifier.notifier_call = gtp_fb_notifier_callback;
	fb_register_client(&gt1x_fb_notifier);

#elif defined(CONFIG_HAS_EARLYSUSPEND)
	register_early_suspend(&gt1x_early_suspend);
#endif
	return 0;
}

static int gt1x_unregister_powermanger(void)
{
#if   defined(CONFIG_FB)
	fb_unregister_client(&gt1x_fb_notifier);

#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&gt1x_early_suspend);
#endif
	return 0;
}

#ifdef GTP_CONFIG_OF
static const struct of_device_id gt1x_match_table[] = {
		{.compatible = "goodix,gt1x",},
		{ },
};
#endif

static const struct i2c_device_id gt1x_ts_id[] = {
	{GTP_I2C_NAME, 0},
	{}
};

static int gt1x_dev_suspend(struct i2c_client *client,pm_message_t msg)
{
	GTP_INFO("enter deep sleep.");
	gt1x_dev_halt = true;
	return 0;
}
static int gt1x_dev_resume(struct i2c_client *client)
{
	GTP_INFO("exit deep sleep.");
	gt1x_dev_halt = false;
	return 0;
}
static struct i2c_driver gt1x_ts_driver = {
	.probe = gt1x_ts_probe,
	.remove = gt1x_ts_remove,
	.id_table = gt1x_ts_id,
	.driver = {
		   .name = GTP_I2C_NAME,
		   .owner = THIS_MODULE,
#ifdef GTP_CONFIG_OF
		   .of_match_table = gt1x_match_table,
#endif
#if !defined(CONFIG_FB) && defined(CONFIG_PM)
		   .pm = &gt1x_ts_pm_ops,
#endif
		   },
	.suspend = gt1x_dev_suspend,
	.resume = gt1x_dev_resume,
};

/**
 * gt1x_ts_init - Driver Install function.
 * Return   0---succeed.
 */
static int __init gt1x_ts_init(void)
{
	GTP_DEBUG_FUNC();
	GTP_INFO("GTP driver installing...");
	gt1x_wq = create_singlethread_workqueue("gt1x_wq");
	if (!gt1x_wq) {
		GTP_ERROR("Creat workqueue failed.");
		return -ENOMEM;
	}

	return i2c_add_driver(&gt1x_ts_driver);
}

/**
 * gt1x_ts_exit - Driver uninstall function.
 * Return   0---succeed.
 */
static void __exit gt1x_ts_exit(void)
{
	GTP_DEBUG_FUNC();
	GTP_INFO("GTP driver exited.");
	i2c_del_driver(&gt1x_ts_driver);
	if (gt1x_wq) {
		destroy_workqueue(gt1x_wq);
	}
}

late_initcall(gt1x_ts_init);
module_exit(gt1x_ts_exit);

MODULE_DESCRIPTION("GTP Series Driver");
MODULE_LICENSE("GPL");
