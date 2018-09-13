/*
 * Sunplus Technology
 * SPDX-License-Identifier:     GPL-2.0+
 */

#ifndef __SP_BCH_H
#define __SP_BCH_H

struct sp_bch_regs {
	uint32_t cr0;		/* control register */
	uint32_t buf;		/* data base address register */
	uint32_t ecc;		/* parity base address register */
	uint32_t isr;		/* interrupt status register */

	uint32_t srr;		/* software reset register */
	uint32_t ier;		/* interrupt enable register */
	uint32_t sr;		/* status register */
	uint32_t esr;		/* error status register */

	uint32_t fsr;		/* failure status register */
	uint32_t res[2];	/* reserved*/
	uint32_t cr1;		/* control register */
};

#define CR0_START			BIT(0)
#define CR0_ENCODE			0
#define CR0_DECODE			BIT(4)

/* Correct mode */
#define CR0_CMODE_1024x60	(0 << 8)
#define CR0_CMODE_1024x40	(1 << 8)
#define CR0_CMODE_1024x24	(2 << 8)
#define CR0_CMODE_1024x16	(3 << 8)
#define CR0_CMODE_512x8		(4 << 8)
#define CR0_CMODE_512x4		(5 << 8)

#define CR0_CMODE(n)		(((n) & 7) << 8)
#define CR0_DMODE(n)		((n) ?  BIT(11) : 0)
#define CR0_NBLKS(n)		((((n) - 1) & 0x1f) << 16)
#define CR0_BMODE(n)		(((n) & 7) << 28)

/* 32-bits word aligned */
#define CR1_WORD_ALIGN		BIT(0)
/* Non-buffered write */
#define CR1_NBWR			BIT(1)
/* Non-buffered write for last correction write only */
#define CR1_NBWR_LAST		BIT(2)
/* Threshold for blank check (0x00 or 0xff) */
#define CR1_BLANK_THRES(n)	(((n) & 0x3ff) << 4)

#define ISR_BCH				BIT(0)
#define ISR_BUSY			BIT(4)
#define ISR_CURR_DBLK(x)	(((x) >> 8) & 0x1f)
#define ISR_CURR_CBLK(x)	(((x) >> 16) & 0x1f)
#define IER_DONE			BIT(0)
#define IER_FAIL			BIT(1)
#define SRR_RESET			BIT(0)
#define SR_DONE				BIT(0)
#define SR_FAIL				BIT(4)
/* (((x) >> 5) & 0xfff) */
#define SR_ERR_BITS(x)		(((x) >> 8) & 0x7ff)
/* (((x) >> 17) & 0x3f) */
#define SR_ERR_MAX(x)		(((x) >> 20) & 0x7f)

struct sp_bch_req {
	uint8_t buf[1024];
	uint8_t ecc[128];
};

int sp_bch_init(struct mtd_info *mtd);
int sp_bch_encode(struct mtd_info *mtd, void *buf, void *ecc);
int sp_bch_decode(struct mtd_info *mtd, void *buf, void *ecc);

int sp_bch_encode_1024x60(void *buf, void *ecc);
int sp_bch_decode_1024x60(void *buf, void *ecc);

#endif /* __SP_BCH_H */
