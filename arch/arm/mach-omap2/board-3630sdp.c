/*
 * Copyright (C) 2009 Texas Instruments Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/mtd/nand.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>

#include "common.h"
#include <plat/board.h>
#include <plat/gpmc-smc91x.h>
#include <plat/usb.h>

#include <mach/board-zoom.h>

#include "board-flash.h"
#include "mux.h"
#include "sdram-hynix-h8mbx00u0mer-0em.h"

#if defined(CONFIG_SMC91X) || defined(CONFIG_SMC91X_MODULE)

static struct omap_smc91x_platform_data board_smc91x_data = {
	.cs             = 3,
	.flags          = GPMC_MUX_ADD_DATA | IORESOURCE_IRQ_LOWLEVEL,
};

static void __init board_smc91x_init(void)
{
	board_smc91x_data.gpio_irq = 158;
	gpmc_smc91x_init(&board_smc91x_data);
}

#else

static inline void board_smc91x_init(void)
{
}

#endif 

static void enable_board_wakeup_source(void)
{
	
	omap_mux_init_signal("sys_nirq",
		OMAP_WAKEUP_EN | OMAP_PIN_INPUT_PULLUP);
}

static const struct usbhs_omap_board_data usbhs_bdata __initconst = {

	.port_mode[0] = OMAP_EHCI_PORT_MODE_PHY,
	.port_mode[1] = OMAP_EHCI_PORT_MODE_PHY,
	.port_mode[2] = OMAP_USBHS_PORT_MODE_UNUSED,

	.phy_reset  = true,
	.reset_gpio_port[0]  = 126,
	.reset_gpio_port[1]  = 61,
	.reset_gpio_port[2]  = -EINVAL
};

static struct omap_board_config_kernel sdp_config[] __initdata = {
};

#ifdef CONFIG_OMAP_MUX
static struct omap_board_mux board_mux[] __initdata = {
	{ .reg_offset = OMAP_MUX_TERMINATOR },
};
#endif

static char chip_sel_sdp[][GPMC_CS_NUM] = {
	{PDC_NOR, PDC_NAND, PDC_ONENAND, DBG_MPDB, 0, 0, 0, 0}, 
	{PDC_ONENAND, PDC_NAND, PDC_NOR, DBG_MPDB, 0, 0, 0, 0}, 
	{PDC_NAND, PDC_ONENAND, PDC_NOR, DBG_MPDB, 0, 0, 0, 0}, 
};

static struct mtd_partition sdp_nor_partitions[] = {
	
	{
		.name		= "Bootloader-NOR",
		.offset		= 0,
		.size		= SZ_256K,
		.mask_flags	= MTD_WRITEABLE, 
	},
	
	{
		.name		= "Params-NOR",
		.offset		= MTDPART_OFS_APPEND,
		.size		= SZ_256K,
		.mask_flags	= 0,
	},
	
	{
		.name		= "Kernel-NOR",
		.offset		= MTDPART_OFS_APPEND,
		.size		= SZ_2M,
		.mask_flags	= 0
	},
	
	{
		.name		= "Filesystem-NOR",
		.offset		= MTDPART_OFS_APPEND,
		.size		= MTDPART_SIZ_FULL,
		.mask_flags	= 0
	}
};

static struct mtd_partition sdp_onenand_partitions[] = {
	{
		.name		= "X-Loader-OneNAND",
		.offset		= 0,
		.size		= 4 * (64 * 2048),
		.mask_flags	= MTD_WRITEABLE  
	},
	{
		.name		= "U-Boot-OneNAND",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 2 * (64 * 2048),
		.mask_flags	= MTD_WRITEABLE  
	},
	{
		.name		= "U-Boot Environment-OneNAND",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 1 * (64 * 2048),
	},
	{
		.name		= "Kernel-OneNAND",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 16 * (64 * 2048),
	},
	{
		.name		= "File System-OneNAND",
		.offset		= MTDPART_OFS_APPEND,
		.size		= MTDPART_SIZ_FULL,
	},
};

static struct mtd_partition sdp_nand_partitions[] = {
	
	{
		.name		= "X-Loader-NAND",
		.offset		= 0,
		.size		= 4 * (64 * 2048),
		.mask_flags	= MTD_WRITEABLE,	
	},
	{
		.name		= "U-Boot-NAND",
		.offset		= MTDPART_OFS_APPEND,	
		.size		= 10 * (64 * 2048),
		.mask_flags	= MTD_WRITEABLE,	
	},
	{
		.name		= "Boot Env-NAND",

		.offset		= MTDPART_OFS_APPEND,	
		.size		= 6 * (64 * 2048),
	},
	{
		.name		= "Kernel-NAND",
		.offset		= MTDPART_OFS_APPEND,	
		.size		= 40 * (64 * 2048),
	},
	{
		.name		= "File System - NAND",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,	
	},
};

static struct flash_partitions sdp_flash_partitions[] = {
	{
		.parts = sdp_nor_partitions,
		.nr_parts = ARRAY_SIZE(sdp_nor_partitions),
	},
	{
		.parts = sdp_onenand_partitions,
		.nr_parts = ARRAY_SIZE(sdp_onenand_partitions),
	},
	{
		.parts = sdp_nand_partitions,
		.nr_parts = ARRAY_SIZE(sdp_nand_partitions),
	},
};

static void __init omap_sdp_init(void)
{
	omap3_mux_init(board_mux, OMAP_PACKAGE_CBP);
	omap_board_config = sdp_config;
	omap_board_config_size = ARRAY_SIZE(sdp_config);
	zoom_peripherals_init();
	omap_sdrc_init(h8mbx00u0mer0em_sdrc_params,
				  h8mbx00u0mer0em_sdrc_params);
	zoom_display_init();
	board_smc91x_init();
	board_flash_init(sdp_flash_partitions, chip_sel_sdp, NAND_BUSWIDTH_16);
	enable_board_wakeup_source();
	usbhs_init(&usbhs_bdata);
}

MACHINE_START(OMAP_3630SDP, "OMAP 3630SDP board")
	.atag_offset	= 0x100,
	.reserve	= omap_reserve,
	.map_io		= omap3_map_io,
	.init_early	= omap3630_init_early,
	.init_irq	= omap3_init_irq,
	.handle_irq	= omap3_intc_handle_irq,
	.init_machine	= omap_sdp_init,
	.timer		= &omap3_timer,
	.restart	= omap_prcm_restart,
MACHINE_END
