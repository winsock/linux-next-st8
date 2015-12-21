/*
 * arch/arm/mach-tegra/board-paz00.c
 *  
 * Copyright (C) 2015 <andrew.querol02@gmail.com>
 *
 * Based on board-paz00.c
 * Copyright (C) 2011 Marc Dietrich <marvin24@gmx.de>
 *
 * Based on board-harmony.c
 * Copyright (C) 2010 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/gpio/machine.h>
#include <linux/platform_device.h>
#include <linux/rfkill-gpio.h>
#include <linux/platform_data/tegra_usb.h>
#include <linux/usb/tegra_usb_phy.h>
#include <linux/dma-mapping.h>

#include "board.h"

#define INT_GIC_BASE            0
#define INT_PRI_BASE            (INT_GIC_BASE + 32)
#define INT_SEC_BASE			(INT_PRI_BASE + 32)
#define INT_TRI_BASE			(INT_SEC_BASE + 32)
#define INT_QUAD_BASE			(INT_TRI_BASE + 32)

#define INT_USB                 (INT_PRI_BASE + 20)
#define INT_USB2                (INT_PRI_BASE + 21)
#define INT_USB3                (INT_QUAD_BASE + 1)

#define INT_USB3_DEV_SMI		(INT_SEC_BASE + 24)
#define INT_USB3_DEV_PME		(INT_SEC_BASE + 25)

#define INT_USB3_HOST_PME		(INT_SEC_BASE + 11)
#define INT_USB3_DEV_HOST		(INT_SEC_BASE + 12)

#define TEGRA_USB_BASE			0x7D000000
#define TEGRA_USB_SIZE			SZ_16K

#define TEGRA_USB2_BASE			0x7D004000
#define TEGRA_USB2_SIZE			SZ_16K

#define TEGRA_USB3_BASE			0x7D008000
#define TEGRA_USB3_SIZE			SZ_16K

static struct resource tegra_usb1_resources[] = {
    [0] = {
        .start	= TEGRA_USB_BASE,
        .end	= TEGRA_USB_BASE + TEGRA_USB_SIZE - 1,
        .flags	= IORESOURCE_MEM,
    },
    [1] = {
        .start	= INT_USB,
        .end	= INT_USB,
        .flags	= IORESOURCE_IRQ,
    },
};

static struct resource tegra_usb2_resources[] = {
    [0] = {
        .start	= TEGRA_USB2_BASE,
        .end	= TEGRA_USB2_BASE + TEGRA_USB2_SIZE - 1,
        .flags	= IORESOURCE_MEM,
    },
    [1] = {
        .start	= INT_USB2,
        .end	= INT_USB2,
        .flags	= IORESOURCE_IRQ,
    },
};

static struct resource tegra_usb3_resources[] = {
    [0] = {
        .start	= TEGRA_USB3_BASE,
        .end	= TEGRA_USB3_BASE + TEGRA_USB3_SIZE - 1,
        .flags	= IORESOURCE_MEM,
    },
    [1] = {
        .start	= INT_USB3,
        .end	= INT_USB3,
        .flags	= IORESOURCE_IRQ,
    },
};

static u64 tegra_ehci_dmamask = DMA_BIT_MASK(64);

struct platform_device tegra_ehci1_device = {
    .name	= "tegra-ehci",
    .id	= 0,
    .dev	= {
        .dma_mask	= &tegra_ehci_dmamask,
        .coherent_dma_mask = DMA_BIT_MASK(64),
    },
    .resource = tegra_usb1_resources,
    .num_resources = ARRAY_SIZE(tegra_usb1_resources),
};

struct platform_device tegra_ehci2_device = {
    .name	= "tegra-ehci",
    .id	= 1,
    .dev	= {
        .dma_mask	= &tegra_ehci_dmamask,
        .coherent_dma_mask = DMA_BIT_MASK(64),
    },
    .resource = tegra_usb2_resources,
    .num_resources = ARRAY_SIZE(tegra_usb2_resources),
};

struct platform_device tegra_ehci3_device = {
    .name	= "tegra-ehci",
    .id	= 2,
    .dev	= {
        .dma_mask	= &tegra_ehci_dmamask,
        .coherent_dma_mask = DMA_BIT_MASK(64),
    },
    .resource = tegra_usb3_resources,
    .num_resources = ARRAY_SIZE(tegra_usb3_resources),
};

static struct tegra_usb_platform_data tegra_udc_pdata = {
    .port_otg = true,
    .has_hostpc = true,
    .unaligned_dma_buf_supported = false,
    .phy_intf = TEGRA_USB_PHY_INTF_UTMI,
    .op_mode = TEGRA_USB_OPMODE_DEVICE,
    .u_data.dev = {
        .vbus_pmu_irq = 0,
        .vbus_gpio = -1,
        .charging_supported = true,
        .remote_wakeup_supported = false,
    },
    .u_cfg.utmi = {
        .hssync_start_delay = 0,
        .elastic_limit = 16,
        .idle_wait_delay = 17,
        .term_range_adj = 6,
        .xcvr_setup = 8,
        .xcvr_lsfslew = 2,
        .xcvr_lsrslew = 2,
        .xcvr_setup_offset = 0,
        .xcvr_use_fuses = 1,
    },
};

static struct tegra_usb_platform_data tegra_ehci1_utmi_pdata = {
    .port_otg = true,
    .has_hostpc = true,
    .unaligned_dma_buf_supported = true,
    .phy_intf = TEGRA_USB_PHY_INTF_UTMI,
    .op_mode = TEGRA_USB_OPMODE_HOST,
    .u_data.host = {
        .vbus_gpio = -1,
        .hot_plug = false,
        .remote_wakeup_supported = true,
        .power_off_on_suspend = true,
        .skip_resume = false,
    },
    .u_cfg.utmi = {
        .hssync_start_delay = 0,
        .elastic_limit = 16,
        .idle_wait_delay = 17,
        .term_range_adj = 6,
        .xcvr_setup = 15,
        .xcvr_lsfslew = 0,
        .xcvr_lsrslew = 3,
        .xcvr_setup_offset = 0,
        .xcvr_use_fuses = 1,
        .vbus_oc_map = 0x4,
        .xcvr_hsslew_lsb = 2,
    },
};

static struct tegra_usb_platform_data tegra_ehci2_utmi_pdata = {
    .port_otg = false,
    .has_hostpc = true,
    .unaligned_dma_buf_supported = false,
    .phy_intf = TEGRA_USB_PHY_INTF_UTMI,
    .op_mode = TEGRA_USB_OPMODE_HOST,
    .u_data.host = {
        .vbus_gpio = -1,
        .hot_plug = false,
        .remote_wakeup_supported = true,
        .power_off_on_suspend = true,
        .skip_resume = true,
    },
    .u_cfg.utmi = {
        .hssync_start_delay = 0,
        .elastic_limit = 16,
        .idle_wait_delay = 17,
        .term_range_adj = 6,
        .xcvr_setup = 8,
        .xcvr_lsfslew = 2,
        .xcvr_lsrslew = 2,
        .xcvr_setup_offset = 0,
        .xcvr_use_fuses = 1,
        .vbus_oc_map = 0x5,
    },
};

static struct tegra_usb_platform_data tegra_ehci3_utmi_pdata = {
    .port_otg = false,
    .has_hostpc = true,
    .unaligned_dma_buf_supported = false,
    .phy_intf = TEGRA_USB_PHY_INTF_UTMI,
    .op_mode = TEGRA_USB_OPMODE_HOST,
    .u_data.host = {
        .vbus_gpio = -1,
        .hot_plug = false,
        .remote_wakeup_supported = true,
        .power_off_on_suspend = true,
        .skip_resume = true,
    },
    .u_cfg.utmi = {
        .hssync_start_delay = 0,
        .elastic_limit = 16,
        .idle_wait_delay = 17,
        .term_range_adj = 6,
        .xcvr_setup = 8,
        .xcvr_lsfslew = 2,
        .xcvr_lsrslew = 2,
        .xcvr_setup_offset = 0,
        .xcvr_use_fuses = 1,
        .vbus_oc_map = 0x5,
    },
};


static struct tegra_usb_otg_data tegra_otg_pdata = {
    .ehci_device = &tegra_ehci1_device,
    .ehci_pdata = &tegra_ehci1_utmi_pdata,
};

static struct resource tegra_otg_resources[] = {
    [0] = {
        .start	= TEGRA_USB_BASE,
        .end	= TEGRA_USB_BASE + TEGRA_USB_SIZE - 1,
        .flags	= IORESOURCE_MEM,
    },
    [1] = {
        .start	= INT_USB,
        .end	= INT_USB,
        .flags	= IORESOURCE_IRQ,
    },
};

struct platform_device tegra_otg_device = {
    .name		= "tegra-otg",
    .id		= -1,
    .resource	= tegra_otg_resources,
    .num_resources	= ARRAY_SIZE(tegra_otg_resources),
};

static struct resource tegra_udc_resources[] = {
    [0] = {
        .start	= TEGRA_USB_BASE,
        .end	= TEGRA_USB_BASE + TEGRA_USB_SIZE - 1,
        .flags	= IORESOURCE_MEM,
    },
    [1] = {
        .start	= INT_USB,
        .end	= INT_USB,
        .flags	= IORESOURCE_IRQ,
    },
};

static u64 tegra_udc_dmamask = DMA_BIT_MASK(64);

struct platform_device tegra_udc_device = {
    .name	= "tegra-udc",
    .id	= 0,
    .dev	= {
        .dma_mask	= &tegra_udc_dmamask,
        .coherent_dma_mask = DMA_BIT_MASK(64),
    },
    .resource = tegra_udc_resources,
    .num_resources = ARRAY_SIZE(tegra_udc_resources),
};

static void st8_usb_init(void)
{
    tegra_ehci1_utmi_pdata.u_data.host.turn_off_vbus_on_lp0 = true;
    
    /* Ardbeg and TN8 */
    pr_info("%s: Shield Tablet USB initializing!\n", __func__);
    
    /* Need these settings for HS USB EMI on T124 */
    tegra_udc_pdata.u_cfg.utmi.xcvr_hsslew_lsb = 0x3;
    tegra_udc_pdata.u_cfg.utmi.xcvr_hsslew_msb = 0xf;
    tegra_ehci1_utmi_pdata.u_cfg.utmi.xcvr_hsslew_lsb = 0x3;
    tegra_ehci1_utmi_pdata.u_cfg.utmi.xcvr_hsslew_msb = 0xf;
    tegra_ehci2_utmi_pdata.u_cfg.utmi.xcvr_hsslew_lsb = 0x3;
    tegra_ehci2_utmi_pdata.u_cfg.utmi.xcvr_hsslew_msb = 0xf;
    tegra_ehci3_utmi_pdata.u_cfg.utmi.xcvr_hsslew_lsb = 0x3;
    tegra_ehci3_utmi_pdata.u_cfg.utmi.xcvr_hsslew_msb = 0xf;
    
    /*
     * TN8 supports vbus changing and it can handle
     * vbus voltages larger then 5V.  Enable this.
     
     * Set the maximum voltage that can be supplied
     * over USB vbus that the board supports if we use
     * a quick charge 2 wall charger.
     */
    tegra_udc_pdata.qc2_voltage = TEGRA_USB_QC2_12V;
    tegra_udc_pdata.u_data.dev.qc2_current_limit_ma = 1300;
    
    /* charger needs to be set to 3A - h/w will do 2A  */
    tegra_udc_pdata.u_data.dev.dcp_current_limit_ma = 3000;
    /* Device cable is detected through PMU Interrupt */
    tegra_udc_pdata.support_pmu_vbus = true;
    tegra_udc_pdata.vbus_extcon_dev_name = "palmas-extcon";
    tegra_ehci1_utmi_pdata.support_pmu_vbus = true;
    tegra_ehci1_utmi_pdata.vbus_extcon_dev_name =
    "palmas-extcon";
    /* Host cable is detected through PMU Interrupt */
    tegra_udc_pdata.id_det_type = TEGRA_USB_PMU_ID;
    tegra_ehci1_utmi_pdata.id_det_type = TEGRA_USB_PMU_ID;
    tegra_ehci1_utmi_pdata.id_extcon_dev_name =
    "palmas-extcon";
    
    /* Enable Y-Cable support */
    tegra_ehci1_utmi_pdata.u_data.host.support_y_cable = true;
    tegra_otg_device.dev.platform_data = &tegra_otg_pdata;
    platform_device_register(&tegra_otg_device);
    /* Setup the udc platform data */
    tegra_udc_device.dev.platform_data = &tegra_udc_pdata;
}

// TODO: Finally found an accessble UART!
///* Pinmux changes to support UART over uSD adapter E2542 */
//static __initdata struct tegra_pingroup_config ardbeg_sdmmc3_uart_pinmux[] = {
//    DEFAULT_PINMUX(SDMMC3_CMD,    UARTA,      NORMAL,   NORMAL,   INPUT),
//    DEFAULT_PINMUX(SDMMC3_DAT1,   UARTA,      NORMAL,   NORMAL,   OUTPUT),
//};
//
//int __init st8_pinmux_init(void)
//{
//    if (is_uart_over_sd_enabled()) {
//        tegra_pinmux_config_table(ardbeg_sdmmc3_uart_pinmux,
//                                  ARRAY_SIZE(ardbeg_sdmmc3_uart_pinmux));
//        /* On ST8, UART-A is the physical device for
//         * UART over uSD card
//         */
//        set_sd_uart_port_id(0);
//    }
//    return 0;
//}

void __init tegra_st8_late_init(void) {
    pr_info("%s: Shield Tablet initializing!\n", __func__);
    st8_usb_init();
}
