/*
 *  HIDPP protocol for Logitech Unifying receivers
 *
 *  Copyright (c) 2011 Logitech (c)
 *  Copyright (c) 2012-2013 Google (c)
 *  Copyright (c) 2013 Red Hat Inc.
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include "hid-logitech-dj.h"
#include "hid-logitech-hidpp.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_AUTHOR("Nestor Lopez Casado <nlopezcasad@logitech.com>");

#define MAX_INIT_RETRY 5

enum delayed_work_type {
	HIDPP_INIT = 0,
};

static int __hidpp_send_report(struct hid_device *hdev,
				struct hidpp_report *hidpp_report)
{
	struct hid_report* report;
	struct hid_report_enum *output_report_enum;
	int i;

	if (hidpp_report->report_id != REPORT_ID_HIDPP_SHORT &&
	    hidpp_report->report_id != REPORT_ID_HIDPP_LONG)
		return -ENODEV;

	output_report_enum = &hdev->report_enum[HID_OUTPUT_REPORT];
	report = output_report_enum->report_id_hash[hidpp_report->report_id];

	for (i = 0; i < sizeof(struct fap); i++)
		hid_set_field(report->field[0], i+1, hidpp_report->rawbytes[i]);

	hid_hw_request(hdev, report, HID_REQ_SET_REPORT);

	return 0;
}

static int hidpp_send_message_sync(struct hidpp_device *hidpp_dev,
	struct hidpp_report *message,
	struct hidpp_report *response)
{
	int ret;

	mutex_lock(&hidpp_dev->send_mutex);

	hidpp_dev->send_receive_buf = response;
	hidpp_dev->answer_available = false;

	/* So that we can later validate the answer when it arrives
	 * in hidpp_raw_event */
	*response = *message;

	ret = __hidpp_send_report(hidpp_dev->hid_dev, message);

	if (ret) {
          dbg_hid("__hidpp_send_report returned err: %d\n", ret);
		memset(response, 0, sizeof(struct hidpp_report));
		goto exit;
	}

	if (!wait_event_timeout(hidpp_dev->wait, hidpp_dev->answer_available,
				10*HZ)) {
		dbg_hid("%s:timeout waiting for response\n", __func__);
		memset(response, 0, sizeof(struct hidpp_report));
		ret = -1;
	}

	if (response->report_id == REPORT_ID_HIDPP_SHORT &&
	    response->fap.feature_index == HIDPP_ERROR) {
		ret = response->fap.params[0];
		dbg_hid("__hidpp_send_report got hidpp error %d\n", ret);
		goto exit;
	}

exit:
	mutex_unlock(&hidpp_dev->send_mutex);
	return ret;

}

int hidpp_send_fap_command_sync(struct hidpp_device *hidpp_dev,
	u8 feat_index, u8 funcindex_clientid, u8 *params, int param_count,
	struct hidpp_report *response)
{
	struct hidpp_report message;

	if (param_count > sizeof(message.rap.params))
		return -EINVAL;

	memset(&message, 0, sizeof(message));
	message.report_id = REPORT_ID_HIDPP_LONG;
	message.fap.feature_index = feat_index;
	message.fap.funcindex_clientid = funcindex_clientid;
	memcpy(&message.fap.params, params, param_count);

	return hidpp_send_message_sync(hidpp_dev, &message, response);
}
EXPORT_SYMBOL_GPL(hidpp_send_fap_command_sync);

int hidpp_send_rap_command_sync(struct hidpp_device *hidpp_dev,
	u8 report_id, u8 sub_id, u8 reg_address, u8 *params,
	int param_count,
	struct hidpp_report *response)
{
	struct hidpp_report message;

	if ((report_id != REPORT_ID_HIDPP_SHORT) &&
	    (report_id != REPORT_ID_HIDPP_LONG))
		return -EINVAL;

	if (param_count > sizeof(message.rap.params))
		return -EINVAL;

	memset(&message, 0, sizeof(message));
	message.report_id = report_id;
	message.rap.sub_id = sub_id;
	message.rap.reg_address = reg_address;
	memcpy(&message.rap.params, params, param_count);

	return hidpp_send_message_sync(hidpp_dev, &message, response);
}
EXPORT_SYMBOL_GPL(hidpp_send_rap_command_sync);


static void schedule_delayed_hidpp_init(struct hidpp_device *hidpp_dev)
{
	enum delayed_work_type work_type = HIDPP_INIT;

	if (hidpp_dev->initialized)
		return;

	kfifo_in(&hidpp_dev->delayed_work_fifo, &work_type,
				sizeof(enum delayed_work_type));

	if (schedule_work(&hidpp_dev->work) == 0) {
		dbg_hid("%s: did not schedule the work item,"
			" was already queued\n",
			__func__);
	}
}

static void hidpp_delayed_init(struct hidpp_device *hidpp_dev)
{
	int ret = 0;
	u8 major, minor;

	pr_err("%s  %s:%d\n", __func__, __FILE__, __LINE__);

	if (hidpp_dev->initialized)
		return;

//	/* ping to see if the device is powered-up */
//	if (hidpp_root_get_protocol_version(hidpp_dev, &major, &minor))
//		return;

	pr_err("%s  %s:%d\n", __func__, __FILE__, __LINE__);

	if (down_trylock(&hidpp_dev->hid_dev->driver_lock)) {
		if (hidpp_dev->init_retry < MAX_INIT_RETRY) {
			dbg_hid("%s: we need to reschedule the work item."
				"Semaphore still held on device\n", __func__);
			schedule_delayed_hidpp_init(hidpp_dev);
			hidpp_dev->init_retry++;
		} else {
			dbg_hid("%s: giving up initialization now.", __func__);
			hidpp_dev->init_retry = 0;
		}
		return;
	}
	up(&hidpp_dev->hid_dev->driver_lock);

	if (hidpp_dev->device_init)
		ret = hidpp_dev->device_init(hidpp_dev);

	if (!ret)
		hidpp_dev->initialized = true;
}

static void delayed_work_cb(struct work_struct *work)
{
	struct hidpp_device *hidpp_device =
		container_of(work, struct hidpp_device, work);
	unsigned long flags;
	int count;
	enum delayed_work_type work_type;

	dbg_hid("%s\n", __func__);

	spin_lock_irqsave(&hidpp_device->lock, flags);

	count = kfifo_out(&hidpp_device->delayed_work_fifo, &work_type,
				sizeof(enum delayed_work_type));

	if (count != sizeof(enum delayed_work_type)) {
		dev_err(&hidpp_device->hid_dev->dev, "%s: workitem triggered without "
			"notifications available\n", __func__);
		spin_unlock_irqrestore(&hidpp_device->lock, flags);
		return;
	}

	if (!kfifo_is_empty(&hidpp_device->delayed_work_fifo)) {
		if (schedule_work(&hidpp_device->work) == 0) {
			dbg_hid("%s: did not schedule the work item, was "
				"already queued\n", __func__);
		}
	}

	spin_unlock_irqrestore(&hidpp_device->lock, flags);

	switch (work_type) {
	case HIDPP_INIT:
		hidpp_delayed_init(hidpp_device);
		break;
	default:
		dbg_hid("%s: unexpected report type\n", __func__);
	}
}

int hidpp_init(struct hidpp_device *hidpp_dev, struct hid_device *hid_dev)
{
	if (hidpp_dev->initialized)
		return 0;

	hidpp_dev->init_retry = 0;
	hidpp_dev->hid_dev = hid_dev;
	hidpp_dev->initialized = 0;

	INIT_WORK(&hidpp_dev->work, delayed_work_cb);
	mutex_init(&hidpp_dev->send_mutex);
	init_waitqueue_head(&hidpp_dev->wait);

	spin_lock_init(&hidpp_dev->lock);
	if (kfifo_alloc(&hidpp_dev->delayed_work_fifo,
			4 * sizeof(struct hidpp_report),
			GFP_KERNEL)) {
		dev_err(&hidpp_dev->hid_dev->dev,
			"%s:failed allocating delayed_work_fifo\n", __func__);
		mutex_destroy(&hidpp_dev->send_mutex);
		return -ENOMEM;
	}

//	schedule_delayed_hidpp_init(hidpp_dev);

	return 0;
}
EXPORT_SYMBOL_GPL(hidpp_init);

void hidpp_remove(struct hidpp_device *hidpp_dev)
{
	dbg_hid("%s\n", __func__);
	cancel_work_sync(&hidpp_dev->work);
	mutex_destroy(&hidpp_dev->send_mutex);
	kfifo_free(&hidpp_dev->delayed_work_fifo);
	hidpp_dev->initialized = false;
	hidpp_dev->hid_dev = NULL;
}
EXPORT_SYMBOL_GPL(hidpp_remove);

static int hidpp_raw_dj_event(struct hidpp_device *hidpp_dev,
		struct dj_report *report)
{
	__u8 connection_status;

	if ((report->report_id == REPORT_ID_DJ_SHORT) &&
	    (report->report_type == REPORT_TYPE_NOTIF_CONNECTION_STATUS)) {
		connection_status = report->report_params[CONNECTION_STATUS_PARAM_STATUS];
		if (connection_status != STATUS_LINKLOSS)
			schedule_delayed_hidpp_init(hidpp_dev);
	}
	return 0;
}

static int hidpp_raw_hidpp_event(struct hidpp_device *hidpp_dev, u8 *data,
		int size)
{
	struct hidpp_report *question = hidpp_dev->send_receive_buf;
	struct hidpp_report *answer = hidpp_dev->send_receive_buf;
	struct hidpp_report *report = (struct hidpp_report *)data;

	/* If the mutex is locked then we have a pending answer from a
	 * previoulsly sent command
	 */
	if (unlikely(mutex_is_locked(&hidpp_dev->send_mutex))) {
		/* Check for a correct hidpp20 answer */
		bool correct_answer =
			report->fap.feature_index == question->fap.feature_index &&
		report->fap.funcindex_clientid == question->fap.funcindex_clientid;
		dbg_hid("%s mutex is locked, waiting for reply\n", __func__);

		/* Check for a "correct" hidpp10 error message, this means the
		 * device is hidpp10 and does not support the command sent */
		correct_answer = correct_answer ||
			(report->fap.feature_index == HIDPP_ERROR &&
		report->fap.funcindex_clientid == question->fap.feature_index &&
		report->fap.params[0] == question->fap.funcindex_clientid);

		if (correct_answer) {
			*answer = *report;
			hidpp_dev->answer_available = true;
			wake_up(&hidpp_dev->wait);
			/* This was an answer to a command that this driver sent
			 * we return 1 to hid-core to avoid forwarding the command
			 * upstream as it has been treated by the driver */

			return 1;
		}
	}

	if (hidpp_dev->raw_event != NULL)
		return hidpp_dev->raw_event(hidpp_dev, (u8*)report, size);

	return 0;
}

int hidpp_raw_event(struct hid_device *hdev, struct hid_report *hid_report,
			u8 *data, int size)
{
	struct hidpp_device *hidpp_dev = hid_get_drvdata(hdev);
	int len = (size >> 3) + 1;

	switch (data[0]) {
	case REPORT_ID_DJ_LONG:
	case REPORT_ID_DJ_SHORT:
		return hidpp_raw_dj_event(hidpp_dev, (struct dj_report *)data);
	case REPORT_ID_HIDPP_LONG:
		if (len != HIDPP_REPORT_LONG_LENGTH) {
			hid_err(hdev, "received hid++ report of bad size (%d)",
				size);
			return 1;
		}
		return hidpp_raw_hidpp_event(hidpp_dev, data, size);
	case REPORT_ID_HIDPP_SHORT:
		if (len != HIDPP_REPORT_SHORT_LENGTH) {
			hid_err(hdev, "received hid++ report of bad size (%d)",
				size);
			return 1;
		}
		return hidpp_raw_hidpp_event(hidpp_dev, data, size);
	}

	if (hidpp_dev->raw_event != NULL)
		return hidpp_dev->raw_event(hidpp_dev, data, size);

	return 0;
}
EXPORT_SYMBOL_GPL(hidpp_raw_event);

/* -------------------------------------------------------------------------- */
/* 0x0000: Root                                                               */
/* -------------------------------------------------------------------------- */

int hidpp_root_get_feature(struct hidpp_device *hidpp, u16 feature,
	u8 *feature_index, u8 *feature_type)
{
	struct hidpp_report response;
	int ret;
	u8 params[2] = { feature >> 8, feature & 0x00FF };

	ret = hidpp_send_fap_command_sync(hidpp,
			HIDPP_PAGE_ROOT,
			CMD_ROOT_GET_FEATURE,
			params, 2, &response);
	if (ret)
		return -ret;

	*feature_index = response.fap.params[0];
	*feature_type = response.fap.params[1];

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_root_get_feature);

int hidpp_root_get_protocol_version(struct hidpp_device *hidpp,
	u8 *protocol_major, u8 *protocol_minor)
{
	struct hidpp_report response;
	int ret;

	ret = hidpp_send_fap_command_sync(hidpp,
			HIDPP_PAGE_ROOT,
			CMD_ROOT_GET_PROTOCOL_VERSION,
			NULL, 0, &response);

	if (ret == 1) {
		*protocol_major = 1;
		*protocol_minor = 0;
		return 0;
	}

	if (ret)
		return -ret;

	*protocol_major = response.fap.params[0];
	*protocol_minor = response.fap.params[1];

	return ret;
}

///* -------------------------------------------------------------------------- */
///* 0x0001: IFeatureSet                                                        */
///* -------------------------------------------------------------------------- */
//
//int hidpp_featureset_get_count(struct hidpp_device *hidpp,
//	u8 *feature_count)
//{
//	struct hidpp_report response;
//	int ret;
//	ret = hidpp_send_report_with_feature(hidpp,
//		HIDPP_PAGE_IFEATURE_SET,
//		CMD_FEATURESET_GET_COUNT, NULL, 0, &response);
//
//	if (ret)
//		return -ret;
//
//	*feature_count = response.report_params[0];
//
//	return ret;
//}
//EXPORT_SYMBOL_GPL(hidpp_featureset_get_count);
//
//int hidpp_featureset_get_feature(struct hidpp_device *hidpp, u8 index,
//	u16 *feature)
//{
//	struct hidpp_report response;
//	int ret;
//	ret = hidpp_send_report_with_feature(hidpp,
//		HIDPP_PAGE_IFEATURE_SET,
//		CMD_FEATURESET_GET_FEATURE, &index, 1, &response);
//
//	if (ret)
//		return -ret;
//
//	*feature = response.report_params[0] << 8 | response.report_params[1];
//
//	hidpp->feature_map[*feature] = 0x0100 | index;
//	hidpp->feature_map_indexes[index] = *feature;
//
//	return ret;
//}
//EXPORT_SYMBOL_GPL(hidpp_featureset_get_feature);
//
/* -------------------------------------------------------------------------- */
/* 0x0005: GetDeviceNameType                                                  */
/* -------------------------------------------------------------------------- */

#define CMD_GET_DEVICE_NAME_TYPE_GET_DEVICE_NAME	0x11
#define CMD_GET_DEVICE_NAME_TYPE_GET_TYPE		0x21

int hidpp_get_device_name_type_get_count(struct hidpp_device *hidpp_dev,
	u8 feature_index, u8 *nameLength)
{
	struct hidpp_report response;
	int ret;
	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_GET_DEVICE_NAME_TYPE_GET_COUNT, NULL, 0, &response);

	if (ret)
		return -ret;

	*nameLength = response.fap.params[0];

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_get_device_name_type_get_count);

int hidpp_get_device_name_type_get_device_name(struct hidpp_device *hidpp_dev,
	u8 feature_index, u8 char_index, char *device_name, int len_buf)
{
	struct hidpp_report response;
	int ret, i;
	int count;
	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_GET_DEVICE_NAME_TYPE_GET_DEVICE_NAME, &char_index, 1,
		&response);

	if (ret)
		return -ret;

	if (response.report_id == REPORT_ID_HIDPP_LONG)
		count = HIDPP_REPORT_LONG_LENGTH - 4;
	else
		count = HIDPP_REPORT_SHORT_LENGTH - 4;

	if (len_buf < count)
		count = len_buf;

	for (i = 0; i < count; i++)
		device_name[i] = response.fap.params[i];

	return count;
}
EXPORT_SYMBOL_GPL(hidpp_get_device_name_type_get_device_name);

int hidpp_get_device_name_type_get_type(struct hidpp_device *hidpp_dev,
	u8 feature_index, u8 *device_type)
{
	struct hidpp_report response;
	int ret;
	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_GET_DEVICE_NAME_TYPE_GET_TYPE, NULL, 0, &response);

	if (ret)
		return -ret;

	*device_type = response.fap.params[0];

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_get_device_name_type_get_type);

char *hidpp_get_device_name(struct hidpp_device *hidpp_dev, u8 *name_length)
{
	u8 feature_type;
	u8 feature_index;
	u8 __name_length;
	char *name;
	unsigned index = 0;

	hidpp_root_get_feature(hidpp_dev, HIDPP_PAGE_GET_DEVICE_NAME_TYPE,
		&feature_index, &feature_type);

	hidpp_get_device_name_type_get_count(hidpp_dev, feature_index, &__name_length);
	name = kzalloc(__name_length + 1, GFP_KERNEL);
	if (!name)
		return NULL;

	*name_length = __name_length + 1;
	while (index < __name_length)
		index += hidpp_get_device_name_type_get_device_name(hidpp_dev,
			feature_index, index, name + index, __name_length - index);

	return name;
}
EXPORT_SYMBOL_GPL(hidpp_get_device_name);

/* -------------------------------------------------------------------------- */
/* 0x6100: TouchPadRawXY                                                      */
/* -------------------------------------------------------------------------- */

int hidpp_touchpad_get_raw_info(struct hidpp_device *hidpp_dev,
	u8 feature_index, struct hidpp_touchpad_raw_info *raw_info)
{
	struct hidpp_report response;
	int ret;
	u8 *params = (u8 *)response.fap.params;

	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_TOUCHPAD_GET_RAW_INFO, NULL, 0, &response);

	if (ret)
		return -ret;

	raw_info->x_size = (params[0] << 8) | params[1];
	raw_info->y_size = (params[2] << 8) | params[3];
	raw_info->z_range = params[4];
	raw_info->area_range = params[5];
	raw_info->maxcontacts = params[7];
	raw_info->origin = params[8];
	raw_info->res = (params[13] << 8) | params[14];

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_touchpad_get_raw_info);

//int hidpp_touchpad_get_raw_report_state(struct hidpp_device *hidpp,
//	bool *send_raw_reports, bool *force_vs_area,
//	bool *sensor_enhanced_settings)
//{
//	struct __hidpp_report response;
//	int ret;
//	ret = hidpp_send_report_with_feature(hidpp,
//		HIDPP_PAGE_TOUCHPAD_RAW_XY,
//		CMD_TOUCHPAD_GET_RAW_REPORT_STATE, NULL, 0, &response);
//
//	if (ret)
//		return -ret;
//
///** TODO */
//
//	return ret;
//}
//EXPORT_SYMBOL_GPL(hidpp_touchpad_get_raw_report_state);

int hidpp_touchpad_set_raw_report_state(struct hidpp_device *hidpp_dev,
		u8 feature_index, bool send_raw_reports, bool force_vs_area,
		bool sensor_enhanced_settings)
{
	struct hidpp_report response;
	int ret;

	/*
	 * Params:
	 *   0x01 - enable raw
	 *   0x02 - 16bit Z, no area
	 *   0x04 - enhanced sensitivity
	 *   0x08 - width, height instead of area
	 *   0x10 - send raw + gestures (degrades smoothness)
	 *   remaining bits - reserved
	 */
	u8 params = send_raw_reports | force_vs_area << 1 |
				sensor_enhanced_settings << 2;

	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_TOUCHPAD_SET_RAW_REPORT_STATE, &params, 1, &response);

	if (ret)
		return -ret;

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_touchpad_set_raw_report_state);

struct touch_hidpp_report {
	u8 x_m;
	u8 x_l;
	u8 y_m;
	u8 y_l;
	u8 z;
	u8 area;
	u8 id;
};

struct dual_touch_hidpp_report {
	u8 report_id;
	u8 device_index;
	u8 feature_index;
	u8 broadcast_event;
	u16 timestamp;
	struct touch_hidpp_report touches[2];
};

static void hidpp_touchpad_touch_event(struct touch_hidpp_report *touch_report,
	struct hidpp_touchpad_raw_xy_finger *finger)
{
	u8 x_m = touch_report->x_m << 2;
	u8 y_m = touch_report->y_m << 2;

	finger->contact_type = touch_report->x_m >> 6;
	finger->x = x_m << 6 | touch_report->x_l;

	finger->contact_status = touch_report->y_m >> 6;
	finger->y = y_m << 6 | touch_report->y_l;

	finger->finger_id = touch_report->id >> 4;
	finger->z = touch_report->z;
	finger->area = touch_report->area;

}

void hidpp_touchpad_raw_xy_event(struct hidpp_device *hidpp_device,
		struct hidpp_report *hidpp_report,
		struct hidpp_touchpad_raw_xy *raw_xy)
{
	struct dual_touch_hidpp_report *dual_touch_report;

	dual_touch_report = (struct dual_touch_hidpp_report *)hidpp_report;
	raw_xy->end_of_frame = dual_touch_report->touches[0].id & 0x01;
	raw_xy->spurious_flag = (dual_touch_report->touches[0].id >> 1) & 0x01;
	raw_xy->finger_count = dual_touch_report->touches[1].id & 0x0f;

	if (raw_xy->finger_count) {
		hidpp_touchpad_touch_event(&dual_touch_report->touches[0],
				&raw_xy->fingers[0]);
		if ((raw_xy->end_of_frame && raw_xy->finger_count == 4) ||
			(!raw_xy->end_of_frame && raw_xy->finger_count >= 2))
			hidpp_touchpad_touch_event(
					&dual_touch_report->touches[1],
					&raw_xy->fingers[1]);
	}
}
EXPORT_SYMBOL_GPL(hidpp_touchpad_raw_xy_event);