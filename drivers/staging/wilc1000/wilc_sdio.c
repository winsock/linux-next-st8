/* ////////////////////////////////////////////////////////////////////////// */
/*  */
/* Copyright (c) Atmel Corporation.  All rights reserved. */
/*  */
/* Module Name:  wilc_sdio.c */
/*  */
/*  */
/* //////////////////////////////////////////////////////////////////////////// */

#include <linux/string.h>
#include "wilc_wlan_if.h"
#include "wilc_wlan.h"
#include "linux_wlan_sdio.h"
#include "wilc_wfi_netdevice.h"

#define WILC_SDIO_BLOCK_SIZE 512

typedef struct {
	bool irq_gpio;
	u32 block_size;
	wilc_debug_func dPrint;
	int nint;
#define MAX_NUN_INT_THRPT_ENH2 (5) /* Max num interrupts allowed in registers 0xf7, 0xf8 */
	int has_thrpt_enh3;
} wilc_sdio_t;

static wilc_sdio_t g_sdio;

static int sdio_write_reg(struct wilc *wilc, u32 addr, u32 data);
static int sdio_read_reg(struct wilc *wilc, u32 addr, u32 *data);

/********************************************
 *
 *      Function 0
 *
 ********************************************/

static int sdio_set_func0_csa_address(struct wilc *wilc, u32 adr)
{
	sdio_cmd52_t cmd;

	/**
	 *      Review: BIG ENDIAN
	 **/
	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x10c;
	cmd.data = (u8)adr;
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0x10c data...\n");
		goto _fail_;
	}

	cmd.address = 0x10d;
	cmd.data = (u8)(adr >> 8);
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0x10d data...\n");
		goto _fail_;
	}

	cmd.address = 0x10e;
	cmd.data = (u8)(adr >> 16);
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0x10e data...\n");
		goto _fail_;
	}

	return 1;
_fail_:
	return 0;
}

static int sdio_set_func0_block_size(struct wilc *wilc, u32 block_size)
{
	sdio_cmd52_t cmd;

	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x10;
	cmd.data = (u8)block_size;
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0x10 data...\n");
		goto _fail_;
	}

	cmd.address = 0x11;
	cmd.data = (u8)(block_size >> 8);
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0x11 data...\n");
		goto _fail_;
	}

	return 1;
_fail_:
	return 0;
}

/********************************************
 *
 *      Function 1
 *
 ********************************************/

static int sdio_set_func1_block_size(struct wilc *wilc, u32 block_size)
{
	sdio_cmd52_t cmd;

	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x110;
	cmd.data = (u8)block_size;
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0x110 data...\n");
		goto _fail_;
	}
	cmd.address = 0x111;
	cmd.data = (u8)(block_size >> 8);
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0x111 data...\n");
		goto _fail_;
	}

	return 1;
_fail_:
	return 0;
}

static int sdio_clear_int(struct wilc *wilc)
{
	if (!g_sdio.irq_gpio) {
		/* u32 sts; */
		sdio_cmd52_t cmd;

		cmd.read_write = 0;
		cmd.function = 1;
		cmd.raw = 0;
		cmd.address = 0x4;
		cmd.data = 0;
		wilc_sdio_cmd52(wilc, &cmd);

		return cmd.data;
	} else {
		u32 reg;

		if (!sdio_read_reg(wilc, WILC_HOST_RX_CTRL_0, &reg)) {
			g_sdio.dPrint(N_ERR, "[wilc spi]: Failed read reg (%08x)...\n", WILC_HOST_RX_CTRL_0);
			return 0;
		}
		reg &= ~0x1;
		sdio_write_reg(wilc, WILC_HOST_RX_CTRL_0, reg);
		return 1;
	}

}

/********************************************
 *
 *      Sdio interfaces
 *
 ********************************************/
static int sdio_write_reg(struct wilc *wilc, u32 addr, u32 data)
{
#ifdef BIG_ENDIAN
	data = BYTE_SWAP(data);
#endif

	if ((addr >= 0xf0) && (addr <= 0xff)) {
		sdio_cmd52_t cmd;

		cmd.read_write = 1;
		cmd.function = 0;
		cmd.raw = 0;
		cmd.address = addr;
		cmd.data = data;
		if (!wilc_sdio_cmd52(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd 52, read reg (%08x) ...\n", addr);
			goto _fail_;
		}
	} else {
		sdio_cmd53_t cmd;

		/**
		 *      set the AHB address
		 **/
		if (!sdio_set_func0_csa_address(wilc, addr))
			goto _fail_;

		cmd.read_write = 1;
		cmd.function = 0;
		cmd.address = 0x10f;
		cmd.block_mode = 0;
		cmd.increment = 1;
		cmd.count = 4;
		cmd.buffer = (u8 *)&data;
		cmd.block_size = g_sdio.block_size; /* johnny : prevent it from setting unexpected value */

		if (!wilc_sdio_cmd53(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd53, write reg (%08x)...\n", addr);
			goto _fail_;
		}
	}

	return 1;

_fail_:

	return 0;
}

static int sdio_write(struct wilc *wilc, u32 addr, u8 *buf, u32 size)
{
	u32 block_size = g_sdio.block_size;
	sdio_cmd53_t cmd;
	int nblk, nleft;

	cmd.read_write = 1;
	if (addr > 0) {
		/**
		 *      has to be word aligned...
		 **/
		if (size & 0x3) {
			size += 4;
			size &= ~0x3;
		}

		/**
		 *      func 0 access
		 **/
		cmd.function = 0;
		cmd.address = 0x10f;
	} else {
		/**
		 *      has to be word aligned...
		 **/
		if (size & 0x3) {
			size += 4;
			size &= ~0x3;
		}

		/**
		 *      func 1 access
		 **/
		cmd.function = 1;
		cmd.address = 0;
	}

	nblk = size / block_size;
	nleft = size % block_size;

	if (nblk > 0) {
		cmd.block_mode = 1;
		cmd.increment = 1;
		cmd.count = nblk;
		cmd.buffer = buf;
		cmd.block_size = block_size;
		if (addr > 0) {
			if (!sdio_set_func0_csa_address(wilc, addr))
				goto _fail_;
		}
		if (!wilc_sdio_cmd53(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd53 [%x], block send...\n", addr);
			goto _fail_;
		}
		if (addr > 0)
			addr += nblk * block_size;
		buf += nblk * block_size;
	}

	if (nleft > 0) {
		cmd.block_mode = 0;
		cmd.increment = 1;
		cmd.count = nleft;
		cmd.buffer = buf;

		cmd.block_size = block_size; /* johnny : prevent it from setting unexpected value */

		if (addr > 0) {
			if (!sdio_set_func0_csa_address(wilc, addr))
				goto _fail_;
		}
		if (!wilc_sdio_cmd53(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd53 [%x], bytes send...\n", addr);
			goto _fail_;
		}
	}

	return 1;

_fail_:

	return 0;
}

static int sdio_read_reg(struct wilc *wilc, u32 addr, u32 *data)
{
	if ((addr >= 0xf0) && (addr <= 0xff)) {
		sdio_cmd52_t cmd;

		cmd.read_write = 0;
		cmd.function = 0;
		cmd.raw = 0;
		cmd.address = addr;
		if (!wilc_sdio_cmd52(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd 52, read reg (%08x) ...\n", addr);
			goto _fail_;
		}
		*data = cmd.data;
	} else {
		sdio_cmd53_t cmd;

		if (!sdio_set_func0_csa_address(wilc, addr))
			goto _fail_;

		cmd.read_write = 0;
		cmd.function = 0;
		cmd.address = 0x10f;
		cmd.block_mode = 0;
		cmd.increment = 1;
		cmd.count = 4;
		cmd.buffer = (u8 *)data;

		cmd.block_size = g_sdio.block_size; /* johnny : prevent it from setting unexpected value */

		if (!wilc_sdio_cmd53(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd53, read reg (%08x)...\n", addr);
			goto _fail_;
		}
	}

#ifdef BIG_ENDIAN
	*data = BYTE_SWAP(*data);
#endif

	return 1;

_fail_:

	return 0;
}

static int sdio_read(struct wilc *wilc, u32 addr, u8 *buf, u32 size)
{
	u32 block_size = g_sdio.block_size;
	sdio_cmd53_t cmd;
	int nblk, nleft;

	cmd.read_write = 0;
	if (addr > 0) {
		/**
		 *      has to be word aligned...
		 **/
		if (size & 0x3) {
			size += 4;
			size &= ~0x3;
		}

		/**
		 *      func 0 access
		 **/
		cmd.function = 0;
		cmd.address = 0x10f;
	} else {
		/**
		 *      has to be word aligned...
		 **/
		if (size & 0x3) {
			size += 4;
			size &= ~0x3;
		}

		/**
		 *      func 1 access
		 **/
		cmd.function = 1;
		cmd.address = 0;
	}

	nblk = size / block_size;
	nleft = size % block_size;

	if (nblk > 0) {
		cmd.block_mode = 1;
		cmd.increment = 1;
		cmd.count = nblk;
		cmd.buffer = buf;
		cmd.block_size = block_size;
		if (addr > 0) {
			if (!sdio_set_func0_csa_address(wilc, addr))
				goto _fail_;
		}
		if (!wilc_sdio_cmd53(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd53 [%x], block read...\n", addr);
			goto _fail_;
		}
		if (addr > 0)
			addr += nblk * block_size;
		buf += nblk * block_size;
	}       /* if (nblk > 0) */

	if (nleft > 0) {
		cmd.block_mode = 0;
		cmd.increment = 1;
		cmd.count = nleft;
		cmd.buffer = buf;

		cmd.block_size = block_size; /* johnny : prevent it from setting unexpected value */

		if (addr > 0) {
			if (!sdio_set_func0_csa_address(wilc, addr))
				goto _fail_;
		}
		if (!wilc_sdio_cmd53(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd53 [%x], bytes read...\n", addr);
			goto _fail_;
		}
	}

	return 1;

_fail_:

	return 0;
}

/********************************************
 *
 *      Bus interfaces
 *
 ********************************************/

static int sdio_deinit(struct wilc *wilc)
{
	return 1;
}

static int sdio_sync(struct wilc *wilc)
{
	u32 reg;

	/**
	 *      Disable power sequencer
	 **/
	if (!sdio_read_reg(wilc, WILC_MISC, &reg)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed read misc reg...\n");
		return 0;
	}

	reg &= ~BIT(8);
	if (!sdio_write_reg(wilc, WILC_MISC, reg)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed write misc reg...\n");
		return 0;
	}

	if (g_sdio.irq_gpio) {
		u32 reg;
		int ret;

		/**
		 *      interrupt pin mux select
		 **/
		ret = sdio_read_reg(wilc, WILC_PIN_MUX_0, &reg);
		if (!ret) {
			g_sdio.dPrint(N_ERR, "[wilc spi]: Failed read reg (%08x)...\n", WILC_PIN_MUX_0);
			return 0;
		}
		reg |= BIT(8);
		ret = sdio_write_reg(wilc, WILC_PIN_MUX_0, reg);
		if (!ret) {
			g_sdio.dPrint(N_ERR, "[wilc spi]: Failed write reg (%08x)...\n", WILC_PIN_MUX_0);
			return 0;
		}

		/**
		 *      interrupt enable
		 **/
		ret = sdio_read_reg(wilc, WILC_INTR_ENABLE, &reg);
		if (!ret) {
			g_sdio.dPrint(N_ERR, "[wilc spi]: Failed read reg (%08x)...\n", WILC_INTR_ENABLE);
			return 0;
		}
		reg |= BIT(16);
		ret = sdio_write_reg(wilc, WILC_INTR_ENABLE, reg);
		if (!ret) {
			g_sdio.dPrint(N_ERR, "[wilc spi]: Failed write reg (%08x)...\n", WILC_INTR_ENABLE);
			return 0;
		}
	}

	return 1;
}

static int sdio_init(struct wilc *wilc, wilc_debug_func func)
{
	sdio_cmd52_t cmd;
	int loop;
	u32 chipid;

	memset(&g_sdio, 0, sizeof(wilc_sdio_t));

	g_sdio.dPrint = func;
	g_sdio.irq_gpio = (wilc->dev_irq_num);

	if (!wilc_sdio_init()) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed io init bus...\n");
		return 0;
	} else {
		return 0;
	}

	/**
	 *      function 0 csa enable
	 **/
	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 1;
	cmd.address = 0x100;
	cmd.data = 0x80;
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Fail cmd 52, enable csa...\n");
		goto _fail_;
	}

	/**
	 *      function 0 block size
	 **/
	if (!sdio_set_func0_block_size(wilc, WILC_SDIO_BLOCK_SIZE)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Fail cmd 52, set func 0 block size...\n");
		goto _fail_;
	}
	g_sdio.block_size = WILC_SDIO_BLOCK_SIZE;

	/**
	 *      enable func1 IO
	 **/
	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 1;
	cmd.address = 0x2;
	cmd.data = 0x2;
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio] Fail cmd 52, set IOE register...\n");
		goto _fail_;
	}

	/**
	 *      make sure func 1 is up
	 **/
	cmd.read_write = 0;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x3;
	loop = 3;
	do {
		cmd.data = 0;
		if (!wilc_sdio_cmd52(wilc, &cmd)) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Fail cmd 52, get IOR register...\n");
			goto _fail_;
		}
		if (cmd.data == 0x2)
			break;
	} while (loop--);

	if (loop <= 0) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Fail func 1 is not ready...\n");
		goto _fail_;
	}

	/**
	 *      func 1 is ready, set func 1 block size
	 **/
	if (!sdio_set_func1_block_size(wilc, WILC_SDIO_BLOCK_SIZE)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Fail set func 1 block size...\n");
		goto _fail_;
	}

	/**
	 *      func 1 interrupt enable
	 **/
	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 1;
	cmd.address = 0x4;
	cmd.data = 0x3;
	if (!wilc_sdio_cmd52(wilc, &cmd)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Fail cmd 52, set IEN register...\n");
		goto _fail_;
	}

	/**
	 *      make sure can read back chip id correctly
	 **/
	if (!sdio_read_reg(wilc, 0x1000, &chipid)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Fail cmd read chip id...\n");
		goto _fail_;
	}
	g_sdio.dPrint(N_ERR, "[wilc sdio]: chipid (%08x)\n", chipid);
	if ((chipid & 0xfff) > 0x2a0)
		g_sdio.has_thrpt_enh3 = 1;
	else
		g_sdio.has_thrpt_enh3 = 0;
	g_sdio.dPrint(N_ERR, "[wilc sdio]: has_thrpt_enh3 = %d...\n", g_sdio.has_thrpt_enh3);

	return 1;

_fail_:

	return 0;
}

static int sdio_read_size(struct wilc *wilc, u32 *size)
{

	u32 tmp;
	sdio_cmd52_t cmd;

	/**
	 *      Read DMA count in words
	 **/
	cmd.read_write = 0;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0xf2;
	cmd.data = 0;
	wilc_sdio_cmd52(wilc, &cmd);
	tmp = cmd.data;

	/* cmd.read_write = 0; */
	/* cmd.function = 0; */
	/* cmd.raw = 0; */
	cmd.address = 0xf3;
	cmd.data = 0;
	wilc_sdio_cmd52(wilc, &cmd);
	tmp |= (cmd.data << 8);

	*size = tmp;
	return 1;
}

static int sdio_read_int(struct wilc *wilc, u32 *int_status)
{

	u32 tmp;
	sdio_cmd52_t cmd;

	sdio_read_size(wilc, &tmp);

	/**
	 *      Read IRQ flags
	 **/
	if (!g_sdio.irq_gpio) {
		int i;

		cmd.function = 1;
		cmd.address = 0x04;
		cmd.data = 0;
		wilc_sdio_cmd52(wilc, &cmd);

		if (cmd.data & BIT(0))
			tmp |= INT_0;
		if (cmd.data & BIT(2))
			tmp |= INT_1;
		if (cmd.data & BIT(3))
			tmp |= INT_2;
		if (cmd.data & BIT(4))
			tmp |= INT_3;
		if (cmd.data & BIT(5))
			tmp |= INT_4;
		if (cmd.data & BIT(6))
			tmp |= INT_5;
		for (i = g_sdio.nint; i < MAX_NUM_INT; i++) {
			if ((tmp >> (IRG_FLAGS_OFFSET + i)) & 0x1) {
				g_sdio.dPrint(N_ERR, "[wilc sdio]: Unexpected interrupt (1) : tmp=%x, data=%x\n", tmp, cmd.data);
				break;
			}
		}
	} else {
		u32 irq_flags;

		cmd.read_write = 0;
		cmd.function = 0;
		cmd.raw = 0;
		cmd.address = 0xf7;
		cmd.data = 0;
		wilc_sdio_cmd52(wilc, &cmd);
		irq_flags = cmd.data & 0x1f;
		tmp |= ((irq_flags >> 0) << IRG_FLAGS_OFFSET);
	}

	*int_status = tmp;

	return 1;
}

static int sdio_clear_int_ext(struct wilc *wilc, u32 val)
{
	int ret;

	if (g_sdio.has_thrpt_enh3) {
		u32 reg;

		if (g_sdio.irq_gpio) {
			u32 flags;

			flags = val & (BIT(MAX_NUN_INT_THRPT_ENH2) - 1);
			reg = flags;
		} else {
			reg = 0;
		}
		/* select VMM table 0 */
		if ((val & SEL_VMM_TBL0) == SEL_VMM_TBL0)
			reg |= BIT(5);
		/* select VMM table 1 */
		if ((val & SEL_VMM_TBL1) == SEL_VMM_TBL1)
			reg |= BIT(6);
		/* enable VMM */
		if ((val & EN_VMM) == EN_VMM)
			reg |= BIT(7);
		if (reg) {
			sdio_cmd52_t cmd;

			cmd.read_write = 1;
			cmd.function = 0;
			cmd.raw = 0;
			cmd.address = 0xf8;
			cmd.data = reg;

			ret = wilc_sdio_cmd52(wilc, &cmd);
			if (!ret) {
				g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0xf8 data (%d) ...\n", __LINE__);
				goto _fail_;
			}

		}
	} else {
		if (g_sdio.irq_gpio) {
			/* see below. has_thrpt_enh2 uses register 0xf8 to clear interrupts. */
			/* Cannot clear multiple interrupts. Must clear each interrupt individually */
			u32 flags;

			flags = val & (BIT(MAX_NUM_INT) - 1);
			if (flags) {
				int i;

				ret = 1;
				for (i = 0; i < g_sdio.nint; i++) {
					if (flags & 1) {
						sdio_cmd52_t cmd;

						cmd.read_write = 1;
						cmd.function = 0;
						cmd.raw = 0;
						cmd.address = 0xf8;
						cmd.data = BIT(i);

						ret = wilc_sdio_cmd52(wilc, &cmd);
						if (!ret) {
							g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0xf8 data (%d) ...\n", __LINE__);
							goto _fail_;
						}

					}
					if (!ret)
						break;
					flags >>= 1;
				}
				if (!ret)
					goto _fail_;
				for (i = g_sdio.nint; i < MAX_NUM_INT; i++) {
					if (flags & 1)
						g_sdio.dPrint(N_ERR, "[wilc sdio]: Unexpected interrupt cleared %d...\n", i);
					flags >>= 1;
				}
			}
		}

		{
			u32 vmm_ctl;

			vmm_ctl = 0;
			/* select VMM table 0 */
			if ((val & SEL_VMM_TBL0) == SEL_VMM_TBL0)
				vmm_ctl |= BIT(0);
			/* select VMM table 1 */
			if ((val & SEL_VMM_TBL1) == SEL_VMM_TBL1)
				vmm_ctl |= BIT(1);
			/* enable VMM */
			if ((val & EN_VMM) == EN_VMM)
				vmm_ctl |= BIT(2);

			if (vmm_ctl) {
				sdio_cmd52_t cmd;

				cmd.read_write = 1;
				cmd.function = 0;
				cmd.raw = 0;
				cmd.address = 0xf6;
				cmd.data = vmm_ctl;
				ret = wilc_sdio_cmd52(wilc, &cmd);
				if (!ret) {
					g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed cmd52, set 0xf6 data (%d) ...\n", __LINE__);
					goto _fail_;
				}
			}
		}
	}

	return 1;
_fail_:
	return 0;
}

static int sdio_sync_ext(struct wilc *wilc, int nint)
{
	u32 reg;

	if (nint > MAX_NUM_INT) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Too many interupts (%d)...\n", nint);
		return 0;
	}
	if (nint > MAX_NUN_INT_THRPT_ENH2) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Error: Cannot support more than 5 interrupts when has_thrpt_enh2=1.\n");
		return 0;
	}

	g_sdio.nint = nint;

	/**
	 *      Disable power sequencer
	 **/
	if (!sdio_read_reg(wilc, WILC_MISC, &reg)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed read misc reg...\n");
		return 0;
	}

	reg &= ~BIT(8);
	if (!sdio_write_reg(wilc, WILC_MISC, reg)) {
		g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed write misc reg...\n");
		return 0;
	}

	if (g_sdio.irq_gpio) {
		u32 reg;
		int ret, i;

		/**
		 *      interrupt pin mux select
		 **/
		ret = sdio_read_reg(wilc, WILC_PIN_MUX_0, &reg);
		if (!ret) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed read reg (%08x)...\n", WILC_PIN_MUX_0);
			return 0;
		}
		reg |= BIT(8);
		ret = sdio_write_reg(wilc, WILC_PIN_MUX_0, reg);
		if (!ret) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed write reg (%08x)...\n", WILC_PIN_MUX_0);
			return 0;
		}

		/**
		 *      interrupt enable
		 **/
		ret = sdio_read_reg(wilc, WILC_INTR_ENABLE, &reg);
		if (!ret) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed read reg (%08x)...\n", WILC_INTR_ENABLE);
			return 0;
		}

		for (i = 0; (i < 5) && (nint > 0); i++, nint--)
			reg |= BIT((27 + i));
		ret = sdio_write_reg(wilc, WILC_INTR_ENABLE, reg);
		if (!ret) {
			g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed write reg (%08x)...\n", WILC_INTR_ENABLE);
			return 0;
		}
		if (nint) {
			ret = sdio_read_reg(wilc, WILC_INTR2_ENABLE, &reg);
			if (!ret) {
				g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed read reg (%08x)...\n", WILC_INTR2_ENABLE);
				return 0;
			}

			for (i = 0; (i < 3) && (nint > 0); i++, nint--)
				reg |= BIT(i);

			ret = sdio_read_reg(wilc, WILC_INTR2_ENABLE, &reg);
			if (!ret) {
				g_sdio.dPrint(N_ERR, "[wilc sdio]: Failed write reg (%08x)...\n", WILC_INTR2_ENABLE);
				return 0;
			}
		}
	}
	return 1;
}

/********************************************
 *
 *      Global sdio HIF function table
 *
 ********************************************/

const struct wilc_hif_func wilc_hif_sdio = {
	.hif_init = sdio_init,
	.hif_deinit = sdio_deinit,
	.hif_read_reg = sdio_read_reg,
	.hif_write_reg = sdio_write_reg,
	.hif_block_rx = sdio_read,
	.hif_block_tx = sdio_write,
	.hif_sync = sdio_sync,
	.hif_clear_int = sdio_clear_int,
	.hif_read_int = sdio_read_int,
	.hif_clear_int_ext = sdio_clear_int_ext,
	.hif_read_size = sdio_read_size,
	.hif_block_tx_ext = sdio_write,
	.hif_block_rx_ext = sdio_read,
	.hif_sync_ext = sdio_sync_ext,
	.enable_interrupt = wilc_sdio_enable_interrupt,
	.disable_interrupt = wilc_sdio_disable_interrupt,
};

