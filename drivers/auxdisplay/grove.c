#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/string.h>

#include <uapi/linux/grove_ioctl.h>

#define RED 0x04
#define GREEN 0x03
#define BLUE 0x02

#define LCD_CMD 0x80
#define TXT_CMD 0x40

#define LINE_SIZE 16

struct i2c_cmd_t {
	uint8_t cmd;
	uint8_t val;
};

struct color_t {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

struct string_t {
	uint8_t position;
	char data[(LINE_SIZE * 2) + 2];
};

struct grove_t {
	dev_t devnum;
	struct cdev cdev;
	struct i2c_client *rgb_client;
	struct i2c_client *lcd_client;
	struct color_t color;
	char line_one[LINE_SIZE + 1];
	char line_two[LINE_SIZE + 1];
};

static struct class *grove_class;
static DEFINE_MUTEX(grove_mutex);

static long grove_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct grove_t *grove;
	struct device *dev;
	struct color_t *color;
	struct string_t *string;
	char tmp[LINE_SIZE + 3];
	int ret = 0;
	int i = 0;
	int size = 0;

	grove = file->private_data;
	dev = &grove->lcd_client->dev;
	color = kzalloc(sizeof(struct color_t), GFP_KERNEL);
	if (IS_ERR(color))
		return -ENOMEM;

	string = kzalloc(sizeof(struct string_t), GFP_KERNEL);
	if (IS_ERR(string))
		return -ENOMEM;

	dev_info(dev, "%s", __func__);

	switch (cmd) {
	case GROVE_SET_COLOR:
		dev_info(dev, " set color\n");
		mutex_lock(&grove_mutex);
		if (copy_from_user(color, (const void *)arg,
				   sizeof(struct color_t))) {
			dev_err(dev, "copy from user failed\n");
			mutex_unlock(&grove_mutex);
			break;
		}

		struct i2c_cmd_t cmds[] = {
			{ RED, color->red },
			{ GREEN, color->green },
			{ BLUE, color->blue },
		};
		for (i = 0; i < (int)ARRAY_SIZE(cmds); i++) {
			ret = i2c_smbus_write_byte_data(
				grove->rgb_client, cmds[i].cmd, cmds[i].val);
			if (ret) {
				dev_err(dev, "set new color failed\n");
				mutex_unlock(&grove_mutex);
				break;
			}
		}
		grove->color = *color;
		dev_info(dev, "new color: r: 0x%x, g: 0x%x, b: 0x%x\n",
			 color->red, color->green, color->blue);
		break;

	case GROVE_GET_COLOR:
		dev_info(dev, " get color\n");
		mutex_lock(&grove_mutex);
		if (copy_to_user((void *)arg, (const void *)&grove->color,
				 sizeof(struct color_t))) {
			mutex_unlock(&grove_mutex);
		}
		break;

	case GROVE_CLEAR_LCD:
		dev_info(dev, " clear lcd\n");
		mutex_lock(&grove_mutex);
		ret = i2c_smbus_write_byte_data(grove->lcd_client, LCD_CMD,
						0x01);
		if (ret) {
			dev_err(dev, "clear display failed\n");
			mutex_unlock(&grove_mutex);
			break;
		}
		memset(grove->line_one, '\n', LINE_SIZE + 1);
		memset(grove->line_two, '\n', LINE_SIZE + 1);
		break;

	case GROVE_WRITE_FIRST_LINE:
		dev_info(dev, " write first line\n");
		mutex_lock(&grove_mutex);
		if (copy_from_user(string, (const void *)arg,
				   sizeof(struct string_t))) {
			mutex_unlock(&grove_mutex);
			break;
		}
		ret = i2c_smbus_write_byte_data(grove->lcd_client, LCD_CMD,
						(string->position | 0x80));
		if (ret) {
			dev_err(dev, "set position failed\n");
			mutex_unlock(&grove_mutex);
			break;
		}
		size = snprintf(tmp, LINE_SIZE + 3, "@%s", string->data);
		dev_info(dev, "input %s %s\n", string->data, tmp);
		ret = i2c_master_send(grove->lcd_client, tmp,
				      size < 17 ? (size - 2) : 17);
		if (ret < 0) {
			dev_err(dev, "write first line failed\n");
			mutex_unlock(&grove_mutex);
			break;
		}
		snprintf(grove->line_one, sizeof(grove->line_one), "%s\n",
			 string->data);
		break;

	case GROVE_WRITE_SECOND_LINE:
		dev_info(dev, " write second line\n");
		mutex_lock(&grove_mutex);
		if (copy_from_user(string, (const void *)arg,
				   sizeof(struct string_t))) {
			dev_err(dev, "copy from user failed\n");
			mutex_unlock(&grove_mutex);
			break;
		}
		ret = i2c_smbus_write_byte_data(grove->lcd_client, LCD_CMD,
						(string->position | 0xc0));
		if (ret) {
			dev_err(dev, "set position failed\n");
			mutex_unlock(&grove_mutex);
			break;
		}
		size = snprintf(tmp, LINE_SIZE + 3, "@%s", string->data);
		ret = i2c_master_send(grove->lcd_client, tmp,
				      size < 17 ? (size - 2) : 17);
		if (ret < 0) {
			dev_err(dev, "write first line failed\n");
			mutex_unlock(&grove_mutex);
			break;
		}
		snprintf(grove->line_two, sizeof(grove->line_two), "%s\n",
			 string->data);
		break;

	case GROVE_READ_LCD:
		dev_info(dev, " read lcd\n");
		mutex_lock(&grove_mutex);
		snprintf(string->data, sizeof(string->data), "%s%s",
			 grove->line_one, grove->line_two);
		if (copy_to_user((void *)arg, (const void *)string,
				 sizeof(struct string_t))) {
			mutex_unlock(&grove_mutex);
			break;
		}
		break;

	case GROVE_GET_LINE_SIZE:
		dev_info(dev, " get line size\n");
		mutex_lock(&grove_mutex);
		if (put_user(LINE_SIZE, (unsigned long *)arg))
			mutex_unlock(&grove_mutex);

		break;

	default:
		return -EINVAL;
	} // end switch

	mutex_unlock(&grove_mutex);
	return 0;
}

static int grove_open(struct inode *inode, struct file *file)
{
	struct grove_t *grove;

	grove = container_of(inode->i_cdev, struct grove_t, cdev);
	file->private_data = grove;

	return 0;
}

static int grove_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int grove_init_lcd(struct grove_t *grove)
{
	int i = 0;
	int ret = 0;

	struct i2c_cmd_t cmds[] = {
		{ LCD_CMD, 0x01 },
		{ LCD_CMD, 0x02 },
		{ LCD_CMD, 0x0c },
		{ LCD_CMD, 0x28 },
	};

	dev_info(&grove->lcd_client->dev, "%s\n", __func__);

	mutex_lock(&grove_mutex);
	for (i = 0; i < (int)ARRAY_SIZE(cmds); i++) {
		ret = i2c_smbus_write_byte_data(grove->lcd_client, cmds[i].cmd,
						cmds[i].val);
		if (ret) {
			dev_err(&grove->lcd_client->dev,
				"failed to initialize the LCD\n");
			goto fail;
		}
	}

	/* Sometimes, the display has some issues with block writes like this.
     * But this operation is less string parsing than sending single chars
     * and for this, much nicer. Also, it is more comprehensible to Zircon,
     * where exactly such operations are working, even with a higher clock
     * cycle. For this, the Linux driver uses also block writes, without the
     * need for additional string parsing and artifical delays, and if the
     * result on the Grove LCD is not ok, we take a look on the Oscilloscope.
     * Or use a Raspi...
     */
	char init[] = "@Init";

	ret = i2c_master_send(grove->lcd_client, init, sizeof(init) - 1);
	if (ret < sizeof(init - 1)) {
		dev_err(&grove->lcd_client->dev,
			"failed to initialize the LCD\n");
		goto fail;
	}
	ret = 0;

fail:
	mutex_unlock(&grove_mutex);
	return ret;
}

static int grove_init_rgb(struct grove_t *grove)
{
	int i = 0;
	int ret = 0;

	uint8_t green = 0xff;

	struct i2c_cmd_t cmds[] = {
		{ 0x00, 0x00 }, { 0x01, 0x00 },   { 0x08, 0xaa },
		{ RED, 0 },     { GREEN, green }, { BLUE, 0 },
	};

	dev_info(&grove->rgb_client->dev, "%s\n", __func__);

	mutex_lock(&grove_mutex);
	for (i = 0; i < (int)ARRAY_SIZE(cmds); i++) {
		ret = i2c_smbus_write_byte_data(grove->rgb_client, cmds[i].cmd,
						cmds[i].val);
		if (ret) {
			dev_err(&grove->rgb_client->dev,
				"failed to initialize the RGB backlight\n");
			goto fail;
		}
	}
	grove->color.green = green;

fail:
	mutex_unlock(&grove_mutex);
	return ret;
}

static const struct file_operations grove_fops = {
	.owner = THIS_MODULE,
	.open = grove_open,
	.release = grove_release,
	.unlocked_ioctl = grove_ioctl,
};

static int grove_probe(struct i2c_client *client)
{
	int ret = 0;
	struct grove_t *grove;
	struct device *dev = &client->dev;
	struct device *device;

	grove = kzalloc(sizeof(struct grove_t), GFP_KERNEL);
	if (IS_ERR(grove)) {
		dev_err(dev,
			"failed to allocate a private memory area for the device\n");
		return -ENOMEM;
	}

	grove_class = class_create(THIS_MODULE, "grove");
	if (IS_ERR(grove_class)) {
		dev_err(dev, "failed to create sysfs class\n");
		return -ENOMEM;
	}

	if (alloc_chrdev_region(&grove->devnum, 0, 1, "grove") < 0) {
		dev_err(dev, "failed to allocate char dev region\n");
		goto free_class;
	}

	cdev_init(&grove->cdev, &grove_fops);
	grove->cdev.owner = THIS_MODULE;

	if (cdev_add(&grove->cdev, grove->devnum, 1))
		goto free_cdev;

	device = device_create(grove_class, NULL, grove->devnum, "%s", "grove");
	if (IS_ERR(device)) {
		dev_err(dev, "failed to create dev entry\n");
		goto free_cdev;
	}

	grove->lcd_client = client;
	i2c_set_clientdata(client, grove);
	ret = grove_init_lcd(grove);
	if (ret) {
		dev_err(dev, "failed to init LCD, free resources\n");
		goto free_device;
	}

	/* The Grove-LCD RGB backlight v4.0 is a composed device made from two
     * individual I2C controllers.
     * Each controller has two addresses, but only one each controller are of
     * interest. To control such a device with only one driver, we use the
     * new (v4.8) API "i2c_new_secondary_device".
     */
	grove->rgb_client =
		i2c_new_secondary_device(grove->lcd_client, "grovergb", 0x62);
	if (grove->rgb_client == NULL) {
		dev_info(dev, "can not fetch secondary I2C device\n");
		goto free_device;
	}
	i2c_set_clientdata(grove->rgb_client, grove);
	ret = grove_init_rgb(grove);
	if (ret) {
		dev_err(dev, "failed to init RGB, free resources\n");
		goto free_device;
	}

	dev_info(dev, "%s finished\n", __func__);

	return 0;

free_device:
	device_destroy(grove_class, grove->devnum);
free_cdev:
	cdev_del(&grove->cdev);
	unregister_chrdev_region(grove->devnum, 1);
free_class:
	class_destroy(grove_class);
	kfree(grove);
	return -EIO;
}

static int grove_remove(struct i2c_client *client)
{
	int i = 0;
	int ret = 0;
	struct grove_t *grove = i2c_get_clientdata(client);
	struct i2c_client *lcd_client = grove->lcd_client;
	struct i2c_client *rgb_client = grove->rgb_client;

	struct i2c_cmd_t rgb_cmds[] = {
		{ RED, 0x00 },
		{ GREEN, 0x00 },
		{ BLUE, 0x00 },
	};

	dev_info(&client->dev, "%s\n", __func__);

	mutex_lock(&grove_mutex);

	for (i = 0; i < (int)ARRAY_SIZE(rgb_cmds); i++) {
		ret = i2c_smbus_write_byte_data(rgb_client, rgb_cmds[i].cmd,
						rgb_cmds[i].val);
		if (ret) {
			dev_err(&client->dev,
				"failed to deinitialize the RGB backlight\n");
			return ret;
		}
	}

	ret = i2c_smbus_write_byte_data(lcd_client, LCD_CMD, 0x01);

	grove->color.red = 0x00;
	grove->color.green = 0x00;
	grove->color.blue = 0x00;

	i2c_unregister_device(rgb_client);
	device_destroy(grove_class, grove->devnum);
	cdev_del(&grove->cdev);
	unregister_chrdev_region(grove->devnum, 1);
	class_destroy(grove_class);
	kfree(grove);
	mutex_unlock(&grove_mutex);

	return 0;
}

static const struct of_device_id grove_of_idtable[] = { { .compatible =
								  "grove,lcd" },
							{} };
MODULE_DEVICE_TABLE(of, grove_of_idtable);

static struct i2c_driver grove_driver = {
	.driver = { .name = "grove", .of_match_table = grove_of_idtable },
	.probe_new = grove_probe,
	.remove = grove_remove,
};

module_i2c_driver(grove_driver);

MODULE_AUTHOR("Anna-Lena Marx <anna-lena.marx@inovex.de>");
MODULE_DESCRIPTION("Grove-LCD RGB backlight v4.0 Driver");
MODULE_LICENSE("GPL");
