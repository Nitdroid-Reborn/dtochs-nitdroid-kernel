/*
 * drivers/media/video/smiaregs.c
 *
 * Copyright (C) 2008 Nokia Corporation
 *
 * Contact: Sakari Ailus <sakari.ailus@nokia.com>
 *          Tuukka Toivonen <tuukka.o.toivonen@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/sort.h>
#include <media/smiaregs.h>

/*
 *
 * Video control helpers
 *
 */

int smia_ctrl_find(struct v4l2_queryctrl *ctrls, size_t nctrls, int id)
{
	size_t i;

	for (i = 0; i < nctrls; i++)
		if (ctrls[i].id == id)
			break;

	if (i == nctrls)
		i = -EINVAL;

	return i;
}
EXPORT_SYMBOL_GPL(smia_ctrl_find);

int smia_ctrl_find_next(struct v4l2_queryctrl *ctrls, size_t nctrls, int id)
{
	int i;
	u32 best = (u32)-1;

	for (i = 0; i < nctrls; i++)
		if (ctrls[i].id > id
		    && (best == (u32)-1 || ctrls[i].id < ctrls[best].id))
			best = i;

	if (best == (u32)-1)
		return -EINVAL;

	return best;
}
EXPORT_SYMBOL_GPL(smia_ctrl_find_next);

int smia_ctrl_query(struct v4l2_queryctrl *ctrls, size_t nctrls,
		    struct v4l2_queryctrl *a)
{
	int id, i;

	id = a->id;
	if (id & V4L2_CTRL_FLAG_NEXT_CTRL) {
		id &= ~V4L2_CTRL_FLAG_NEXT_CTRL;
		i = smia_ctrl_find_next(ctrls, nctrls, id);
	} else {
		i = smia_ctrl_find(ctrls, nctrls, id);
	}

	if (i < 0)
		return -EINVAL;

	*a = ctrls[i];

	return 0;
}
EXPORT_SYMBOL_GPL(smia_ctrl_query);

int smia_mode_query(const __u32 *ctrls, size_t nctrls, struct v4l2_queryctrl *a)
{
	static const struct {
		__u32 id;
		char *name;
	} ctrl[] = {
		{ .id = V4L2_CID_MODE_FRAME_WIDTH,    .name = "Frame width" },
		{ .id = V4L2_CID_MODE_FRAME_HEIGHT,   .name = "Frame height" },
		{ .id = V4L2_CID_MODE_VISIBLE_WIDTH,  .name = "Visible width" },
		{ .id = V4L2_CID_MODE_VISIBLE_HEIGHT,
		  .name = "Visible height" },
		{ .id = V4L2_CID_MODE_PIXELCLOCK,
		  .name = "Pixel clock [Hz]" },
		{ .id = V4L2_CID_MODE_SENSITIVITY,    .name = "Sensitivity" },
	};
	int id, next = 0, i;

	id = a->id;
	if (id & V4L2_CTRL_FLAG_NEXT_CTRL) {
		id &= ~V4L2_CTRL_FLAG_NEXT_CTRL;
		next = 1;
	}

	for (i = 0; i < ARRAY_SIZE(ctrl); i++) {
		if ((!next && ctrl[i].id == id) ||
		    (next && ctrl[i].id > id)) {
			int j;
			for (j = 0; j < nctrls; j++)
				if (ctrl[i].id == ctrls[j])
					goto found;
		}
	}
	return -EINVAL;

found:
	a->id            = ctrl[i].id;
	strcpy(a->name, ctrl[i].name);
	a->type          = V4L2_CTRL_TYPE_INTEGER;
	a->minimum       = 0;
	a->maximum       = 0;
	a->step          = 0;
	a->default_value = 0;
	a->flags         = V4L2_CTRL_FLAG_READ_ONLY;

	return 0;
}
EXPORT_SYMBOL_GPL(smia_mode_query);

int smia_mode_g_ctrl(const __u32 *ctrls, size_t nctrls, struct v4l2_control *vc,
		     const struct smia_mode *sm)
{
	int i;

	for (i = 0; i < nctrls; i++)
		if (ctrls[i] == vc->id)
			break;
	if (i >= nctrls)
		return -EINVAL;

	switch (vc->id) {
	case V4L2_CID_MODE_FRAME_WIDTH:
		vc->value = sm->width;
		break;
	case V4L2_CID_MODE_FRAME_HEIGHT:
		vc->value = sm->height;
		break;
	case V4L2_CID_MODE_VISIBLE_WIDTH:
		vc->value = sm->window_width;
		break;
	case V4L2_CID_MODE_VISIBLE_HEIGHT:
		vc->value = sm->window_height;
		break;
	case V4L2_CID_MODE_PIXELCLOCK:
		vc->value = sm->pixel_clock;
		break;
	case V4L2_CID_MODE_SENSITIVITY:
		vc->value = sm->sensitivity;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(smia_mode_g_ctrl);

/*
 *
 * Reglist helpers
 *
 */

static int smia_reglist_cmp(const void *a, const void *b)
{
	const struct smia_reglist **list1 = (const struct smia_reglist **)a,
		**list2 = (const struct smia_reglist **)b;

	/* Put real modes in the beginning. */
	if ((*list1)->type == SMIA_REGLIST_MODE &&
	    (*list2)->type != SMIA_REGLIST_MODE)
		return -1;
	else if ((*list1)->type != SMIA_REGLIST_MODE &&
		 (*list2)->type == SMIA_REGLIST_MODE)
		return 1;

	/* Descending width. */
	if ((*list1)->mode.window_width > (*list2)->mode.window_width)
		return -1;
	else if ((*list1)->mode.window_width < (*list2)->mode.window_width)
		return 1;
	else
		return 0;
}

/*
 * Prepare register list created by dcc-pulautin for use in kernel.
 * The pointers in the list are actually offsets from the beginning of
 * the blob.
 */
int smia_reglist_import(struct smia_meta_reglist *meta)
{
	uintptr_t nlists = 0;

	if (meta->magic != SMIA_MAGIC) {
		printk(KERN_ERR "invalid camera sensor firmware (0x%08X)\n",
		       meta->magic);
		return -EILSEQ;
	}

	printk(KERN_ALERT "%s: meta_reglist version %s\n",
	       __func__, meta->version);

	while (meta->reglist[nlists].offset != 0) {
		struct smia_reglist *list;

		meta->reglist[nlists].offset =
			(uintptr_t)meta + meta->reglist[nlists].offset;

		list = meta->reglist[nlists].ptr;

		nlists++;
	}

	if (!nlists)
		return -EINVAL;

	sort(&meta->reglist[0].offset, nlists, sizeof(meta->reglist[0].offset),
	     smia_reglist_cmp, NULL);

	nlists = 0;
	while (meta->reglist[nlists].offset != 0) {
		struct smia_reglist *list;

		list = meta->reglist[nlists].ptr;

		printk(KERN_DEBUG
		       "%s: type %d\tw %d\th %d\tfmt %x\tival %d/%d\tptr %p\n",
		       __func__,
		       list->type,
		       list->mode.window_width, list->mode.window_height,
		       list->mode.pixel_format,
		       list->mode.timeperframe.numerator,
		       list->mode.timeperframe.denominator,
		       (void *)meta->reglist[nlists].offset);

		nlists++;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(smia_reglist_import);

struct smia_reglist *smia_reglist_find_type(struct smia_meta_reglist *meta,
					    u16 type)
{
	struct smia_reglist **next = &meta->reglist[0].ptr;

	while (*next) {
		if ((*next)->type == type)
			return *next;

		next++;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(smia_reglist_find_type);

struct smia_reglist **smia_reglist_first(struct smia_meta_reglist *meta)
{
	return &meta->reglist[0].ptr;
}
EXPORT_SYMBOL_GPL(smia_reglist_first);

struct smia_reglist *smia_reglist_find_mode_fmt(
	struct smia_meta_reglist *meta,
	struct smia_reglist *current_reglist,
	struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct smia_reglist **list = smia_reglist_first(meta);

	for (; *list; list++) {
		struct smia_mode *mode = &(*list)->mode;

		if ((*list)->type != SMIA_REGLIST_MODE)
			continue;

		/* Ignore modes that do cropping on sensor */
		if (mode->sensor_window_width + 8 < mode->sensor_width ||
		    mode->sensor_window_height + 8 < mode->sensor_height)
			continue;

		if (mode->window_width == pix->width &&
		    mode->window_height == pix->height)
			return *list;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(smia_reglist_find_mode_fmt);

#define TIMEPERFRAME_AVG_FPS(t)						\
	(((t).denominator + ((t).numerator >> 1)) / (t).numerator)
struct smia_reglist *smia_reglist_find_mode_streamparm(
	struct smia_meta_reglist *meta,
	struct smia_reglist *current_reglist,
	struct v4l2_streamparm *a)
{
	struct v4l2_fract *timeperframe = &a->parm.capture.timeperframe;
	int fps = TIMEPERFRAME_AVG_FPS(*timeperframe);
	struct smia_reglist **list = smia_reglist_first(meta);
	struct smia_mode *current_mode = &current_reglist->mode;

	for (; *list; list++) {
		struct smia_mode *mode = &(*list)->mode;

		if ((*list)->type != SMIA_REGLIST_MODE)
			continue;

		if (mode->window_width != current_mode->window_width
		    || mode->window_height != current_mode->window_height)
			continue;

		/* Ignore modes that do cropping on sensor */
		if (mode->sensor_window_width + 8 < mode->sensor_width ||
		    mode->sensor_window_height + 8 < mode->sensor_height)
			continue;

		if (TIMEPERFRAME_AVG_FPS(mode->timeperframe) == fps)
			return *list;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(smia_reglist_find_mode_streamparm);

#define MAX_FMTS 4
int smia_reglist_enum_fmt(struct smia_meta_reglist *meta,
			  struct v4l2_fmtdesc *f)
{
	struct smia_reglist **list = smia_reglist_first(meta);
	u32 pixelformat[MAX_FMTS];
	int npixelformat = 0;

	if (f->index >= MAX_FMTS)
		return -EINVAL;

	for (; *list; list++) {
		struct smia_mode *mode = &(*list)->mode;
		int i;

		if ((*list)->type != SMIA_REGLIST_MODE)
			continue;

		for (i = 0; i < npixelformat; i++) {
			if (pixelformat[i] == mode->pixel_format)
				break;
		}
		if (i != npixelformat)
			continue;

		if (f->index == npixelformat) {
			f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			f->pixelformat = mode->pixel_format;

			return 0;
		}

		pixelformat[npixelformat] = mode->pixel_format;
		npixelformat++;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(smia_reglist_enum_fmt);

int smia_reglist_enum_framesizes(struct smia_meta_reglist *meta,
				 struct v4l2_frmsizeenum *frm)
{
	struct smia_reglist **list = smia_reglist_first(meta);
	int frm_index = frm->index;
	int width_low = INT_MAX;

	for (; *list; list++) {
		struct smia_mode *mode = &(*list)->mode;

		if ((*list)->type != SMIA_REGLIST_MODE)
			continue;

		/* Ignore modes that do cropping on sensor */
		if (mode->sensor_window_width + 8 < mode->sensor_width ||
		    mode->sensor_window_height + 8 < mode->sensor_height)
			continue;

		if (frm->pixel_format != mode->pixel_format
		    && frm->pixel_format != -1)
			continue;

		/*
		 * Assume that the modes are in descending width
		 * ordered.
		 */
		if (mode->window_width >= width_low)
			continue;

		width_low = mode->window_width;

		if (frm_index-- == 0) {
			frm->type = V4L2_FRMSIZE_TYPE_DISCRETE;
			frm->discrete.width = mode->window_width;
			frm->discrete.height = mode->window_height;

			return 0;
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(smia_reglist_enum_framesizes);

int smia_reglist_enum_frameintervals(struct smia_meta_reglist *meta,
				     struct v4l2_frmivalenum *frm)
{
	struct smia_reglist **list = smia_reglist_first(meta);
	int frm_index = frm->index;

	/* FIXME: check pixelformat! */

	for (; *list; list++) {
		struct smia_mode *mode = &(*list)->mode;

		if ((*list)->type != SMIA_REGLIST_MODE)
			continue;

		/* Ignore modes that do cropping on sensor */
		if (mode->sensor_window_width + 8 < mode->sensor_width ||
		    mode->sensor_window_height + 8 < mode->sensor_height)
			continue;

		if (frm->pixel_format != mode->pixel_format
		    && frm->pixel_format != -1)
			continue;

		if (frm->width != mode->window_width ||
		    frm->height != mode->window_height)
			continue;

		if (frm_index-- != 0)
			continue;

		frm->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		/* FIXME: try to fix standard... */
		frm->discrete = mode->timeperframe;
		if (frm->reserved[0] == 0xdeafbeef) {
			frm->discrete.numerator = 1;
			frm->discrete.denominator =
				TIMEPERFRAME_AVG_FPS(mode->timeperframe);
		}

		return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(smia_reglist_enum_frameintervals);

/*
 *
 * Register access helpers
 *
 */

/*
 * Read a 8/16/32-bit i2c register.  The value is returned in 'val'.
 * Returns zero if successful, or non-zero otherwise.
 */
int smia_i2c_read_reg(struct i2c_client *client, u16 data_length,
		      u16 reg, u32 *val)
{
	int r;
	struct i2c_msg msg[1];
	unsigned char data[4];

	if (!client->adapter)
		return -ENODEV;
	if (data_length != SMIA_REG_8BIT && data_length != SMIA_REG_16BIT)
		return -EINVAL;

	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = 2;
	msg->buf = data;

	/* high byte goes out first */
	data[0] = (u8) (reg >> 8);;
	data[1] = (u8) (reg & 0xff);
	r = i2c_transfer(client->adapter, msg, 1);
	if (r < 0)
		goto err;

	msg->len = data_length;
	msg->flags = I2C_M_RD;
	r = i2c_transfer(client->adapter, msg, 1);
	if (r < 0)
		goto err;

	*val = 0;
	/* high byte comes first */
	if (data_length == SMIA_REG_8BIT)
		*val = data[0];
	else
		*val = (data[0] << 8) + data[1];

	return 0;

err:
	dev_err(&client->dev, "read from offset 0x%x error %d\n", reg, r);

	return r;
}
EXPORT_SYMBOL_GPL(smia_i2c_read_reg);

static void smia_i2c_create_msg(struct i2c_client *client, u16 len, u16 reg,
				u32 val, struct i2c_msg *msg,
				unsigned char *buf)
{
	msg->addr = client->addr;
	msg->flags = 0; /* Write */
	msg->len = 2 + len;
	msg->buf = buf;

	/* high byte goes out first */
	buf[0] = (u8) (reg >> 8);;
	buf[1] = (u8) (reg & 0xff);

	switch (len) {
	case SMIA_REG_8BIT:
		buf[2] = (u8) (val) & 0xff;
		break;
	case SMIA_REG_16BIT:
		buf[2] = (u8) (val >> 8) & 0xff;
		buf[3] = (u8) (val & 0xff);
		break;
	case SMIA_REG_32BIT:
		buf[2] = (u8) (val >> 24) & 0xff;
		buf[3] = (u8) (val >> 16) & 0xff;
		buf[4] = (u8) (val >> 8) & 0xff;
		buf[5] = (u8) (val & 0xff);
		break;
	default:
		BUG();
	}
}

/*
 * Write to a 8/16-bit register.
 * Returns zero if successful, or non-zero otherwise.
 */
int smia_i2c_write_reg(struct i2c_client *client, u16 data_length, u16 reg,
		       u32 val)
{
	int r;
	struct i2c_msg msg[1];
	unsigned char data[6];

	if (!client->adapter)
		return -ENODEV;
	if (data_length != SMIA_REG_8BIT && data_length != SMIA_REG_16BIT)
		return -EINVAL;

	smia_i2c_create_msg(client, data_length, reg, val, msg, data);

	r = i2c_transfer(client->adapter, msg, 1);
	if (r < 0)
		dev_err(&client->dev,
			"wrote 0x%x to offset 0x%x error %d\n", val, reg, r);

	return 0;
}
EXPORT_SYMBOL_GPL(smia_i2c_write_reg);

/*
 * A buffered write method that puts the wanted register write
 * commands in a message list and passes the list to the i2c framework
 */
static int smia_i2c_buffered_write_regs(struct i2c_client *client,
					const struct smia_reg *wnext, int cnt)
{
	/* FIXME: check how big cnt is */
	struct i2c_msg msg[cnt];
	unsigned char data[cnt][6];
	int wcnt = 0;
	u16 reg, data_length;
	u32 val;

	/* Create new write messages for all writes */
	while (wcnt < cnt) {
		data_length = wnext->type;
		reg = wnext->reg;
		val = wnext->val;
		wnext++;

		smia_i2c_create_msg(client, data_length, reg,
				    val, &msg[wcnt], &data[wcnt][0]);

		/* Update write count */
		wcnt++;
	}

	/* Now we send everything ... */
	return i2c_transfer(client->adapter, msg, wcnt);
}

/*
 * Write a list of registers to i2c device.
 *
 * The list of registers is terminated by SMIA_REG_TERM.
 * Returns zero if successful, or non-zero otherwise.
 */
int smia_i2c_write_regs(struct i2c_client *client,
			const struct smia_reg reglist[])
{
	int r, cnt = 0;
	const struct smia_reg *next, *wnext;

	if (!client->adapter)
		return -ENODEV;

	if (reglist == NULL)
		return -EINVAL;

	/* Initialize list pointers to the start of the list */
	next = wnext = reglist;

	do {
		/*
		 * We have to go through the list to figure out how
		 * many regular writes we have in a row
		 */
		while (next->type != SMIA_REG_TERM
		       && next->type != SMIA_REG_DELAY) {
			/*
			 * Here we check that the actual lenght fields
			 * are valid
			 */
			if (next->type != SMIA_REG_8BIT
			    &&  next->type != SMIA_REG_16BIT) {
				dev_err(&client->dev,
					"Invalid value on entry %d 0x%x\n",
					cnt, next->type);
				return -EINVAL;
			}

			/*
			 * Increment count of successive writes and
			 * read pointer
			 */
			cnt++;
			next++;
		}

		/* Now we start writing ... */
		r = smia_i2c_buffered_write_regs(client, wnext, cnt);

		/* ... and then check that everything was OK */
		if (r < 0) {
			dev_err(&client->dev, "i2c transfer error !!!\n");
			return r;
		}

		/*
		 * If we ran into a sleep statement when going through
		 * the list, this is where we snooze for the required time
		 */
		if (next->type == SMIA_REG_DELAY) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(msecs_to_jiffies(next->val));
			/*
			 * ZZZ ...
			 * Update list pointers and cnt and start over ...
			 */
			next++;
			wnext = next;
			cnt = 0;
		}
	} while (next->type != SMIA_REG_TERM);

	return 0;
}
EXPORT_SYMBOL_GPL(smia_i2c_write_regs);

int smia_i2c_reglist_find_write(struct i2c_client *client,
				struct smia_meta_reglist *meta, u16 type)
{
	struct smia_reglist *reglist;

	reglist = smia_reglist_find_type(meta, type);
	if (IS_ERR(reglist))
		return PTR_ERR(reglist);

	return smia_i2c_write_regs(client, reglist->regs);
}
EXPORT_SYMBOL_GPL(smia_i2c_reglist_find_write);

MODULE_AUTHOR("Sakari Ailus <sakari.ailus@nokia.com>");
MODULE_DESCRIPTION("Generic SMIA configuration and i2c register access");
MODULE_LICENSE("GPL");
