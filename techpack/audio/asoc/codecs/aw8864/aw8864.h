#ifndef _AW8864_H_
#define _AW8864_H_


/*
 * i2c transaction on Linux limited to 64k
 * (See Linux kernel documentation: Documentation/i2c/writing-clients)
*/
#define MAX_I2C_BUFFER_SIZE 65536

#define AW8864_FLAG_START_ON_MUTE   (1 << 0)
#define AW8864_FLAG_SKIP_INTERRUPTS     (1 << 1)
#define AW8864_FLAG_SAAM_AVAILABLE      (1 << 2)
#define AW8864_FLAG_STEREO_DEVICE       (1 << 3)
#define AW8864_FLAG_MULTI_MIC_INPUTS    (1 << 4)

#define AW8864_NUM_RATES                9

#define AW8864_MAX_REGISTER             0xff

//#define AW8864_VBAT_MONITOR
#ifdef AW8864_VBAT_MONITOR
#define SYS_BAT_DEV "/sys/class/power_supply/battery/voltage_now"
#define AW8864_SYS_VBAT_LIMIT           3600000
#define AW8864_SYS_VBAT_MIN             3000000
#endif
enum aw8864_chipid{
    AW8864_ID,
};

enum aw8864_mode_spk_rcv{
    AW8864_SPEAKER_MODE = 0,
    AW8864_RECEIVER_MODE = 1,
};

struct aw8864 {
    struct regmap *regmap;
    struct i2c_client *i2c;
    struct snd_soc_codec *codec;
    struct device *dev;
    struct mutex cfg_lock;
#ifdef AW8864_VBAT_MONITOR
    struct hrtimer vbat_monitor_timer;
    struct work_struct vbat_monitor_work;
#endif
    int sysclk;
    int rate;
    int pstream;
    int cstream;

    int reset_gpio;
    int irq_gpio;

#ifdef CONFIG_DEBUG_FS
    struct dentry *dbg_dir;
#endif
    u8 reg;

    unsigned int flags;
    unsigned int chipid;
    unsigned int init;
    unsigned int spk_rcv_mode;
    unsigned int bst_ilimit;
};

struct aw8864_container{
    int len;
    unsigned char data[];
};


#endif
