/*
 * OMAP4XXX L3 Interconnect error handling driver
 *
 * Copyright (C) 2011 Texas Corporation
 *	Santosh Shilimkar <santosh.shilimkar@ti.com>
 *	Sricharan <r.sricharan@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "omap_l3_noc.h"

#define NUM_OF_L3_MASTERS ARRAY_SIZE(l3_masters)

static void l3_dump_targ_context(u32 baseaddr)
{
	pr_err("COREREG      : 0x%08x\n", readl(baseaddr + L3_COREREG));
	pr_err("VERSIONREG   : 0x%08x\n", readl(baseaddr + L3_VERSIONREG));
	pr_err("MAINCTLREG   : 0x%08x\n", readl(baseaddr + L3_MAINCTLREG));
	pr_err("NTTPADDR_0   : 0x%08x\n", readl(baseaddr + L3_NTTPADDR_0));
	pr_err("SVRTSTDLVL   : 0x%08x\n", readl(baseaddr + L3_SVRTSTDLVL));
	pr_err("SVRTCUSTOMLVL: 0x%08x\n", readl(baseaddr + L3_SVRTCUSTOMLVL));
	pr_err("MAIN         : 0x%08x\n", readl(baseaddr + L3_MAIN));
	pr_err("HDR          : 0x%08x\n", readl(baseaddr + L3_HDR));
	pr_err("MSTADDR      : 0x%08x\n", readl(baseaddr + L3_MSTADDR));
	pr_err("SLVADDR      : 0x%08x\n", readl(baseaddr + L3_SLVADDR));
	pr_err("INFO         : 0x%08x\n", readl(baseaddr + L3_INFO));
	pr_err("SLVOFSLSB    : 0x%08x\n", readl(baseaddr + L3_SLVOFSLSB));
	pr_err("SLVOFSMSB    : 0x%08x\n", readl(baseaddr + L3_SLVOFSMSB));
	pr_err("CUSTOMINFO_INFO   : 0x%08x\n", readl(baseaddr + L3_CUSTOMINFO_INFO));
	pr_err("CUSTOMINFO_MSTADDR: 0x%08x\n", readl(baseaddr + L3_CUSTOMINFO_MSTADDR));
	pr_err("CUSTOMINFO_OPCODE : 0x%08x\n", readl(baseaddr + L3_CUSTOMINFO_OPCODE));
	pr_err("ADDRSPACESIZELOG  : 0x%08x\n", readl(baseaddr + L3_ADDRSPACESIZELOG));
}

/*
 * Interrupt Handler for L3 error detection.
 *	1) Identify the L3 clockdomain partition to which the error belongs to.
 *	2) Identify the slave where the error information is logged
 *	3) Print the logged information.
 *	4) Add dump stack to provide kernel trace.
 *
 * Two Types of errors :
 *	1) Custom errors in L3 :
 *		Target like DMM/FW/EMIF generates SRESP=ERR error
 *	2) Standard L3 error:
 *		- Unsupported CMD.
 *			L3 tries to access target while it is idle
 *		- OCP disconnect.
 *		- Address hole error:
 *			If DSS/ISS/FDIF/USBHOSTFS access a target where they
 *			do not have connectivity, the error is logged in
 *			their default target which is DMM2.
 *
 *	On High Secure devices, firewall errors are possible and those
 *	can be trapped as well. But the trapping is implemented as part
 *	secure software and hence need not be implemented here.
 */

static bool boot_compleate = 0;
void LGE_boot_compleate(bool flag)
{
    boot_compleate = flag;
}

extern void omap4_prm_global_warm_sw_reset(void);

static irqreturn_t l3_interrupt_handler(int irq, void *_l3)
{

	struct omap4_l3 *l3 = _l3;
	int inttype, i, k;
	int err_src = 0;
	u32 std_err_main, err_reg, clear, masterid;
	void __iomem *base, *l3_targ_base;
	char *target_name, *master_name = "UN IDENTIFIED";

	/* Get the Type of interrupt */
	inttype = irq == l3->app_irq ? L3_APPLICATION_ERROR : L3_DEBUG_ERROR;

	for (i = 0; i < L3_MODULES; i++) {
		/*
		 * Read the regerr register of the clock domain
		 * to determine the source
		 */
		base = l3->l3_base[i];
		err_reg = __raw_readl(base + l3_flagmux[i] +
					+ L3_FLAGMUX_REGERR0 + (inttype << 3));

		/* Get the corresponding error and analyse */
		if (err_reg) {
			/* Identify the source from control status register */
			err_src = __ffs(err_reg);

			/* Read the stderrlog_main_source from clk domain */
			l3_targ_base = base + *(l3_targ[i] + err_src);
			std_err_main =  __raw_readl(l3_targ_base +
					L3_TARG_STDERRLOG_MAIN);
			masterid = __raw_readl(l3_targ_base +
					L3_TARG_STDERRLOG_MSTADDR);

			switch (std_err_main & CUSTOM_ERROR) {
			case STANDARD_ERROR:
				target_name =
					l3_targ_inst_name[i][err_src];
				WARN(true, "L3 standard error: TARGET:%s at address 0x%x\n",
					target_name,
					__raw_readl(l3_targ_base +
						L3_TARG_STDERRLOG_SLVOFSLSB));
				/* clear the std error log*/
				clear = std_err_main | CLEAR_STDERR_LOG;
				writel(clear, l3_targ_base +
					L3_TARG_STDERRLOG_MAIN);
				break;

			case CUSTOM_ERROR:
				target_name =
					l3_targ_inst_name[i][err_src];
				for (k = 0; k < NUM_OF_L3_MASTERS; k++) {
					if (masterid == l3_masters[k].id)
						master_name =
							l3_masters[k].name;
				}
				WARN(true, "L3 custom error: MASTER:%s TARGET:%s\n",
					master_name, target_name);
				/* clear the std error log*/
				clear = std_err_main | CLEAR_STDERR_LOG;
				writel(clear, l3_targ_base +
					L3_TARG_STDERRLOG_MAIN);
				break;

			default:
				/* Nothing to be handled here as of now */
				break;
			}
		/* Error found so break the for loop */
		break;
		}
        else
            {
//            WARN(true, "######### boot_compleate=%d\n",boot_compleate);
               if(boot_compleate==0)
                {
                       omap4_prm_global_warm_sw_reset();
                }            
            }
	}
	return IRQ_HANDLED;
}

static int __init omap4_l3_probe(struct platform_device *pdev)
{
	static struct omap4_l3 *l3;
	struct resource	*res;
	int ret;

	l3 = kzalloc(sizeof(*l3), GFP_KERNEL);
	if (!l3)
		return -ENOMEM;

	platform_set_drvdata(pdev, l3);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "couldn't find resource 0\n");
		ret = -ENODEV;
		goto err0;
	}

	l3->l3_base[0] = ioremap(res->start, resource_size(res));
	if (!l3->l3_base[0]) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENOMEM;
		goto err0;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(&pdev->dev, "couldn't find resource 1\n");
		ret = -ENODEV;
		goto err1;
	}

	l3->l3_base[1] = ioremap(res->start, resource_size(res));
	if (!l3->l3_base[1]) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENOMEM;
		goto err1;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!res) {
		dev_err(&pdev->dev, "couldn't find resource 2\n");
		ret = -ENODEV;
		goto err2;
	}

	l3->l3_base[2] = ioremap(res->start, resource_size(res));
	if (!l3->l3_base[2]) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENOMEM;
		goto err2;
	}

	/*
	 * Setup interrupt Handlers
	 */
	l3->debug_irq = platform_get_irq(pdev, 0);
	ret = request_irq(l3->debug_irq,
			l3_interrupt_handler,
			IRQF_DISABLED, "l3-dbg-irq", l3);
	if (ret) {
		pr_crit("L3: request_irq failed to register for 0x%x\n",
						OMAP44XX_IRQ_L3_DBG);
		goto err3;
	}

	l3->app_irq = platform_get_irq(pdev, 1);
	ret = request_irq(l3->app_irq,
			l3_interrupt_handler,
			IRQF_DISABLED, "l3-app-irq", l3);
	if (ret) {
		pr_crit("L3: request_irq failed to register for 0x%x\n",
						OMAP44XX_IRQ_L3_APP);
		goto err4;
	}

	return 0;

err4:
	free_irq(l3->debug_irq, l3);
err3:
	iounmap(l3->l3_base[2]);
err2:
	iounmap(l3->l3_base[1]);
err1:
	iounmap(l3->l3_base[0]);
err0:
	kfree(l3);
	return ret;
}

static int __exit omap4_l3_remove(struct platform_device *pdev)
{
	struct omap4_l3 *l3 = platform_get_drvdata(pdev);

	free_irq(l3->app_irq, l3);
	free_irq(l3->debug_irq, l3);
	iounmap(l3->l3_base[0]);
	iounmap(l3->l3_base[1]);
	iounmap(l3->l3_base[2]);
	kfree(l3);

	return 0;
}

static struct platform_driver omap4_l3_driver = {
	.remove	= __exit_p(omap4_l3_remove),
	.driver	= {
	.name = "omap_l3_noc",
	},
};

static int __init omap4_l3_init(void)
{
	return platform_driver_probe(&omap4_l3_driver, omap4_l3_probe);
}
postcore_initcall_sync(omap4_l3_init);

static void __exit omap4_l3_exit(void)
{
	platform_driver_unregister(&omap4_l3_driver);
}
module_exit(omap4_l3_exit);
