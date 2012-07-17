#include <linux/module.h> 	/* Pour la création du module*/
#include <linux/slab.h>  	/* Pour l’allocation mémoire kmalloc()/kfree()*/
#include <linux/ioport.h>
#include <linux/vmalloc.h>
#include <linux/init.h> 	/* Pour l’utilisation du module*/
#include <linux/list.h>
#include <linux/delay.h>

#include <linux/types.h>	/* the dev_t type */
#include <linux/string.h>
#include <linux/device.h> 	/* class_create */
#include <linux/fs.h> 		/* Pour la structure file_operations*/
#include <linux/cdev.h> 	/* Pour la structure cdev*/
#include <linux/kernel.h>  	/* Pour l’utilisation de printk()*/
#include <linux/gpio.h>		/* Pour la gestion des GPIOs */
#include <linux/hrtimer.h>  /* Pour la gestion du timer */
#include <asm/uaccess.h>

/*   
 *                    VCC
 *                    ---          LCD HD44780
 *                     |      +--------------------
 *       +-------------+      |
 *       |             |      |
 *      +-+     +------c------|(1)  VSS
 *  10k | |     |      +------|(2)  VDD
 *  Ohm | |<----u-------------|(3)  Contraste
 *      +-+     |   GPIO_17 --|(4)  R/S
 *       |      +-------------|(5)  R/W
 *       |      |   GPIO_18 --|(6)  EN
 *       |      +-------------|(7)  D0
 *       |      +-------------|(8)  D1
 *       |      +-------------|(9)  D2
 *       |      +-------------|(10) D3
 *       |      |   GPIO_21 --|(11) D4
 *       |      |   GPIO_22 --|(12) D5
 *       +------+   GPIO_23 --|(13) D6
 *              |   GPIO_24 --|(14) D7
 *              |       +-----|(15) A+
 *              +-------c-----|(16) A-
 *              |       |     |
 *              |      +-+    |
 *              |      | |100 +----------------------
 *            -----    | |Ohm
 *           /////     +-+
 *                      |
 *                     ---
 *                     VCC
 */ 

/*RS: Register select */
#define LCD_RS  		17
#define LCD_EN  		18
#define LCD_D4  		21
#define LCD_D5  		22
#define LCD_D6  		23
#define LCD_D7  		24

#define DEVICE_NAME 	"rpilcd"
#define CLASS_NAME 		"raspberry_pi"

/* numero majeur */
static int i32_rpilcd_major = 0;
/* numero mineur */
static int i32_rpilcd_minor = 0;

/* to store major and minor numbers */
static dev_t  gst_dev;
static struct hrtimer htimer;

struct class * gpst_rpilcd_class = (struct class *)NULL;
struct device * gpst_rpilcd_device = (struct device *)NULL;

static enum hrtimer_restart timer_oscillateur(struct hrtimer *);

static int periode_us = 1000;
module_param(periode_us, int, 0644);
static ktime_t kt_periode;

int rpilcd_open(struct inode *inode, struct file *filp);
int rpilcd_release(struct inode *inode, struct file *filp);
ssize_t rpilcd_read(struct file *filp, char __user *buff, size_t count, loff_t *offp);
ssize_t rpilcd_write(struct file *filp, const char __user *buff, size_t count, loff_t *offp);

/* operations permitted by this module */
static struct file_operations rpilcd_fops = {
	.owner		= THIS_MODULE,
	.read		= NULL,
	.write		= NULL,
	.open		= NULL,
	.release	= NULL,
};

/* representation of my device (pas obligatoire => on aurait pu faire 
   static struct cdev rpilcd_cdev;) */
static struct rpilcd_dev_t {
	struct cdev st_cdev;		/* char device structure */
};

/*
 * three defined values for the flags (asm-generic/gpio.h) :
 * GPIOF_IN 			=> GPIO defined as input
 * GPIOF_OUT_INIT_LOW 	=> GPIO defined as output, initial level LOW
 * GPIOF_OUT_INIT_HIGH	=> GPIO defined as output, initial level HIGH
 */
static struct gpio rpilcd_gpios[] = {
	{ LCD_RS, GPIOF_OUT_INIT_LOW, "LCD_RS" },
	{ LCD_EN, GPIOF_OUT_INIT_LOW, "LCD_EN" },
	{ LCD_D4, GPIOF_OUT_INIT_LOW, "LCD_D4" },
	{ LCD_D5, GPIOF_OUT_INIT_LOW, "LCD_D5" },
	{ LCD_D6, GPIOF_OUT_INIT_LOW, "LCD_D6" },
	{ LCD_D7, GPIOF_OUT_INIT_LOW, "LCD_D7" },
};

struct rpilcd_dev_t * pst_rpilcd = (struct rpilcd_dev_t *)NULL;

/*
 * write a byte to lcd HD44780 controller
 */ 
void rpilcd_write_byte(const unsigned char /* in */ ui8_byte) {
	gpio_set_value(LCD_D4, (ui8_byte >> 4) & 0x01);
	gpio_set_value(LCD_D5, (ui8_byte >> 5) & 0x01);
	gpio_set_value(LCD_D6, (ui8_byte >> 6) & 0x01);
	gpio_set_value(LCD_D7, (ui8_byte >> 7) & 0x01);

	gpio_set_value(LCD_EN, 1);
	udelay(1);
	gpio_set_value(LCD_EN, 0);
	if(gpio_get_value(LCD_RS) == 1) {
		udelay(200);
	}
	else {
		usleep_range(4500, 5500);
	}
	gpio_set_value(LCD_D4, ui8_byte & 0x01);
	gpio_set_value(LCD_D5, (ui8_byte >> 1) & 0x01);
	gpio_set_value(LCD_D6, (ui8_byte >> 2) & 0x01);
	gpio_set_value(LCD_D7, (ui8_byte >> 3) & 0x01);

	gpio_set_value(LCD_EN, 1);
	udelay(1);
	gpio_set_value(LCD_EN, 0);
	if(gpio_get_value(LCD_RS) == 1) {
		udelay(200);
	}
	else {
		usleep_range(4500, 5500);
	}
}

/*
 * 
 */
void rpilcd_set_cursor(const unsigned char /* in */ ui8_row,
                       const unsigned char /* in */ ui8_column) {
	uint8_t ui8_command = 0x80;
	gpio_set_value(LCD_RS, 0);
	switch(ui8_row) {
		case 1:
			ui8_command += ui8_column - 1;
			break;

		case 2:
			ui8_command += 0x40 + (ui8_column - 1);
			break;

		default:
				break;
	}

	rpilcd_write_byte(ui8_command);
}


/*
 * Write a string of chars to the LCD 
 */
int rpilcd_put_string(const char * /* in */ sz_string) {
	if(sz_string != (char *)NULL) {
		gpio_set_value(LCD_RS, 1);     // write characters
		while(*sz_string != '\0') {
			rpilcd_write_byte(*sz_string++);
		}
		gpio_set_value(LCD_RS, 0);
		return 0;
	}
	else {
		return -1;
	}
}

/* 
 * Write one character to the LCD 
 */
void rpilcd_put_char(const char /* in */ i8_char) {
	gpio_set_value(LCD_RS, 1);     // write character
	rpilcd_write_byte(i8_char);
	gpio_set_value(LCD_RS, 0);
}

/*
 * clear HD44780 lcd controller
 */
void rpilcd_clear_display(void) {
	gpio_set_value(LCD_RS, 0);
	rpilcd_write_byte(0x01);
}

/*
 * initialise HD44780 lcd controller
 * voir http://www.mjmwired.net/kernel/Documentation/timers/timers-howto.txt
 */
int rpilcd_init_display(void) {
	int i32_ret = 0;
	/* Wait for more than 15 ms after VCC rises to 4.5 V */
	usleep_range(15000, 16000);
	
	/*
	 *  RS R/W DB7 DB6 DB5 DB4
	 * 0   0   0   0   1   1
	 */
	gpio_set_value(LCD_D4, 1);
	gpio_set_value(LCD_D5, 1);
	gpio_set_value(LCD_D6, 0);
	gpio_set_value(LCD_D7, 0);
	
	/* Il faut toujours envoyer une impulsion positive d'au moins 450ns, 
	 * après la mise à l'état haut des broches D5 et D4, sur la broche EN.
	 */
	gpio_set_value(LCD_EN, 1);
	udelay(1);
	gpio_set_value(LCD_EN, 0);
	
	/* Wait for more than 4.1 ms */
	usleep_range(4200, 5000);
	
	gpio_set_value(LCD_EN, 1);
	udelay(1);
	gpio_set_value(LCD_EN, 0);
	
	/* Wait for more than 100 μs */
	udelay(200);
	gpio_set_value(LCD_EN, 1);
	udelay(1);
	gpio_set_value(LCD_EN, 0);
	udelay(200);
	
	// RS R/W DB7 DB6 DB5 DB4 
	// 0   0   0   0   1   0  => interface four bits mode 
	gpio_set_value(LCD_D4, 0);
	gpio_set_value(LCD_D5, 1);
	gpio_set_value(LCD_D6, 0);
	gpio_set_value(LCD_D7, 0);

	gpio_set_value(LCD_EN, 1);
	udelay(1);
	gpio_set_value(LCD_EN, 0);

	usleep_range(4200, 5000);
	
	/* => Set interface length
	 * RS R/W DB7 DB6 DB5 DB4 
	 * 0   0   0   0   1   0  
	 * 0   0   N   F   *   * 
	 */
	rpilcd_write_byte(0x28);
	
	/* => Display On, Cursor On, Cursor Blink Off
	 * RS R/W DB7 DB6 DB5 DB4 
	 * 0   0   0   0   0   0  
	 * 0   0   1   0   0   0
	 */
	rpilcd_write_byte(0x08);

	/* => Clear screen
	 * RS R/W DB7 DB6 DB5 DB4 
	 * 0   0   0   0   0   0  
	 * 0   0   0   0   0   1
	 */
	rpilcd_write_byte(0x01);

	/* => Set entry Mode
	 * RS R/W DB7 DB6 DB5 DB4 
	 * 0   0   0   0   0   0  
	 * 0   0   0   1   D   S
	 */
	rpilcd_write_byte(0x06);
	
	/* RS R/W DB7 DB6 DB5 DB4 
	 * 0   0   0   0   0   0  
	 * 0   0   1   D   C   B
	 */ 
	rpilcd_write_byte(0x0C);

	return i32_ret;
}

/*
 * same function as pow(x,p)
 */
int rpilcd_pow(const int i32_value, const int i32_power) {
	int i32_idx = 0;
	int i32_ret = 1;
	if(i32_value > 0) {
		i32_ret = i32_power;
	}
	/* else nothing to do */

	for(i32_idx = 1; i32_idx < i32_value; i32_idx ++) {
		i32_ret *= i32_power;
	}
	return i32_ret;
}

/*
 * convert number from ascii buf to integer value
 */
int rpilcd_atoi(const char * const buf, const size_t count, int * const pi32_value) {
	const char * pst_buf = &buf[0];
	int i32_idx = 0;
	*pi32_value = 0;
	for(i32_idx = (int)count - 1; i32_idx >= 0; i32_idx --) {
		if((pst_buf[i32_idx] >= 0x30) && (pst_buf[i32_idx] <= 0x39)) {
			*pi32_value += (pst_buf[i32_idx] - 0x30) * (rpilcd_pow(((int)(count - 1) - i32_idx), 10));
		} else { /* error byte is not a number */
			return -1;
		}
	}
	return 0;
}

/*
 * Open method
 */
int rpilcd_open(struct inode *inode, struct file *filp) {
	int i32_ret = 0;
	struct rpilcd_dev_t * pst_rpilcd = (struct rpilcd_dev_t *)NULL;
	printk(KERN_INFO "[RPILCD] rpilcd_open\n");
	/* récupération de l’adresse d’une structure à partir d’un de ses membres */
	pst_rpilcd = container_of(inode->i_cdev, struct rpilcd_dev_t, st_cdev);
	/* store pointer to it in the private_data field of the file structure 
     * for easier access in the future */
	filp->private_data = pst_rpilcd;
	return 0;
}

/*
 * Release method
 */
int rpilcd_release(struct inode *inode, struct file *filp) {
	printk(KERN_INFO "[RPILCD] rpilcd_release\n");
	return 0;
}

/*
 * Read method
 */
ssize_t rpilcd_read(struct file *filp, char __user *buff, size_t count, loff_t *offp) {
	/* retreive pointer from private data */
	struct rpilcd_dev_t * pst_rpilcd = filp->private_data;
	char sz_value[5];
	int i32_ret = -1;
	printk(KERN_INFO "[RPILCD] rpilcd_read\n");
	
	//~ i32_ret = gpio_get_value(PIN_LED);
	//~ pst_rpilcd->ui8_value = (unsigned char)i32_ret;
	//~ snprintf(sz_value, sizeof(sz_value), "%d\n", pst_rpilcd->ui8_value);
	//~ i32_ret = sizeof(sz_value) + 1;
	//~ 
	//~ if(i32_ret > count) {
		//~ return -ENOMEM;
	//~ }
	//~ /* else nothing to do */

	//~ if(copy_to_user(buff, sz_value, i32_ret)) {
		//~ return -EFAULT;
	//~ }
	//~ /* else nothing to do */
	
	return i32_ret;
}

/*
 * 
 */ 
//~ static enum hrtimer_restart timer_oscillateur(struct hrtimer * unused) {
	//~ hrtimer_forward_now(& htimer, kt_periode);
	//~ return HRTIMER_RESTART;
//~ }

/*
 * Initialize the driver. 
 */
static int __init init_rpilcd(void) {
	int i32_ret = -1;

	/* set a ktime_t variable from a seconds/nanoseconds value */
	kt_periode = ktime_set(0, 1000 * periode_us);

	/* allocate a private structure and reference it as driver’s data */
	pst_rpilcd = (struct rpilcd_dev_t *)kmalloc(sizeof(struct rpilcd_dev_t), GFP_KERNEL);
	if(pst_rpilcd == (struct rpilcd_dev_t *)NULL) {
		printk(KERN_WARNING "[RPILCD] error mem struct\n");
		goto failed_kmalloc;
	}
	/* else nothing to do */
	/* connait-on le numero majeur de notre module ? */
	if(i32_rpilcd_major) {
		gst_dev = MKDEV(i32_rpilcd_major, i32_rpilcd_minor);
		/* static allocation */
		i32_ret = register_chrdev_region(gst_dev, 1, DEVICE_NAME);
		if(i32_ret < 0) {
			printk(KERN_WARNING "[RPILCD] error static allocation\n");
			goto failed_register;
		}
	} else { /* non on le connait pas */
		/* The disadvantage of dynamic assignment is 
           that you can’t create the device nodes in advance, 
           because the major number assigned to your module will vary */

		/* dynamic allocation */
		i32_ret = alloc_chrdev_region(&gst_dev, i32_rpilcd_minor, 1, DEVICE_NAME);
		if(i32_ret < 0) {
			printk(KERN_WARNING "[RPILCD] error dynamic allocation\n");
			goto failed_register;
		}
		/* extract major number */
		i32_rpilcd_major = MAJOR(gst_dev);
	}

	/* populate sysfs entry -> create /sys/class/DEVICE_NAME */
	gpst_rpilcd_class = class_create(THIS_MODULE, DEVICE_NAME);
	if(gpst_rpilcd_class == (struct class *)NULL) {
		printk(KERN_WARNING "[RPILCD] can't create class\n");
		goto failed_class;
	}
	/* else nothing to do */

	/* initialise and connect the file operations with the cdev struct */
	cdev_init(&pst_rpilcd->st_cdev, &rpilcd_fops);
	pst_rpilcd->st_cdev.owner = THIS_MODULE;

	/* add the device to the kernel */
	i32_ret = cdev_add(&pst_rpilcd->st_cdev, i32_rpilcd_minor, 1);
	if(i32_ret < 0) {
		printk(KERN_WARNING "[RPILCD] error add device to the kernel\n");
		goto failed_cdev;
	}
	/* else nothing to do */
	
	/* create a device and register it with sysfs -> automaticaly create /dev/rpilcd node */
	gpst_rpilcd_device = device_create(gpst_rpilcd_class, NULL, gst_dev, NULL, DEVICE_NAME);
	if(gpst_rpilcd_device == (struct device *)NULL) {
		printk(KERN_WARNING "[RPILCD] error create device\n");
		goto failed_device;
	}
	/* else nothing to do */
 
	/* request multiple GPIOs */
	i32_ret = gpio_request_array(rpilcd_gpios, ARRAY_SIZE(rpilcd_gpios));
	if(i32_ret != 0) {
		printk(KERN_WARNING, "[RPILCD] Error request multiple gpios\n");
		goto failed_request;
	}
	/* else nothing to do */
	
	/*
	 * High Resolution Timer 
	 */
	
	/* initialize a timer to the given clock  */
	//~ hrtimer_init(&htimer, CLOCK_REALTIME, HRTIMER_MODE_REL);
	//~ htimer.function = timer_oscillateur;
	
	/* start an hrtimer on the current CPU 
	 * kt_periode => expiry time
	 * expiry mode: absolute (HRTIMER_ABS) or relative (HRTIMER_REL) */
	//~ hrtimer_start(&htimer, kt_periode, HRTIMER_MODE_REL);
	return 0;

failed_request:
	device_destroy(gpst_rpilcd_class, gst_dev);
failed_device:
	cdev_del(&pst_rpilcd->st_cdev);
failed_cdev:
	class_destroy(gpst_rpilcd_class);
failed_class:
	unregister_chrdev_region(gst_dev, 1);
failed_register:
	kfree(pst_rpilcd);
failed_kmalloc:
	return -1;
}

/*
 * Cleanup and unregister the driver. 
 */
static void __exit cleanup_rpilcd(void) {
	//~ hrtimer_cancel(& htimer);
	/* release multiple GPIOs */
	gpio_free_array(rpilcd_gpios, ARRAY_SIZE(rpilcd_gpios));
	device_destroy(gpst_rpilcd_class, gst_dev);
	cdev_del(&pst_rpilcd->st_cdev);
	class_destroy(gpst_rpilcd_class);
	kfree(pst_rpilcd);
	/* unregistred driver from the kernel */
	unregister_chrdev_region(gst_dev, 1);
}

module_init(init_rpilcd);
module_exit(cleanup_rpilcd);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sinseman44 <sinseman44@gmail.com>");
MODULE_DESCRIPTION("LCD HD44780 driver for raspberry PI");
