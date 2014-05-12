/*
 *  Zorro Driver Services
 *
 *  Copyright (C) 2003 Geert Uytterhoeven
 *
 *  Loosely based on drivers/pci/pci-driver.c
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive
 *  for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/zorro.h>



const struct zorro_device_id *
zorro_match_device(const struct zorro_device_id *ids,
		   const struct zorro_dev *z)
{
	while (ids->id) {
		if (ids->id == ZORRO_WILDCARD || ids->id == z->id)
			return ids;
		ids++;
	}
	return NULL;
}
EXPORT_SYMBOL(zorro_match_device);


static int zorro_device_probe(struct device *dev)
{
	int error = 0;
	struct zorro_driver *drv = to_zorro_driver(dev->driver);
	struct zorro_dev *z = to_zorro_dev(dev);

	if (!z->driver && drv->probe) {
		const struct zorro_device_id *id;

		id = zorro_match_device(drv->id_table, z);
		if (id)
			error = drv->probe(z, id);
		if (error >= 0) {
			z->driver = drv;
			error = 0;
		}
	}
	return error;
}


static int zorro_device_remove(struct device *dev)
{
	struct zorro_dev *z = to_zorro_dev(dev);
	struct zorro_driver *drv = to_zorro_driver(dev->driver);

	if (drv) {
		if (drv->remove)
			drv->remove(z);
		z->driver = NULL;
	}
	return 0;
}



int zorro_register_driver(struct zorro_driver *drv)
{
	
	drv->driver.name = drv->name;
	drv->driver.bus = &zorro_bus_type;

	
	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(zorro_register_driver);



void zorro_unregister_driver(struct zorro_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(zorro_unregister_driver);



static int zorro_bus_match(struct device *dev, struct device_driver *drv)
{
	struct zorro_dev *z = to_zorro_dev(dev);
	struct zorro_driver *zorro_drv = to_zorro_driver(drv);
	const struct zorro_device_id *ids = zorro_drv->id_table;

	if (!ids)
		return 0;

	while (ids->id) {
		if (ids->id == ZORRO_WILDCARD || ids->id == z->id)
			return 1;
		ids++;
	}
	return 0;
}

static int zorro_uevent(struct device *dev, struct kobj_uevent_env *env)
{
#ifdef CONFIG_HOTPLUG
	struct zorro_dev *z;

	if (!dev)
		return -ENODEV;

	z = to_zorro_dev(dev);
	if (!z)
		return -ENODEV;

	if (add_uevent_var(env, "ZORRO_ID=%08X", z->id) ||
	    add_uevent_var(env, "ZORRO_SLOT_NAME=%s", dev_name(dev)) ||
	    add_uevent_var(env, "ZORRO_SLOT_ADDR=%04X", z->slotaddr) ||
	    add_uevent_var(env, "MODALIAS=" ZORRO_DEVICE_MODALIAS_FMT, z->id))
		return -ENOMEM;

	return 0;
#else 
	return -ENODEV;
#endif 
}

struct bus_type zorro_bus_type = {
	.name	= "zorro",
	.match	= zorro_bus_match,
	.uevent	= zorro_uevent,
	.probe	= zorro_device_probe,
	.remove	= zorro_device_remove,
};
EXPORT_SYMBOL(zorro_bus_type);


static int __init zorro_driver_init(void)
{
	return bus_register(&zorro_bus_type);
}

postcore_initcall(zorro_driver_init);

