#ifdef CONFIG_LOGLEVEL
#undef CONFIG_LOGLEVEL
#endif
#define CONFIG_LOGLEVEL   7
//#include <linux/printk.h>

#include <common.h>
#include <malloc.h>
#include <asm/io.h>
#include <nand.h>

#include <linux/mtd/nand.h>
#include <dm.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <mach/gpio_drv.h>

#include "sp_bch.h"
#include "sp_spinand.h"


/**************************************************************************
 *                             M A C R O S                                *
 **************************************************************************/

DECLARE_GLOBAL_DATA_PTR;
/**************************************************************************
 *                         D A T A   T Y P E S                            *
 **************************************************************************/

/**************************************************************************
 *                        G L O B A L   D A T A                           *
 **************************************************************************/
static struct sp_spinand_info *our_spinfc = NULL;

/**************************************************************************
 *                 E X T E R N A L   R E F E R E N C E S                  *
 **************************************************************************/
extern ulong get_timer_us(ulong base);
extern const struct nand_flash_dev sp_spinand_ids[];

/**************************************************************************/
/*static void printf_block(uint8_t *data, uint32_t size)
{
	int i;
	for(i=0; i<size; i++) {
		if( i%16 == 0)
			printf("\n\t");
		printf("0x%02x,", data[i]);
	}
	printf("\n");
}*/

void dump_spi_regs(struct sp_spinand_info *info)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 *p = (u32 *)regs;
	int i, value;
	const char *reg_name[] = {
		"spi_ctrl",
		"spi_timing",
		"spi_page_addr",
		"spi_data",
		"spi_status",
		"spi_auto_cfg",
		"spi_cfg0",
		"spi_cfg1",
		"spi_cfg2",
		"spi_data_64",
		"spi_buf_addr",
		"spi_statu_2",
		"spi_err_status",
		"mem_data_addr",
		"mem_parity_addr",
		"spi_col_addr",
		"spi_bch",
		"spi_intr_msk",
		"spi_intr_sts",
		"spi_page_size",
	};

	for (i=0; i<20; i++, p++) {
		value = readl(p);
		pr_info("%s = 0x%08X\n", reg_name[i], value);
	}
}

struct sp_spinand_info* get_spinand_info(void)
{
	return our_spinfc;
}

static int get_iomode_cfg(u32 io_mode)
{
	int cfg = -1;
	if (io_mode == SPINAND_1BIT_MODE) {
		cfg = SPINAND_CMD_BITMODE(1)
			| SPINAND_CMD_DQ(1)
			| SPINAND_ADDR_BITMODE(1)
			| SPINAND_ADDR_DQ(1)
			| SPINAND_DATA_BITMODE(1)
			| SPINAND_DATAOUT_DQ(1)
			| SPINAND_DATAIN_DQ(2);
	} else if (io_mode == SPINAND_2BIT_MODE) {
		cfg = SPINAND_CMD_BITMODE(1)
			| SPINAND_CMD_DQ(1)
			| SPINAND_ADDR_BITMODE(1)
			| SPINAND_ADDR_DQ(1)
			| SPINAND_DATA_BITMODE(2);
	} else if (io_mode == SPINAND_4BIT_MODE) {
		cfg = SPINAND_CMD_BITMODE(1)
			| SPINAND_CMD_DQ(1)
			| SPINAND_ADDR_BITMODE(1)
			| SPINAND_ADDR_DQ(1)
			| SPINAND_DATA_BITMODE(3);
	} else if (io_mode == SPINAND_DUAL_MODE) {
		cfg = SPINAND_CMD_BITMODE(1)
			| SPINAND_ADDR_BITMODE(2)
			| SPINAND_DATA_BITMODE(2);
	} else if (io_mode == SPINAND_QUAD_MODE) {
		cfg = SPINAND_CMD_BITMODE(1)
			| SPINAND_ADDR_BITMODE(3)
			| SPINAND_DATA_BITMODE(3);
	}

	return cfg;
}

static int get_iomode_readcmd(u32 io_mode)
{
	int cmd = -1;
	if (io_mode == SPINAND_1BIT_MODE) {
		cmd = SPINAND_CMD_PAGEREAD;
	} else if (io_mode == SPINAND_2BIT_MODE) {
		cmd = SPINAND_CMD_PAGEREAD_X2;
	} else if (io_mode == SPINAND_4BIT_MODE) {
		cmd = SPINAND_CMD_PAGEREAD_X4;
	} else if (io_mode == SPINAND_DUAL_MODE) {
		cmd = SPINAND_CMD_PAGEREAD_DUAL;
	} else if (io_mode == SPINAND_QUAD_MODE) {
		cmd = SPINAND_CMD_PAGEREAD_QUAD;
	}
	return cmd;
}

static int get_iomode_writecmd(u32 io_mode)
{
	int cmd = -1;
	if (io_mode == SPINAND_1BIT_MODE) {
		cmd = SPINAND_CMD_PROGLOAD;
	} else if (io_mode == SPINAND_4BIT_MODE) {
		cmd = SPINAND_CMD_PROGLOAD_X4;
	}
	return cmd;
}

int wait_spi_idle(struct sp_spinand_info *info)
{
	struct sp_spinand_regs *regs = info->regs;
	unsigned long now = get_timer(0);
	int ret = -1;

	do {
		if (!(readl(&regs->spi_ctrl) & SPINAND_BUSY_MASK)) {
			ret = 0;
			break;
		}
	} while(get_timer(now) < CONFIG_SPINAND_TIMEOUT);

	if (ret < 0) {
		pr_warn("%s timeout \n", __FUNCTION__);
		//dump_spi_regs(info);
	}

	return ret;
}

int spi_nand_trigger_and_wait_dma(struct sp_spinand_info *info)
{
	struct sp_spinand_regs *regs = info->regs;
	unsigned long timeout_ms = CONFIG_SPINAND_TIMEOUT;
	unsigned long now;
	u32 value;
	int ret = -1;

	value = ~SPINAND_DMA_DONE_MASK;
	writel(value, &regs->spi_intr_msk);

	value = readl(&regs->spi_intr_sts);
	writel(value, &regs->spi_intr_sts);

	value = readl(&regs->spi_auto_cfg);
	value |= SPINAND_DMA_TRIGGER;
	writel(value, &regs->spi_auto_cfg);

	now = get_timer(0);
	do {
		if ((readl(&regs->spi_intr_sts) & SPINAND_DMA_DONE_MASK)) {
			ret = 0;
			break;
		}
	} while(get_timer(now) < timeout_ms);

	if(ret < 0) {
		//dump_spi_regs(info);
		pr_warn("%s timeout\n", __FUNCTION__);
	}

	return ret;
}

int spi_nand_trigger_and_wait_pio(struct sp_spinand_info *info)
{
	struct sp_spinand_regs *regs = info->regs;
	unsigned long timeout_ms = CONFIG_SPINAND_TIMEOUT;
	unsigned long now;
	u32 value;
	int ret = -1;

	value = ~SPINAND_PIO_DONE_MASK;
	writel(value, &regs->spi_intr_msk);

	value = readl(&regs->spi_intr_sts);
	writel(value, &regs->spi_intr_sts);

	value = readl(&regs->spi_auto_cfg);
	value |= SPINAND_USR_CMD_TRIGGER;
	writel(value, &regs->spi_auto_cfg);

	now = get_timer(0);
	do {
		if ((readl(&regs->spi_intr_sts) & SPINAND_PIO_DONE_MASK)) {
			ret = 0;
			break;
		}
	} while(get_timer(now) < timeout_ms);

	if(ret < 0) {
		//dump_spi_regs(info);
		pr_warn("%s timeout\n", __FUNCTION__);
	}

	return ret;
}

static int spi_nand_getfeatures(struct sp_spinand_info *info, u32 addr)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 value = 0;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_USR_CMD(SPINAND_CMD_GETFEATURES)
		| SPINAND_CTRL_EN
		| SPINAND_USRCMD_DATASZ(1)
		| SPINAND_READ_MODE
		| SPINAND_USRCMD_ADDRSZ(1);
	writel(value, &regs->spi_ctrl);

	writel(addr, &regs->spi_page_addr);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE;
	writel(value, &regs->spi_cfg[0]);

	value = SPINAND_CMD_BITMODE(1)
		| SPINAND_CMD_DQ(1)
		| SPINAND_ADDR_BITMODE(1)
		| SPINAND_ADDR_DQ(1)
		| SPINAND_DATA_BITMODE(1)
		| SPINAND_DATAIN_DQ(2);
	writel(value, &regs->spi_cfg[1]);

	value = SPINAND_USR_CMD_TRIGGER;
	writel(value, &regs->spi_auto_cfg);

	wait_spi_idle(info);

	return (readl(&regs->spi_data) & 0xFF);
}

static int spi_nand_setfeatures(struct sp_spinand_info *info, u32 addr,u32 data)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 value = 0;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_AUTOWEL_EN
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_USR_CMD(SPINAND_CMD_SETFEATURES)
		| SPINAND_CTRL_EN
		| SPINAND_USRCMD_DATASZ(1)
		| SPINAND_WRITE_MODE
		| SPINAND_USRCMD_ADDRSZ(1);
	writel(value, &regs->spi_ctrl);

	writel(addr, &regs->spi_page_addr);

	writel(data, &regs->spi_data);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE;
	writel(value, &regs->spi_cfg[0]);

	value = SPINAND_CMD_BITMODE(1)
		| SPINAND_CMD_DQ(1)
		| SPINAND_ADDR_BITMODE(1)
		| SPINAND_ADDR_DQ(1)
		| SPINAND_DATA_BITMODE(1)
		| SPINAND_DATAOUT_DQ(1);
	writel(value, &regs->spi_cfg[1]);

	value = SPINAND_USR_CMD_TRIGGER;
	writel(value, &regs->spi_auto_cfg);

	return wait_spi_idle(info);
}

static int spi_nand_reset(struct sp_spinand_info *info)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 value = 0;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(7)
		| SPINAND_USR_CMD(SPINAND_CMD_RESET)
		| SPINAND_CTRL_EN
		| SPINAND_USRCMD_DATASZ(0)
		| SPINAND_WRITE_MODE
		| SPINAND_USRCMD_ADDRSZ(0);
	writel(value, &regs->spi_ctrl);

	value = SPINAND_READ_TIMING(CONFIG_READ_TIMING_SEL);
	writel(value ,&regs->spi_timing);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE;
	writel(value, &regs->spi_cfg[0]);

	value = SPINAND_CMD_BITMODE(1)
		| SPINAND_CMD_DQ(1)
		| SPINAND_ADDR_BITMODE(0)
		| SPINAND_ADDR_DQ(0)
		| SPINAND_DATA_BITMODE(0)
		| SPINAND_DATAIN_DQ(0)
		| SPINAND_DATAOUT_DQ(0);
	writel(value, &regs->spi_cfg[1]);

	writel(0, &regs->spi_bch);

	value = SPINAND_USR_CMD_TRIGGER;
	writel(value, &regs->spi_auto_cfg);

	wait_spi_idle(info);

	value = SPINAND_CHECK_OIP_EN;
	writel(value, &regs->spi_auto_cfg);

	return wait_spi_idle(info);
}

static void spi_nand_readid(struct sp_spinand_info *info, u32 addr, u8 *data)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 value = 0;

	/*read 3 byte cycle same to 8388 */
	value = SPINAND_SEL_CHIP_A
	        | SPINAND_SCK_DIV(7)
	        | SPINAND_USR_CMD(SPINAND_CMD_READID)
	        | SPINAND_CTRL_EN
	        | SPINAND_USRCMD_DATASZ(3)
	        | SPINAND_READ_MODE
	        | SPINAND_USRCMD_ADDRSZ(1);
	writel(value, &regs->spi_ctrl);

	writel(addr, &regs->spi_page_addr);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE;
	writel(value, &regs->spi_cfg[0]);

	value = SPINAND_CMD_BITMODE(1)
		| SPINAND_CMD_DQ(1)
		| SPINAND_ADDR_BITMODE(1)
		| SPINAND_ADDR_DQ(1)
		| SPINAND_DATA_BITMODE(1)
		| SPINAND_DATAIN_DQ(2)
		| SPINAND_DATAOUT_DQ(0);
	writel(value, &regs->spi_cfg[1]);

	value = SPINAND_USR_CMD_TRIGGER;
	writel(value, &regs->spi_auto_cfg);

	wait_spi_idle(info);

	value = readl(&regs->spi_data);

	*(u32 *)data = value;
}

static int spi_nand_blkerase(struct sp_spinand_info *info, u32 row)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 value = 0;
	int ret;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_AUTOWEL_EN
		| SPINAND_SCK_DIV(7)
		| SPINAND_USR_CMD(SPINAND_CMD_BLKERASE)
		| SPINAND_CTRL_EN
		| SPINAND_USRCMD_DATASZ(0)
		| SPINAND_WRITE_MODE
		| SPINAND_USRCMD_ADDRSZ(3);
	writel(value, &regs->spi_ctrl);

	writel(row, &regs->spi_page_addr);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE;
	writel(value, &regs->spi_cfg[0]);

	value = SPINAND_CMD_BITMODE(1)
		| SPINAND_CMD_DQ(1)
		| SPINAND_ADDR_BITMODE(1)
		| SPINAND_ADDR_DQ(1)
		| SPINAND_DATA_BITMODE(1)
		| SPINAND_DATAIN_DQ(2)
		| SPINAND_DATAOUT_DQ(1);
	writel(value, &regs->spi_cfg[1]);

	value = SPINAND_AUTO_RDSR_EN;
	writel(value, &regs->spi_auto_cfg);

	ret = spi_nand_trigger_and_wait_pio(info);
	if (!ret) {
		value = readl(&regs->spi_status);
		ret = (value & ERASE_STATUS) ? (-1) : 0;
	}

	return ret;
}

static int spi_nand_program_exec(struct sp_spinand_info *info, u32 row)
{
	struct sp_spinand_regs *regs = info->regs;
	int ret = 0;
	u32 value = 0;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_USR_CMD(SPINAND_CMD_PROGEXEC)
		| SPINAND_CTRL_EN
		| SPINAND_WRITE_MODE
		| SPINAND_USRCMD_ADDRSZ(3);
	writel(value, &regs->spi_ctrl);

	writel(row, &regs->spi_page_addr);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE;
	writel(value, &regs->spi_cfg[0]);

	value = SPINAND_CMD_BITMODE(1)
		| SPINAND_CMD_DQ(1)
		| SPINAND_ADDR_BITMODE(1)
		| SPINAND_ADDR_DQ(1)
		| SPINAND_DATA_BITMODE(1)
		| SPINAND_DATAIN_DQ(2)
		| SPINAND_DATAOUT_DQ(1);
	writel(value, &regs->spi_cfg[1]);

	value = SPINAND_USR_PRGMLOAD_CMD(SPINAND_CMD_PROGEXEC)
		| SPINAND_USR_PRGMLOAD_EN
		| SPINAND_AUTO_RDSR_EN;
	writel(value, &regs->spi_auto_cfg);

	ret = spi_nand_trigger_and_wait_pio(info);
	if (!ret) {
		value = readl(&regs->spi_status);
		ret = ((value&0x08) ? -1 : 0);
	}

	return ret;
}

static int spi_nand_program_load(struct sp_spinand_info *info, u32 io_mode,
				u32 col, u8 *buf, u32 size)
{
	struct sp_spinand_regs *regs = info->regs;
	int cfg = get_iomode_cfg(io_mode);
	int cmd = get_iomode_writecmd(io_mode);
	u32 value = 0;
	u32 i;

	if (cfg < 0 || cmd < 0)
		return -1;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_CTRL_EN
		| SPINAND_USR_CMD(cmd)
		| SPINAND_AUTOWEL_EN
		| SPINAND_WRITE_MODE
		| SPINAND_USRCMD_ADDRSZ(2);
	writel(value, &regs->spi_ctrl);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_DATA64_EN
		| SPINAND_TRS_MODE
		| SPINAND_DATA_LEN(size);
	writel(value, &regs->spi_cfg[0]);

	writel(cfg, &regs->spi_cfg[1]);

	value = col;
	writel(value, &regs->spi_page_addr);

	value = SPINAND_USR_PRGMLOAD_CMD(cmd)
		| SPINAND_USR_PRGMLOAD_EN
		| SPINAND_AUTOWEL_BF_PRGMLOAD
		| SPINAND_USR_CMD_TRIGGER;
	writel(value, &regs->spi_auto_cfg);

	size = (size + 3) & (~3);
	if (!((u32)buf & 0x03)) {
		/* the buf address is aligned to 4 bytes*/
		for(i=0; i<size; i+=4) {
			value = *(u32 *)(buf+i);
			writel(value, &regs->spi_data_64);
		}
	} else {
		/* the buf address is not aligned to 4 bytes*/
		for(i=0,value=0; i<size; i++) {
			value |= (buf[i] << (8*(i&0x03)));
			if ((i&0x03) == 0x03) {
				writel(value, &regs->spi_data_64);
				value = 0;
			}
		}
	}

	return wait_spi_idle(info);
}

static int spi_nand_pagecache(struct sp_spinand_info *info, u32 row)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 value = 0;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_USR_CMD(SPINAND_CMD_PAGE2CACHE)
		| SPINAND_CTRL_EN
		| SPINAND_USRCMD_DATASZ(0)
		| SPINAND_READ_MODE
		| SPINAND_USRCMD_ADDRSZ(3);
	writel(value, &regs->spi_ctrl);

	writel(row, &regs->spi_page_addr);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE;
	writel(value, &regs->spi_cfg[0]);

	value = SPINAND_CMD_BITMODE(1)
		| SPINAND_CMD_DQ(1)
		| SPINAND_ADDR_BITMODE(1)
		| SPINAND_ADDR_DQ(1)
		| SPINAND_DATA_BITMODE(1)
		| SPINAND_DATAIN_DQ(2)
		| SPINAND_DATAOUT_DQ(1);
	writel(value, &regs->spi_cfg[1]);

	value = SPINAND_AUTO_RDSR_EN;
	writel(value, &regs->spi_auto_cfg);

	return spi_nand_trigger_and_wait_pio(info);
}

/*
 *   memory access mode to read data.
 */
static int spi_nand_readcache(struct sp_spinand_info *info, u32 io_mode,
				u32 col, u8 *buf, u32 size)
{
	struct sp_spinand_regs *regs = info->regs;
	int cfg = get_iomode_cfg(io_mode);
	int cmd = get_iomode_readcmd(io_mode);
	u32 value = 0;
	u32 i;

	if (cfg < 0 || cmd < 0)
		return -1;

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE;
	writel(value, &regs->spi_cfg[0]);

	value = SPINAND_DUMMY_CYCLES(8) | cfg;
	writel(value, &regs->spi_cfg[2]);

	value = SPINAND_USR_READCACHE_CMD(cmd)
		| SPINAND_USR_READCACHE_EN
		| SPINAND_AUTO_RDSR_EN;
	writel(value, &regs->spi_auto_cfg);

	do {
		value = readl(&regs->spi_auto_cfg);
	} while((value >> 24) != cmd);

	i = col;
	if (!(col&0x03) && !((u32)buf&0x03)) {
		/* 4 byte aligned case */
		for (; i<(col+size)>>2<<2; i+=4,buf+=4) {
			*(u32*)buf = *(u32*)(SPI_NAND_DIRECT_MAP + i);
		}
	}
	for (; i<(col+size); i++,buf++)
		*buf = *(u8*)(SPI_NAND_DIRECT_MAP + i);

	return wait_spi_idle(info);
}

// real pio mode
/*
static int spi_nand_readcache(struct sp_spinand_info *info, u32 io_mode,
				u32 col, u8 *buf, u32 size)
{
	struct sp_spinand_regs *regs = info->regs;
	int cfg = get_iomode_cfg(io_mode);
	int cmd = get_iomode_readcmd(io_mode);
	u32 value = 0;
	u32 i;

	if (cfg < 0 || cmd < 0)
		return -1;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_CTRL_EN
		| SPINAND_USR_CMD(cmd)
		| SPINAND_READ_MODE
		| SPINAND_USRCMD_ADDRSZ(2);
	writel(value, &regs->spi_ctrl);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_DATA64_EN
		| SPINAND_TRS_MODE
		| SPINAND_DATA_LEN(size);
	writel(value, &regs->spi_cfg[0]);

	value = cfg | SPINAND_DUMMY_CYCLES(8);
	writel(value, &regs->spi_cfg[1]);

	value = col;
	writel(value, &regs->spi_page_addr);

	value = SPINAND_USR_READCACHE_CMD(cmd)
		| SPINAND_USR_READCACHE_EN
		| SPINAND_USR_CMD_TRIGGER;
	writel(value, &regs->spi_auto_cfg);

	for (i=0; i<size; i++) {
		if ((i&0x03) == 0)
			value = readl(&regs->spi_data_64);
		buf[i] = value & 0xff;
		value >>= 8;
	}

	return wait_spi_idle(info);
}
*/

static int spi_nand_read_by_pio(struct sp_spinand_info *info, u32 io_mode,
				u32 row, u32 col, u8 *buf, u32 size)
{
	u32 plane_sel_mode = info->plane_sel_mode;
	int ret = 0;

	if ((plane_sel_mode & 0x1)) {
		u32 pagemark = (plane_sel_mode >> 2) & 0xfff;
		u32 colmark = (plane_sel_mode >> 16) & 0xffff;
		col |= ((row & pagemark) != 0) ? colmark : 0;
	}
	ret = spi_nand_pagecache(info, row);
	if (!ret)
		ret = spi_nand_readcache(info, io_mode, col, buf, size);

	return ret;
}

static int spi_nand_write_by_pio(struct sp_spinand_info *info, u32 io_mode,
				u32 row, u32 col, u8 *buf, u32 size)
{
	u32 plane_sel_mode = info->plane_sel_mode;
	int ret = 0;

	if ((plane_sel_mode & 0x1)) {
		u32 pagemark = (plane_sel_mode >> 2) & 0xfff;
		u32 colmark = (plane_sel_mode >> 16) & 0xffff;
		col |= ((row & pagemark) != 0) ? colmark : 0;
	}
	ret = spi_nand_program_load(info, io_mode, col, buf, size);
	if (!ret)
		ret = spi_nand_program_exec(info, row);

	return ret;
}

static int spi_nand_read_by_pio_auto(struct sp_spinand_info *info, u32 io_mode,
				u32 row, u32 col, u8 *buf, u32 size)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 plane_sel_mode = info->plane_sel_mode;
	u32 page_size = info->page_size;
	int cfg = get_iomode_cfg(io_mode);
	int cmd = get_iomode_readcmd(io_mode);
	u32 value;
	u32 i;

	if (cfg < 0 || cmd < 0)
		return -1;

	value = SPINAND_AUTOMODE_EN
		| SPINAND_AUTOCMD_EN
		| SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_USR_CMD(SPINAND_CMD_PAGE2CACHE)
		| SPINAND_CTRL_EN
		| SPINAND_READ_MODE
		| SPINAND_USRCMD_ADDRSZ(2);
	writel(value, &regs->spi_ctrl);

	value = (size+3) & (~3);
	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_DATA64_EN
		| SPINAND_TRS_MODE
		| SPINAND_DATA_LEN(value);
	writel(value, &regs->spi_cfg[0]);

	cfg |= SPINAND_DUMMY_CYCLES(8);
	writel(cfg, &regs->spi_cfg[1]);

	writel(row, &regs->spi_page_addr);

	if ((plane_sel_mode & 0x1)) {
		u32 pagemark = (plane_sel_mode >> 2) & 0xfff;
		u32 colmark = (plane_sel_mode >> 16) & 0xffff;
		col |= ((row & pagemark) != 0) ? colmark : 0;
		page_size += ((row & pagemark) != 0) ? colmark : 0;
	}
	writel(col, &regs->spi_col_addr);

	value = SPINAND_SPARE_SIZE(info->oob_size)
		| SPINAND_PAGE_SIZE((page_size >> 10) - 1);
	writel(value, &regs->spi_page_size);

	value = SPINAND_USR_CMD_TRIGGER
		| SPINAND_USR_READCACHE_CMD(cmd)
		| SPINAND_USR_READCACHE_EN;
	writel(value, &regs->spi_auto_cfg);

	for (i=0; i<size; i++) {
		if ((i&0x03) == 0)
			value = readl(&regs->spi_data_64);
		buf[i] = value & 0xff;
		value >>= 8;
	}

	return wait_spi_idle(info);
}

static int spi_nand_write_by_pio_auto(struct sp_spinand_info *info, u32 io_mode,
					u32 row, u32 col, u8 *buf, u32 size)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 plane_sel_mode = info->plane_sel_mode;
	u32 page_size = info->page_size;
	int cfg = get_iomode_cfg(io_mode);
	int cmd = get_iomode_writecmd(io_mode);
	u32 value;
	u32 i;

	if (cfg < 0 || cmd < 0)
		return -1;

	value = SPINAND_AUTOMODE_EN
		| SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_USR_CMD(SPINAND_CMD_PROGEXEC)
		| SPINAND_CTRL_EN
		| SPINAND_WRITE_MODE
		| SPINAND_USRCMD_ADDRSZ(2);
	writel(value, &regs->spi_ctrl);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_DATA64_EN
		| SPINAND_TRS_MODE
		| SPINAND_DATA_LEN(size);
	writel(value, &regs->spi_cfg[0]);

	writel(cfg, &regs->spi_cfg[1]);

	writel(row, &regs->spi_page_addr);

	if ((plane_sel_mode & 0x1)) {
		u32 pagemark = (plane_sel_mode >> 2) & 0xfff;
		u32 colmark = (plane_sel_mode >> 16) & 0xffff;
		col |= ((row & pagemark) != 0) ? colmark : 0;
		page_size += ((row & pagemark) != 0) ? colmark : 0;
	}
	writel(col, &regs->spi_col_addr);

	value = SPINAND_SPARE_SIZE(info->oob_size)
		| SPINAND_PAGE_SIZE((page_size >> 10) - 1);
	writel(value, &regs->spi_page_size);

	value = SPINAND_USR_CMD_TRIGGER
		| SPINAND_USR_PRGMLOAD_CMD(cmd)
		| SPINAND_AUTOWEL_BF_PRGMLOAD
		| SPINAND_USR_PRGMLOAD_EN;
	writel(value, &regs->spi_auto_cfg);

	size = (size + 3) & (~3);
	if (!((u32)buf & 0x03)) {
		/* the buf address is aligned to 4 bytes */
		for(i=0; i<size; i+=4) {
			value = *(u32 *)(buf+i);
			writel(value, &regs->spi_data_64);
		}
	} else {
		/* the buf address is not aligned to 4 bytes */
		for(i=0,value=0; i<size; i++) {
			value |= (buf[i] << (8*(i&0x03)));
			if ((i&0x03) == 0x03) {
				writel(value, &regs->spi_data_64);
				value = 0;
			}
		}
	}

	wait_spi_idle(info);

	value = readl(&regs->spi_status);

	return ((value&0x08) ? -1 : 0);
}

static int spi_nand_read_by_dma(struct sp_spinand_info *info, u32 io_mode,
				u32 row, u32 col, u8 *buf, u32 size)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 plane_sel_mode = info->plane_sel_mode;
	u32 page_size = info->page_size;
	int cmd = get_iomode_readcmd(io_mode);
	int cfg = get_iomode_cfg(io_mode);
	u32 value = 0;

	if (cmd < 0 || cfg < 0)
		return -1;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_CTRL_EN
		| SPINAND_USRCMD_ADDRSZ(2);
	writel(value, &regs->spi_ctrl);

	writel(row, &regs->spi_page_addr);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE
		| SPINAND_DATA_LEN(size);
	writel(value, &regs->spi_cfg[0]);

	value = cfg | SPINAND_DUMMY_CYCLES(8);
	writel(value, &regs->spi_cfg[1]);

	if ((plane_sel_mode & 0x1) != 0) {
		u32 pagemark = (plane_sel_mode>>2)&0xfff;
		u32 colmark = (plane_sel_mode>>16)&0xffff;
		col |= ((row & pagemark) != 0) ? colmark : 0;
		page_size += ((row & pagemark) != 0) ? colmark : 0;
	}
	writel(col, &regs->spi_col_addr);

	value = SPINAND_SPARE_SIZE(info->oob_size)
		| SPINAND_PAGE_SIZE((page_size >> 10) - 1);
	writel(value, &regs->spi_page_size);

	writel((u32)buf, &regs->mem_data_addr);

	value = SPINAND_USR_READCACHE_CMD(cmd)
		| SPINAND_USR_READCACHE_EN;
	writel(value, &regs->spi_auto_cfg);

	return spi_nand_trigger_and_wait_dma(info);
}

static int spi_nand_write_by_dma(struct sp_spinand_info *info, u32 io_mode,
				u32 row, u32 col, u8 *buf, u32 size)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 plane_sel_mode = info->plane_sel_mode;
	u32 page_size = info->page_size;
	int cmd = get_iomode_writecmd(io_mode);
	int cfg = get_iomode_cfg(io_mode);
	u32 value = 0;
	int ret;

	if (cmd < 0 || cfg < 0)
		return -1;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_CTRL_EN
		| SPINAND_WRITE_MODE
		| SPINAND_USRCMD_ADDRSZ(2);
	writel(value, &regs->spi_ctrl);

	writel(row, &regs->spi_page_addr);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE
		| SPINAND_DATA_LEN(size);
	writel(value, &regs->spi_cfg[0]);

	writel(cfg, &regs->spi_cfg[1]);

	if ((plane_sel_mode & 0x1) != 0) {
		u32 pagemark = (plane_sel_mode>>2)&0xfff;
		u32 colmark = (plane_sel_mode>>16)&0xffff;
		col |= (row & pagemark) ? colmark : 0;
		page_size += (row & pagemark) ? colmark : 0;
	}
	writel(col, &regs->spi_col_addr);

	value = SPINAND_SPARE_SIZE(info->oob_size)
		| SPINAND_PAGE_SIZE((page_size >> 10) - 1);
	writel(value, &regs->spi_page_size);

	writel((u32)buf, &regs->mem_data_addr);

	value = SPINAND_DMA_DONE_MASK;
	writel(value, &regs->spi_intr_msk);
	writel(value, &regs->spi_intr_sts);

	value = SPINAND_USR_PRGMLOAD_CMD(cmd)
		| SPINAND_USR_PRGMLOAD_EN
		| SPINAND_AUTOWEL_BF_PRGMLOAD;
	writel(value, &regs->spi_auto_cfg);

	ret = spi_nand_trigger_and_wait_dma(info);

	if(!ret) {
		value = readl(&regs->spi_status);
		ret = (value & 0x08) ? (-1) : 0;
	}
	return ret;
}

static int spi_nand_pageread_autobch(struct sp_spinand_info *info, u32 io_mode,
					u32 row, u8 *buf)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 plane_sel_mode = info->plane_sel_mode;
	u32 page_size = info->page_size;
	int cmd = get_iomode_readcmd(io_mode);
	int cfg = get_iomode_cfg(io_mode);
	u32 value = 0;
	int ret;

	if (cmd < 0 || cfg < 0)
		return -1;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_CTRL_EN
		| SPINAND_USRCMD_ADDRSZ(2);
	writel(value, &regs->spi_ctrl);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE
		| SPINAND_DATA_LEN(info->page_size);
	writel(value, &regs->spi_cfg[0]);

	value = cfg | SPINAND_DUMMY_CYCLES(8);
	writel(value, &regs->spi_cfg[1]);

	writel(row, &regs->spi_page_addr);

	value = 0;
	if ((plane_sel_mode & 0x1) != 0) {
		u32 pagemark = (plane_sel_mode>>2)&0xfff;
		u32 colmark = (plane_sel_mode>>16)&0xffff;
		value |= ((row & pagemark) != 0) ? colmark : 0;
		page_size += ((row & pagemark) != 0) ? colmark : 0;
	}
	writel(value, &regs->spi_col_addr);

	value = SPINAND_SPARE_SIZE(info->oob_size)
		| SPINAND_PAGE_SIZE((page_size >> 10) - 1);
	writel(value, &regs->spi_page_size);

	writel((u32)buf, &regs->mem_data_addr);
	writel((u32)buf+info->page_size, &regs->mem_parity_addr);

	value = SPINAND_BCH_DATA_LEN(info->parity_sector_size)
		| SPINAND_BCH_BLOCKS(info->nand.ecc.steps - 1)
		| SPINAND_BCH_AUTO_EN;
	value |= ((info->cr0 >> 11) & 0x01) ?
			SPINAND_BCH_ALIGN_16B : SPINAND_BCH_ALIGN_32B;
	value |= (info->nand.ecc.size == 1024) ?
			SPINAND_BCH_1K_MODE : SPINAND_BCH_512B_MODE;
	writel(value, &regs->spi_bch);

	value = SPINAND_USR_READCACHE_CMD(cmd)
		| SPINAND_USR_READCACHE_EN;
	writel(value, &regs->spi_auto_cfg);

	sp_autobch_config(info->mtd, buf, buf+info->page_size, 0);

	ret = spi_nand_trigger_and_wait_dma(info);

	if(!ret)
		ret = sp_autobch_result(info->mtd);

	writel(0, &regs->spi_bch); /* close auto bch */

	return ret;
}

static int spi_nand_pagewrite_autobch(struct sp_spinand_info *info, u32 io_mode,
					u32 row, u8 *buf)
{
	struct sp_spinand_regs *regs = info->regs;
	u32 plane_sel_mode = info->plane_sel_mode;
	u32 page_size = info->page_size;
	int cmd = get_iomode_writecmd(io_mode);
	int cfg = get_iomode_cfg(io_mode);
	u32 value = 0;
	int ret = 0;

	if (cmd < 0 || cfg < 0)
		return -1;

	value = SPINAND_SEL_CHIP_A
		| SPINAND_SCK_DIV(info->spi_clk_div)
		| SPINAND_USR_CMD(SPINAND_CMD_PROGEXEC)
		| SPINAND_CTRL_EN
		| SPINAND_WRITE_MODE
		| SPINAND_USRCMD_ADDRSZ(2);
	writel(value, &regs->spi_ctrl);

	value = SPINAND_LITTLE_ENDIAN
		| SPINAND_TRS_MODE
		| SPINAND_DATA_LEN(info->page_size);
	writel(value, &regs->spi_cfg[0]);

	value = cfg;
	writel(value, &regs->spi_cfg[1]);

	writel(row, &regs->spi_page_addr);

	value = 0;
	if ((plane_sel_mode & 0x1) != 0) {
		u32 pagemark = (plane_sel_mode>>2)&0xfff;
		u32 colmark = (plane_sel_mode>>16)&0xffff;
		value |= ((row & pagemark) != 0) ? colmark : 0;
		page_size += ((row & pagemark) != 0) ? colmark : 0;
	}
	writel(value, &regs->spi_col_addr);

	value = SPINAND_SPARE_SIZE(info->oob_size)
		| SPINAND_PAGE_SIZE((page_size >> 10) - 1);
	writel(value, &regs->spi_page_size);

	writel((u32)buf, &regs->mem_data_addr);
	writel((u32)buf+info->page_size, &regs->mem_parity_addr);

	value = SPINAND_BCH_DATA_LEN(info->parity_sector_size)
		| SPINAND_BCH_BLOCKS(info->nand.ecc.steps - 1)
		| SPINAND_BCH_AUTO_EN;
	value |= ((info->cr0 >> 11) & 0x01) ?
			SPINAND_BCH_ALIGN_16B : SPINAND_BCH_ALIGN_32B;
	value |= (info->nand.ecc.size == 1024) ?
			SPINAND_BCH_1K_MODE : SPINAND_BCH_512B_MODE;
	writel(value, &regs->spi_bch);

	sp_autobch_config(info->mtd, buf, buf+info->page_size, 1);

	value = SPINAND_USR_PRGMLOAD_CMD(cmd)
		| SPINAND_USR_PRGMLOAD_EN
		| SPINAND_AUTOWEL_BF_PRGMLOAD;
	writel(value, &regs->spi_auto_cfg);

	ret = spi_nand_trigger_and_wait_dma(info);
	if(!ret)
		ret = sp_autobch_result(info->mtd);

	writel(0, &regs->spi_bch); /* close auto bch */

	return ret;
}

static int sp_spinand_read_raw(struct sp_spinand_info *info,
				u32 row, u32 col, u32 size)
{
	int ret = -1;
	u8 io = info->read_bitmode;
	u8 trsmode = info->raw_trs_mode;
	u8 *va = info->buff.virt + info->buff.idx;
	u8 *pa = (u8*)info->buff.phys + info->buff.idx;

	if (trsmode == SPINAND_TRS_DMA) {
		ret = spi_nand_read_by_dma(info, io, row, col, pa, size);
	} else if (trsmode == SPINAND_TRS_PIO_AUTO) {
		ret = spi_nand_read_by_pio_auto(info, io, row, col, va, size);
	} else if (trsmode == SPINAND_TRS_PIO) {
		ret = spi_nand_read_by_pio(info, io, row, col, va, size);
	}
	return ret;
}

static int sp_spinand_write_raw(struct sp_spinand_info *info,
				u32 row, u32 col, u32 size)
{
	int ret = -1;
	u8 io = info->write_bitmode;
	u8 trsmode = info->raw_trs_mode;
	u8 *va = info->buff.virt + info->buff.idx;
	u8 *pa = (u8*)info->buff.phys + info->buff.idx;

	if (trsmode == SPINAND_TRS_DMA) {
		ret = spi_nand_write_by_dma(info, io, row, col, pa, size);
	} else if (trsmode == SPINAND_TRS_PIO_AUTO) {
		ret = spi_nand_write_by_pio_auto(info, io, row, col, va, size);
	} else if (trsmode == SPINAND_TRS_PIO) {
		ret = spi_nand_write_by_pio(info, io, row, col, va, size);
	}
	return ret;
}

static void sp_spinand_select_chip(struct mtd_info *mtd, int chipnr)
{
	struct sp_spinand_info *info = get_spinand_info();

	switch (chipnr) {
	case 0:
	case 1:
	case 2:
	case 3:
		info->cs = chipnr;
		break;
	default:
		break;
	}
}

static void sp_spinand_cmd_ctrl(struct mtd_info *mtd, int cmd, u32 ctrl)
{
	return;
}

static void sp_spinand_cmdfunc(struct mtd_info *mtd, u32 cmd, int col, int row)
{
	struct sp_spinand_info *info = get_spinand_info();

	info->cmd = cmd;
	switch (cmd) {
	case NAND_CMD_READOOB:
		info->buff.idx = 0;
		info->col = col + info->mtd->writesize;
		info->row = row;
		break;
	case NAND_CMD_READ0:
		info->buff.idx = 0;
		info->col = col;
		info->row = row;
		break;
	case NAND_CMD_SEQIN:
		info->buff.idx = 0;
		info->col = col;
		info->row = row;
		break;
	case NAND_CMD_PAGEPROG:
		if(info->buff.idx) {
			u32 size = info->buff.idx;
			info->buff.idx = 0;
			#ifndef CONFIG_SPINAND_USE_SRAM
			flush_dcache_range((unsigned long)info->buff.virt,
				(unsigned long)info->buff.virt + size);
			#endif
			sp_spinand_write_raw(info, info->row, info->col, size);
		}
		break;
	case NAND_CMD_ERASE1:
		row &= ~(info->mtd->erasesize / info->mtd->writesize - 1);
		spi_nand_blkerase(info, row);
		break;
	case NAND_CMD_ERASE2:
		break;
	case NAND_CMD_STATUS:
		info->buff.idx = 0;
		break;
	case NAND_CMD_RESET:
		spi_nand_reset(info);
		break;
	case NAND_CMD_READID:
		info->buff.idx = 0;
		info->col = col;
		break;
	case NAND_CMD_PARAM:
	case NAND_CMD_GET_FEATURES:
	case NAND_CMD_SET_FEATURES:
		/* these cmds are p-nand related, ignore them */
		break;
	default:
		pr_warn("sp_spinand: unknown command=0x%02x.\n", cmd);
		break;
	}
}

static int sp_spinand_dev_ready(struct mtd_info *mtd)
{
	struct sp_spinand_info *info = get_spinand_info();

	return ((spi_nand_getfeatures(info, DEVICE_STATUS_ADDR) & 0x01) == 0);
}

static int sp_spinand_waitfunc(struct mtd_info *mtd, struct nand_chip *chip)
{
	struct sp_spinand_info *info = get_spinand_info();
	u32 timeout = (CONFIG_SYS_HZ * 400) / 1000;
 	u32 time_start;
	int status;
	int ret;

 	time_start = get_timer(0);
	do {
		status = spi_nand_getfeatures(info, DEVICE_STATUS_ADDR);
		if ((status & 0x01) == 0)
			break;
	} while (get_timer(time_start) < timeout);

	/* program/erase fail bit */
	if (info->cmd == NAND_CMD_PAGEPROG && (status&0x08))
		ret = 1;
	else if ((info->cmd == NAND_CMD_ERASE2) && (status&0x04))
		ret = 1;
	else
		ret = (status & 0x0c) ? 0x01 : 0x00;

	/* ready bit */
	ret |= (status & 0x01) ? 0x00 : 0x40;

	/* write protection bit*/
	ret |= (info->dev_protection & PROTECT_STATUS) ? 0x00 : 0x80;

	return ret;
}

static void sp_spinand_read_buf(struct mtd_info *mtd, u8 *buf, int len)
{
	struct sp_spinand_info *info = get_spinand_info();
	u32 value;

	if (info->buff.idx == 0) {
		switch (info->cmd) {
		case NAND_CMD_READOOB:
		case NAND_CMD_READ0: {
			u32 size = info->page_size + info->oob_size - info->col;
			sp_spinand_read_raw(info, info->row, info->col, size);
			#ifndef CONFIG_SPINAND_USE_SRAM
			invalidate_dcache_range( (unsigned long)info->buff.virt,
				(unsigned long)info->buff.virt + size);
			#endif
			break;
		}
		case NAND_CMD_READID:
			spi_nand_readid(info, info->col, info->buff.virt);
			break;
		case NAND_CMD_STATUS:
			value = spi_nand_getfeatures(info, DEVICE_STATUS_ADDR);
			*(u32 *)info->buff.virt  = (value&0x0c) ? 0x01 : 0x00;
			*(u32 *)info->buff.virt |= (value&0x01) ? 0x00 : 0x40;
			if(!(info->dev_protection & PROTECT_STATUS))
				*(u32 *)info->buff.virt |= 0X80;
			memcpy(buf, info->buff.virt + info->buff.idx, len);
			return;
		default:
			break;
		}
	}

	memcpy(buf, info->buff.virt + info->buff.idx, len);
	info->buff.idx += len;
}

static u8 sp_spinand_read_byte(struct mtd_info *mtd)
{
	u8 ret = 0;
	sp_spinand_read_buf(mtd, &ret, 1);
	return ret;
}

static void sp_spinand_write_buf(struct mtd_info *mtd, const u8 *buf, int len)
{
	struct sp_spinand_info *info = get_spinand_info();

	memcpy(info->buff.virt + info->buff.idx, buf, len);
	info->buff.idx += len;
}

static int sp_spinand_read_page(struct mtd_info *mtd, struct nand_chip *chip,
				u8 *buf, int oob_required, int page)
{
	struct sp_spinand_info *info = get_spinand_info();
	u8 *data_va = info->buff.virt;
	u8 *oob_va = data_va + mtd->writesize;
	dma_addr_t data_pa = info->buff.phys;
	dma_addr_t oob_pa = data_pa + mtd->writesize;
	int ret;

	if (info->trs_mode == SPINAND_TRS_DMA_AUTOBCH) {
		ret = spi_nand_pageread_autobch(info,
			info->read_bitmode, page, (u8*)data_pa);
	} else {
		ret = sp_spinand_read_raw(info,
			page, 0, mtd->writesize + mtd->oobsize);
		if(ret == 0)
			ret = sp_bch_decode(mtd, (void*)data_pa, (void*)oob_pa);
	}

	if (ret < 0) {
		pr_warn("sp_spinand: bch decode failed at page=%d\n", page);
		return ret;
	}

	#ifndef CONFIG_SPINAND_USE_SRAM
	invalidate_dcache_range( (unsigned long)data_va,
		(unsigned long)data_va + mtd->writesize + mtd->oobsize);
	#endif

	memcpy(buf, data_va, mtd->writesize);
	if (oob_required)
		memcpy(chip->oob_poi, oob_va, mtd->oobsize);
	return 0;
}

static int sp_spinand_write_page(struct mtd_info *mtd, struct nand_chip *chip,
				 const u8 *buf, int oob_required, int page)
{
	struct sp_spinand_info *info = get_spinand_info();
	u8 *data_va = info->buff.virt;
	u8 *oob_va = data_va + mtd->writesize;
	dma_addr_t data_pa = info->buff.phys;
	dma_addr_t oob_pa = data_pa + mtd->writesize;
	int ret;

	memcpy(data_va, buf, mtd->writesize);
	memcpy(oob_va, chip->oob_poi, mtd->oobsize);

	#ifndef CONFIG_SPINAND_USE_SRAM
	flush_dcache_range( (unsigned long)data_va,
		(unsigned long)data_va + mtd->writesize + mtd->oobsize);
	#endif

	if(info->trs_mode == SPINAND_TRS_DMA_AUTOBCH) {
		ret = spi_nand_pagewrite_autobch(info,
			info->write_bitmode, info->row, (u8*)data_pa);
	} else {
		sp_bch_encode(mtd, (void*)data_pa, (void*)oob_pa);
		ret = sp_spinand_write_raw(info,
			info->row, 0, mtd->writesize+mtd->oobsize);
	}
	return ret;
}

static int sp_spinand_init(struct sp_spinand_info *info)
{
	struct nand_chip *nand = &info->nand;
	struct mtd_info *mtd = &nand->mtd;
	u32 value;
	int ret;

	info->mtd = mtd;
	nand->IO_ADDR_R = nand->IO_ADDR_W = info->regs;

	info->buff.size = CONFIG_SPINAND_BUF_SZ;
	#ifdef CONFIG_SPINAND_USE_SRAM
	info->buff.virt = (u8*)CONFIG_SPINAND_SRAM_ADDR;
	#else
	u8 *buf = (u8*)malloc(info->buff.size + CONFIG_SYS_CACHELINE_SIZE);
	/* the buff should be cacheline-aligned */
	buf = buf + CONFIG_SYS_CACHELINE_SIZE - 1;
	info->buff.virt = (u8*)((u32)buf & (~(CONFIG_SYS_CACHELINE_SIZE-1)));
	#endif
	info->buff.phys = (dma_addr_t)info->buff.virt;
	pr_info("sp_spinand: buff=0x%p@0x%08x size=%u\n", info->buff.virt,
		info->buff.phys, info->buff.size);

	info->nand.select_chip = sp_spinand_select_chip;
	info->nand.cmd_ctrl = sp_spinand_cmd_ctrl;
	info->nand.cmdfunc = sp_spinand_cmdfunc;
	info->nand.dev_ready = sp_spinand_dev_ready;
	info->nand.waitfunc = sp_spinand_waitfunc;

	info->nand.read_byte = sp_spinand_read_byte;
	info->nand.read_buf = sp_spinand_read_buf;
	info->nand.write_buf = sp_spinand_write_buf;

	info->nand.ecc.read_page = sp_spinand_read_page;
	info->nand.ecc.write_page = sp_spinand_write_page;
	info->nand.ecc.layout = &info->ecc_layout;
	info->nand.ecc.mode = NAND_ECC_HW;

	if (spi_nand_reset(info) < 0)
		return -EIO;

	/* Read ID */
	spi_nand_readid(info, 0, (u8*)&info->id);

	ret = nand_scan_ident(mtd, 1, (struct nand_flash_dev *)sp_spinand_ids);
	if (ret < 0) {
		pr_warn("sp_spinand: Unsupport device(id=0x%08x)!\n", info->id);
		return ret;
	}

	if (info->nand.drv_options & SPINAND_OPT_HAS_TWO_PLANE) {
		info->plane_sel_mode = (0x1000<<16) | (0x40<<2) | 3;
	} else {
		info->plane_sel_mode = 0;
	}
	info->page_size = mtd->writesize;
	info->oob_size = mtd->oobsize;

	info->trs_mode = CONFIG_DEFAULT_TRSMODE;
	info->raw_trs_mode = CONFIG_DEFAULT_TRSMODE_RAW;
	info->read_bitmode = SPINAND_4BIT_MODE;
	info->write_bitmode = SPINAND_4BIT_MODE;
	if (info->nand.drv_options & SPINAND_OPT_NO_4BIT_PROGRAM)
		info->write_bitmode = SPINAND_1BIT_MODE;

	info->spi_clk_div = 1;
	SPINAND_SET_CLKSEL(14);   /* 202.5MHZ */

	value = spi_nand_getfeatures(info, DEVICE_FEATURE_ADDR);
	value &= ~0x10;          /* disable internal ECC */
	if (info->nand.drv_options & SPINAND_OPT_HAS_BUF_BIT)
		value |= 0x08;   /* use buffer read mode */
	if (info->nand.drv_options & SPINAND_OPT_HAS_CONTI_RD)
		value &= ~0x01;  /* disable continuous read mode */
	if (info->nand.drv_options & SPINAND_OPT_HAS_QE_BIT)
		value |= 0x01;   /* enable quad io */
	spi_nand_setfeatures(info, DEVICE_FEATURE_ADDR, value);

	/* close write protection */
	spi_nand_setfeatures(info, DEVICE_PROTECTION_ADDR, 0x0);
	info->dev_protection=spi_nand_getfeatures(info, DEVICE_PROTECTION_ADDR);

	pr_info("spi nand device info:\n");
	pr_info("\tdevice name : %s\n", mtd->name);
	pr_info("\tdevice id   : 0x%08x\n", info->id);
	pr_info("\toptions     : 0x%08x\n", nand->options);
	pr_info("\tdrv options : 0x%08x\n", nand->drv_options);
	pr_info("\tblock size  : %d\n", mtd->erasesize);
	pr_info("\tpage size   : %d\n", mtd->writesize);
	pr_info("\toob size    : %d\n", mtd->oobsize);

	ret = sp_bch_init(mtd);
	if (ret < 0) {
		pr_err("sp_spinand: initialize BCH controller fail!\n");
		return ret;
	}

	pr_info("\toob avail   : %d\n", nand->ecc.layout->oobavail);
	pr_info("\tecc size    : %d\n", nand->ecc.size);
	pr_info("\tecc strength: %d\n", nand->ecc.strength);
	pr_info("\tecc steps   : %d\n", nand->ecc.steps);
	pr_info("\tecc options : 0x%08x\n", nand->ecc.options);
	ret = nand_scan_tail(mtd);
	if (ret < 0)
		return ret;

	nand_register(0, mtd);

	return 0;
}

static int sp_spinand_probe(struct udevice *dev)
{
	struct resource res;
	int ret;
	struct sp_spinand_info *info;

	info = our_spinfc = dev_get_priv(dev);

	/* get spi-nand reg */
	ret = dev_read_resource_byname(dev, "spinand_reg", &res);
	if (ret)
		return ret;

	info->regs = devm_ioremap(dev, res.start, resource_size(&res));
	pr_info("\nsp_spinand: regs@0x%p\n", info->regs);

	/* get bch reg */
	ret = dev_read_resource_byname(dev, "bch_reg", &res);
	if (ret)
		return ret;

	info->bch_regs = devm_ioremap(dev, res.start, resource_size(&res));
	pr_info("sp_bch    : regs@0x%p\n", info->bch_regs);

	ret = sp_spinand_init(info);
	return ret;

}

static const struct udevice_id sunplus_spinand[] = {
	{
		.compatible = "sunplus,sunplus-q628-spinand",
	},
};


U_BOOT_DRIVER(pentagram_spi_nand) = {
	.name                 = "pentagram_spi_nand",
	.id                   = UCLASS_MTD,
	.of_match             = sunplus_spinand,
	.priv_auto_alloc_size = sizeof(struct sp_spinand_info),
	.probe                = sp_spinand_probe,
};

void board_spinand_init(void)
{
	struct udevice *dev;
	int ret;

	ret = uclass_get_device_by_driver(UCLASS_MTD,
					  DM_GET_DRIVER(pentagram_spi_nand),
					  &dev);

	if (ret && ret != -ENODEV)
		pr_warn("Failed to initialize sunplus SPI NAND controller.(error %d)\n", ret);
}

void pattern_generate(u32 data, u8 *buf, u32 size)
{
	u32 i = 0;
	u8 *p = (u8 *)&data;
	for(i=0; i<size; i++) {
		buf[i] = *(p+(i&0x03));
		if((i&0x03) == 0x03)
			data++;
	}
}

int pattern_check(u32 data, u8 *buf, u32 size)
{
	int i = 0;
	u8 *p = (u8*)&data;
	for(i=0; i<size; i++) {
		if(buf[i] != *(p+(i&0x03))) {
			pr_warn("data mismatch at %d\n", i);
			return -1;
		}
		if((i&0x03) == 0x03)
			data++;
	}
	return 0;
}

static int sp_spinand_test_speed(void)
{
	#define TEST_BLOCK_NUM (10)
	const char *trs_mode[] = {"pio", "pio+auto", "dma", "dma+autobch"};
	const char *io_mode[] = {"x1", "x2", "x4", "dualIO", "quadIO"};
	const u32 clk_div[] = {0, 2, 4, 6, 8, 16, 24, 32};
	const u32 clk_sel[] = {
		0,      0,      0,      0,      67500,  81000,  94500,  108000,
		121500,	135000, 148500, 162000, 175500, 189000, 202500, 0};
	struct sp_spinand_info *info = get_spinand_info();
	struct mtd_info *mtd = info->mtd;
	nand_erase_options_t erase_opts;
	u32 test_block_table[TEST_BLOCK_NUM];
	unsigned long now_time;
	unsigned long used_time;
	unsigned long speed;
	unsigned long average;
	unsigned long data_size = mtd->erasesize * TEST_BLOCK_NUM;
	u32 clksel = SPINAND_GET_CLKSEL();
	u32 freq;
	u32 actual_size;
	u8  *buf = (u8*)0;
	u32 i, j;
	int ret;

	for(i=0,j=3; i<TEST_BLOCK_NUM; i++,j++) {
		while(nand_block_isbad(mtd, j*mtd->erasesize))
			j++;
		test_block_table[i] = j*mtd->erasesize;
	}

	pattern_generate(0, buf, data_size);

	freq = clk_sel[clksel]/clk_div[info->spi_clk_div];
	pr_info("[freq=%d.%d MHZ, "
		"clk_sel:%d, "
		"clk_div:%d, "
		"trs_mode:%s, "
		"read_io:%s, "
		"write_io:%s]\n",
		freq/1000,
		freq%1000,
		clksel,
		info->spi_clk_div,
		trs_mode[info->trs_mode],
		io_mode[info->read_bitmode],
		io_mode[info->write_bitmode]);
	pr_info("Erase: ");
	now_time = get_timer(0);
	for(i=0,j=TEST_BLOCK_NUM; i<TEST_BLOCK_NUM; i++) {
		memset(&erase_opts, 0, sizeof(erase_opts));
		erase_opts.quiet = 1;
		erase_opts.offset = test_block_table[i];
		erase_opts.length = mtd->erasesize;
		ret = nand_erase_opts(mtd, &erase_opts);
		if(ret) {
			pr_info("failed at 0x%08x\n", test_block_table[i]);
			return -1;
		}
	}
	used_time = get_timer(now_time);
	average = used_time * 100 / TEST_BLOCK_NUM;
	speed = ((data_size>>10) * 1000 * 100 / used_time) >> 10; //unit:100MB/s
	pr_info("total_size=%ld Byte, "
		"total_time:%ld ms, "
		"average:%ld.%ld ms/block, "
		"speed:%ld.%ld MB/s\n",
		data_size,
		used_time,
		average/100, average%100,
		speed/100, speed%100);

	pr_info("Write: ");
	now_time = get_timer(0);
	for(i=0,j=TEST_BLOCK_NUM; i<TEST_BLOCK_NUM; i++) {
		actual_size = mtd->erasesize;
		ret = nand_write(mtd, test_block_table[i],
				&actual_size, buf+mtd->erasesize*i);
		if(ret) {
			pr_info("failed at 0x%08x\n", test_block_table[i]);
			return -1;
		}
	}
	used_time = get_timer(now_time);
	average = used_time * 100 / TEST_BLOCK_NUM;
	speed = ((data_size>>10) * 1000 * 100 / used_time) >> 10; //unit:100MB/s
	pr_info("total_size=%ld Byte, "
		"total_time:%ld ms, "
		"average:%ld.%ld ms/block, "
		"speed:%ld.%ld MB/s\n",
		data_size,
		used_time,
		average/100, average%100,
		speed/100, speed%100);

	memset(buf, 0, data_size);

	pr_info("Read: ");
	now_time = get_timer(0);
	for(i=0,j=TEST_BLOCK_NUM; i<TEST_BLOCK_NUM; i++) {
		actual_size = mtd->erasesize;
		ret = nand_read(mtd, test_block_table[i],
				&actual_size, buf+mtd->erasesize*i);
		if(ret) {
			pr_info("failed at :0x%08x\n\n",test_block_table[i]);
			return -1;
		}
	}
	used_time = get_timer(now_time);

	if(pattern_check(0, buf, data_size)) {
		pr_info("failed for data mismatch\n\n");
		return -1;
	}
	average = used_time * 100 / TEST_BLOCK_NUM;
	speed = ((data_size>>10) * 1000 * 100 / used_time) >> 10; //unit:100MB/s
	pr_info("total_size=%ld Byte, "
		"total_time:%ld ms, "
		"average:%ld.%ld ms/block, "
		"speed:%ld.%ld MB/s\n\n",
		data_size,
		used_time,
		average/100, average%100,
		speed/100, speed%100);

	return 0;
}

static int sp_spinand_test_stress(void)
{
	struct sp_spinand_info *info = get_spinand_info();
	struct mtd_info *mtd = info->mtd;
	struct nand_chip *nand = &info->nand;
	nand_erase_options_t erase_opts;
	u32 blocks = nand->chipsize / mtd->erasesize;
	u32 data_size = mtd->erasesize;
	u32 actual_size;
	u32 progress;
	u32 offset;
	u8  *src_buf = (u8*)0;
	u8  *dst_buf = src_buf + data_size;
	u8  *block_table = dst_buf + data_size;
	u32 i, j, k;
	int ret;

	pattern_generate(0, src_buf, data_size);

	for(k=0; ; k++) {
		memset(block_table, 0, blocks);
		for(i=0,j=0,progress=0; i<blocks; i++) {
			/* erase */
			memset(&erase_opts, 0, sizeof(erase_opts));
			offset = mtd->erasesize * i;
			erase_opts.offset = offset;
			erase_opts.length = mtd->erasesize;
			erase_opts.quiet = 1;
			ret = nand_erase_opts(mtd, &erase_opts);
			if(ret) {
				block_table[i] = 0xff;
				j++;
				continue;
			}

			/* write */
			actual_size = mtd->erasesize;
			ret = nand_write(mtd,  offset, &actual_size, src_buf);
			if (ret || actual_size!=mtd->erasesize) {
				block_table[i] = 0xff;
				j++;
				continue;
			}

			/* read */
			actual_size = mtd->erasesize;
			ret = nand_read(mtd, offset, &actual_size, dst_buf);
			if (ret
				|| actual_size != mtd->erasesize
				|| pattern_check(0, dst_buf, data_size)<0) {
				block_table[i] = 0xff;
				j++;
				continue;
			}
			block_table[i] = 1;
			if(progress != i*100/blocks) {
				progress = i*100/blocks;
				pr_info("\rprgress : %d%%", progress);
			}
		}
		pr_info("\r==> %d round, %d block fail\n", k, j);
		if(j > 0) {
			pr_info("bad blocks:\n");
			for (i=0; i < blocks; i++) {
				if(block_table[i] == 0xff)
					pr_info("0x%08x\n", i*mtd->erasesize);
			}
		}
	}

	return 0;
}

static int sp_spinand_test_stress1(u32 block)
{
	struct sp_spinand_info *info = get_spinand_info();
	struct mtd_info *mtd = info->mtd;
	nand_erase_options_t erase_opts;
	u32 row_addr = mtd->erasesize * block;
	u32 data_size = mtd->erasesize;
	u32 actual_size;
	u8  *src_buf = (u8*)0;
	u8  *dst_buf = src_buf + data_size;
	u32 k;
	int ret;

	pattern_generate(0, src_buf, data_size);

	for(k=0; ; k++) {
		/* erase */
		memset(&erase_opts, 0, sizeof(erase_opts));
		erase_opts.quiet = 1;
		erase_opts.offset = row_addr;
		erase_opts.length = mtd->erasesize;
		ret = nand_erase_opts(mtd, &erase_opts);
		if(ret) {
			pr_info("\nErase fail at 0x%08x\n", row_addr);
			break;
		}

		/* write */
		actual_size = mtd->erasesize;
		ret = nand_write(mtd,  row_addr, &actual_size, src_buf);
		if (ret || actual_size!=mtd->erasesize) {
			pr_info("\nWrite fail at 0x%08x\n", row_addr);
			break;
		}

		/* read */
		actual_size = mtd->erasesize;
		ret = nand_read(mtd, row_addr, &actual_size, dst_buf);
		if (ret
			|| actual_size != mtd->erasesize
			|| pattern_check(0, dst_buf, data_size)<0) {
			pr_info("\nWrite fail at 0x%08x\n", row_addr);
			break;
		}

		if(k%10 == 0) {
			pr_info("\r%d round test pass!", k);
		}
	}

	pr_info("Stress test exit at %d round.\n", k);
	return 0;
}

static int sp_spinand_test_timing(void)
{
	u32 i;
	for(i=5; i<15; i++) {
		SPINAND_SET_CLKSEL(i);
		sp_spinand_test_speed();

	}
	return 0;
}

static int sp_spinand_test(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	char *cmd = argv[1];
	int ret = 0;
	struct sp_spinand_info *info = get_spinand_info();
	struct sp_spinand_regs *regs = info->regs;

	if (argc < 2)
		return CMD_RET_USAGE;

	if (strncmp(cmd, "reset", 5) == 0) {
		spi_nand_reset(info);
	} else if (strncmp(cmd, "regs", 4) == 0) {
		dump_spi_regs(info);
	} else if (strncmp(cmd, "feature", 7) == 0) {
		u32 addr, value;
		if (argc == 2) {
			pr_info("feature(A0) = 0x%02x\n",
				spi_nand_getfeatures(info, 0xA0));
			pr_info("feature(B0) = 0x%02x\n",
				spi_nand_getfeatures(info, 0xB0));
			pr_info("feature(C0) = 0x%02x\n",
				spi_nand_getfeatures(info, 0xC0));
		} else {
			addr = simple_strtoul(argv[2], NULL, 16);
			if(addr!=0xA0 && addr!=0xB0 && addr!=0xC0 && addr!=0xD0){
				pr_info("invalid addr(0x%02x)!\n",addr);
				ret = CMD_RET_USAGE;
			} else {
				if(argc == 4) {
					value = simple_strtoul(argv[3],NULL,16);
					value &= 0xff;
					spi_nand_setfeatures(info, addr, value);
					pr_info("set feature(%02X) to 0x%02x\n",
						addr, value);
				} else {
					pr_info("feature(%02X) = 0x%02x\n",addr,
						spi_nand_getfeatures(info, addr));
				}
			}
		}
	} else if (strncmp(cmd, "id", 2) == 0) {
		u32 devid = 0;
		spi_nand_readid(info, 0, (u8*)&devid);
		pr_info("device id: 0x%08x\n", devid);
	} else if (strncmp(cmd, "speed", 5) == 0) {
		sp_spinand_test_speed();
	} else if (strncmp(cmd, "stress", 6) == 0) {
		u32 block = -1;
		if(argc >= 3)
			block = simple_strtol(argv[2], NULL, 10);
		if(block == -1)
			sp_spinand_test_stress();
		else
			sp_spinand_test_stress1(block);
	} else if (strncmp(cmd, "timing", 6) == 0) {
		sp_spinand_test_timing();
	} else if (strncmp(cmd, "trsmode", 7) == 0) {
		if (argc >= 3) {
			u32 trsmode = simple_strtoul(argv[2], NULL, 10);
			if (trsmode >= SPINAND_TRS_MAX) {
				pr_info("invalid value!\n");
				ret = CMD_RET_USAGE;
			} else {
				info->trs_mode = trsmode;
				pr_info("set trs_mode => %d\n", trsmode);
			}
		} else {
			pr_info("trs_mode = %d\n", info->trs_mode);
		}
	}
	else if (strncmp(cmd, "wio", 3) == 0) {
		if (argc >= 3) {
			u32 bitmode = simple_strtoul(argv[2], NULL, 10);
			if (bitmode != SPINAND_1BIT_MODE
				&& bitmode != SPINAND_4BIT_MODE) {
				pr_info("invalid value!\n");
				ret = CMD_RET_USAGE;
			} else {
				info->write_bitmode = bitmode;
				pr_info("set write_bitmode => %d\n", bitmode);
			}
		} else {
			pr_info("write_bitmode = %d\n", info->write_bitmode );
		}
	} else if (strncmp(cmd, "rio", 3) == 0) {
		if (argc >= 3) {
			u32 bitmode = simple_strtoul(argv[2], NULL, 10);
			if(bitmode > SPINAND_4BIT_MODE) {
				pr_info("invalid value!\n");
				ret = CMD_RET_USAGE;
			} else {
				info->read_bitmode = bitmode;
				pr_info("set read_bitmode => %d\n", bitmode);
			}
		} else {
			pr_info("read_bitmode = %d\n", info->read_bitmode);
		}
	} else if (strncmp(cmd, "clksel", 6) == 0) {
		if (argc >= 3) {
			u32 clksel = simple_strtoul(argv[2], NULL, 10);
			if (clksel < 4 && clksel>14) {
				pr_info("invalid value!\n");
				ret = CMD_RET_USAGE;
			} else {
				SPINAND_SET_CLKSEL(clksel);
				pr_info("set spi_clk_sel => %d\n", clksel);
			}
		} else {
			pr_info("spi_clk_sel = %d\n", SPINAND_GET_CLKSEL());
		}
	} else if (strncmp(cmd, "clkdiv", 6) == 0) {
		if (argc >= 3) {
			u32 clkdiv = simple_strtoul(argv[2], NULL, 10);
			if (clkdiv < 1 && clkdiv>7) {
				pr_info("invalid value!\n");
				ret = CMD_RET_USAGE;
			} else {
				info->spi_clk_div = clkdiv;
				pr_info("set spi_clk_div => %d\n", clkdiv);
			}
		} else {
			pr_info("spi_clk_div = %d\n", info->spi_clk_div);
		}
	} else if (strncmp(cmd, "rts", 3) == 0) {
		u32 rts;
		if(argc >= 3) {
			rts = simple_strtoul(argv[2], NULL, 10);
			if(rts > 7) {
				pr_info("invalid value!\n");
				ret = CMD_RET_USAGE;
			} else {
				writel(SPINAND_READ_TIMING(rts), &regs->spi_timing);
				pr_info("set rts => %d\n", rts);
			}
		} else {
			rts = readl(&regs->spi_timing);
			pr_info("rts = %d\n",(rts>>1)&0x07);
		}
	} else if (strncmp(cmd, "gpioset", 7) == 0) {
		if (argc >= 4) {
			u32 port = simple_strtoul(argv[2], NULL, 10);
			u32 level = simple_strtoul(argv[3], NULL, 10);
			level = level ? 1 : 0;
			GPIO_DEBUG_SET(port, level);
			pr_info("set gpio[%d] to %d\n", port, level);
		} else {
			ret = CMD_RET_USAGE;
		}
	} else if (strncmp(cmd, "gpioget", 7) == 0) {
		if (argc >= 3) {
			u32 port = simple_strtoul(argv[2], NULL, 10);
			u32 level = GPIO_I_GET(port);
			pr_info("gpio[%d] = %d\n", port, level);
		} else {
			ret = CMD_RET_USAGE;
		}
	} else {
		ret = CMD_RET_USAGE;
	}

	return ret;
}

U_BOOT_CMD(ssnand, CONFIG_SYS_MAXARGS, 1, sp_spinand_test,
	"sunplus spi-nand tests",
	"reset  - send reset cmd to device.\n"
	"ssnand regs  - dump spi-nand registers.\n"
	"ssnand id  - send ReadId cmd to device to achieve device id.\n"
	"ssnand feature [addr] [value]- get/set the device's feature values.\n"
	"\t if 'addr' is not set, it shows all feature values.\n"
	"\t if 'value' is not set, it shows the feature value of 'addr'.\n"
	"\t if 'value' is set, it sets 'value' to feature of 'addr'.\n"
	"ssnand speed - test erase,write,read the speed.\n"
	"ssnand stress [block] - do stress test on 'block',\n"
	"\tif 'block' is -1 or not specified, all blocks would be tested.\n"
	"ssnand timing - timing test\n"
	"ssnand trsmode [value] - set/show trs_mode, 0~3 are allowed.\n"
	"ssnand wio [value] - set/show write_bitmode, 0/2 are allowed.\n"
	"ssnand rio [value] - set/show read_bitmode, 0/1/2 are allowed.\n"
	"ssnand clksel [value] - set/show spi_clk_sel, 4~15 are allowed.\n"
	"ssnand clkdiv [value] - set/show spi_clk_div, 1~7 are allowed.\n"
	"ssnand rts [value] - set/show rts(read timing select). 0~7 are allowed.\n "
);


