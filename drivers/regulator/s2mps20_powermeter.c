/*
 * s2mps20.c
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 *              http://www.samsung.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/mfd/samsung/s2mps20.h>
#include <linux/mfd/samsung/s2mps20-regulator.h>
#include <linux/platform_device.h>

#define CURRENT_METER		1
#define POWER_METER 		2
#define SYNC_MODE	1
#define ASYNC_MODE	2

static struct adc_info *adc_meter;
struct device *s2mps20_adc_dev;
struct class *s2mps20_adc_class;

struct adc_info {
	struct i2c_client *i2c;
	u8 adc_mode;
	u8 adc_sync_mode;
	u8 *adc_reg;
	u16 *adc_val;
	u16 *current_val;
	u16 *power_val;
	u8 adc_ctrl1;
	u8 ptr_base;
	struct mutex adc_lock;
};

static const unsigned int current_buck_coeffs[S2MPS20_BUCK_CNT] =
	{CURRENT_BD, CURRENT_BS, CURRENT_BS};

static const unsigned int current_ldo_coeffs[S2MPS20_LDO_CNT] =
	{CURRENT_L300, CURRENT_L450, CURRENT_L300, CURRENT_L150, CURRENT_L150, CURRENT_L150,
	 CURRENT_L300, CURRENT_L450, CURRENT_L600, CURRENT_L300, CURRENT_L150, CURRENT_L150};

static const unsigned int power_buck_coeffs[S2MPS20_BUCK_CNT] =
	{POWER_BD, POWER_BS, POWER_BS};

static const unsigned int power_ldo_coeffs[S2MPS20_LDO_CNT] =
	{POWER_D300, POWER_P450, POWER_N300, POWER_N150, POWER_N150, POWER_P150, POWER_P300,
	 POWER_N450, POWER_N600, POWER_P300, POWER_P150, POWER_P150};

static unsigned int get_coeff_c(struct device *dev, u8 adc_reg_num)
{
	unsigned int coeff;

		/* if the regulator is LDO */
		if (adc_reg_num >= S2MPS20_LDO_START && adc_reg_num <= S2MPS20_LDO_END)
			coeff = current_ldo_coeffs[adc_reg_num - S2MPS20_LDO_START];
		/* if the regulator is BUCK */
		else if (adc_reg_num >= S2MPS20_BUCK_START && adc_reg_num <= S2MPS20_BUCK_END)
			coeff = current_buck_coeffs[adc_reg_num - S2MPS20_BUCK_START];
		else {
			dev_err(dev, "%s: invalid adc regulator number(%d)\n", __func__, adc_reg_num);
			coeff = 0;
		}
	return coeff;
}

static unsigned int get_coeff_p(struct device *dev, u8 adc_reg_num)
{
	unsigned int coeff;
		/* if the regulator is LDO */
		if (adc_reg_num >= S2MPS20_LDO_START && adc_reg_num <= S2MPS20_LDO_END)
			coeff = power_ldo_coeffs[adc_reg_num - S2MPS20_LDO_START];
		/* if the regulator is BUCK */
		else if (adc_reg_num >= S2MPS20_BUCK_START && adc_reg_num <= S2MPS20_BUCK_END)
			coeff = power_buck_coeffs[adc_reg_num - S2MPS20_BUCK_START];
		else {
			dev_err(dev, "%s: invalid adc regulator number(%d)\n", __func__, adc_reg_num);
			coeff = 0;
		}

	return coeff;
}

static void s2m_adc_read_data(struct device *dev, int channel)
{
	int i;
	u8 data_l, data_h;

	mutex_lock(&adc_meter->adc_lock);

	/* ASYNCRD bit '1' --> 2ms delay --> read  in case of ADC Async mode */
	if (adc_meter->adc_sync_mode == ASYNC_MODE) {
		s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL2, ADC_ASYNCRD_MASK, ADC_ASYNCRD_MASK);
		usleep_range(2000, 2100);
	}

	if (channel < 0) {
		/* current */
		for (i = 0; i < S2MPS20_MAX_ADC_CHANNEL; i++) {
			s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3,
				(i + CURRENT_PTR_BASE) | ADC_EN_MASK, ADC_PTR_MASK | ADC_EN_MASK);
			s2mps20_read_reg(adc_meter->i2c, S2MPS20_REG_ADC_DATA, &data_l);
			adc_meter->current_val[i] = data_l;
		}
		/* power */
		for (i = 0; i < S2MPS20_MAX_ADC_CHANNEL; i++) {
			s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3,
				(2*i + POWER_PTR_BASE) | ADC_EN_MASK, ADC_PTR_MASK | ADC_EN_MASK);
			s2mps20_read_reg(adc_meter->i2c, S2MPS20_REG_ADC_DATA, &data_l);

			s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3,
				(2*i+1 + POWER_PTR_BASE) | ADC_EN_MASK, ADC_PTR_MASK | ADC_EN_MASK);
			s2mps20_read_reg(adc_meter->i2c, S2MPS20_REG_ADC_DATA, &data_h);

			adc_meter->power_val[i] = ((data_h & 0xff) << 8) | (data_l & 0xff);
		}
	} else {
		/* current */
		s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3,
			(channel + CURRENT_PTR_BASE) | ADC_EN_MASK, ADC_PTR_MASK | ADC_EN_MASK);
		s2mps20_read_reg(adc_meter->i2c, S2MPS20_REG_ADC_DATA, &data_l);
		adc_meter->current_val[channel] = data_l;

		/* power */
		s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3,
			(2*channel + POWER_PTR_BASE) | ADC_EN_MASK, ADC_PTR_MASK | ADC_EN_MASK);
		s2mps20_read_reg(adc_meter->i2c, S2MPS20_REG_ADC_DATA, &data_l);

		s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3,
			(2*channel+1 + POWER_PTR_BASE) | ADC_EN_MASK, ADC_PTR_MASK | ADC_EN_MASK);
		s2mps20_read_reg(adc_meter->i2c, S2MPS20_REG_ADC_DATA, &data_h);

		adc_meter->power_val[channel] = ((data_h & 0xff) << 8) | (data_l & 0xff);
	}

	mutex_unlock(&adc_meter->adc_lock);
}

static ssize_t adc_val_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
		s2m_adc_read_data(dev, -1);
		return snprintf(buf, PAGE_SIZE, "CH0[%x]:%d uW (%d), CH1[%x]:%d uW (%d), CH2[%x]:%d uW (%d)\n",
			adc_meter->adc_reg[0], ((adc_meter->power_val[0] >> 8) * get_coeff_p(dev, adc_meter->adc_reg[0])), adc_meter->power_val[0],
			adc_meter->adc_reg[1], ((adc_meter->power_val[1] >> 8) * get_coeff_p(dev, adc_meter->adc_reg[1])), adc_meter->power_val[1],
			adc_meter->adc_reg[2], ((adc_meter->power_val[2] >> 8) * get_coeff_p(dev, adc_meter->adc_reg[2])), adc_meter->power_val[2]);
}

static ssize_t adc_val_current_show(struct device *dev, struct device_attribute *attr, char *buf)
{
		s2m_adc_read_data(dev, -1);
		return snprintf(buf, PAGE_SIZE, "CH0[%x]:%d uA (%d), CH1[%x]:%d uA (%d), CH2[%x]:%d uA (%d)\n",
			adc_meter->adc_reg[0], adc_meter->current_val[0] * get_coeff_c(dev, adc_meter->adc_reg[0])/1000, adc_meter->current_val[0],
			adc_meter->adc_reg[1], adc_meter->current_val[1] * get_coeff_c(dev, adc_meter->adc_reg[1])/1000, adc_meter->current_val[1],
			adc_meter->adc_reg[2], adc_meter->current_val[2] * get_coeff_c(dev, adc_meter->adc_reg[2])/1000, adc_meter->current_val[2]);
}

static ssize_t adc_en_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 adc_ctrl3;
	s2mps20_read_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3, &adc_ctrl3);

	if (adc_ctrl3 & ADC_EN_MASK)
		return snprintf(buf, PAGE_SIZE, "ADC enable (%x)\n", adc_ctrl3);
	else
		return snprintf(buf, PAGE_SIZE, "ADC disable (%x)\n", adc_ctrl3);
}

static ssize_t adc_en_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	u8 temp, val;

	ret = kstrtou8(buf, 16, &temp);
	if (ret)
		return -EINVAL;
	else {
		switch (temp) {
		case 0 :
			val = 0x00;
			break;
		case 1 :
			val = 0x80;
			break;
		default :
			val = 0x00;
			break;
		}
	}

	s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3, val, ADC_EN_MASK);
	return count;
}

static ssize_t adc_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	switch (adc_meter->adc_mode) {
		case CURRENT_METER :
			return snprintf(buf, PAGE_SIZE, "CURRENT MODE (%d)\n", CURRENT_METER);
		case POWER_METER :
			return snprintf(buf, PAGE_SIZE, "POWER MODE (%d)\n", POWER_METER);
		default :
			return snprintf(buf, PAGE_SIZE, "error\n");
	}
}

static ssize_t adc_mode_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	u8 temp;

	ret = kstrtou8(buf, 16, &temp);

	if (ret)
		return -EINVAL;
	else {
		switch (temp) {
		case CURRENT_METER :
			adc_meter->adc_mode = CURRENT_METER;
			adc_meter->ptr_base = CURRENT_PTR_BASE;
			break;
		case POWER_METER :
			adc_meter->adc_mode = POWER_METER;
			adc_meter->ptr_base = POWER_PTR_BASE;
			break;
		default :
			adc_meter->adc_mode = CURRENT_METER;
			adc_meter->ptr_base = CURRENT_PTR_BASE;
			break;
		}
		return count;
	}
}

static ssize_t adc_sync_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{

	switch (adc_meter->adc_sync_mode) {
	case SYNC_MODE:
		return snprintf(buf, PAGE_SIZE, "SYNC_MODE (%d)\n", adc_meter->adc_sync_mode);
	case ASYNC_MODE:
		return snprintf(buf, PAGE_SIZE, "ASYNC_MODE (%d)\n", adc_meter->adc_sync_mode);
	default:
		return snprintf(buf, PAGE_SIZE, "error (%x)\n", adc_meter->adc_sync_mode);
	}
}

static ssize_t adc_sync_mode_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	u8 temp;

	ret = kstrtou8(buf, 16, &temp);
	if (ret)
		return -EINVAL;

	switch (temp) {
	case SYNC_MODE:
		adc_meter->adc_sync_mode = SYNC_MODE;
		break;
	case ASYNC_MODE:
		adc_meter->adc_sync_mode = ASYNC_MODE;
		break;
	default:
		adc_meter->adc_sync_mode = SYNC_MODE;
		break;
	}
	return count;
}

static ssize_t adc_val_0_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int coeff_p = get_coeff_p(dev, adc_meter->adc_reg[0]);
	unsigned int coeff_c = get_coeff_c(dev, adc_meter->adc_reg[0]);

	s2m_adc_read_data(dev, 0);

	return snprintf(buf, PAGE_SIZE, "[CH0] %d(0x%x)uA, %d(0x%x) uW\n", 
	adc_meter->current_val[0] * coeff_c / 1000, adc_meter->current_val[0],
	(adc_meter->power_val[0] >> 8) * coeff_p, adc_meter->power_val[0]);
}

static ssize_t adc_val_1_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int coeff_p = get_coeff_p(dev, adc_meter->adc_reg[1]);
	unsigned int coeff_c = get_coeff_c(dev, adc_meter->adc_reg[1]);

	s2m_adc_read_data(dev, 1);

	return snprintf(buf, PAGE_SIZE, "[CH0] %d(0x%x)uA, %d(0x%x) uW\n", 
	adc_meter->current_val[1] * coeff_c / 1000, adc_meter->current_val[1],
	(adc_meter->power_val[1] >> 8) * coeff_p, adc_meter->power_val[1]);
}

static ssize_t adc_val_2_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int coeff_p = get_coeff_p(dev, adc_meter->adc_reg[2]);
	unsigned int coeff_c = get_coeff_c(dev, adc_meter->adc_reg[2]);

	s2m_adc_read_data(dev, 2);

	return snprintf(buf, PAGE_SIZE, "[CH0] %d(0x%x)uA, %d(0x%x) uW\n", 
	adc_meter->current_val[2] * coeff_c / 1000, adc_meter->current_val[2],
	(adc_meter->power_val[2] >> 8) * coeff_p, adc_meter->power_val[2]);
}

static ssize_t adc_reg_0_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%2x\n", adc_meter->adc_reg[0]);
}

static ssize_t adc_reg_1_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%2x\n", adc_meter->adc_reg[1]);
}

static ssize_t adc_reg_2_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%2x\n", adc_meter->adc_reg[2]);
}

static void adc_reg_update(struct device *dev)
{
	int i = 0;

	/* ADC OFF */
	s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3,
				0x00, ADC_EN_MASK);

	/* CHANNEL setting */
	for (i = 0; i < S2MPS20_MAX_ADC_CHANNEL; i++) {
		s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3,
			i + MUX_PTR_BASE, ADC_PTR_MASK);
		s2mps20_write_reg(adc_meter->i2c, S2MPS20_REG_ADC_DATA, adc_meter->adc_reg[i]);
	}

	/* ADC EN */
	switch (adc_meter->adc_mode) {
	case CURRENT_METER :
		adc_meter->adc_mode = CURRENT_METER;
		adc_meter->ptr_base = CURRENT_PTR_BASE;
		pr_info("%s: current mode enable\n", __func__);
		break;
	case POWER_METER:
		adc_meter->adc_mode = POWER_METER;
		adc_meter->ptr_base = POWER_PTR_BASE;
		pr_info("%s: power mode enable\n",  __func__);
		break;
	default :
		adc_meter->adc_mode = CURRENT_METER;
		adc_meter->ptr_base = CURRENT_PTR_BASE;
		pr_info("%s: current mode enable\n", __func__);
		break;
	}
}

static u8 buf_to_adc_reg(const char *buf)
{
	u8 adc_reg_num;

	if (kstrtou8(buf, 16, &adc_reg_num))
		return 0;

	if ((adc_reg_num >= S2MPS20_BUCK_START && adc_reg_num <= S2MPS20_BUCK_END) ||
		(adc_reg_num >= S2MPS20_LDO_START && adc_reg_num <= S2MPS20_LDO_END))
		return adc_reg_num;
	else
		return 0;
}

static ssize_t adc_reg_0_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	u8 adc_reg_num = buf_to_adc_reg(buf);
	if (!adc_reg_num)
		return -EINVAL;
	else {
		adc_meter->adc_reg[0] = adc_reg_num;
		adc_reg_update(dev);
		return count;
	}
}

static ssize_t adc_reg_1_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	u8 adc_reg_num = buf_to_adc_reg(buf);
	if (!adc_reg_num)
		return -EINVAL;
	else {
		adc_meter->adc_reg[1] = adc_reg_num;
		adc_reg_update(dev);
		return count;
	}
}

static ssize_t adc_reg_2_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	u8 adc_reg_num = buf_to_adc_reg(buf);
	if (!adc_reg_num)
		return -EINVAL;
	else {
		adc_meter->adc_reg[2] = adc_reg_num;
		adc_reg_update(dev);
		return count;
	}
}

static void adc_ctrl1_update(struct device *dev)
{
	/* ADC temporarily off */
	s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3, 0x00, ADC_EN_MASK);

	/* update ADC_CTRL1 register */
	s2mps20_write_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL1, adc_meter->adc_ctrl1);

	/* ADC Continuous ON */
	s2mps20_update_reg(adc_meter->i2c, S2MPS20_REG_ADC_CTRL3, ADC_EN_MASK, ADC_EN_MASK);
}

static ssize_t adc_ctrl1_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%2x\n", adc_meter->adc_ctrl1);
}

static ssize_t adc_ctrl1_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	u8 temp;

	ret = kstrtou8(buf, 16, &temp);
	if (ret)
		return -EINVAL;
	else {
		temp &= 0x0f;
		adc_meter->adc_ctrl1 &= 0xf0;
		adc_meter->adc_ctrl1 |= temp;
		adc_ctrl1_update(dev);
		return count;
	}
}

static DEVICE_ATTR(power_val_all, 0444, adc_val_power_show, NULL);
static DEVICE_ATTR(current_val_all, 0444, adc_val_current_show, NULL);
static DEVICE_ATTR(adc_en, 0644, adc_en_show, adc_en_store);
static DEVICE_ATTR(adc_mode, 0644, adc_mode_show, adc_mode_store);
static DEVICE_ATTR(adc_sync_mode, 0644, adc_sync_mode_show, adc_sync_mode_store);
static DEVICE_ATTR(adc_val_0, 0444, adc_val_0_show, NULL);
static DEVICE_ATTR(adc_val_1, 0444, adc_val_1_show, NULL);
static DEVICE_ATTR(adc_val_2, 0444, adc_val_2_show, NULL);
static DEVICE_ATTR(adc_reg_0, 0644, adc_reg_0_show, adc_reg_0_store);
static DEVICE_ATTR(adc_reg_1, 0644, adc_reg_1_show, adc_reg_1_store);
static DEVICE_ATTR(adc_reg_2, 0644, adc_reg_2_show, adc_reg_2_store);
static DEVICE_ATTR(adc_ctrl1, 0644, adc_ctrl1_show, adc_ctrl1_store);

void s2mps20_powermeter_init(struct s2mps20_dev *s2mps20)
{
	int i, ret;

	adc_meter = kzalloc(sizeof(struct adc_info), GFP_KERNEL);
	if (!adc_meter) {
		pr_err("%s: adc_meter alloc fail.\n", __func__);
		return;
	}

	adc_meter->current_val = kzalloc(sizeof(u16)*S2MPS20_MAX_ADC_CHANNEL, GFP_KERNEL);
	adc_meter->power_val = kzalloc(sizeof(u16)*S2MPS20_MAX_ADC_CHANNEL, GFP_KERNEL);
	adc_meter->adc_reg = kzalloc(sizeof(u8)*S2MPS20_MAX_ADC_CHANNEL, GFP_KERNEL);

	pr_info("%s: s2mps20 power meter init start\n", __func__);

	/* initial regulators : BUCK 1,2,3,4,5,6,7,8 */
	adc_meter->adc_reg[0] = 0x1;
	adc_meter->adc_reg[1] = 0x2;
	adc_meter->adc_reg[2] = 0x3;

	adc_meter->adc_mode = s2mps20->adc_mode;
	adc_meter->adc_sync_mode = s2mps20->adc_sync_mode;
	mutex_init(&adc_meter->adc_lock);

	/* SMP_NUM=1011(16384), RATIO=10(125khz), (8us x 16384 x 16 x 8ch)=~16s in case of async mode */
	if (adc_meter->adc_sync_mode == ASYNC_MODE) {
		adc_meter->adc_ctrl1 = 0x2B;
		s2mps20_write_reg(s2mps20->pmic, S2MPS20_REG_ADC_CTRL1, adc_meter->adc_ctrl1);
	}

	/* enable DC offset calibration */
	s2mps20_update_reg(s2mps20->pmic, S2MPS20_REG_ADC_CTRL2, ADC_CAL_EN_MASK, ADC_CAL_EN_MASK);

	/* CHANNEL setting */
	for (i = 0; i < S2MPS20_MAX_ADC_CHANNEL; i++) {
		s2mps20_update_reg(s2mps20->pmic, S2MPS20_REG_ADC_CTRL3, i + MUX_PTR_BASE, ADC_PTR_MASK);
		s2mps20_write_reg(s2mps20->pmic, S2MPS20_REG_ADC_DATA, adc_meter->adc_reg[i]);
	}

	/* set ptr_base according to adc_mode */
	switch (adc_meter->adc_mode) {
		case CURRENT_METER :
			adc_meter->ptr_base = CURRENT_PTR_BASE;
			break;
		case POWER_METER :
			adc_meter->ptr_base = POWER_PTR_BASE;
			break;
		default :
			adc_meter->adc_mode = CURRENT_METER;
			adc_meter->ptr_base = CURRENT_PTR_BASE;
			break;
	}

	/* turn on ADC */
	s2mps20_update_reg(s2mps20->pmic, S2MPS20_REG_ADC_CTRL3, ADC_EN_MASK, ADC_EN_MASK);

	adc_meter->i2c = s2mps20->pmic;

	s2mps20_adc_class = class_create(THIS_MODULE, "s2mps20_adc_meter");
	s2mps20_adc_dev = device_create(s2mps20_adc_class, NULL, 0, NULL, "s2mps20_adc");

	/* create sysfs entries */
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_en);
	if (ret)
		goto err_free;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_mode);
	if (ret)
		goto remove_adc_en;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_sync_mode);
	if (ret)
		goto remove_adc_mode;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_power_val_all);
	if (ret)
		goto remove_adc_sync_mode;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_current_val_all);
	if (ret)
		goto remove_power_val_all;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_val_0);
	if (ret)
		goto remove_current_val_all;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_val_1);
	if (ret)
		goto remove_adc_val_0;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_val_2);
	if (ret)
		goto remove_adc_val_1;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_reg_0);
	if (ret)
		goto remove_adc_val_2;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_reg_1);
	if (ret)
		goto remove_adc_reg_0;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_reg_2);
	if (ret)
		goto remove_adc_reg_1;
	ret = device_create_file(s2mps20_adc_dev, &dev_attr_adc_ctrl1);
	if (ret)
		goto remove_adc_reg_2;

	pr_info("%s: s2mps20 power meter init end\n", __func__);
	return ;

remove_adc_reg_2:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_reg_2);
remove_adc_reg_1:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_reg_1);
remove_adc_reg_0:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_reg_0);
remove_adc_val_2:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_val_2);
remove_adc_val_1:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_val_1);
remove_adc_val_0:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_val_0);
remove_current_val_all:
	device_remove_file(s2mps20_adc_dev, &dev_attr_current_val_all);
remove_power_val_all:
	device_remove_file(s2mps20_adc_dev, &dev_attr_power_val_all);
remove_adc_sync_mode:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_sync_mode);
remove_adc_mode:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_mode);
remove_adc_en:
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_en);
err_free:
	kfree(adc_meter->adc_val);
	kfree(adc_meter->adc_reg);
	dev_info(s2mps20->dev, "%s : fail to create sysfs\n",__func__);
	return ;
}

void s2mps20_powermeter_deinit(struct s2mps20_dev *s2mps20)
{
	/* remove sysfs entries */
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_en);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_mode);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_sync_mode);
	device_remove_file(s2mps20_adc_dev, &dev_attr_power_val_all);
	device_remove_file(s2mps20_adc_dev, &dev_attr_current_val_all);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_val_0);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_val_1);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_val_2);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_reg_0);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_reg_1);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_reg_2);
	device_remove_file(s2mps20_adc_dev, &dev_attr_adc_ctrl1);

	/* ADC turned off */
	s2mps20_update_reg(s2mps20->pmic, S2MPS20_REG_ADC_CTRL3, 0, ADC_EN_MASK);
	kfree(adc_meter->adc_val);
	kfree(adc_meter->adc_reg);
}
