/*
 * HP i8042 SDC + MSM-58321 BBRTC driver.
 *
 * Copyright (c) 2001 Brian S. Julin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL").
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *
 * References:
 * System Device Controller Microprocessor Firmware Theory of Operation
 *      for Part Number 1820-4784 Revision B.  Dwg No. A-1820-4784-2
 * efirtc.c by Stephane Eranian/Hewlett Packard
 *
 */

#include <linux/hp_sdc.h>
#include <linux/module.h>
#include <linux/rtc.h>
#include <linux/semaphore.h>

MODULE_AUTHOR("Brian S. Julin <bri@calyx.com>");
MODULE_DESCRIPTION("HP i8042 SDC + MSM-58321 RTC Driver");
MODULE_LICENSE("Dual BSD/GPL");

#define RTC_VERSION "1.2"

static struct rtc_device *rtc;

static struct semaphore i8042tregs;

static DECLARE_WAIT_QUEUE_HEAD(hp_sdc_rtc_wait);

static void hp_sdc_rtc_isr (int irq, void *dev_id, 
			    uint8_t status, uint8_t data) 
{
	/* Notify RTC core on alarm event */
	rtc_update_irq(rtc, 1, RTC_IRQF | RTC_AF);
}

static int hp_sdc_rtc_do_read_bbrtc (struct rtc_time *rtctm)
{
	struct semaphore tsem;
	hp_sdc_transaction t;
	uint8_t tseq[91];
	int i;
	
	i = 0;
	while (i < 91) {
		tseq[i++] = HP_SDC_ACT_DATAREG |
			HP_SDC_ACT_POSTCMD | HP_SDC_ACT_DATAIN;
		tseq[i++] = 0x01;			/* write i8042[0x70] */
	  	tseq[i]   = i / 7;			/* BBRTC reg address */
		i++;
		tseq[i++] = HP_SDC_CMD_DO_RTCR;		/* Trigger command   */
		tseq[i++] = 2;		/* expect 1 stat/dat pair back.   */
		i++; i++;               /* buffer for stat/dat pair       */
	}
	tseq[84] |= HP_SDC_ACT_SEMAPHORE;
	t.endidx =		91;
	t.seq =			tseq;
	t.act.semaphore =	&tsem;
	sema_init(&tsem, 0);
	
	if (hp_sdc_enqueue_transaction(&t))
		return -EIO;
	
	/* Put ourselves to sleep for results. */
	if (WARN_ON(down_interruptible(&tsem)))
		return -EIO;
	
	/* Check for nonpresence of BBRTC */
	if (!((tseq[83] | tseq[90] | tseq[69] | tseq[76] |
	       tseq[55] | tseq[62] | tseq[34] | tseq[41] |
	       tseq[20] | tseq[27] | tseq[6]  | tseq[13]) & 0x0f))
		return -ENODEV;

	memset(rtctm, 0, sizeof(struct rtc_time));
	rtctm->tm_year = (tseq[83] & 0x0f) + (tseq[90] & 0x0f) * 10;
	rtctm->tm_mon  = (tseq[69] & 0x0f) + (tseq[76] & 0x0f) * 10;
	rtctm->tm_mday = (tseq[55] & 0x0f) + (tseq[62] & 0x0f) * 10;
	rtctm->tm_wday = (tseq[48] & 0x0f);
	rtctm->tm_hour = (tseq[34] & 0x0f) + (tseq[41] & 0x0f) * 10;
	rtctm->tm_min  = (tseq[20] & 0x0f) + (tseq[27] & 0x0f) * 10;
	rtctm->tm_sec  = (tseq[6]  & 0x0f) + (tseq[13] & 0x0f) * 10;
	
	return 0;
}

static int hp_sdc_rtc_read_bbrtc (struct rtc_time *rtctm)
{
	struct rtc_time tm, tm_last;
	int i = 0, ret;

	/* MSM-58321 has no read latch, so must read twice and compare. */

	ret = hp_sdc_rtc_do_read_bbrtc(&tm_last);
	if (ret)
		return ret;
	ret = hp_sdc_rtc_do_read_bbrtc(&tm);
	if (ret)
		return ret;

	while (memcmp(&tm, &tm_last, sizeof(struct rtc_time))) {
		if (i++ > 4)
			return -EAGAIN;
		memcpy(&tm_last, &tm, sizeof(struct rtc_time));
		ret = hp_sdc_rtc_do_read_bbrtc(&tm);
		if (ret)
			return ret;
	}

	memcpy(rtctm, &tm, sizeof(struct rtc_time));

	return 0;
}

static int64_t hp_sdc_rtc_read_i8042timer (uint8_t loadcmd, int numreg)
{
	hp_sdc_transaction t;
	uint8_t tseq[26] = {
		HP_SDC_ACT_PRECMD | HP_SDC_ACT_POSTCMD | HP_SDC_ACT_DATAIN,
		0,
		HP_SDC_CMD_READ_T1, 2, 0, 0,
		HP_SDC_ACT_POSTCMD | HP_SDC_ACT_DATAIN, 
		HP_SDC_CMD_READ_T2, 2, 0, 0,
		HP_SDC_ACT_POSTCMD | HP_SDC_ACT_DATAIN, 
		HP_SDC_CMD_READ_T3, 2, 0, 0,
		HP_SDC_ACT_POSTCMD | HP_SDC_ACT_DATAIN, 
		HP_SDC_CMD_READ_T4, 2, 0, 0,
		HP_SDC_ACT_POSTCMD | HP_SDC_ACT_DATAIN, 
		HP_SDC_CMD_READ_T5, 2, 0, 0
	};

	t.endidx = numreg * 5;

	tseq[1] = loadcmd;
	tseq[t.endidx - 4] |= HP_SDC_ACT_SEMAPHORE; /* numreg assumed > 1 */

	t.seq =			tseq;
	t.act.semaphore =	&i8042tregs;

	/* Sleep if output regs in use. */
	if (WARN_ON(down_interruptible(&i8042tregs)))
		return -EIO;

	if (hp_sdc_enqueue_transaction(&t)) {
		up(&i8042tregs);
		return -EIO;
	}
	
	/* Sleep until results come back. */
	if (WARN_ON(down_interruptible(&i8042tregs)))
		return -EIO;

	up(&i8042tregs);

	return (tseq[5] | 
		((uint64_t)(tseq[10]) << 8)  | ((uint64_t)(tseq[15]) << 16) |
		((uint64_t)(tseq[20]) << 24) | ((uint64_t)(tseq[25]) << 32));
}


/* Read the i8042 real-time clock */
static int hp_sdc_rtc_read_rt(struct timespec64 *res) {
	int64_t raw;
	uint32_t tenms; 
	unsigned int days;

	raw = hp_sdc_rtc_read_i8042timer(HP_SDC_CMD_LOAD_RT, 5);
	if (raw < 0)
		return -EIO;

	tenms = (uint32_t)raw & 0xffffff;
	days  = (unsigned int)(raw >> 24) & 0xffff;

	res->tv_nsec = (long)(tenms % 100) * 10000 * 1000;
	res->tv_sec =  (tenms / 100) + (time64_t)days * 86400;

	return 0;
}


/* Read the i8042 fast handshake timer */
static inline int hp_sdc_rtc_read_fhs(struct timespec64 *res) {
	int64_t raw;
	unsigned int tenms;

	raw = hp_sdc_rtc_read_i8042timer(HP_SDC_CMD_LOAD_FHS, 2);
	if (raw < 0)
		return -EIO;

	tenms = (unsigned int)raw & 0xffff;

	res->tv_nsec = (long)(tenms % 100) * 10000 * 1000;
	res->tv_sec  = (time64_t)(tenms / 100);

	return 0;
}


/* Read the i8042 match timer (a.k.a. alarm) */
static inline int hp_sdc_rtc_read_mt(struct timespec64 *res) {
	int64_t raw;	
	uint32_t tenms; 

	raw = hp_sdc_rtc_read_i8042timer(HP_SDC_CMD_LOAD_MT, 3);
	if (raw < 0)
		return -EIO;

	tenms = (uint32_t)raw & 0xffffff;

	res->tv_nsec = (long)(tenms % 100) * 10000 * 1000;
	res->tv_sec  = (time64_t)(tenms / 100);

	return 0;
}


/* Read the i8042 delay timer */
static inline int hp_sdc_rtc_read_dt(struct timespec64 *res) {
	int64_t raw;
	uint32_t tenms;

	raw = hp_sdc_rtc_read_i8042timer(HP_SDC_CMD_LOAD_DT, 3);
	if (raw < 0)
		return -EIO;

	tenms = (uint32_t)raw & 0xffffff;

	res->tv_nsec = (long)(tenms % 100) * 10000 * 1000;
	res->tv_sec  = (time64_t)(tenms / 100);

	return 0;
}


/* Read the i8042 cycle timer (a.k.a. periodic) */
static inline int hp_sdc_rtc_read_ct(struct timespec64 *res) {
	int64_t raw;
	uint32_t tenms;

	raw = hp_sdc_rtc_read_i8042timer(HP_SDC_CMD_LOAD_CT, 3);
	if (raw < 0)
		return -EIO;

	tenms = (uint32_t)raw & 0xffffff;

	res->tv_nsec = (long)(tenms % 100) * 10000 * 1000;
	res->tv_sec  = (time64_t)(tenms / 100);

	return 0;
}


/* Set the i8042 real-time clock */
static int hp_sdc_rtc_set_rt(struct rtc_time *tm)
{
	uint32_t tenms;
	time64_t seconds;
	unsigned int days;
	hp_sdc_transaction t;
	uint8_t tseq[11] = {
		HP_SDC_ACT_PRECMD | HP_SDC_ACT_DATAOUT,
		HP_SDC_CMD_SET_RTMS, 3, 0, 0, 0,
		HP_SDC_ACT_PRECMD | HP_SDC_ACT_DATAOUT,
		HP_SDC_CMD_SET_RTD, 2, 0, 0 
	};

	t.endidx = 10;

	seconds = rtc_tm_to_time64(tm);
	days = seconds / 86400;
	if (days > 0xffff)
		return -EINVAL;
	tenms  = (seconds % 86400) * 100;
	if (tenms > 0xffffff)
		return -EINVAL;

	tseq[3] = (uint8_t)(tenms & 0xff);
	tseq[4] = (uint8_t)((tenms >> 8)  & 0xff);
	tseq[5] = (uint8_t)((tenms >> 16) & 0xff);

	tseq[9] = (uint8_t)(days & 0xff);
	tseq[10] = (uint8_t)((days >> 8) & 0xff);

	t.seq =	tseq;

	if (hp_sdc_enqueue_transaction(&t))
		return -EIO;
	return 0;
}

/* Set the i8042 fast handshake timer */
#if 0
static int hp_sdc_rtc_set_fhs(struct timespec64 *setto)
{
	uint32_t tenms;
	hp_sdc_transaction t;
	uint8_t tseq[5] = {
		HP_SDC_ACT_PRECMD | HP_SDC_ACT_DATAOUT,
		HP_SDC_CMD_SET_FHS, 2, 0, 0
	};

	t.endidx = 4;

	tenms  = setto->tv_sec * 100;
	tenms += setto->tv_nsec / 10000 / 1000;
	if (tenms > 0xffff)
		return -EINVAL;

	tseq[3] = (uint8_t)(tenms & 0xff);
	tseq[4] = (uint8_t)((tenms >> 8)  & 0xff);

	t.seq =	tseq;

	if (hp_sdc_enqueue_transaction(&t))
		return -EIO;
	return 0;
}
#endif

/* Set the i8042 match timer (a.k.a. alarm) */
#define hp_sdc_rtc_set_mt(setto) \
	hp_sdc_rtc_set_i8042timer(setto, HP_SDC_CMD_SET_MT)

/* Set the i8042 delay timer */
#define hp_sdc_rtc_set_dt(setto) \
	hp_sdc_rtc_set_i8042timer(setto, HP_SDC_CMD_SET_DT)

/* Set the i8042 cycle timer (a.k.a. periodic) */
#define hp_sdc_rtc_set_ct(setto) \
	hp_sdc_rtc_set_i8042timer(setto, HP_SDC_CMD_SET_CT)

/* Set one of the i8042 3-byte wide timers */
static int hp_sdc_rtc_set_i8042timer(struct timespec64 *setto, uint8_t setcmd)
{
	uint32_t tenms;
	hp_sdc_transaction t;
	uint8_t tseq[6] = {
		HP_SDC_ACT_PRECMD | HP_SDC_ACT_DATAOUT,
		0, 3, 0, 0, 0
	};

	t.endidx = 6;

	if (setto->tv_sec > 0xffffff)
		return -EINVAL;
	tenms  = setto->tv_sec * 100;
	if (setto->tv_nsec / 10000 / 1000 > 0xffffff)
		return -EINVAL;
	tenms += setto->tv_nsec / 10000 / 1000;
	if (tenms > 0xffffff)
		return -EINVAL;

	tseq[1] = setcmd;
	tseq[3] = (uint8_t)(tenms & 0xff);
	tseq[4] = (uint8_t)((tenms >> 8)  & 0xff);
	tseq[5] = (uint8_t)((tenms >> 16)  & 0xff);

	t.seq =	tseq;

	if (hp_sdc_enqueue_transaction(&t)) { 
		return -EIO;
	}
	return 0;
}

static int hp_sdc_rtc_get_time(struct device *dev, struct rtc_time *tm)
{
	return hp_sdc_rtc_read_bbrtc(tm);
}

static int hp_sdc_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	if (!rtc_valid_tm(tm))
		return -EINVAL;

	return hp_sdc_rtc_set_rt(tm);
}

static int hp_sdc_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *wa)
{
	/* Read the present alarm time */
	int ret;
	struct timespec64 ttime;
	struct rtc_time *wtime = &wa->time;

	ret = hp_sdc_rtc_read_mt(&ttime);
	if (ret)
		return ret;
	ret = hp_sdc_rtc_read_bbrtc(wtime);
	if (ret)
		return ret;

	wtime->tm_hour = ttime.tv_sec / 3600;  ttime.tv_sec %= 3600;
	wtime->tm_min  = ttime.tv_sec / 60;    ttime.tv_sec %= 60;
	wtime->tm_sec  = ttime.tv_sec;

	wa->enabled = 0;
	wa->pending = 0;

	return 0;
}

static int hp_sdc_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *wa)
{
	/* Set the alarm time */
	struct timespec64 ttime;

	/*
	 * This expects a struct hp_sdc_rtc_time. Writing 0xff means
	 * "don't care" or "match all" for PC timers.  The HP SDC
	 * does not support that perk, but it could be emulated fairly
	 * easily.  Only the tm_hour, tm_min and tm_sec are used.
	 * We could do it with 10ms accuracy with the HP SDC, if the
	 * rtc interface left us a way to do that.
	 */

	if (wa->time.tm_hour > 23)
		return -EINVAL;
	if (wa->time.tm_min  > 59)
		return -EINVAL;
	if (wa->time.tm_sec  > 59)
		return -EINVAL;

	ttime.tv_sec = wa->time.tm_hour * 3600 +
	  wa->time.tm_min * 60 + wa->time.tm_sec;
	ttime.tv_nsec = 0;
	if (hp_sdc_rtc_set_mt(&ttime))
		return -EIO;

	return 0;
}

static int hp_sdc_rtc_proc(struct device *dev, struct seq_file *m)
{
#define YN(bit) ("no")
#define NY(bit) ("yes")
        struct rtc_time tm;
	struct timespec64 tv;

	memset(&tm, 0, sizeof(struct rtc_time));

	if (hp_sdc_rtc_read_bbrtc(&tm)) {
		seq_puts(m, "BBRTC\t\t: READ FAILED!\n");
	} else {
		seq_printf(m,
			     "rtc_time\t: %02d:%02d:%02d\n"
			     "rtc_date\t: %04d-%02d-%02d\n",
			     tm.tm_hour, tm.tm_min, tm.tm_sec,
			     tm.tm_year + 1900, tm.tm_mon + 1, 
			     tm.tm_mday);
	}

	if (hp_sdc_rtc_read_rt(&tv)) {
		seq_puts(m, "i8042 rtc\t: READ FAILED!\n");
	} else {
		seq_printf(m, "i8042 rtc\t: %lld.%02ld seconds\n",
			     (s64)tv.tv_sec, (long)tv.tv_nsec/1000000L);
	}

	if (hp_sdc_rtc_read_fhs(&tv)) {
		seq_puts(m, "handshake\t: READ FAILED!\n");
	} else {
		seq_printf(m, "handshake\t: %lld.%02ld seconds\n",
			     (s64)tv.tv_sec, (long)tv.tv_nsec/1000000L);
	}

	if (hp_sdc_rtc_read_mt(&tv)) {
		seq_puts(m, "alarm\t\t: READ FAILED!\n");
	} else {
		seq_printf(m, "alarm\t\t: %lld.%02ld seconds\n",
			     (s64)tv.tv_sec, (long)tv.tv_nsec/1000000L);
	}

	if (hp_sdc_rtc_read_dt(&tv)) {
		seq_puts(m, "delay\t\t: READ FAILED!\n");
	} else {
		seq_printf(m, "delay\t\t: %lld.%02ld seconds\n",
			     (s64)tv.tv_sec, (long)tv.tv_nsec/1000000L);
	}

	if (hp_sdc_rtc_read_ct(&tv)) {
		seq_puts(m, "periodic\t: READ FAILED!\n");
	} else {
		seq_printf(m, "periodic\t: %lld.%02ld seconds\n",
			     (s64)tv.tv_sec, (long)tv.tv_nsec/1000000L);
	}

        seq_printf(m,
                     "DST_enable\t: %s\n"
                     "BCD\t\t: %s\n"
                     "24hr\t\t: %s\n"
                     "square_wave\t: %s\n"
                     "alarm_IRQ\t: %s\n"
                     "update_IRQ\t: %s\n"
                     "periodic_IRQ\t: %s\n"
		     "periodic_freq\t: %ld\n"
                     "batt_status\t: %s\n",
                     YN(RTC_DST_EN),
                     NY(RTC_DM_BINARY),
                     YN(RTC_24H),
                     YN(RTC_SQWE),
                     YN(RTC_AIE),
                     YN(RTC_UIE),
                     YN(RTC_PIE),
                     1UL,
                     1 ? "okay" : "dead");

        return 0;
#undef YN
#undef NY
}

static int hp_sdc_rtc_ioctl(struct device *dev,
			    unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case RTC_AIE_OFF:       /* Mask alarm int. enab. bit    */
	case RTC_AIE_ON:        /* Allow alarm interrupts.      */
	case RTC_PIE_OFF:       /* Mask periodic int. enab. bit */
	case RTC_PIE_ON:        /* Allow periodic ints          */
	case RTC_UIE_ON:        /* Allow ints for RTC updates.  */
	case RTC_UIE_OFF:       /* Allow ints for RTC updates.  */
	{
		/* We cannot mask individual user timers and we
		   cannot tell them apart when they occur, so it 
		   would be disingenuous to succeed these IOCTLs */
		return -EINVAL;
	}
	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

static const struct rtc_class_ops hp_sdc_rtc_ops = {
	.ioctl = hp_sdc_rtc_ioctl,
	.proc = hp_sdc_rtc_proc,
	.read_time = hp_sdc_rtc_get_time,
	.set_time = hp_sdc_rtc_set_time,
	.read_alarm = hp_sdc_rtc_read_alarm,
	.set_alarm = hp_sdc_rtc_set_alarm,
	// int (*alarm_irq_enable)(struct device *, unsigned int enabled);
};

static int __init hp_sdc_rtc_init(void)
{
	int ret;
	struct rtc_time tm;

#ifdef __mc68000__
	if (!MACH_IS_HP300)
		return -ENODEV;
#endif

	sema_init(&i8042tregs, 1);

	/* check if BBRTC is available */
	ret = hp_sdc_rtc_read_bbrtc(&tm);
	if (ret) {
		pr_info("hil-rtc: BBRTC not available.");
		return -ENODEV;
	}

	if ((ret = hp_sdc_request_timer_irq(&hp_sdc_rtc_isr)))
		return ret;

	rtc = devm_rtc_device_register(hp_sdc_get_sdc_device(),
			"hil-rtc", &hp_sdc_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);

	pr_info("hil-rtc: HP i8042 SDC + MSM-58321 RTC support loaded "
			 "(RTC v " RTC_VERSION ")\n");

	return 0;
}

static void __exit hp_sdc_rtc_exit(void)
{
	devm_rtc_device_unregister(hp_sdc_get_sdc_device(), rtc);
	hp_sdc_release_timer_irq(hp_sdc_rtc_isr);
        pr_info("hil-rtc: HP i8042 SDC + MSM-58321 RTC support unloaded\n");
}

module_init(hp_sdc_rtc_init);
module_exit(hp_sdc_rtc_exit);
