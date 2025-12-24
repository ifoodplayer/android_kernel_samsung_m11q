/*
 * aw8864.c   aw8864 codec module
 *
 * Version: v1.1.3
 *
 * Copyright (c) 2017 AWINIC Technology CO., LTD
 *
 *  Author: Nick Li <liweilei@awinic.com.cn>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/debugfs.h>
#include <linux/version.h>
#include <linux/input.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/syscalls.h>
#include <sound/tlv.h>
#include "aw8864.h"
#include "aw8864_reg.h"

/******************************************************
 *
 * Marco
 *
 ******************************************************/
#define AW8864_I2C_NAME "aw8864_smartpa"


#define AW8864_VERSION "v1.1.3"

#define AW8864_RATES SNDRV_PCM_RATE_8000_48000
#define AW8864_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | \
                                    SNDRV_PCM_FMTBIT_S24_LE | \
                                    SNDRV_PCM_FMTBIT_S32_LE)

//#define AWINIC_I2C_REGMAP

#define AW_I2C_RETRIES 5
#define AW_I2C_RETRY_DELAY 5  // 5ms
#define AW_READ_CHIPID_RETRIES 5
#define AW_READ_CHIPID_RETRY_DELAY 5

static int aw8864_spk_control = 0;
static int aw8864_rcv_control = 0;

#define AW8864_MAX_FIRMWARE_LOAD_CNT 20
static char *aw8864_cfg_name = "aw8864_cfg.bin";

#ifdef AW8864_VBAT_MONITOR
static int aw8864_vbat_monitor_start(struct aw8864 *aw8864);
static int aw8864_vbat_monitor_stop(struct aw8864 *aw8864);
#endif
 /******************************************************
 *
 * aw8864 i2c write/read
 *
 ******************************************************/
#ifndef AWINIC_I2C_REGMAP
static int i2c_write(struct aw8864 *aw8864,
        unsigned char addr, unsigned int reg_data)
{
    int ret = -1;
    u8 wbuf[512] = {0};

    struct i2c_msg msgs[] = {
        {
            .addr   = aw8864->i2c->addr,
            .flags  = 0,
            .len    = 3,
            .buf    = wbuf,
        },
    };

    wbuf[0] = addr;
    wbuf[1] = (unsigned char)((reg_data & 0xff00)>>8);
    wbuf[2] = (unsigned char)(reg_data & 0x00ff);

    ret = i2c_transfer(aw8864->i2c->adapter, msgs, 1);
    if (ret < 0) {
        pr_err("%s: i2c write error: %d\n", __func__, ret);
    }

    return ret;
}

static int i2c_read(struct aw8864 *aw8864,
        unsigned char addr, unsigned int *reg_data)
{
    int ret = -1;
    unsigned char rbuf[512] = {0};
    unsigned int get_data = 0;

    struct i2c_msg msgs[] = {
        {
            .addr   = aw8864->i2c->addr,
            .flags  = 0,
            .len    = 1,
            .buf    = &addr,
        },
        {
            .addr   = aw8864->i2c->addr,
            .flags  = I2C_M_RD,
            .len    = 2,
            .buf    = rbuf,
        },
    };

    ret = i2c_transfer(aw8864->i2c->adapter, msgs, 2);
    if (ret < 0) {
        pr_err("%s: i2c read error: %d\n", __func__, ret);
        return ret;
    }

    get_data = (unsigned int)(rbuf[0] & 0x00ff);
    get_data <<= 8;
    get_data |= (unsigned int)rbuf[1];

    *reg_data = get_data;

    return ret;
}
#endif

static int aw8864_i2c_write(struct aw8864 *aw8864,
        unsigned char reg_addr, unsigned int reg_data)
{
    int ret = -1;
    unsigned char cnt = 0;

    while(cnt < AW_I2C_RETRIES) {
#ifdef AWINIC_I2C_REGMAP
    ret = regmap_write(aw8864->regmap, reg_addr, reg_data);
    if(ret < 0) {
        pr_err("%s: regmap_write cnt=%d error=%d\n", __func__, cnt, ret);
    } else {
        break;
    }
#else
    ret = i2c_write(aw8864, reg_addr, reg_data);
    if(ret < 0) {
        pr_err("%s: i2c_write cnt=%d error=%d\n", __func__, cnt, ret);
    } else {
        break;
    }
#endif
        cnt ++;
    }

    return ret;
}

static int aw8864_i2c_read(struct aw8864 *aw8864,
        unsigned char reg_addr, unsigned int *reg_data)
{
    int ret = -1;
    unsigned char cnt = 0;

    while(cnt < AW_I2C_RETRIES) {
#ifdef AWINIC_I2C_REGMAP
    ret = regmap_read(aw8864->regmap, reg_addr, reg_data);
    if(ret < 0) {
        pr_err("%s: regmap_read cnt=%d error=%d\n", __func__, cnt, ret);
    } else {
        break;
    }
#else
    ret = i2c_read(aw8864, reg_addr, reg_data);
    if(ret < 0) {
        pr_err("%s: i2c_read cnt=%d error=%d\n", __func__, cnt, ret);
    } else {
        break;
    }
#endif
        cnt ++;
    }

    return ret;
}

static int aw8864_i2c_write_bits(struct aw8864 *aw8864,
        unsigned char reg_addr, unsigned int mask, unsigned int reg_data)
{
    unsigned int reg_val;

    aw8864_i2c_read(aw8864, reg_addr, &reg_val);
    reg_val &= mask;
    reg_val |= reg_data;
    aw8864_i2c_write(aw8864, reg_addr, reg_val);

    return 0;
}

/******************************************************
 *
 * aw8864 control
 *
 ******************************************************/
static void aw8864_run_mute(struct aw8864 *aw8864, bool mute)
{
    pr_debug("%s enter\n", __func__);

    if(mute) {
        aw8864_i2c_write_bits(aw8864, AW8864_REG_PWMCTRL,
                AW8864_BIT_PWMCTRL_HMUTE_MASK, AW8864_BIT_PWMCTRL_HMUTE_ENABLE);
    } else {
        aw8864_i2c_write_bits(aw8864, AW8864_REG_PWMCTRL,
                AW8864_BIT_PWMCTRL_HMUTE_MASK, AW8864_BIT_PWMCTRL_HMUTE_DISABLE);
    }
}

static void aw8864_run_pwd(struct aw8864 *aw8864, bool pwd)
{
    pr_debug("%s enter\n", __func__);

    if(pwd) {
        aw8864_i2c_write_bits(aw8864, AW8864_REG_SYSCTRL,
                AW8864_BIT_SYSCTRL_PW_MASK, AW8864_BIT_SYSCTRL_PW_PDN);
    } else {
        aw8864_i2c_write_bits(aw8864, AW8864_REG_SYSCTRL,
                AW8864_BIT_SYSCTRL_PW_MASK, AW8864_BIT_SYSCTRL_PW_ACTIVE);
    }
}

static void aw8864_spk_rcv_mode(struct aw8864 *aw8864)
{
    pr_debug("%s spk_rcv=%d\n", __func__, aw8864->spk_rcv_mode);

    if(aw8864->spk_rcv_mode == AW8864_SPEAKER_MODE) {
        aw8864_i2c_write_bits(aw8864, AW8864_REG_SYSCTRL,
                AW8864_BIT_SYSCTRL_MODE_MASK, AW8864_BIT_SYSCTRL_SPK_MODE);
    } else if(aw8864->spk_rcv_mode == AW8864_RECEIVER_MODE){
        aw8864_i2c_write_bits(aw8864, AW8864_REG_SYSCTRL,
                AW8864_BIT_SYSCTRL_MODE_MASK, AW8864_BIT_SYSCTRL_RCV_MODE);
    } else {
    }
}


static void aw8864_start(struct aw8864 *aw8864)
{
    unsigned int reg_val = 0;
    unsigned int iis_check_max = 5;
    unsigned int i = 0;

    pr_debug("%s enter\n", __func__);

    aw8864_run_pwd(aw8864, false);
    msleep(2);
    for(i=0; i<iis_check_max; i++) {
        aw8864_i2c_read(aw8864, AW8864_REG_SYSST, &reg_val);
        if(reg_val & AW8864_BIT_SYSST_PLLS) {
              aw8864_run_mute(aw8864, false);
              pr_debug("%s iis signal check pass!\n", __func__);
#ifdef AW8864_VBAT_MONITOR
            aw8864_vbat_monitor_start(aw8864);
#endif
            return;
        }
        msleep(2);
    }
    aw8864_run_pwd(aw8864, true);
    pr_err("%s: iis signal check error\n", __func__);
}
static void aw8864_stop(struct aw8864 *aw8864)
{
    pr_debug("%s enter\n", __func__);

    aw8864_run_mute(aw8864, true);
    aw8864_run_pwd(aw8864, true);
#ifdef AW8864_VBAT_MONITOR
    aw8864_vbat_monitor_stop(aw8864);
#endif
}

static void aw8864_container_update(struct aw8864 *aw8864,
        struct aw8864_container *aw8864_cont)
{
    int i = 0;
    int reg_addr = 0;
    int reg_val = 0;

    pr_debug("%s enter\n", __func__);

    for(i=0; i<aw8864_cont->len; i+=4) {
        reg_addr = (aw8864_cont->data[i+1]<<8) + aw8864_cont->data[i+0];
        reg_val = (aw8864_cont->data[i+3]<<8) + aw8864_cont->data[i+2];
        pr_info("%s: reg=0x%04x, val = 0x%04x\n", __func__, reg_addr, reg_val);
        aw8864_i2c_write(aw8864, (unsigned char)reg_addr, (unsigned int)reg_val);
    }

    pr_debug("%s exit\n", __func__);
}

static void aw8864_cfg_loaded(const struct firmware *cont, void *context)
{
    struct aw8864 *aw8864 = context;
    struct aw8864_container *aw8864_cfg;
    unsigned int i = 0;

    if (!cont) {
        pr_err("%s: failed to read %s\n", __func__, aw8864_cfg_name);
        release_firmware(cont);
        return;
    }

    pr_info("%s: loaded %s - size: %zu\n", __func__, aw8864_cfg_name,
            cont ? cont->size : 0);

    for(i=0; i<cont->size; i++) {
        pr_info("%s: addr:0x%04x, data:0x%02x\n", __func__, i, *(cont->data+i));
    }

    aw8864_cfg = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
    if (!aw8864_cfg) {
        release_firmware(cont);
        pr_err("%s: error allocating memory\n", __func__);
        return;
    }
    aw8864_cfg->len = cont->size;
    memcpy(aw8864_cfg->data, cont->data, cont->size);
    release_firmware(cont);

    aw8864_container_update(aw8864, aw8864_cfg);

    kfree(aw8864_cfg);

    aw8864->init = 1;
    pr_info("%s: cfg update complete\n", __func__);

    aw8864_spk_rcv_mode(aw8864);
    aw8864_start(aw8864);
}

static int aw8864_load_cfg(struct aw8864 *aw8864)
{
    pr_info("%s enter\n", __func__);

    return request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
            aw8864_cfg_name, aw8864->dev, GFP_KERNEL,
            aw8864, aw8864_cfg_loaded);
}

static void aw8864_cold_start(struct aw8864 *aw8864)
{
    int ret = -1;

    pr_info("%s enter\n", __func__);

    ret = aw8864_load_cfg(aw8864);
    if(ret) {
        pr_err("%s: cfg loading requested failed: %d\n", __func__, ret);
    }
}

static void aw8864_smartpa_cfg(struct aw8864 *aw8864, bool flag)
{
    pr_info("%s, flag = %d\n", __func__, flag);

    if(flag == true) {
        if(aw8864->init == 0) {
            pr_info("%s, init = %d\n", __func__, aw8864->init);
            aw8864_cold_start(aw8864);
        } else {
            aw8864_spk_rcv_mode(aw8864);
            aw8864_start(aw8864);
        }
    } else {
        aw8864_stop(aw8864);
    }
}

/******************************************************
 *
 * kcontrol
 *
 ******************************************************/
 static const char *const spk_function[] = { "Off", "On" };
 static const char *const rcv_function[] = { "Off", "On" };
 static const DECLARE_TLV_DB_SCALE(digital_gain,0,50,0);

 struct soc_mixer_control aw8864_mixer ={
    .reg    = AW8864_REG_HAGCCFG7,
    .shift  = AW8864_VOL_REG_SHIFT,
    .max    = AW8864_VOLUME_MAX,
    .min    = AW8864_VOLUME_MIN,
 };

static int aw8864_volume_info(struct snd_kcontrol *kcontrol,struct snd_ctl_elem_info *uinfo)
{
    struct soc_mixer_control *mc = (struct soc_mixer_control*) kcontrol->private_value;

    //set kcontrol info
    uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    uinfo->count = 1;
    uinfo->value.integer.min = 0;
    uinfo->value.integer.max = mc->max - mc->min;
    return 0;
}

static int aw8864_volume_get(struct snd_kcontrol *kcontrol,struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);
    unsigned int reg_val = 0;
    unsigned int value = 0;
    struct soc_mixer_control *mc = (struct soc_mixer_control*) kcontrol->private_value;

    aw8864_i2c_read(aw8864, AW8864_REG_HAGCCFG7, &reg_val);
    ucontrol->value.integer.value[0] = (value >> mc->shift)\
            &(AW8864_BIT_HAGCCFG7_VOL_MASK);
    return 0;
}

static int aw8864_volume_put(struct snd_kcontrol *kcontrol,struct snd_ctl_elem_value *ucontrol)
{
    struct soc_mixer_control *mc = (struct soc_mixer_control*) kcontrol->private_value;
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);
    unsigned int value = 0;
    unsigned int reg_value = 0;

    //value is right
    value = ucontrol->value.integer.value[0];
    if(value > (mc->max-mc->min)|| value <0){
      pr_err("%s:value over range \n",__func__);
      return -1;
    }

    //smartpa have clk
    aw8864_i2c_read(aw8864, AW8864_REG_SYSST, &reg_value);
    if(!(reg_value&AW8864_BIT_SYSST_PLLS)){
      pr_err("%s: NO I2S CLK ,cat not write reg \n",__func__);
      return 0;
    }
    //cal real value
    value = value << mc->shift&AW8864_BIT_HAGCCFG7_VOL_MASK;
    aw8864_i2c_read(aw8864, AW8864_REG_HAGCCFG7, &reg_value);
    value = value | (reg_value&0x00ff);

    //write value
    aw8864_i2c_write(aw8864, AW8864_REG_HAGCCFG7, value);

    return 0;
}

static struct snd_kcontrol_new aw8864_volume = {
    .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
    .name  = "aw8864_rx_volume",
    .access= SNDRV_CTL_ELEM_ACCESS_TLV_READ|SNDRV_CTL_ELEM_ACCESS_READWRITE,
    .tlv.p  = (digital_gain),
    .info = aw8864_volume_info,
    .get =  aw8864_volume_get,
    .put =  aw8864_volume_put,
    .private_value = (unsigned long)&aw8864_mixer,
};

static int aw8864_spk_get(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
    pr_debug("%s: aw8864_spk_control=%d\n", __func__, aw8864_spk_control);
    ucontrol->value.integer.value[0] = aw8864_spk_control;
    return 0;
}

static int aw8864_spk_set(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);

    pr_debug("%s: ucontrol->value.integer.value[0]=%ld\n ",
            __func__, ucontrol->value.integer.value[0]);
    if(ucontrol->value.integer.value[0] == aw8864_spk_control)
        return 1;

    aw8864_spk_control = ucontrol->value.integer.value[0];

    aw8864->spk_rcv_mode = AW8864_SPEAKER_MODE;

    aw8864_spk_rcv_mode(aw8864);

    return 0;
}

static int aw8864_rcv_get(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
    pr_debug("%s: aw8864_rcv_control=%d\n", __func__, aw8864_rcv_control);
    ucontrol->value.integer.value[0] = aw8864_rcv_control;
    return 0;
}
static int aw8864_rcv_set(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);
    pr_debug("%s: ucontrol->value.integer.value[0]=%ld\n ",
            __func__, ucontrol->value.integer.value[0]);
    if(ucontrol->value.integer.value[0] == aw8864_rcv_control)
        return 1;

    aw8864_rcv_control = ucontrol->value.integer.value[0];

    aw8864->spk_rcv_mode = AW8864_RECEIVER_MODE;

    aw8864_spk_rcv_mode(aw8864);

    return 0;
}
static const struct soc_enum aw8864_snd_enum[] = {
    SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(spk_function), spk_function),
    SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(rcv_function), rcv_function),
};

static struct snd_kcontrol_new aw8864_controls[] = {
    SOC_ENUM_EXT("aw8864_speaker_switch", aw8864_snd_enum[0],
            aw8864_spk_get, aw8864_spk_set),
    SOC_ENUM_EXT("aw8864_receiver_switch", aw8864_snd_enum[1],
            aw8864_rcv_get, aw8864_rcv_set),
};

static void aw8864_add_codec_controls(struct aw8864 *aw8864)
{
    pr_info("%s enter\n", __func__);

    snd_soc_add_codec_controls(aw8864->codec, aw8864_controls,
            ARRAY_SIZE(aw8864_controls));

    snd_soc_add_codec_controls(aw8864->codec, &aw8864_volume,1);
}

/******************************************************
 *
 * DAPM Widget & Route
 *
 ******************************************************/
#if 0
static struct snd_soc_dapm_widget aw8864_dapm_widgets_common[] = {
    /* Stream widgets */
    SND_SOC_DAPM_AIF_IN("AIF_IN", "AW89xx_AIF_Playback", 0, SND_SOC_NOPM, 0, 0),
    SND_SOC_DAPM_AIF_OUT("AIF_OUT", "AW89xx_AIF_Capture", 0, SND_SOC_NOPM, 0, 0),

    SND_SOC_DAPM_OUTPUT("OUTL"),
    SND_SOC_DAPM_INPUT("AEC_Loopback"),
};

static const struct snd_soc_dapm_route aw8864_dapm_routes_common[] = {
    { "OUTL", NULL, "AIF_IN" },
    { "AIF_OUT", NULL, "AEC_Loopback" },
};

static void aw8864_add_widgets(struct aw8864 *aw8864)
{
    //struct snd_soc_dapm_context *dapm = &aw8864->codec->dapm;
    struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(aw8864->codec);
    struct snd_soc_dapm_widget *widgets;

    pr_info("%s enter\n", __func__);
    widgets = devm_kzalloc(&aw8864->i2c->dev,
            sizeof(struct snd_soc_dapm_widget) *
            ARRAY_SIZE(aw8864_dapm_widgets_common),
            GFP_KERNEL);
    if (!widgets)
        return;

    memcpy(widgets, aw8864_dapm_widgets_common,
            sizeof(struct snd_soc_dapm_widget) *
            ARRAY_SIZE(aw8864_dapm_widgets_common));

    snd_soc_dapm_new_controls(dapm, widgets,
            ARRAY_SIZE(aw8864_dapm_widgets_common));
    snd_soc_dapm_add_routes(dapm, aw8864_dapm_routes_common,
            ARRAY_SIZE(aw8864_dapm_routes_common));
}
#endif
/******************************************************
 *
 * Digital Audio Interface
 *
 ******************************************************/
static int aw8864_startup(struct snd_pcm_substream *substream,
        struct snd_soc_dai *dai)
{
    struct snd_soc_codec *codec = dai->codec;
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);

    pr_info("%s: enter\n", __func__);
    aw8864_run_pwd(aw8864, false);

    return 0;
}

static int aw8864_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
    //struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(dai->codec);
    struct snd_soc_codec *codec = dai->codec;

    pr_info("%s: fmt=0x%x\n", __func__, fmt);

    /* Supported mode: regular I2S, slave, or PDM */
    switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
    case SND_SOC_DAIFMT_I2S:
        if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS) {
            dev_err(codec->dev, "%s: invalid codec master mode\n", __func__);
            return -EINVAL;
        }
        break;
    default:
        dev_err(codec->dev, "%s: unsupported DAI format %d\n", __func__,
                fmt & SND_SOC_DAIFMT_FORMAT_MASK);
        return -EINVAL;
    }
    return 0;
}

static int aw8864_set_dai_sysclk(struct snd_soc_dai *codec_dai,
        int clk_id, unsigned int freq, int dir)
{
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec_dai->codec);

    pr_info("%s: freq=%d\n", __func__, freq);

    aw8864->sysclk = freq;
    return 0;
}

static int aw8864_hw_params(struct snd_pcm_substream *substream,
    struct snd_pcm_hw_params *params,
    struct snd_soc_dai *dai)
{
    struct snd_soc_codec *codec = dai->codec;
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);
    unsigned int rate = 0;
    int reg_value = 0;
    int width = 0;
    /* Supported */

    //get rate param
    aw8864->rate=rate = params_rate(params);
    pr_debug("%s: requested rate: %d, sample size: %d\n", __func__, rate,
            snd_pcm_format_width(params_format(params)));
    //match rate
    switch(rate)
    {
        case 8000:
            reg_value = AW8864_BIT_I2SCTRL_SR_8K;
            break;
        case 16000:
            reg_value = AW8864_BIT_I2SCTRL_SR_16K;
            break;
        case 32000:
            reg_value = AW8864_BIT_I2SCTRL_SR_32K;
            break;
        case 44100:
            reg_value = AW8864_BIT_I2SCTRL_SR_44P1K;
            break;
        case 48000:
            reg_value = AW8864_BIT_I2SCTRL_SR_48K;
            break;
        default:
            reg_value = AW8864_BIT_I2SCTRL_SR_48K;
            pr_err("%s: rate can not support\n", __func__);
            break;
    }
    //set chip rate
    if(-1 != reg_value){
        aw8864_i2c_write_bits(aw8864, AW8864_REG_I2SCTRL,
                AW8864_BIT_I2SCTRL_SR_MASK, reg_value);
    }

    //get bit width
    width = params_width(params);
    pr_debug("%s: width = %d \n",__func__,width);
    switch(width)
    {
        case 16:
            reg_value = AW8864_BIT_I2SCTRL_FMS_16BIT;
            break;
        case 20:
            reg_value = AW8864_BIT_I2SCTRL_FMS_20BIT;
            break;
        case 24:
            reg_value = AW8864_BIT_I2SCTRL_FMS_24BIT;
            break;
        case 32:
            reg_value = AW8864_BIT_I2SCTRL_FMS_32BIT;
            break;
        default:
            reg_value = AW8864_BIT_I2SCTRL_FMS_16BIT;
            pr_err("%s: width can not support\n", __func__);
            break;
    }
    //set width
    if(-1 != reg_value){
        aw8864_i2c_write_bits(aw8864, AW8864_REG_I2SCTRL,
                AW8864_BIT_I2SCTRL_FMS_MASK, reg_value);
    }

    return 0;
}

static int aw8864_mute(struct snd_soc_dai *dai, int mute, int stream)
{
    struct snd_soc_codec *codec = dai->codec;
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);

    pr_info("%s: mute state=%d\n", __func__, mute);

    if (!(aw8864->flags & AW8864_FLAG_START_ON_MUTE))
        return 0;

    if (mute) {
        if (stream == SNDRV_PCM_STREAM_PLAYBACK)
            aw8864->pstream = 0;
        else
            aw8864->cstream = 0;
        if (aw8864->pstream != 0 || aw8864->cstream != 0)
            return 0;

        mutex_lock(&aw8864->cfg_lock);
        aw8864_smartpa_cfg(aw8864, false);
        mutex_unlock(&aw8864->cfg_lock);
    } else {
        if (stream == SNDRV_PCM_STREAM_PLAYBACK)
            aw8864->pstream = 1;
        else
            aw8864->cstream = 1;

        mutex_lock(&aw8864->cfg_lock);
        aw8864_smartpa_cfg(aw8864, true);
        mutex_unlock(&aw8864->cfg_lock);
    }

    return 0;
}

static void aw8864_shutdown(struct snd_pcm_substream *substream,
        struct snd_soc_dai *dai)
{
    struct snd_soc_codec *codec = dai->codec;
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);

    aw8864->rate = 0;
    aw8864_run_pwd(aw8864, true);
}

static const struct snd_soc_dai_ops aw8864_dai_ops = {
    .startup = aw8864_startup,
    .set_fmt = aw8864_set_fmt,
    .set_sysclk = aw8864_set_dai_sysclk,
    .hw_params = aw8864_hw_params,
    .mute_stream = aw8864_mute,
    .shutdown = aw8864_shutdown,
};

static struct snd_soc_dai_driver aw8864_dai[] = {
    {
        .name = "aw8864-aif",
        .id = 1,
        .playback = {
            .stream_name = "Speaker_Playback",
            .channels_min = 1,
            .channels_max = 2,
            .rates = AW8864_RATES,
            .formats = AW8864_FORMATS,
        },
        .capture = {
            .stream_name = "Speaker_Capture",
            .channels_min = 1,
            .channels_max = 2,
            .rates = AW8864_RATES,
            .formats = AW8864_FORMATS,
         },
        .ops = &aw8864_dai_ops,
        .symmetric_rates = 1,
        .symmetric_channels = 1,
        .symmetric_samplebits = 1,
    },
};

/*****************************************************
 *
 * codec driver
 *
 *****************************************************/
extern int card_smartpa;
static int aw8864_probe(struct snd_soc_codec *codec)
{
    struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);
    int ret = 0;

    pr_info("%s enter\n", __func__);

    aw8864->codec = codec;

    //aw8864_add_widgets(aw8864);

    aw8864_add_codec_controls(aw8864);

    if (codec->dev->of_node)
        dev_set_name(codec->dev, "%s", "aw8864_smartpa");

    pr_info("%s exit\n", __func__);

    return ret;
}

static int aw8864_remove(struct snd_soc_codec *codec)
{
    //struct aw8864 *aw8864 = snd_soc_codec_get_drvdata(codec);
    pr_info("%s enter\n", __func__);

    //aw8864_inputdev_unregister(aw8864);

    return 0;
}

/*
struct regmap *aw8864_get_regmap(struct device *dev)
{
    struct aw8864 *aw8864 = dev_get_drvdata(dev);

    return aw8864->regmap;
}
*/

static unsigned int aw8864_codec_read(struct snd_soc_codec *codec,unsigned int reg)
{
    struct aw8864 *aw8864=snd_soc_codec_get_drvdata(codec);
    unsigned int value =0;
    int ret;
    pr_debug("%s:enter \n",__func__);

    if(aw8864_reg_access[reg]&REG_RD_ACCESS){
        ret=aw8864_i2c_read(aw8864,reg,&value);
    if(ret<0){
        pr_debug("%s: read register failed \n",__func__);
        return ret;
    }
    }else{
        pr_debug("%s:Register 0x%x NO read access\n",__func__,reg);
        return -1;
    }
    return value;
}

static int aw8864_codec_write(struct snd_soc_codec *codec,unsigned int reg,unsigned int value)
{
    int ret ;
    struct aw8864 *aw8864=snd_soc_codec_get_drvdata(codec);
    pr_debug("%s:enter ,reg is 0x%x value is 0x%x\n",__func__,reg,value);

    if(aw8864_reg_access[reg]&REG_WR_ACCESS){
        ret=aw8864_i2c_write(aw8864,reg,value);
        return ret;
    }else{
        pr_debug("%s: Register 0x%x NO write access \n",__func__,reg);
    }

    return -1;
}
/*
static int aw8864_codec_readable(struct snd_soc_codec *codec,unsigned int reg)
{
    return aw8864_reg_access[reg]&REG_RD_ACCESS;
}
*/
static struct snd_soc_codec_driver soc_codec_dev_aw8864 = {
    .probe = aw8864_probe,
    .remove = aw8864_remove,
    //.get_regmap = aw8864_get_regmap,
    .read = aw8864_codec_read,
    .write= aw8864_codec_write,
    .reg_cache_size= AW8864_REG_MAX,
    .reg_word_size=2,
};

/*****************************************************
 *
 * regmap
 *
 *****************************************************/
bool aw8864_writeable_register(struct device *dev, unsigned int reg)
{
    /* enable read access for all registers */
    return 1;
}

bool aw8864_readable_register(struct device *dev, unsigned int reg)
{
    /* enable read access for all registers */
    return 1;
}

bool aw8864_volatile_register(struct device *dev, unsigned int reg)
{
    /* enable read access for all registers */
    return 1;
}

static const struct regmap_config aw8864_regmap = {
    .reg_bits = 8,
    .val_bits = 16,

    .max_register = AW8864_MAX_REGISTER,
    .writeable_reg = aw8864_writeable_register,
    .readable_reg = aw8864_readable_register,
    .volatile_reg = aw8864_volatile_register,
    .cache_type = REGCACHE_RBTREE,
};

/******************************************************
 *
 * irq
 *
 ******************************************************/
static void aw8864_interrupt_setup(struct aw8864 *aw8864)
{
    unsigned int reg_val;

    pr_info("%s enter\n", __func__);

    aw8864_i2c_read(aw8864, AW8864_REG_SYSINTM, &reg_val);
    reg_val &= (~AW8864_BIT_SYSINTM_PLLM);
    reg_val &= (~AW8864_BIT_SYSINTM_OTHM);
    reg_val &= (~AW8864_BIT_SYSINTM_OCDM);
    aw8864_i2c_write(aw8864, AW8864_REG_SYSINTM, reg_val);
}

static void aw8864_interrupt_clear(struct aw8864 *aw8864)
{
    unsigned int reg_val = 0;

    pr_info("%s enter\n", __func__);

    aw8864_i2c_read(aw8864, AW8864_REG_SYSST, &reg_val);
    pr_info("%s: reg SYSST=0x%x\n", __func__, reg_val);

    aw8864_i2c_read(aw8864, AW8864_REG_SYSINT, &reg_val);
    pr_info("%s: reg SYSINT=0x%x\n", __func__, reg_val);

    aw8864_i2c_read(aw8864, AW8864_REG_SYSINTM, &reg_val);
    pr_info("%s: reg SYSINTM=0x%x\n", __func__, reg_val);
}

static irqreturn_t aw8864_irq(int irq, void *data)
{
    struct aw8864 *aw8864 = data;

    pr_info("%s enter\n", __func__);

    aw8864_interrupt_clear(aw8864);

    pr_info("%s exit\n", __func__);

    return IRQ_HANDLED;
}

/*****************************************************
 *
 * device tree
 *
 *****************************************************/
static int aw8864_parse_dt(struct device *dev, struct aw8864 *aw8864,
        struct device_node *np)
{
    aw8864->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
    if (aw8864->reset_gpio < 0) {
        dev_err(dev, "%s: no reset gpio provided, will not HW reset device\n", __func__);
        //return -1;
    } else {
        dev_info(dev, "%s: reset gpio provided ok\n", __func__);
    }
    aw8864->irq_gpio =  of_get_named_gpio(np, "irq-gpio", 0);
    if (aw8864->irq_gpio < 0) {
        dev_info(dev, "%s: no irq gpio provided.\n", __func__);
    } else {
        dev_info(dev, "%s: irq gpio provided ok.\n", __func__);
    }

    return 0;
}

int aw8864_hw_reset(struct aw8864 *aw8864)
{
    pr_info("%s enter\n", __func__);

    if (aw8864 && gpio_is_valid(aw8864->reset_gpio)) {
        gpio_set_value_cansleep(aw8864->reset_gpio, 0);
        msleep(1);
        gpio_set_value_cansleep(aw8864->reset_gpio, 1);
        msleep(1);
    } else {
        dev_err(aw8864->dev, "%s:  failed\n", __func__);
    }
    return 0;
}

/*****************************************************
 *
 * check chip id
 *
 *****************************************************/
int aw8864_read_chipid(struct aw8864 *aw8864)
{
    int ret = -1;
    unsigned int cnt = 0;
    unsigned int reg = 0;

    while(cnt < AW_READ_CHIPID_RETRIES) {
        ret = aw8864_i2c_read(aw8864, AW8864_REG_ID, &reg);
        if (ret < 0) {
            dev_err(aw8864->dev, "%s: failed to read register AW8864_REG_ID: %d\n", __func__, ret);
            return -EIO;
        }
        switch (reg) {
        case 0x1702:
            pr_info("%s aw8864 detected\n", __func__);
            aw8864->flags |= AW8864_FLAG_SKIP_INTERRUPTS;
            aw8864->flags |= AW8864_FLAG_START_ON_MUTE;
            aw8864->chipid = AW8864_ID;
            pr_info("%s aw8864->flags=0x%x\n", __func__, aw8864->flags);
            return 0;
        default:
            pr_info("%s unsupported device revision (0x%x)\n", __func__, reg );
            break;
        }
        cnt ++;

        msleep(AW_READ_CHIPID_RETRY_DELAY);
    }

    return -EINVAL;
}

/*****************************************************
 *
 * vbat monitor
 *
 *****************************************************/
#ifdef AW8864_VBAT_MONITOR
static int aw8864_vbat_monitor_stop(struct aw8864 *aw8864)
{
    pr_info("%s enter\n", __func__);

    if(hrtimer_active(&aw8864->vbat_monitor_timer)) {
        pr_info("%s: cancel vbat monitor\n", __func__);
        hrtimer_cancel(&aw8864->vbat_monitor_timer);
    }
    return 0;
}

static int aw8864_vbat_monitor_start(struct aw8864 *aw8864)
{
    int ram_timer_val = 30000;

    pr_info("%s enter\n", __func__);

    if(hrtimer_active(&aw8864->vbat_monitor_timer)) {
    } else {
        pr_info("%s: start vbat monitor\n", __func__);
        hrtimer_start(&aw8864->vbat_monitor_timer,
                ktime_set(ram_timer_val/1000, (ram_timer_val%1000)*1000000),
                HRTIMER_MODE_REL);
    }
    return 0;
}

static enum hrtimer_restart aw8864_vbat_monitor_timer_func(struct hrtimer *timer)
{
    struct aw8864 *aw8864 = container_of(timer, struct aw8864, vbat_monitor_timer);

    pr_info("%s enter\n", __func__);

    schedule_work(&aw8864->vbat_monitor_work);

    return HRTIMER_NORESTART;
}

static int aw8864_get_sys_battery_info(char *dev)
{
    int fd;
    int eCheck;
    int nReadSize;
    char buf[64],*pvalue;
    mm_segment_t oldfs;

    oldfs = get_fs();
    set_fs(KERNEL_DS);
    fd = sys_open(dev, O_RDONLY, 0);
    if (fd < 0) {
        pr_err("%s: open fail dev:%s fd:%d\n", __func__, dev, fd);
        set_fs(oldfs);
        return fd;
    }

    nReadSize = sys_read(fd, buf, sizeof(buf) - 1);
    pr_debug("%s: nReadSize:%d\n", __func__, nReadSize);

    eCheck = simple_strtoul(buf,&pvalue,10);
    pr_debug("%s: eCheck = %d\n", __func__, eCheck);

    set_fs(oldfs);
    sys_close(fd);

    if (eCheck > 0)
        return eCheck;
    else
        return 0;
}

static void aw8864_vbat_monitor_work_routine(struct work_struct *work)
{
    struct aw8864 *aw8864 = container_of(work, struct aw8864, vbat_monitor_work);
    unsigned int reg_val = 0;
    int sys_vbat_vol = 0;

    pr_info("%s enter\n", __func__);

    aw8864_i2c_read(aw8864, AW8864_REG_PWMCTRL, &reg_val);
    if((reg_val&AW8864_BIT_PWMCTRL_HMUTE_ENABLE) == AW8864_BIT_PWMCTRL_HMUTE_DISABLE) {
        sys_vbat_vol = aw8864_get_sys_battery_info(SYS_BAT_DEV);
        pr_info("%s: get sys battery = %d\n", __func__, sys_vbat_vol);
        if((sys_vbat_vol < AW8864_SYS_VBAT_LIMIT) && (sys_vbat_vol > AW8864_SYS_VBAT_MIN)) {
            aw8864_i2c_write_bits(aw8864, AW8864_REG_GENCTRL,
                AW8864_BIT_GENCTRL_BST_ILIMIT_MASK, (aw8864->bst_ilimit<<4));
        }
        aw8864_vbat_monitor_start(aw8864);
    }
}

static int aw8864_vbat_monitor_init(struct aw8864 *aw8864)
{
    pr_info("%s enter\n", __func__);

    aw8864->bst_ilimit = 0x00;

    hrtimer_init(&aw8864->vbat_monitor_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    aw8864->vbat_monitor_timer.function = aw8864_vbat_monitor_timer_func;
    INIT_WORK(&aw8864->vbat_monitor_work, aw8864_vbat_monitor_work_routine);
    return 0;
}
#endif
/******************************************************
 *
 * sys bin attribute
 *
 *****************************************************/
static ssize_t aw8864_reg_write(struct file *filp, struct kobject *kobj,
        struct bin_attribute *bin_attr,
        char *buf, loff_t off, size_t count)
{
    struct device *dev = container_of(kobj, struct device, kobj);
    struct aw8864 *aw8864 = dev_get_drvdata(dev);

    if (count != 1) {
        pr_info("invalid register address");
        return -EINVAL;
    }

    aw8864->reg = buf[0];

    return 1;
}

static ssize_t aw8864_rw_write(struct file *filp, struct kobject *kobj,
        struct bin_attribute *bin_attr,
        char *buf, loff_t off, size_t count)
{
    struct device *dev = container_of(kobj, struct device, kobj);
    struct aw8864 *aw8864 = dev_get_drvdata(dev);
    u8 *data;
    int ret;
    int retries = AW_I2C_RETRIES;

    data = kmalloc(count+1, GFP_KERNEL);
    if (data == NULL) {
        pr_err("can not allocate memory\n");
        return  -ENOMEM;
    }

    data[0] = aw8864->reg;
    memcpy(&data[1], buf, count);

    retry:
    ret = i2c_master_send(aw8864->i2c, data, count+1);
    if (ret < 0) {
        pr_warn("i2c error, retries left: %d\n", retries);
        if (retries) {
              retries--;
              msleep(AW_I2C_RETRY_DELAY);
              goto retry;
        }
    }

    kfree(data);
    return ret;
}

static ssize_t aw8864_rw_read(struct file *filp, struct kobject *kobj,
        struct bin_attribute *bin_attr,
        char *buf, loff_t off, size_t count)
{
    struct device *dev = container_of(kobj, struct device, kobj);
    struct aw8864 *aw8864 = dev_get_drvdata(dev);
    struct i2c_msg msgs[] = {
        {
            .addr = aw8864->i2c->addr,
            .flags = 0,
            .len = 1,
            .buf = &aw8864->reg,
        },
        {
            .addr = aw8864->i2c->addr,
            .flags = I2C_M_RD,
            .len = count,
            .buf = buf,
        },
    };
    int ret;
    int retries = AW_I2C_RETRIES;
    retry:
    ret = i2c_transfer(aw8864->i2c->adapter, msgs, ARRAY_SIZE(msgs));
    if (ret < 0) {
        pr_warn("i2c error, retries left: %d\n", retries);
        if (retries) {
            retries--;
            msleep(AW_I2C_RETRY_DELAY);
            goto retry;
        }
        return ret;
    }
    /* ret contains the number of i2c messages send */
    return 1 + ((ret > 1) ? count : 0);
}

static struct bin_attribute dev_attr_rw = {
    .attr = {
        .name = "rw",
        .mode = S_IRUSR | S_IWUSR,
    },
    .size = 0,
    .read = aw8864_rw_read,
    .write = aw8864_rw_write,
};

static struct bin_attribute dev_attr_regaddr = {
    .attr = {
        .name = "regaddr",
        .mode = S_IWUSR,
    },
    .size = 0,
    .read = NULL,
    .write = aw8864_reg_write,
};

/******************************************************
 *
 * sys group attribute: reg
 *
 ******************************************************/
static ssize_t aw8864_reg_store(struct device *dev, struct device_attribute *attr,
        const char *buf, size_t count)
{
    struct aw8864 *aw8864 = dev_get_drvdata(dev);

    unsigned int databuf[2] = {0};

    if(2 == sscanf(buf, "%x %x", &databuf[0], &databuf[1])) {
        aw8864_i2c_write(aw8864, databuf[0], databuf[1]);
    }

    return count;
}

static ssize_t aw8864_reg_show(struct device *dev, struct device_attribute *attr,
        char *buf)
{
    struct aw8864 *aw8864 = dev_get_drvdata(dev);
    ssize_t len = 0;
    unsigned char i = 0;
    unsigned int reg_val = 0;
    for(i = 0; i < AW8864_REG_MAX; i ++) {
    if(!(aw8864_reg_access[i]&REG_RD_ACCESS))
       continue;
        aw8864_i2c_read(aw8864, i, &reg_val);
        len += snprintf(buf+len, PAGE_SIZE-len, "reg:0x%02x=0x%04x \n", i, reg_val);
    }
    return len;
}

static ssize_t aw8864_spk_rcv_store(struct device *dev, struct device_attribute *attr,
        const char *buf, size_t count)
{
    struct aw8864 *aw8864 = dev_get_drvdata(dev);

    unsigned int databuf[2] = {0};

    if(1 == sscanf(buf, "%d", &databuf[0])) {
        aw8864->spk_rcv_mode = databuf[0];
    }

    return count;
}

static ssize_t aw8864_spk_rcv_show(struct device *dev, struct device_attribute *attr,
        char *buf)
{
    struct aw8864 *aw8864 = dev_get_drvdata(dev);
    ssize_t len = 0;
    if(aw8864->spk_rcv_mode == AW8864_SPEAKER_MODE) {
        len += snprintf(buf+len, PAGE_SIZE-len, "aw8864 spk_rcv: %d, speaker mode\n", aw8864->spk_rcv_mode);
    } else if (aw8864->spk_rcv_mode == AW8864_RECEIVER_MODE) {
        len += snprintf(buf+len, PAGE_SIZE-len, "aw8864 spk_rcv: %d, receiver mode\n", aw8864->spk_rcv_mode);
    } else {
        len += snprintf(buf+len, PAGE_SIZE-len, "aw8864 spk_rcv: %d, unknown mode\n", aw8864->spk_rcv_mode);
    }

    return len;
}

static ssize_t aw8864_bst_ilimit_store(struct device *dev, struct device_attribute *attr,
        const char *buf, size_t count)
{
    struct aw8864 *aw8864 = dev_get_drvdata(dev);

    unsigned int databuf[2] = {0};

    if(1 == sscanf(buf, "%x", &databuf[0])) {
        aw8864->bst_ilimit = databuf[0];
    }

    return count;
}

static ssize_t aw8864_bst_ilimit_show(struct device *dev, struct device_attribute *attr,
        char *buf)
{
    struct aw8864 *aw8864 = dev_get_drvdata(dev);
    ssize_t len = 0;

    len += snprintf(buf+len, PAGE_SIZE-len, "aw8864 bst_ilimit=0x%02x\n", aw8864->bst_ilimit);

    return len;
}
static DEVICE_ATTR(reg, S_IWUSR | S_IRUGO, aw8864_reg_show, aw8864_reg_store);
static DEVICE_ATTR(spk_rcv, S_IWUSR | S_IRUGO, aw8864_spk_rcv_show, aw8864_spk_rcv_store);
static DEVICE_ATTR(bst_ilimit, S_IWUSR | S_IRUGO, aw8864_bst_ilimit_show, aw8864_bst_ilimit_store);

static struct attribute *aw8864_attributes[] = {
    &dev_attr_reg.attr,
    &dev_attr_spk_rcv.attr,
    &dev_attr_bst_ilimit.attr,
    NULL
};

static struct attribute_group aw8864_attribute_group = {
    .attrs = aw8864_attributes
};


/******************************************************
 *
 * i2c driver
 *
 ******************************************************/
static int aw8864_i2c_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
    struct snd_soc_dai_driver *dai;
    struct aw8864 *aw8864;
    struct device_node *np = i2c->dev.of_node;
    int irq_flags = 0;
    int ret = -1;

    pr_info("%s enter, i2c addr=%x\n", __func__,i2c->addr);
    /* HS60 add start */
    i2c->addr = 0x34;
    /* HS60 add end */
    if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
        dev_err(&i2c->dev, "check_functionality failed\n");
        return -EIO;
    }

    aw8864 = devm_kzalloc(&i2c->dev, sizeof(struct aw8864), GFP_KERNEL);
    if (aw8864 == NULL)
        return -ENOMEM;

    aw8864->dev = &i2c->dev;
    aw8864->i2c = i2c;

    /* aw8864 regmap */
    aw8864->regmap = devm_regmap_init_i2c(i2c, &aw8864_regmap);
    if (IS_ERR(aw8864->regmap)) {
        ret = PTR_ERR(aw8864->regmap);
        dev_err(&i2c->dev, "%s: failed to allocate register map: %d\n", __func__, ret);
        goto err_regmap;
    }

    i2c_set_clientdata(i2c, aw8864);
    mutex_init(&aw8864->cfg_lock);

    /* aw8864 rst & int */
    if (np) {
        ret = aw8864_parse_dt(&i2c->dev, aw8864, np);
        if (ret) {
              dev_err(&i2c->dev, "%s: failed to parse device tree node\n", __func__);
              goto err_parse_dt;
        }
    } else {
        aw8864->reset_gpio = -1;
        aw8864->irq_gpio = -1;
    }

    if (gpio_is_valid(aw8864->reset_gpio)) {
        ret = devm_gpio_request_one(&i2c->dev, aw8864->reset_gpio,
              GPIOF_OUT_INIT_LOW, "aw8864_rst");
        if (ret){
              dev_err(&i2c->dev, "%s: rst request failed\n", __func__);
              goto err_gpio_request;
        }
    }
/*
    if (gpio_is_valid(aw8864->irq_gpio)) {
        ret = devm_gpio_request_one(&i2c->dev, aw8864->irq_gpio,
              GPIOF_DIR_IN, "aw8864_int");
        if (ret){
              dev_err(&i2c->dev, "%s: int request failed\n", __func__);
              goto err_gpio_request;
        }
    }
*/
    /* hardware reset */
    aw8864_hw_reset(aw8864);

    /* aw8864 chip id */
    ret = aw8864_read_chipid(aw8864);
    if (ret < 0) {
        dev_err(&i2c->dev, "%s: aw8864_read_chipid failed ret=%d\n", __func__, ret);
	gpio_set_value_cansleep(aw8864->reset_gpio, 0);
        msleep(1);
        goto err_id;
    }

    /* aw8864 device name */
    if (i2c->dev.of_node) {
        dev_set_name(&i2c->dev, "%s", "aw8864_smartpa");
    } else {
        dev_err(&i2c->dev, "%s failed to set device name: %d\n", __func__, ret);
    }

    /* register codec */
    dai = devm_kzalloc(&i2c->dev, sizeof(aw8864_dai), GFP_KERNEL);
    if (!dai) {
        goto err_dai_kzalloc;
    }
    memcpy(dai, aw8864_dai, sizeof(aw8864_dai));
    pr_info("%s dai->name(%s)\n", __func__, dai->name);

    ret = snd_soc_register_codec(&i2c->dev, &soc_codec_dev_aw8864,
            dai, ARRAY_SIZE(aw8864_dai));
    if (ret < 0) {
        dev_err(&i2c->dev, "%s failed to register aw8864: %d\n", __func__, ret);
        goto err_register_codec;
    }

    /* aw8864 irq */
    if (gpio_is_valid(aw8864->irq_gpio) &&
        !(aw8864->flags & AW8864_FLAG_SKIP_INTERRUPTS)) {
        aw8864_interrupt_setup(aw8864);
        /* register irq handler */
        irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
        ret = devm_request_threaded_irq(&i2c->dev,
                          gpio_to_irq(aw8864->irq_gpio),
                          NULL, aw8864_irq, irq_flags,
                          "aw8864", aw8864);
        if (ret != 0) {
              dev_err(&i2c->dev, "failed to request IRQ %d: %d\n",
                          gpio_to_irq(aw8864->irq_gpio), ret);
              goto err_irq;
        }
    } else {
        dev_info(&i2c->dev, "%s skipping IRQ registration\n", __func__);
        /* disable feature support if gpio was invalid */
        aw8864->flags |= AW8864_FLAG_SKIP_INTERRUPTS;
    }

    /* Register the sysfs files for climax backdoor access */
    ret = device_create_bin_file(&i2c->dev, &dev_attr_rw);
    if (ret)
        dev_info(&i2c->dev, "%s error creating sysfs files: rw\n", __func__);
    ret = device_create_bin_file(&i2c->dev, &dev_attr_regaddr);
    if (ret)
        dev_info(&i2c->dev, "%s error creating sysfs files: regaddr\n", __func__);

    dev_set_drvdata(&i2c->dev, aw8864);
    ret = sysfs_create_group(&i2c->dev.kobj, &aw8864_attribute_group);
    if (ret < 0) {
        dev_info(&i2c->dev, "%s error creating sysfs attr files\n", __func__);
        goto err_sysfs;
    }

#ifdef AW8864_VBAT_MONITOR
    aw8864_vbat_monitor_init(aw8864);
#endif
    pr_info("%s probe completed successfully!\n", __func__);
    card_smartpa = 6;
    return 0;

err_sysfs:
    device_remove_bin_file(&i2c->dev, &dev_attr_regaddr);
    device_remove_bin_file(&i2c->dev, &dev_attr_rw);
    devm_free_irq(&i2c->dev, gpio_to_irq(aw8864->irq_gpio), aw8864);
err_irq:
    snd_soc_unregister_codec(&i2c->dev);
err_register_codec:
    devm_kfree(&i2c->dev, dai);
    dai = NULL;
err_dai_kzalloc:
err_id:
    if (gpio_is_valid(aw8864->reset_gpio))
        devm_gpio_free(&i2c->dev, aw8864->reset_gpio);
    if (gpio_is_valid(aw8864->irq_gpio))
        devm_gpio_free(&i2c->dev, aw8864->irq_gpio);
err_gpio_request:
err_parse_dt:
err_regmap:
    devm_kfree(&i2c->dev, aw8864);
    aw8864 = NULL;
    return ret;
}

static int aw8864_i2c_remove(struct i2c_client *i2c)
{
    struct aw8864 *aw8864 = i2c_get_clientdata(i2c);

    pr_info("%s enter\n", __func__);

    device_remove_bin_file(&i2c->dev, &dev_attr_regaddr);
    device_remove_bin_file(&i2c->dev, &dev_attr_rw);
    devm_free_irq(&i2c->dev, gpio_to_irq(aw8864->irq_gpio), aw8864);

    snd_soc_unregister_codec(&i2c->dev);

    if (gpio_is_valid(aw8864->irq_gpio))
        devm_gpio_free(&i2c->dev, aw8864->irq_gpio);
    if (gpio_is_valid(aw8864->reset_gpio))
        devm_gpio_free(&i2c->dev, aw8864->reset_gpio);

    return 0;
}

static const struct i2c_device_id aw8864_i2c_id[] = {
    { AW8864_I2C_NAME, 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, aw8864_i2c_id);

static struct of_device_id aw8864_dt_match[] = {
    { .compatible = "awinic,aw8864_smartpa" },
    { },
};

static struct i2c_driver aw8864_i2c_driver = {
    .driver = {
        .name = AW8864_I2C_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(aw8864_dt_match),
    },
    .probe = aw8864_i2c_probe,
    .remove = aw8864_i2c_remove,
    .id_table = aw8864_i2c_id,
};


static int __init aw8864_i2c_init(void)
{
    int ret = 0;

    pr_info("aw8864 driver version %s\n", AW8864_VERSION);

    ret = i2c_add_driver(&aw8864_i2c_driver);
    if(ret){
        pr_err("fail to add aw8864 device into i2c\n");
        return ret;
    }

    return 0;
}
module_init(aw8864_i2c_init);


static void __exit aw8864_i2c_exit(void)
{
    i2c_del_driver(&aw8864_i2c_driver);
}
module_exit(aw8864_i2c_exit);


MODULE_DESCRIPTION("ASoC AW8864 Smart PA Driver");
MODULE_LICENSE("GPL v2");
