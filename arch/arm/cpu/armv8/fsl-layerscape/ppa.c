/*
 * Copyright 2016 NXP Semiconductor, Inc.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */
#include <common.h>
#include <malloc.h>
#include <config.h>
#include <errno.h>
#include <asm/system.h>
#include <asm/types.h>
#include <asm/arch/soc.h>
#ifdef CONFIG_FSL_LSCH3
#include <asm/arch/immap_lsch3.h>
#elif defined(CONFIG_FSL_LSCH2)
#include <asm/arch/immap_lsch2.h>
#endif
#ifdef CONFIG_ARMV8_SEC_FIRMWARE_SUPPORT
#include <asm/armv8/sec_firmware.h>
#endif
#ifdef CONFIG_CHAIN_OF_TRUST
#include <fsl_validate.h>
#endif

#ifdef CONFIG_SYS_LS_PPA_FW_IN_NAND
#include <nand.h>
#elif defined(CONFIG_SYS_LS_PPA_FW_IN_MMC)
#include <mmc.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

int ppa_init(void)
{
	void *ppa_fit_addr;
	u32 *boot_loc_ptr_l, *boot_loc_ptr_h;
	int ret;

#ifdef CONFIG_CHAIN_OF_TRUST
	uintptr_t ppa_esbc_hdr = CONFIG_SYS_LS_PPA_ESBC_ADDR;
	uintptr_t ppa_img_addr = 0;
#endif

#ifdef CONFIG_SYS_LS_PPA_FW_IN_XIP
	ppa_fit_addr = (void *)CONFIG_SYS_LS_PPA_FW_ADDR;
	debug("%s: PPA image load from XIP\n", __func__);
#else /* !CONFIG_SYS_LS_PPA_FW_IN_XIP */
	size_t fw_length, fdt_header_len = sizeof(struct fdt_header);

	/* Copy PPA image from MMC/SD/NAND to allocated memory */
#ifdef CONFIG_SYS_LS_PPA_FW_IN_MMC
	struct mmc *mmc;
	int dev = CONFIG_SYS_MMC_ENV_DEV;
	struct fdt_header *fitp;
	u32 cnt;
	u32 blk = CONFIG_SYS_LS_PPA_FW_ADDR / 512;

	debug("%s: PPA image load from eMMC/SD\n", __func__);

	mmc_initialize(gd->bd);
	mmc = find_mmc_device(dev);
	if (!mmc) {
		printf("PPA: MMC cannot find device for PPA firmware\n");
		return -ENODEV;
	}

	mmc_init(mmc);

	fitp = malloc(roundup(fdt_header_len, 512));
	if (!fitp) {
		printf("PPA: malloc failed for FIT header(size 0x%zx)\n",
		       roundup(fdt_header_len, 512));
		return -ENOMEM;
	}

	cnt = DIV_ROUND_UP(fdt_header_len, 512);
	debug("%s: MMC read PPA FIT header: dev # %u, block # %u, count %u\n",
	      __func__, dev, blk, cnt);
	ret = mmc->block_dev.block_read(&mmc->block_dev, blk, cnt, fitp);
	if (ret != cnt) {
		free(fitp);
		printf("MMC/SD read of PPA FIT header at offset 0x%x failed\n",
		       CONFIG_SYS_LS_PPA_FW_ADDR);
		return -EIO;
	}

	/* flush cache after read */
	flush_cache((ulong)fitp, cnt * 512);

	fw_length = fdt_totalsize(fitp);
	free(fitp);

	fw_length = roundup(fw_length, 512);
	ppa_fit_addr = malloc(fw_length);
	if (!ppa_fit_addr) {
		printf("PPA: malloc failed for PPA image(size 0x%zx)\n",
		       fw_length);
		return -ENOMEM;
	}

	cnt = DIV_ROUND_UP(fw_length, 512);
	debug("%s: MMC read PPA FIT image: dev # %u, block # %u, count %u\n",
	      __func__, dev, blk, cnt);
	ret = mmc->block_dev.block_read(&mmc->block_dev,
					blk, cnt, ppa_fit_addr);
	if (ret != cnt) {
		free(ppa_fit_addr);
		printf("MMC/SD read of PPA FIT header at offset 0x%x failed\n",
		       CONFIG_SYS_LS_PPA_FW_ADDR);
		return -EIO;
	}

	/* flush cache after read */
	flush_cache((ulong)ppa_fit_addr, cnt * 512);

#elif defined(CONFIG_SYS_LS_PPA_FW_IN_NAND)
	struct fdt_header fit;

	debug("%s: PPA image load from NAND\n", __func__);

	nand_init();
	ret = nand_read(nand_info[0], (loff_t)CONFIG_SYS_LS_PPA_FW_ADDR,
		       &fdt_header_len, (u_char *)&fit);
	if (ret == -EUCLEAN) {
		printf("NAND read of PPA FIT header at offset 0x%x failed\n",
		       CONFIG_SYS_LS_PPA_FW_ADDR);
		return -EIO;
	}

	fw_length = fdt_totalsize(&fit);

	ppa_fit_addr = malloc(fw_length);
	if (!ppa_fit_addr) {
		printf("PPA: malloc failed for PPA image(size 0x%zx)\n",
		       fw_length);
		return -ENOMEM;
	}

	ret = nand_read(nand_info[0], (loff_t)CONFIG_SYS_LS_PPA_FW_ADDR,
		       &fw_length, (u_char *)ppa_fit_addr);
	if (ret == -EUCLEAN) {
		free(ppa_fit_addr);
		printf("NAND read of PPA firmware at offset 0x%x failed\n",
		       CONFIG_SYS_LS_PPA_FW_ADDR);
		return -EIO;
	}

	/* flush cache after read */
	flush_cache((ulong)ppa_fit_addr, fw_length);
#else
#error "No CONFIG_SYS_LS_PPA_FW_IN_xxx defined"
#endif

#endif

#ifdef CONFIG_CHAIN_OF_TRUST
	ppa_img_addr = (uintptr_t)ppa_fit_addr;
	if (fsl_check_boot_mode_secure() != 0) {
		ret = fsl_secboot_validate(ppa_esbc_hdr,
					   CONFIG_PPA_KEY_HASH,
					   &ppa_img_addr);
		if (ret != 0)
			printf("PPA validation failed\n");
		else
			printf("PPA validation Successful\n");
	}
#endif

#ifdef CONFIG_FSL_LSCH3
	struct ccsr_gur __iomem *gur = (void *)(CONFIG_SYS_FSL_GUTS_ADDR);
	boot_loc_ptr_l = &gur->bootlocptrl;
	boot_loc_ptr_h = &gur->bootlocptrh;
#elif defined(CONFIG_FSL_LSCH2)
	struct ccsr_scfg __iomem *scfg = (void *)(CONFIG_SYS_FSL_SCFG_ADDR);
	boot_loc_ptr_l = &scfg->scratchrw[1];
	boot_loc_ptr_h = &scfg->scratchrw[0];
#endif

	debug("fsl-ppa: boot_loc_ptr_l = 0x%p, boot_loc_ptr_h =0x%p\n",
	      boot_loc_ptr_l, boot_loc_ptr_h);
	ret = sec_firmware_init(ppa_fit_addr, boot_loc_ptr_l, boot_loc_ptr_h);

#if defined(CONFIG_SYS_LS_PPA_FW_IN_MMC) || \
	defined(CONFIG_SYS_LS_PPA_FW_IN_NAND)
	free(ppa_fit_addr);
#endif

	return ret;
}
