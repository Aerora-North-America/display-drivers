// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2019, 2021 The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/delay.h>


#include "dsi_pll.h"
#include "dsi_pll_14nm.h"

#define DSI_PLL_POLL_MAX_READS			15
#define DSI_PLL_POLL_TIMEOUT_US			1000
#define MSM8996_DSI_PLL_REVISION_2		2

#define VCO_REF_CLK_RATE 19200000

#define CEIL(x, y)		(((x) + ((y)-1)) / (y))

static int dsi_pll_read_stored_trim_codes(
		struct dsi_pll_resource *dsi_pll_res, s64 vco_clk_rate)
{
	int i;
	int rc = 0;
	bool found = false;

	if (!dsi_pll_res->dfps) {
		rc = -EINVAL;
		goto end_read;
	}

	for (i = 0; i < dsi_pll_res->dfps->vco_rate_cnt; i++) {
		struct dfps_codes_info *codes_info =
			&dsi_pll_res->dfps->codes_dfps[i];

		pr_debug("valid=%d vco_rate=%d, code %d %d\n",
			codes_info->is_valid, codes_info->clk_rate,
			codes_info->pll_codes.pll_codes_1,
			codes_info->pll_codes.pll_codes_2);

		if (vco_clk_rate != codes_info->clk_rate &&
				codes_info->is_valid)
			continue;

		dsi_pll_res->cache_pll_trim_codes[0] =
			codes_info->pll_codes.pll_codes_1;
		dsi_pll_res->cache_pll_trim_codes[1] =
			codes_info->pll_codes.pll_codes_2;
		found = true;
		break;
	}

	if (!found) {
		rc = -EINVAL;
		goto end_read;
	}

	pr_debug("core_kvco_code=0x%x core_vco_tune=0x%x\n",
			dsi_pll_res->cache_pll_trim_codes[0],
			dsi_pll_res->cache_pll_trim_codes[1]);

end_read:
	return rc;
}

int post_n1_div_set_div(void *context, unsigned int reg, unsigned int div)
{
	struct dsi_pll_resource *pll = context;
	struct dsi_pll_db *pdb;
	struct dsi_pll_output *pout;

	u32 n1div = 0;


	/* in common clock framework the divider value provided is one less */
	div++;

	pdb = (struct dsi_pll_db *)pll->priv;
	pout = &pdb->out;

	/*
	 * vco rate = bit_clk * postdiv * n1div
	 * vco range from 1300 to 2600 Mhz
	 * postdiv = 1
	 * n1div = 1 to 15
	 * n1div = roundup(1300Mhz / bit_clk)
	 * support bit_clk above 86.67Mhz
	 */

	pout->pll_n1div  = div;

	n1div = DSI_PLL_REG_R(pll->pll_base, DSIPHY_CMN_CLK_CFG0);
	n1div &= ~0xf;
	n1div |= (div & 0xf);
	DSI_PLL_REG_W(pll->pll_base, DSIPHY_CMN_CLK_CFG0, n1div);
	/* ensure n1 divider is programed */
	wmb();
	pr_debug("ndx=%d div=%d postdiv=%x n1div=%x\n",
			pll->index, div, pout->pll_postdiv, pout->pll_n1div);


	return 0;
}

int post_n1_div_get_div(void *context, unsigned int reg, unsigned int *div)
{
	int rc = 0;
	struct dsi_pll_resource *pll = context;
	struct dsi_pll_db *pdb;
	struct dsi_pll_output *pout;

	pdb = (struct dsi_pll_db *)pll->priv;
	pout = &pdb->out;

	if (is_gdsc_disabled(pll))
		return 0;


	/*
	 * postdiv = 1/2/4/8
	 * n1div = 1 - 15
	 * fot the time being, assume postdiv = 1
	 */

	*div = DSI_PLL_REG_R(pll->pll_base, DSIPHY_CMN_CLK_CFG0);
	*div &= 0xF;

	/*
	 * initialize n1div here, it will get updated when
	 * corresponding set_div is called.
	 */
	pout->pll_n1div = *div;

	/* common clock framework will add one to the divider value sent */
	if (*div == 0)
		*div = 1; /* value of zero means div is 2 as per SWI */
	else
		*div -= 1;

	pr_debug("post n1 get div = %d\n", *div);


	return rc;
}

int n2_div_set_div(void *context, unsigned int reg, unsigned int div)
{
	int rc = 0;
	u32 n2div;
	struct dsi_pll_resource *pll = context;
	struct dsi_pll_db *pdb;
	struct dsi_pll_output *pout;
	struct dsi_pll_resource *slave;


	/*
	 * in common clock framework the actual divider value
	 * provided is one less.
	 */
	div++;

	pdb = (struct dsi_pll_db *)pll->priv;
	pout = &pdb->out;

	/* this is for pixel clock */
	n2div = DSI_PLL_REG_R(pll->pll_base, DSIPHY_CMN_CLK_CFG0);
	n2div &= ~0xf0;	/* bits 4 to 7 */
	n2div |= (div << 4);
	DSI_PLL_REG_W(pll->pll_base, DSIPHY_CMN_CLK_CFG0, n2div);

	/* commit slave if split display is enabled */
	slave = pll->slave;
	if (slave)
		DSI_PLL_REG_W(slave->pll_base, DSIPHY_CMN_CLK_CFG0, n2div);

	pout->pll_n2div = div;

	/* set dsiclk_sel=1 so that n2div *= 2 */
	DSI_PLL_REG_W(pll->pll_base, DSIPHY_CMN_CLK_CFG1, 1);
	pr_debug("ndx=%d div=%d n2div=%x\n", pll->index, div, n2div);



	return rc;
}

int shadow_n2_div_set_div(void *context, unsigned int reg, unsigned int div)
{
	struct dsi_pll_resource *pll = context;
	struct dsi_pll_db *pdb;
	struct dsi_pll_output *pout;
	u32 data;

	pdb = pll->priv;
	pout = &pdb->out;

	/*
	 * in common clock framework the actual divider value
	 * provided is one less.
	 */
	div++;

	pout->pll_n2div = div;

	data = (pout->pll_n1div | (pout->pll_n2div << 4));
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
			DSI_DYNAMIC_REFRESH_PLL_CTRL19,
			DSIPHY_CMN_CLK_CFG0, DSIPHY_CMN_CLK_CFG1,
			data, 1);
	return 0;
}

int n2_div_get_div(void *context, unsigned int reg, unsigned int *div)
{
	int rc = 0;
	u32 n2div;
	struct dsi_pll_resource *pll = context;
	struct dsi_pll_db *pdb;
	struct dsi_pll_output *pout;

	if (is_gdsc_disabled(pll))
		return 0;

	pdb = (struct dsi_pll_db *)pll->priv;
	pout = &pdb->out;



	n2div = DSI_PLL_REG_R(pll->pll_base, DSIPHY_CMN_CLK_CFG0);
	n2div >>= 4;
	n2div &= 0x0f;
	/*
	 * initialize n2div here, it will get updated when
	 * corresponding set_div is called.
	 */
	pout->pll_n2div = n2div;

	*div = n2div;

	/* common clock framework will add one to the divider value sent */
	if (*div == 0)
		*div = 1; /* value of zero means div is 2 as per SWI */
	else
		*div -= 1;

	pr_debug("ndx=%d div=%d\n", pll->index, *div);

	return rc;
}

static bool pll_is_pll_locked_14nm(struct dsi_pll_resource *pll)
{
	u32 status;
	bool pll_locked;

	/* poll for PLL ready status */
	if (readl_poll_timeout_atomic((pll->pll_base +
			DSIPHY_PLL_RESET_SM_READY_STATUS),
			status,
			((status & BIT(5)) > 0),
			DSI_PLL_POLL_MAX_READS,
			DSI_PLL_POLL_TIMEOUT_US)) {
		pr_err("DSI PLL ndx=%d status=%x failed to Lock\n",
				pll->index, status);
		pll_locked = false;
	} else if (readl_poll_timeout_atomic((pll->pll_base +
				DSIPHY_PLL_RESET_SM_READY_STATUS),
				status,
				((status & BIT(0)) > 0),
				DSI_PLL_POLL_MAX_READS,
				DSI_PLL_POLL_TIMEOUT_US)) {
		pr_err("DSI PLL ndx=%d status=%x PLl not ready\n",
				pll->index, status);
		pll_locked = false;
	} else {
		pll_locked = true;
	}

	return pll_locked;
}

static void dsi_pll_start_14nm(void __iomem *pll_base)
{
	pr_debug("start PLL at base=%pK\n", pll_base);

	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_VREF_CFG1, 0x10);
	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_PLL_CNTRL, 1);
}

static void dsi_pll_stop_14nm(void __iomem *pll_base)
{
	pr_debug("stop PLL at base=%pK\n", pll_base);

	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_PLL_CNTRL, 0);
}

int dsi_pll_enable_seq_14nm(struct dsi_pll_resource *pll)
{
	int rc = 0;

	if (!pll) {
		pr_err("Invalid PLL resources\n");
		return -EINVAL;
	}

	dsi_pll_start_14nm(pll->pll_base);

	/*
	 * both DSIPHY_PLL_CLKBUFLR_EN and DSIPHY_CMN_GLBL_TEST_CTRL
	 * enabled at mdss_dsi_14nm_phy_config()
	 */

	if (!pll_is_pll_locked_14nm(pll)) {
		pr_err("DSI PLL ndx=%d lock failed\n", pll->index);
		rc = -EINVAL;
		goto init_lock_err;
	}

	pr_debug("DSI PLL ndx=%d Lock success\n", pll->index);

init_lock_err:
	return rc;
}

static int dsi_pll_enable(struct clk_hw *hw)
{
	int i, rc = 0;
	struct dsi_pll_vco_clk *vco = to_vco_clk_hw(hw);
	struct dsi_pll_resource *pll = vco->priv;

	/* Try all enable sequences until one succeeds */
	for (i = 0; i < vco->pll_en_seq_cnt; i++) {
		rc = vco->pll_enable_seqs[i](pll);
		pr_debug("DSI PLL %s after sequence #%d\n",
			rc ? "unlocked" : "locked", i + 1);
		if (!rc)
			break;
	}

	if (rc)
		pr_err("ndx=%d DSI PLL failed to lock\n", pll->index);
	else
		pll->pll_on = true;

	return rc;
}

static void dsi_pll_disable(struct clk_hw *hw)
{
	struct dsi_pll_vco_clk *vco = to_vco_clk_hw(hw);
	struct dsi_pll_resource *pll = vco->priv;
	struct dsi_pll_resource *slave;

	if (!pll->pll_on) {
		pr_err("Failed to enable mdss dsi pll=%d\n", pll->index);
		return;
	}

	pll->handoff_resources = false;
	slave = pll->slave;

	dsi_pll_stop_14nm(pll->pll_base);


	pll->pll_on = false;

	pr_debug("DSI PLL ndx=%d Disabled\n", pll->index);
}

static void dsi_pll_14nm_input_init(struct dsi_pll_resource *pll,
					struct dsi_pll_db *pdb)
{
	pdb->in.fref = 19200000;	/* 19.2 Mhz*/
	pdb->in.fdata = 0;		/* bit clock rate */
	pdb->in.dsiclk_sel = 1;		/* 1, reg: 0x0014 */
	pdb->in.ssc_en = pll->ssc_en;		/* 1, reg: 0x0494, bit 0 */
	pdb->in.ldo_en = 0;		/* 0,  reg: 0x004c, bit 0 */

	/* fixed  input */
	pdb->in.refclk_dbler_en = 0;	/* 0, reg: 0x04c0, bit 1 */
	pdb->in.vco_measure_time = 5;	/* 5, unknown */
	pdb->in.kvco_measure_time = 5;	/* 5, unknown */
	pdb->in.bandgap_timer = 4;	/* 4, reg: 0x0430, bit 3 - 5 */
	pdb->in.pll_wakeup_timer = 5;	/* 5, reg: 0x043c, bit 0 - 2 */
	pdb->in.plllock_cnt = 1;	/* 1, reg: 0x0488, bit 1 - 2 */
	pdb->in.plllock_rng = 0;	/* 0, reg: 0x0488, bit 3 - 4 */
	pdb->in.ssc_center = pll->ssc_center;/* 0, reg: 0x0494, bit 1 */
	pdb->in.ssc_adj_period = 37;	/* 37, reg: 0x498, bit 0 - 9 */
	pdb->in.ssc_spread = pll->ssc_ppm / 1000;
	pdb->in.ssc_freq = pll->ssc_freq;

	pdb->in.pll_ie_trim = 4;	/* 4, reg: 0x0400 */
	pdb->in.pll_ip_trim = 4;	/* 4, reg: 0x0404 */
	pdb->in.pll_cpcset_cur = 0;	/* 0, reg: 0x04f0, bit 0 - 2 */
	pdb->in.pll_cpmset_cur = 1;	/* 1, reg: 0x04f0, bit 3 - 5 */
	pdb->in.pll_icpmset = 7;	/* 7, reg: 0x04fc, bit 3 - 5 */
	pdb->in.pll_icpcset = 7;	/* 7, reg: 0x04fc, bit 0 - 2 */
	pdb->in.pll_icpmset_p = 0;	/* 0, reg: 0x04f4, bit 0 - 2 */
	pdb->in.pll_icpmset_m = 0;	/* 0, reg: 0x04f4, bit 3 - 5 */
	pdb->in.pll_icpcset_p = 0;	/* 0, reg: 0x04f8, bit 0 - 2 */
	pdb->in.pll_icpcset_m = 0;	/* 0, reg: 0x04f8, bit 3 - 5 */
	pdb->in.pll_lpf_res1 = 3;	/* 3, reg: 0x0504, bit 0 - 3 */
	pdb->in.pll_lpf_cap1 = 11;	/* 11, reg: 0x0500, bit 0 - 3 */
	pdb->in.pll_lpf_cap2 = 1;	/* 1, reg: 0x0500, bit 4 - 7 */
	pdb->in.pll_iptat_trim = 7;
	pdb->in.pll_c3ctrl = 2;		/* 2 */
	pdb->in.pll_r3ctrl = 1;		/* 1 */
	pdb->out.pll_postdiv = 1;
}

static void pll_14nm_ssc_calc(struct dsi_pll_resource *pll,
				struct dsi_pll_db *pdb)
{
	u32 period, ssc_period;
	u32 ref, rem;
	s64 step_size;

	pr_debug("%s: vco=%lld ref=%lld\n", __func__,
		pll->vco_current_rate, pll->vco_ref_clk_rate);

	ssc_period = pdb->in.ssc_freq / 500;
	period = (unsigned long)pll->vco_ref_clk_rate / 1000;
	ssc_period  = CEIL(period, ssc_period);
	ssc_period -= 1;
	pdb->out.ssc_period = ssc_period;

	pr_debug("%s: ssc, freq=%d spread=%d period=%d\n", __func__,
	pdb->in.ssc_freq, pdb->in.ssc_spread, pdb->out.ssc_period);

	step_size = (u32)pll->vco_current_rate;
	ref = pll->vco_ref_clk_rate;
	ref /= 1000;
	step_size = div_s64(step_size, ref);
	step_size <<= 20;
	step_size = div_s64(step_size, 1000);
	step_size *= pdb->in.ssc_spread;
	step_size = div_s64(step_size, 1000);
	step_size *= (pdb->in.ssc_adj_period + 1);

	rem = 0;
	step_size = div_s64_rem(step_size, ssc_period + 1, &rem);
	if (rem)
		step_size++;

	pr_debug("%s: step_size=%lld\n", __func__, step_size);

	step_size &= 0x0ffff;	/* take lower 16 bits */

	pdb->out.ssc_step_size = step_size;
}

static void pll_14nm_dec_frac_calc(struct dsi_pll_resource *pll,
				struct dsi_pll_db *pdb)
{
	struct dsi_pll_input *pin = &pdb->in;
	struct dsi_pll_output *pout = &pdb->out;
	u64 multiplier = BIT(20);
	u64 dec_start_multiple, dec_start, pll_comp_val;
	s32 duration, div_frac_start;
	s64 vco_clk_rate = pll->vco_current_rate;
	s64 fref = pll->vco_ref_clk_rate;

	pr_debug("vco_clk_rate=%lld ref_clk_rate=%lld\n",
				vco_clk_rate, fref);

	dec_start_multiple = div_s64(vco_clk_rate * multiplier, fref);
	div_s64_rem(dec_start_multiple, multiplier, &div_frac_start);

	dec_start = div_s64(dec_start_multiple, multiplier);

	pout->dec_start = (u32)dec_start;
	pout->div_frac_start = div_frac_start;

	if (pin->plllock_cnt == 0)
		duration = 1024;
	else if (pin->plllock_cnt == 1)
		duration = 256;
	else if (pin->plllock_cnt == 2)
		duration = 128;
	else
		duration = 32;

	pll_comp_val =  duration * dec_start_multiple;
	pll_comp_val =  div_u64(pll_comp_val, multiplier);
	do_div(pll_comp_val, 10);

	pout->plllock_cmp = (u32)pll_comp_val;

	pout->pll_txclk_en = 1;
	if (pll->revision == MSM8996_DSI_PLL_REVISION_2)
		pout->cmn_ldo_cntrl = 0x3c;
	else
		pout->cmn_ldo_cntrl = 0x1c;
}

static u32 pll_14nm_kvco_slop(u32 vrate)
{
	u32 slop = 0;

	if (vrate > 1300000000UL && vrate <= 1800000000UL)
		slop =  600;
	else if (vrate > 1800000000UL && vrate < 2300000000UL)
		slop = 400;
	else if (vrate > 2300000000UL && vrate < 2600000000UL)
		slop = 280;

	return slop;
}

static void pll_14nm_calc_vco_count(struct dsi_pll_db *pdb,
			 s64 vco_clk_rate, s64 fref)
{
	struct dsi_pll_input *pin = &pdb->in;
	struct dsi_pll_output *pout = &pdb->out;
	u64 data;
	u32 cnt;

	data = fref * pin->vco_measure_time;
	do_div(data, 1000000);
	data &= 0x03ff;	/* 10 bits */
	data -= 2;
	pout->pll_vco_div_ref = data;

	data = (unsigned long)vco_clk_rate / 1000000;	/* unit is Mhz */
	data *= pin->vco_measure_time;
	do_div(data, 10);
	pout->pll_vco_count = data; /* reg: 0x0474, 0x0478 */

	data = fref * pin->kvco_measure_time;
	do_div(data, 1000000);
	data &= 0x03ff;	/* 10 bits */
	data -= 1;
	pout->pll_kvco_div_ref = data;

	cnt = pll_14nm_kvco_slop(vco_clk_rate);
	cnt *= 2;
	cnt /= 100;
	cnt *= pin->kvco_measure_time;
	pout->pll_kvco_count = cnt;

	pout->pll_misc1 = 16;
	pout->pll_resetsm_cntrl = 48;
	pout->pll_resetsm_cntrl2 = pin->bandgap_timer << 3;
	pout->pll_resetsm_cntrl5 = pin->pll_wakeup_timer;
	pout->pll_kvco_code = 0;
}

static void pll_db_commit_ssc(struct dsi_pll_resource *pll,
					struct dsi_pll_db *pdb)
{
	void __iomem *pll_base = pll->pll_base;
	struct dsi_pll_input *pin = &pdb->in;
	struct dsi_pll_output *pout = &pdb->out;
	char data;

	data = pin->ssc_adj_period;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_ADJ_PER1, data);
	data = (pin->ssc_adj_period >> 8);
	data &= 0x03;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_ADJ_PER2, data);

	data = pout->ssc_period;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_PER1, data);
	data = (pout->ssc_period >> 8);
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_PER2, data);

	data = pout->ssc_step_size;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_STEP_SIZE1, data);
	data = (pout->ssc_step_size >> 8);
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_STEP_SIZE2, data);

	data = (pin->ssc_center & 0x01);
	data <<= 1;
	data |= 0x01; /* enable */
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_EN_CENTER, data);

	wmb();	/* make sure register committed */
}

static void pll_db_commit_common(struct dsi_pll_resource *pll,
					struct dsi_pll_db *pdb)
{
	void __iomem *pll_base = pll->pll_base;
	struct dsi_pll_input *pin = &pdb->in;
	struct dsi_pll_output *pout = &pdb->out;
	char data;

	/* confgiure the non frequency dependent pll registers */
	data = 0;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_SYSCLK_EN_RESET, data);

	/* DSIPHY_PLL_CLKBUFLR_EN updated at dsi phy */

	data = pout->pll_txclk_en;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_TXCLK_EN, data);

	data = pout->pll_resetsm_cntrl;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_RESETSM_CNTRL, data);
	data = pout->pll_resetsm_cntrl2;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_RESETSM_CNTRL2, data);
	data = pout->pll_resetsm_cntrl5;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_RESETSM_CNTRL5, data);

	data = pout->pll_vco_div_ref;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_VCO_DIV_REF1, data);
	data = (pout->pll_vco_div_ref >> 8);
	data &= 0x03;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_VCO_DIV_REF2, data);

	data = pout->pll_kvco_div_ref;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_DIV_REF1, data);
	data = (pout->pll_kvco_div_ref >> 8);
	data &= 0x03;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_DIV_REF2, data);

	data = pout->pll_misc1;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_MISC1, data);

	data = pin->pll_ie_trim;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_IE_TRIM, data);

	data = pin->pll_ip_trim;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_IP_TRIM, data);

	data = ((pin->pll_cpmset_cur << 3) | pin->pll_cpcset_cur);
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_CP_SET_CUR, data);

	data = ((pin->pll_icpcset_p << 3) | pin->pll_icpcset_m);
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_ICPCSET, data);

	data = ((pin->pll_icpmset_p << 3) | pin->pll_icpcset_m);
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_ICPMSET, data);

	data = ((pin->pll_icpmset << 3) | pin->pll_icpcset);
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_ICP_SET, data);

	data = ((pdb->in.pll_lpf_cap2 << 4) | pdb->in.pll_lpf_cap1);
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_LPF1, data);

	data = pin->pll_iptat_trim;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_IPTAT_TRIM, data);

	data = (pdb->in.pll_c3ctrl | (pdb->in.pll_r3ctrl << 4));
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_CRCTRL, data);
}

static void pll_db_commit_14nm(struct dsi_pll_resource *pll,
					struct dsi_pll_db *pdb)
{
	void __iomem *pll_base = pll->pll_base;
	struct dsi_pll_input *pin = &pdb->in;
	struct dsi_pll_output *pout = &pdb->out;
	char data;

	data = pout->cmn_ldo_cntrl;
	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_LDO_CNTRL, data);

	pll_db_commit_common(pll, pdb);

	/* de assert pll start and apply pll sw reset */
	/* stop pll */
	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_PLL_CNTRL, 0);

	/* pll sw reset */
	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_CTRL_1, 0x20);
	wmb();	/* make sure register committed */
	udelay(10);

	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_CTRL_1, 0);
	wmb();	/* make sure register committed */

	data = pdb->in.dsiclk_sel; /* set dsiclk_sel = 1  */
	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_CLK_CFG1, data);

	data = 0xff; /* data, clk, pll normal operation */
	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_CTRL_0, data);

	/* confgiure the frequency dependent pll registers */
	data = pout->dec_start;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_DEC_START, data);

	data = pout->div_frac_start;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_DIV_FRAC_START1, data);
	data = (pout->div_frac_start >> 8);
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_DIV_FRAC_START2, data);
	data = (pout->div_frac_start >> 16);
	data &= 0x0f;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_DIV_FRAC_START3, data);

	data = pout->plllock_cmp;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLLLOCK_CMP1, data);
	data = (pout->plllock_cmp >> 8);
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLLLOCK_CMP2, data);
	data = (pout->plllock_cmp >> 16);
	data &= 0x03;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLLLOCK_CMP3, data);

	data = ((pin->plllock_cnt << 1) | (pin->plllock_rng << 3));
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLLLOCK_CMP_EN, data);

	data = pout->pll_vco_count;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_VCO_COUNT1, data);
	data = (pout->pll_vco_count >> 8);
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_VCO_COUNT2, data);

	data = pout->pll_kvco_count;
	data &= 0x0ff;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_COUNT1, data);
	data = (pout->pll_kvco_count >> 8);
	data &= 0x03;
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_COUNT2, data);

	/*
	 * tx_band = pll_postdiv
	 * 0: divided by 1 <== for now
	 * 1: divided by 2
	 * 2: divided by 4
	 * 3: divided by 8
	 */
	data = (((pout->pll_postdiv - 1) << 4) | pdb->in.pll_lpf_res1);
	DSI_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_LPF2_POSTDIV, data);

	data = (pout->pll_n1div | (pout->pll_n2div << 4));
	DSI_PLL_REG_W(pll_base, DSIPHY_CMN_CLK_CFG0, data);

	if (pll->ssc_en)
		pll_db_commit_ssc(pll, pdb);

	wmb();	/* make sure register committed */
}

/*
 * pll_source_finding:
 * Both GLBL_TEST_CTRL and CLKBUFLR_EN are configured
 * at mdss_dsi_14nm_phy_config()
 */
static int pll_source_finding(struct dsi_pll_resource *pll)
{
	u32 clk_buf_en;
	u32 glbl_test_ctrl;

	glbl_test_ctrl = DSI_PLL_REG_R(pll->pll_base,
				DSIPHY_CMN_GLBL_TEST_CTRL);
	clk_buf_en = DSI_PLL_REG_R(pll->pll_base,
				DSIPHY_PLL_CLKBUFLR_EN);

	glbl_test_ctrl &= BIT(2);
	glbl_test_ctrl >>= 2;

	pr_debug("%s: pll=%d clk_buf_en=%x glbl_test_ctrl=%x\n",
		__func__, pll->index, clk_buf_en, glbl_test_ctrl);

	clk_buf_en &= (PLL_OUTPUT_RIGHT | PLL_OUTPUT_LEFT);

	if ((glbl_test_ctrl == PLL_SOURCE_FROM_LEFT) &&
			(clk_buf_en == PLL_OUTPUT_BOTH))
		return PLL_MASTER;

	if ((glbl_test_ctrl == PLL_SOURCE_FROM_RIGHT) &&
			(clk_buf_en == PLL_OUTPUT_NONE))
		return PLL_SLAVE;

	if ((glbl_test_ctrl == PLL_SOURCE_FROM_LEFT) &&
			(clk_buf_en == PLL_OUTPUT_RIGHT))
		return PLL_STANDALONE;

	pr_debug("%s: Error pll setup, clk_buf_en=%x glbl_test_ctrl=%x\n",
			__func__, clk_buf_en, glbl_test_ctrl);

	return PLL_UNKNOWN;
}

static void pll_source_setup(struct dsi_pll_resource *pll)
{
	int status;
	struct dsi_pll_db *pdb = (struct dsi_pll_db *)pll->priv;
	struct dsi_pll_resource *other;

	if (pdb->source_setup_done)
		return;

	pdb->source_setup_done++;

	status = pll_source_finding(pll);

	if (status == PLL_STANDALONE || status == PLL_UNKNOWN)
		return;

	other = pdb->next->pll;
	if (!other)
		return;

	pr_debug("%s: status=%d pll=%d other=%d\n", __func__,
			status, pll->index, other->index);

	if (status == PLL_MASTER)
		pll->slave = other;
	else
		other->slave = pll;
}

unsigned long pll_vco_recalc_rate_14nm(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct dsi_pll_vco_clk *vco = to_vco_clk_hw(hw);
	struct dsi_pll_resource *pll = vco->priv;

	if (pll->vco_current_rate)
		return (unsigned long)pll->vco_current_rate;

	return 0;
}

int pll_vco_set_rate_14nm(struct clk_hw *hw, unsigned long rate,
					unsigned long parent_rate)
{
	int rc = 0;
	struct dsi_pll_vco_clk *vco = to_vco_clk_hw(hw);
	struct dsi_pll_resource *pll = vco->priv;
	struct dsi_pll_resource *slave;
	struct dsi_pll_db *pdb;

	pdb = (struct dsi_pll_db *)pll->priv;
	if (!pdb) {
		pr_err("No prov found\n");
		return -EINVAL;
	}


	pll_source_setup(pll);

	pr_debug("%s: ndx=%d base=%pK rate=%lu slave=%pK\n", __func__,
				pll->index, pll->pll_base, rate, pll->slave);

	pll->vco_current_rate = rate;
	pll->vco_ref_clk_rate = vco->ref_clk_rate;

	dsi_pll_14nm_input_init(pll, pdb);

	pll_14nm_dec_frac_calc(pll, pdb);

	if (pll->ssc_en)
		pll_14nm_ssc_calc(pll, pdb);

	pll_14nm_calc_vco_count(pdb, pll->vco_current_rate,
					pll->vco_ref_clk_rate);

	/* commit slave if split display is enabled */
	slave = pll->slave;
	if (slave)
		pll_db_commit_14nm(slave, pdb);

	/* commit master itself */
	pll_db_commit_14nm(pll, pdb);

	return rc;
}

static void shadow_pll_dynamic_refresh_14nm(struct dsi_pll_resource *pll,
							struct dsi_pll_db *pdb)
{
	struct dsi_pll_output *pout = &pdb->out;
	u32 data = 0;

	data = (pout->pll_n1div | (pout->pll_n2div << 4));
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL19,
		DSIPHY_CMN_CLK_CFG0, DSIPHY_CMN_CLK_CFG1,
		data, 1);
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL20,
		DSIPHY_CMN_CTRL_0, DSIPHY_PLL_SYSCLK_EN_RESET,
		0xFF, 0x0);
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL21,
		DSIPHY_PLL_DEC_START, DSIPHY_PLL_DIV_FRAC_START1,
		pout->dec_start, (pout->div_frac_start & 0x0FF));
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL22,
		DSIPHY_PLL_DIV_FRAC_START2, DSIPHY_PLL_DIV_FRAC_START3,
		((pout->div_frac_start >> 8) & 0x0FF),
		((pout->div_frac_start >> 16) & 0x0F));
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL23,
		DSIPHY_PLL_PLLLOCK_CMP1, DSIPHY_PLL_PLLLOCK_CMP2,
		(pout->plllock_cmp & 0x0FF),
		((pout->plllock_cmp >> 8) & 0x0FF));
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL24,
		DSIPHY_PLL_PLLLOCK_CMP3, DSIPHY_PLL_PLL_VCO_TUNE,
		((pout->plllock_cmp >> 16) & 0x03),
		(pll->cache_pll_trim_codes[1] | BIT(7))); /* VCO tune*/
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL25,
		DSIPHY_PLL_KVCO_CODE, DSIPHY_PLL_RESETSM_CNTRL,
		(pll->cache_pll_trim_codes[0] | BIT(5)), 0x38);
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL26,
		DSIPHY_PLL_PLL_LPF2_POSTDIV, DSIPHY_CMN_PLL_CNTRL,
		(((pout->pll_postdiv - 1) << 4) | pdb->in.pll_lpf_res1), 0x01);
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL27,
		DSIPHY_CMN_PLL_CNTRL, DSIPHY_CMN_PLL_CNTRL,
		0x01, 0x01);
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL28,
		DSIPHY_CMN_PLL_CNTRL, DSIPHY_CMN_PLL_CNTRL,
		0x01, 0x01);
	DSI_DYN_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_CTRL29,
		DSIPHY_CMN_PLL_CNTRL, DSIPHY_CMN_PLL_CNTRL,
		0x01, 0x01);
	DSI_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_UPPER_ADDR, 0x0000001E);
	DSI_PLL_REG_W(pll->dyn_pll_base,
		DSI_DYNAMIC_REFRESH_PLL_UPPER_ADDR2, 0x001FFE00);

	/*
	 * Ensure all the dynamic refresh registers are written before
	 * dynamic refresh to change the fps is triggered
	 */
	wmb();
}

int shadow_pll_vco_set_rate_14nm(struct clk_hw *hw, unsigned long rate,
					unsigned long parent_rate)
{
	int rc = 0;
	struct dsi_pll_vco_clk *vco = to_vco_clk_hw(hw);
	struct dsi_pll_resource *pll = vco->priv;
	struct dsi_pll_db *pdb;
	s64 vco_clk_rate = (s64)rate;

	if (!pll) {
		pr_err("PLL data not found\n");
		return -EINVAL;
	}

	pdb = pll->priv;
	if (!pdb) {
		pr_err("No priv data found\n");
		return -EINVAL;
	}

	rc = dsi_pll_read_stored_trim_codes(pll, vco_clk_rate);
	if (rc) {
		pr_err("cannot find pll codes rate=%lld\n", vco_clk_rate);
		return -EINVAL;
	}

	pr_debug("%s: ndx=%d base=%pK rate=%lu\n", __func__,
			pll->index, pll->pll_base, rate);

	pll->vco_current_rate = rate;
	pll->vco_ref_clk_rate = vco->ref_clk_rate;

	dsi_pll_14nm_input_init(pll, pdb);

	pll_14nm_dec_frac_calc(pll, pdb);

	pll_14nm_calc_vco_count(pdb, pll->vco_current_rate,
			pll->vco_ref_clk_rate);

	shadow_pll_dynamic_refresh_14nm(pll, pdb);

	return rc;
}

long pll_vco_round_rate_14nm(struct clk_hw *hw, unsigned long rate,
						unsigned long *parent_rate)
{
	unsigned long rrate = rate;
	u64 div;
	struct dsi_pll_vco_clk *vco = to_vco_clk_hw(hw);

	div = vco->min_rate;
	do_div(div, rate);
	if (div > 15) {
		/* rate < 86.67 Mhz */
		pr_err("rate=%lu NOT supportted\n", rate);
		return -EINVAL;
	}

	if (rate < vco->min_rate)
		rrate = vco->min_rate;
	if (rate > vco->max_rate)
		rrate = vco->max_rate;

	*parent_rate = rrate;
	return rrate;
}

int pll_vco_prepare_14nm(struct clk_hw *hw)
{
	int rc = 0;
	struct dsi_pll_vco_clk *vco = to_vco_clk_hw(hw);
	struct dsi_pll_resource *pll = vco->priv;

	if (!pll) {
		pr_err("Dsi pll resources are not available\n");
		return -EINVAL;
	}

	if ((pll->vco_cached_rate != 0)
	    && (pll->vco_cached_rate == clk_hw_get_rate(hw))) {
		rc = pll_vco_set_rate_14nm(hw, pll->vco_cached_rate,
						pll->vco_cached_rate);
		if (rc) {
			pr_err("index=%d vco_set_rate failed. rc=%d\n",
					rc, pll->index);

			goto error;
		}
	}

	rc = dsi_pll_enable(hw);

error:
	return rc;
}

void pll_vco_unprepare_14nm(struct clk_hw *hw)
{
	struct dsi_pll_vco_clk *vco = to_vco_clk_hw(hw);
	struct dsi_pll_resource *pll = vco->priv;

	if (!pll) {
		pr_err("Dsi pll resources are not available\n");
		return;
	}

	pll->vco_cached_rate = clk_hw_get_rate(hw);
	dsi_pll_disable(hw);
}

int dsi_mux_set_parent_14nm(void *context, unsigned int reg, unsigned int val)
{
	return 0;
}

int dsi_mux_get_parent_14nm(void *context, unsigned int reg, unsigned int *val)
{
	*val = 0;
	return 0;
}
