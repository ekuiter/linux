/*
   comedi/drivers/pcl818.c

   Author:  Michal Dobes <dobes@tesnet.cz>

   hardware driver for Advantech cards:
    card:   PCL-818L, PCL-818H, PCL-818HD, PCL-818HG, PCL-818, PCL-718
    driver: pcl818l,  pcl818h,  pcl818hd,  pcl818hg,  pcl818,  pcl718
*/
/*
Driver: pcl818
Description: Advantech PCL-818 cards, PCL-718
Author: Michal Dobes <dobes@tesnet.cz>
Devices: [Advantech] PCL-818L (pcl818l), PCL-818H (pcl818h),
  PCL-818HD (pcl818hd), PCL-818HG (pcl818hg), PCL-818 (pcl818),
  PCL-718 (pcl718)
Status: works

All cards have 16 SE/8 DIFF ADCs, one or two DACs, 16 DI and 16 DO.
Differences are only at maximal sample speed, range list and FIFO
support.
The driver support AI mode 0, 1, 3 other subdevices (AO, DI, DO) support
only mode 0. If DMA/FIFO/INT are disabled then AI support only mode 0.
PCL-818HD and PCL-818HG support 1kword FIFO. Driver support this FIFO
but this code is untested.
A word or two about DMA. Driver support DMA operations at two ways:
1) DMA uses two buffers and after one is filled then is generated
   INT and DMA restart with second buffer. With this mode I'm unable run
   more that 80Ksamples/secs without data dropouts on K6/233.
2) DMA uses one buffer and run in autoinit mode and the data are
   from DMA buffer moved on the fly with 2kHz interrupts from RTC.
   This mode is used if the interrupt 8 is available for allocation.
   If not, then first DMA mode is used. With this I can run at
   full speed one card (100ksamples/secs) or two cards with
   60ksamples/secs each (more is problem on account of ISA limitations).
   To use this mode you must have compiled  kernel with disabled
   "Enhanced Real Time Clock Support".
   Maybe you can have problems if you use xntpd or similar.
   If you've data dropouts with DMA mode 2 then:
    a) disable IDE DMA
    b) switch text mode console to fb.

   Options for PCL-818L:
    [0] - IO Base
    [1] - IRQ	(0=disable, 2, 3, 4, 5, 6, 7)
    [2] - DMA	(0=disable, 1, 3)
    [3] - 0, 10=10MHz clock for 8254
              1= 1MHz clock for 8254
    [4] - 0,  5=A/D input  -5V.. +5V
          1, 10=A/D input -10V..+10V
    [5] - 0,  5=D/A output 0-5V  (internal reference -5V)
          1, 10=D/A output 0-10V (internal reference -10V)
	  2    =D/A output unknown (external reference)

   Options for PCL-818, PCL-818H:
    [0] - IO Base
    [1] - IRQ	(0=disable, 2, 3, 4, 5, 6, 7)
    [2] - DMA	(0=disable, 1, 3)
    [3] - 0, 10=10MHz clock for 8254
              1= 1MHz clock for 8254
    [4] - 0,  5=D/A output 0-5V  (internal reference -5V)
          1, 10=D/A output 0-10V (internal reference -10V)
	  2    =D/A output unknown (external reference)

   Options for PCL-818HD, PCL-818HG:
    [0] - IO Base
    [1] - IRQ	(0=disable, 2, 3, 4, 5, 6, 7)
    [2] - DMA/FIFO  (-1=use FIFO, 0=disable both FIFO and DMA,
                      1=use DMA ch 1, 3=use DMA ch 3)
    [3] - 0, 10=10MHz clock for 8254
              1= 1MHz clock for 8254
    [4] - 0,  5=D/A output 0-5V  (internal reference -5V)
          1, 10=D/A output 0-10V (internal reference -10V)
   	  2    =D/A output unknown (external reference)

   Options for PCL-718:
    [0] - IO Base
    [1] - IRQ	(0=disable, 2, 3, 4, 5, 6, 7)
    [2] - DMA	(0=disable, 1, 3)
    [3] - 0, 10=10MHz clock for 8254
              1= 1MHz clock for 8254
    [4] -     0=A/D Range is +/-10V
	      1=             +/-5V
	      2=             +/-2.5V
	      3=             +/-1V
	      4=             +/-0.5V
	      5=  	     user defined bipolar
	      6=	     0-10V
	      7=	     0-5V
 	      8=	     0-2V
	      9=	     0-1V
	     10=	     user defined unipolar
    [5] - 0,  5=D/A outputs 0-5V  (internal reference -5V)
          1, 10=D/A outputs 0-10V (internal reference -10V)
	      2=D/A outputs unknown (external reference)
    [6] - 0, 60=max  60kHz A/D sampling
          1,100=max 100kHz A/D sampling (PCL-718 with Option 001 installed)

*/

#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <asm/dma.h>

#include "../comedidev.h"

#include "comedi_fc.h"
#include "8253.h"

/* boards constants */

#define boardPCL818L 0
#define boardPCL818H 1
#define boardPCL818HD 2
#define boardPCL818HG 3
#define boardPCL818 4
#define boardPCL718 5

/* W: clear INT request */
#define PCL818_CLRINT 8
/* R: return status byte */
#define PCL818_STATUS 8
/* R: A/D high byte W: A/D range control */
#define PCL818_RANGE 1
/* R: next mux scan channel W: mux scan channel & range control pointer */
#define PCL818_MUX 2
/* R/W: operation control register */
#define PCL818_CONTROL 9
/* W: counter enable */
#define PCL818_CNTENABLE 10

/* R: low byte of A/D W: soft A/D trigger */
#define PCL818_AD_LO 0
/* R: high byte of A/D W: A/D range control */
#define PCL818_AD_HI 1
/* W: D/A low&high byte */
#define PCL818_DA_LO 4
#define PCL818_DA_HI 5
/* R: low&high byte of DI */
#define PCL818_DI_LO 3
#define PCL818_DI_HI 11
/* W: low&high byte of DO */
#define PCL818_DO_LO 3
#define PCL818_DO_HI 11
/* W: PCL718 second D/A */
#define PCL718_DA2_LO 6
#define PCL718_DA2_HI 7

#define PCL818_TIMER_BASE			0x0c

/* W: fifo enable/disable */
#define PCL818_FI_ENABLE 6
/* W: fifo interrupt clear */
#define PCL818_FI_INTCLR 20
/* W: fifo interrupt clear */
#define PCL818_FI_FLUSH 25
/* R: fifo status */
#define PCL818_FI_STATUS 25
/* R: one record from FIFO */
#define PCL818_FI_DATALO 23
#define PCL818_FI_DATAHI 23

/* type of interrupt handler */
#define INT_TYPE_AI1_INT 1
#define INT_TYPE_AI1_DMA 2
#define INT_TYPE_AI1_FIFO 3
#define INT_TYPE_AI3_INT 4
#define INT_TYPE_AI3_DMA 5
#define INT_TYPE_AI3_FIFO 6

#define MAGIC_DMA_WORD 0x5a5a

static const struct comedi_lrange range_pcl818h_ai = {
	9, {
		BIP_RANGE(5),
		BIP_RANGE(2.5),
		BIP_RANGE(1.25),
		BIP_RANGE(0.625),
		UNI_RANGE(10),
		UNI_RANGE(5),
		UNI_RANGE(2.5),
		UNI_RANGE(1.25),
		BIP_RANGE(10)
	}
};

static const struct comedi_lrange range_pcl818hg_ai = {
	10, {
		BIP_RANGE(5),
		BIP_RANGE(0.5),
		BIP_RANGE(0.05),
		BIP_RANGE(0.005),
		UNI_RANGE(10),
		UNI_RANGE(1),
		UNI_RANGE(0.1),
		UNI_RANGE(0.01),
		BIP_RANGE(10),
		BIP_RANGE(1),
		BIP_RANGE(0.1),
		BIP_RANGE(0.01)
	}
};

static const struct comedi_lrange range_pcl818l_l_ai = {
	4, {
		BIP_RANGE(5),
		BIP_RANGE(2.5),
		BIP_RANGE(1.25),
		BIP_RANGE(0.625)
	}
};

static const struct comedi_lrange range_pcl818l_h_ai = {
	4, {
		BIP_RANGE(10),
		BIP_RANGE(5),
		BIP_RANGE(2.5),
		BIP_RANGE(1.25)
	}
};

static const struct comedi_lrange range718_bipolar1 = {
	1, {
		BIP_RANGE(1)
	}
};

static const struct comedi_lrange range718_bipolar0_5 = {
	1, {
		BIP_RANGE(0.5)
	}
};

static const struct comedi_lrange range718_unipolar2 = {
	1, {
		UNI_RANGE(2)
	}
};

static const struct comedi_lrange range718_unipolar1 = {
	1, {
		BIP_RANGE(1)
	}
};

struct pcl818_board {
	const char *name;
	unsigned int ns_min;
	int n_aochan;
	const struct comedi_lrange *ai_range_type;
	unsigned int has_dma:1;
	unsigned int has_fifo:1;
	unsigned int is_818:1;
};

static const struct pcl818_board boardtypes[] = {
	{
		.name		= "pcl818l",
		.ns_min		= 25000,
		.n_aochan	= 1,
		.ai_range_type	= &range_pcl818l_l_ai,
		.has_dma	= 1,
		.is_818		= 1,
	}, {
		.name		= "pcl818h",
		.ns_min		= 10000,
		.n_aochan	= 1,
		.ai_range_type	= &range_pcl818h_ai,
		.has_dma	= 1,
		.is_818		= 1,
	}, {
		.name		= "pcl818hd",
		.ns_min		= 10000,
		.n_aochan	= 1,
		.ai_range_type	= &range_pcl818h_ai,
		.has_dma	= 1,
		.has_fifo	= 1,
		.is_818		= 1,
	}, {
		.name		= "pcl818hg",
		.ns_min		= 10000,
		.n_aochan	= 1,
		.ai_range_type	= &range_pcl818hg_ai,
		.has_dma	= 1,
		.has_fifo	= 1,
		.is_818		= 1,
	}, {
		.name		= "pcl818",
		.ns_min		= 10000,
		.n_aochan	= 2,
		.ai_range_type	= &range_pcl818h_ai,
		.has_dma	= 1,
		.is_818		= 1,
	}, {
		.name		= "pcl718",
		.ns_min		= 16000,
		.n_aochan	= 2,
		.ai_range_type	= &range_unipolar5,
		.has_dma	= 1,
	}, {
		.name		= "pcm3718",
		.ns_min		= 10000,
		.ai_range_type	= &range_pcl818h_ai,
		.has_dma	= 1,
		.is_818		= 1,
	},
};

struct pcl818_private {
	unsigned int dma;	/*  used DMA, 0=don't use DMA */
	unsigned int dmapages;
	unsigned int hwdmasize;
	unsigned long dmabuf[2];	/*  pointers to begin of DMA buffers */
	unsigned int hwdmaptr[2];	/*  hardware address of DMA buffers */
	int next_dma_buf;	/*  which DMA buffer will be used next round */
	long dma_runs_to_end;	/*  how many we must permorm DMA transfer to end of record */
	unsigned long last_dma_run;	/*  how many bytes we must transfer on last DMA page */
	unsigned int ns_min;	/*  manimal allowed delay between samples (in us) for actual card */
	int i8253_osc_base;	/*  1/frequency of on board oscilator in ns */
	int ai_mode;		/*  who now uses IRQ - 1=AI1 int, 2=AI1 dma, 3=AI3 int, 4AI3 dma */
	int ai_act_scan;	/*  how many scans we finished */
	int ai_act_chan;	/*  actual position in actual scan */
	unsigned int act_chanlist[16];	/*  MUX setting for actual AI operations */
	unsigned int act_chanlist_len;	/*  how long is actual MUX list */
	unsigned int act_chanlist_pos;	/*  actual position in MUX list */
	unsigned int ai_data_len;	/*  len of data buffer */
	unsigned int ao_readback[2];
	unsigned int divisor1;
	unsigned int divisor2;
	unsigned int usefifo:1;
	unsigned int ai_cmd_running:1;
	unsigned int irq_was_now_closed:1;
	unsigned int neverending_ai:1;
};

static const unsigned int muxonechan[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,	/*  used for gain list programming */
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};

static void setup_channel_list(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       unsigned int *chanlist, unsigned int n_chan,
			       unsigned int seglen);
static int check_channel_list(struct comedi_device *dev,
			      struct comedi_subdevice *s,
			      unsigned int *chanlist, unsigned int n_chan);

static void pcl818_start_pacer(struct comedi_device *dev, bool load_counters)
{
	struct pcl818_private *devpriv = dev->private;
	unsigned long timer_base = dev->iobase + PCL818_TIMER_BASE;

	i8254_set_mode(timer_base, 0, 2, I8254_MODE2 | I8254_BINARY);
	i8254_set_mode(timer_base, 0, 1, I8254_MODE2 | I8254_BINARY);
	udelay(1);

	if (load_counters) {
		i8254_write(timer_base, 0, 2, devpriv->divisor2);
		i8254_write(timer_base, 0, 1, devpriv->divisor1);
	}
}

static unsigned int pcl818_ai_get_sample(struct comedi_device *dev,
					 struct comedi_subdevice *s,
					 unsigned int *chan)
{
	unsigned int val;

	val = inb(dev->iobase + PCL818_AD_HI) << 8;
	val |= inb(dev->iobase + PCL818_AD_LO);

	if (chan)
		*chan = val & 0xf;

	return (val >> 4) & s->maxdata;
}

static int pcl818_ai_eoc(struct comedi_device *dev,
			 struct comedi_subdevice *s,
			 struct comedi_insn *insn,
			 unsigned long context)
{
	unsigned int status;

	status = inb(dev->iobase + PCL818_STATUS);
	if (status & 0x10)
		return 0;
	return -EBUSY;
}

static int pcl818_ai_insn_read(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       struct comedi_insn *insn, unsigned int *data)
{
	int ret;
	int n;

	/* software trigger, DMA and INT off */
	outb(0, dev->iobase + PCL818_CONTROL);

	/* select channel */
	outb(muxonechan[CR_CHAN(insn->chanspec)], dev->iobase + PCL818_MUX);

	/* select gain */
	outb(CR_RANGE(insn->chanspec), dev->iobase + PCL818_RANGE);

	for (n = 0; n < insn->n; n++) {

		/* clear INT (conversion end) flag */
		outb(0, dev->iobase + PCL818_CLRINT);

		/* start conversion */
		outb(0, dev->iobase + PCL818_AD_LO);

		ret = comedi_timeout(dev, s, insn, pcl818_ai_eoc, 0);
		if (ret) {
			/* clear INT (conversion end) flag */
			outb(0, dev->iobase + PCL818_CLRINT);
			return ret;
		}

		data[n] = pcl818_ai_get_sample(dev, s, NULL);
	}

	return n;
}

static int pcl818_ao_insn_read(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       struct comedi_insn *insn, unsigned int *data)
{
	struct pcl818_private *devpriv = dev->private;
	int n;
	int chan = CR_CHAN(insn->chanspec);

	for (n = 0; n < insn->n; n++)
		data[n] = devpriv->ao_readback[chan];

	return n;
}

static int pcl818_ao_insn_write(struct comedi_device *dev,
				struct comedi_subdevice *s,
				struct comedi_insn *insn, unsigned int *data)
{
	struct pcl818_private *devpriv = dev->private;
	int n;
	int chan = CR_CHAN(insn->chanspec);

	for (n = 0; n < insn->n; n++) {
		devpriv->ao_readback[chan] = data[n];
		outb((data[n] & 0x000f) << 4, dev->iobase +
		     (chan ? PCL718_DA2_LO : PCL818_DA_LO));
		outb((data[n] & 0x0ff0) >> 4, dev->iobase +
		     (chan ? PCL718_DA2_HI : PCL818_DA_HI));
	}

	return n;
}

static int pcl818_di_insn_bits(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       struct comedi_insn *insn, unsigned int *data)
{
	data[1] = inb(dev->iobase + PCL818_DI_LO) |
	    (inb(dev->iobase + PCL818_DI_HI) << 8);

	return insn->n;
}

static int pcl818_do_insn_bits(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       struct comedi_insn *insn,
			       unsigned int *data)
{
	if (comedi_dio_update_state(s, data)) {
		outb(s->state & 0xff, dev->iobase + PCL818_DO_LO);
		outb((s->state >> 8), dev->iobase + PCL818_DO_HI);
	}

	data[1] = s->state;

	return insn->n;
}

static irqreturn_t interrupt_pcl818_ai_mode13_int(int irq, void *d)
{
	struct comedi_device *dev = d;
	struct pcl818_private *devpriv = dev->private;
	struct comedi_subdevice *s = dev->read_subdev;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int chan;
	int timeout = 50;	/* wait max 50us */

	while (timeout--) {
		if (inb(dev->iobase + PCL818_STATUS) & 0x10)
			goto conv_finish;
		udelay(1);
	}
	outb(0, dev->iobase + PCL818_STATUS);	/* clear INT request */
	comedi_error(dev, "A/D mode1/3 IRQ without DRDY!");
	s->cancel(dev, s);
	s->async->events |= COMEDI_CB_EOA | COMEDI_CB_ERROR;
	comedi_event(dev, s);
	return IRQ_HANDLED;

conv_finish:
	comedi_buf_put(s->async, pcl818_ai_get_sample(dev, s, &chan));
	outb(0, dev->iobase + PCL818_CLRINT);	/* clear INT request */

	if (chan != devpriv->act_chanlist[devpriv->act_chanlist_pos]) {
		dev_dbg(dev->class_dev,
			"A/D mode1/3 IRQ - channel dropout %x!=%x !\n",
			chan,
			devpriv->act_chanlist[devpriv->act_chanlist_pos]);
		s->cancel(dev, s);
		s->async->events |= COMEDI_CB_EOA | COMEDI_CB_ERROR;
		comedi_event(dev, s);
		return IRQ_HANDLED;
	}
	devpriv->act_chanlist_pos++;
	if (devpriv->act_chanlist_pos >= devpriv->act_chanlist_len)
		devpriv->act_chanlist_pos = 0;

	s->async->cur_chan++;
	if (s->async->cur_chan >= cmd->chanlist_len) {
		s->async->cur_chan = 0;
		devpriv->ai_act_scan--;
	}

	if (!devpriv->neverending_ai) {
		if (devpriv->ai_act_scan == 0) {	/* all data sampled */
			s->cancel(dev, s);
			s->async->events |= COMEDI_CB_EOA;
		}
	}
	comedi_event(dev, s);
	return IRQ_HANDLED;
}

static irqreturn_t interrupt_pcl818_ai_mode13_dma(int irq, void *d)
{
	struct comedi_device *dev = d;
	struct pcl818_private *devpriv = dev->private;
	struct comedi_subdevice *s = dev->read_subdev;
	struct comedi_cmd *cmd = &s->async->cmd;
	int i, len, bufptr;
	unsigned long flags;
	unsigned short *ptr;

	disable_dma(devpriv->dma);
	devpriv->next_dma_buf = 1 - devpriv->next_dma_buf;
	if ((devpriv->dma_runs_to_end) > -1 || devpriv->neverending_ai) {	/*  switch dma bufs */
		set_dma_mode(devpriv->dma, DMA_MODE_READ);
		flags = claim_dma_lock();
		set_dma_addr(devpriv->dma,
			     devpriv->hwdmaptr[devpriv->next_dma_buf]);
		if (devpriv->dma_runs_to_end || devpriv->neverending_ai)
			set_dma_count(devpriv->dma, devpriv->hwdmasize);
		else
			set_dma_count(devpriv->dma, devpriv->last_dma_run);
		release_dma_lock(flags);
		enable_dma(devpriv->dma);
	}

	devpriv->dma_runs_to_end--;
	outb(0, dev->iobase + PCL818_CLRINT);	/* clear INT request */
	ptr = (unsigned short *)devpriv->dmabuf[1 - devpriv->next_dma_buf];

	len = devpriv->hwdmasize >> 1;
	bufptr = 0;

	for (i = 0; i < len; i++) {
		if ((ptr[bufptr] & 0xf) != devpriv->act_chanlist[devpriv->act_chanlist_pos]) {	/*  dropout! */
			dev_dbg(dev->class_dev,
				"A/D mode1/3 DMA - channel dropout %d(card)!=%d(chanlist) at %d !\n",
				(ptr[bufptr] & 0xf),
				devpriv->act_chanlist[devpriv->act_chanlist_pos],
				devpriv->act_chanlist_pos);
			s->cancel(dev, s);
			s->async->events |= COMEDI_CB_EOA | COMEDI_CB_ERROR;
			comedi_event(dev, s);
			return IRQ_HANDLED;
		}

		comedi_buf_put(s->async, ptr[bufptr++] >> 4);	/*  get one sample */

		devpriv->act_chanlist_pos++;
		if (devpriv->act_chanlist_pos >= devpriv->act_chanlist_len)
			devpriv->act_chanlist_pos = 0;

		s->async->cur_chan++;
		if (s->async->cur_chan >= cmd->chanlist_len) {
			s->async->cur_chan = 0;
			devpriv->ai_act_scan--;
		}

		if (!devpriv->neverending_ai)
			if (devpriv->ai_act_scan == 0) {	/* all data sampled */
				s->cancel(dev, s);
				s->async->events |= COMEDI_CB_EOA;
				comedi_event(dev, s);
				return IRQ_HANDLED;
			}
	}

	if (len > 0)
		comedi_event(dev, s);
	return IRQ_HANDLED;
}

static irqreturn_t interrupt_pcl818_ai_mode13_fifo(int irq, void *d)
{
	struct comedi_device *dev = d;
	struct pcl818_private *devpriv = dev->private;
	struct comedi_subdevice *s = dev->read_subdev;
	struct comedi_cmd *cmd = &s->async->cmd;
	int i, len;
	unsigned char lo;

	outb(0, dev->iobase + PCL818_FI_INTCLR);	/*  clear fifo int request */

	lo = inb(dev->iobase + PCL818_FI_STATUS);

	if (lo & 4) {
		comedi_error(dev, "A/D mode1/3 FIFO overflow!");
		s->cancel(dev, s);
		s->async->events |= COMEDI_CB_EOA | COMEDI_CB_ERROR;
		comedi_event(dev, s);
		return IRQ_HANDLED;
	}

	if (lo & 1) {
		comedi_error(dev, "A/D mode1/3 FIFO interrupt without data!");
		s->cancel(dev, s);
		s->async->events |= COMEDI_CB_EOA | COMEDI_CB_ERROR;
		comedi_event(dev, s);
		return IRQ_HANDLED;
	}

	if (lo & 2)
		len = 512;
	else
		len = 0;

	for (i = 0; i < len; i++) {
		lo = inb(dev->iobase + PCL818_FI_DATALO);
		if ((lo & 0xf) != devpriv->act_chanlist[devpriv->act_chanlist_pos]) {	/*  dropout! */
			dev_dbg(dev->class_dev,
				"A/D mode1/3 FIFO - channel dropout %d!=%d !\n",
				(lo & 0xf),
				devpriv->act_chanlist[devpriv->act_chanlist_pos]);
			s->cancel(dev, s);
			s->async->events |= COMEDI_CB_EOA | COMEDI_CB_ERROR;
			comedi_event(dev, s);
			return IRQ_HANDLED;
		}

		comedi_buf_put(s->async, (lo >> 4) | (inb(dev->iobase + PCL818_FI_DATAHI) << 4));	/*  get one sample */

		devpriv->act_chanlist_pos++;
		if (devpriv->act_chanlist_pos >= devpriv->act_chanlist_len)
			devpriv->act_chanlist_pos = 0;

		s->async->cur_chan++;
		if (s->async->cur_chan >= cmd->chanlist_len) {
			s->async->cur_chan = 0;
			devpriv->ai_act_scan--;
		}

		if (!devpriv->neverending_ai)
			if (devpriv->ai_act_scan == 0) {	/* all data sampled */
				s->cancel(dev, s);
				s->async->events |= COMEDI_CB_EOA;
				comedi_event(dev, s);
				return IRQ_HANDLED;
			}
	}

	if (len > 0)
		comedi_event(dev, s);
	return IRQ_HANDLED;
}

static irqreturn_t interrupt_pcl818(int irq, void *d)
{
	struct comedi_device *dev = d;
	struct pcl818_private *devpriv = dev->private;
	struct comedi_subdevice *s = dev->read_subdev;

	if (!dev->attached) {
		comedi_error(dev, "premature interrupt");
		return IRQ_HANDLED;
	}

	if (devpriv->ai_cmd_running && devpriv->irq_was_now_closed) {
		if ((devpriv->neverending_ai || (!devpriv->neverending_ai &&
						 devpriv->ai_act_scan > 0)) &&
		    (devpriv->ai_mode == INT_TYPE_AI1_DMA ||
		     devpriv->ai_mode == INT_TYPE_AI3_DMA)) {
			/* The cleanup from ai_cancel() has been delayed
			   until now because the card doesn't seem to like
			   being reprogrammed while a DMA transfer is in
			   progress.
			 */
			devpriv->ai_act_scan = 0;
			devpriv->neverending_ai = 0;
			s->cancel(dev, s);
		}

		outb(0, dev->iobase + PCL818_CLRINT);	/* clear INT request */

		return IRQ_HANDLED;
	}

	switch (devpriv->ai_mode) {
	case INT_TYPE_AI1_DMA:
	case INT_TYPE_AI3_DMA:
		return interrupt_pcl818_ai_mode13_dma(irq, d);
	case INT_TYPE_AI1_INT:
	case INT_TYPE_AI3_INT:
		return interrupt_pcl818_ai_mode13_int(irq, d);
	case INT_TYPE_AI1_FIFO:
	case INT_TYPE_AI3_FIFO:
		return interrupt_pcl818_ai_mode13_fifo(irq, d);
	default:
		break;
	}

	outb(0, dev->iobase + PCL818_CLRINT);	/* clear INT request */

	if (!devpriv->ai_cmd_running || !devpriv->ai_mode) {
		comedi_error(dev, "bad IRQ!");
		return IRQ_NONE;
	}

	comedi_error(dev, "IRQ from unknown source!");
	return IRQ_NONE;
}

static void pcl818_ai_mode13dma_int(int mode, struct comedi_device *dev,
				    struct comedi_subdevice *s)
{
	struct pcl818_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int flags;
	unsigned int bytes;

	disable_dma(devpriv->dma);	/*  disable dma */
	bytes = devpriv->hwdmasize;
	if (!devpriv->neverending_ai) {
		bytes = cmd->chanlist_len * cmd->stop_arg * sizeof(short);
		devpriv->dma_runs_to_end = bytes / devpriv->hwdmasize;
		devpriv->last_dma_run = bytes % devpriv->hwdmasize;
		devpriv->dma_runs_to_end--;
		if (devpriv->dma_runs_to_end >= 0)
			bytes = devpriv->hwdmasize;
	}

	devpriv->next_dma_buf = 0;
	set_dma_mode(devpriv->dma, DMA_MODE_READ);
	flags = claim_dma_lock();
	clear_dma_ff(devpriv->dma);
	set_dma_addr(devpriv->dma, devpriv->hwdmaptr[0]);
	set_dma_count(devpriv->dma, bytes);
	release_dma_lock(flags);
	enable_dma(devpriv->dma);

	if (mode == 1) {
		devpriv->ai_mode = INT_TYPE_AI1_DMA;
		outb(0x87 | (dev->irq << 4), dev->iobase + PCL818_CONTROL);	/* Pacer+IRQ+DMA */
	} else {
		devpriv->ai_mode = INT_TYPE_AI3_DMA;
		outb(0x86 | (dev->irq << 4), dev->iobase + PCL818_CONTROL);	/* Ext trig+IRQ+DMA */
	}
}

static int pcl818_ai_cmd_mode(int mode, struct comedi_device *dev,
			      struct comedi_subdevice *s)
{
	struct pcl818_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int seglen;

	if (devpriv->ai_cmd_running)
		return -EBUSY;

	pcl818_start_pacer(dev, false);

	seglen = check_channel_list(dev, s, cmd->chanlist, cmd->chanlist_len);
	if (seglen < 1)
		return -EINVAL;
	setup_channel_list(dev, s, cmd->chanlist, cmd->chanlist_len, seglen);

	udelay(1);

	devpriv->ai_act_scan = cmd->stop_arg;
	devpriv->ai_act_chan = 0;
	devpriv->ai_cmd_running = 1;
	devpriv->irq_was_now_closed = 0;
	devpriv->act_chanlist_pos = 0;
	devpriv->dma_runs_to_end = 0;

	outb(0, dev->iobase + PCL818_CNTENABLE);	/* enable pacer */

	switch (devpriv->dma) {
	case 1:		/*  DMA */
	case 3:
		pcl818_ai_mode13dma_int(mode, dev, s);
		break;
	case 0:
		if (!devpriv->usefifo) {
			/* IRQ */
			if (mode == 1) {
				devpriv->ai_mode = INT_TYPE_AI1_INT;
				/* Pacer+IRQ */
				outb(0x83 | (dev->irq << 4),
				     dev->iobase + PCL818_CONTROL);
			} else {
				devpriv->ai_mode = INT_TYPE_AI3_INT;
				/* Ext trig+IRQ */
				outb(0x82 | (dev->irq << 4),
				     dev->iobase + PCL818_CONTROL);
			}
		} else {
			/* FIFO */
			/* enable FIFO */
			outb(1, dev->iobase + PCL818_FI_ENABLE);
			if (mode == 1) {
				devpriv->ai_mode = INT_TYPE_AI1_FIFO;
				/* Pacer */
				outb(0x03, dev->iobase + PCL818_CONTROL);
			} else {
				devpriv->ai_mode = INT_TYPE_AI3_FIFO;
				outb(0x02, dev->iobase + PCL818_CONTROL);
			}
		}
	}

	pcl818_start_pacer(dev, mode == 1);

	return 0;
}

static int check_channel_list(struct comedi_device *dev,
			      struct comedi_subdevice *s,
			      unsigned int *chanlist, unsigned int n_chan)
{
	unsigned int chansegment[16];
	unsigned int i, nowmustbechan, seglen, segpos;

	/* correct channel and range number check itself comedi/range.c */
	if (n_chan < 1) {
		comedi_error(dev, "range/channel list is empty!");
		return 0;
	}

	if (n_chan > 1) {
		/*  first channel is every time ok */
		chansegment[0] = chanlist[0];
		/*  build part of chanlist */
		for (i = 1, seglen = 1; i < n_chan; i++, seglen++) {
			/* we detect loop, this must by finish */

			if (chanlist[0] == chanlist[i])
				break;
			nowmustbechan =
			    (CR_CHAN(chansegment[i - 1]) + 1) % s->n_chan;
			if (nowmustbechan != CR_CHAN(chanlist[i])) {	/*  channel list isn't continuous :-( */
				dev_dbg(dev->class_dev,
					"channel list must be continuous! chanlist[%i]=%d but must be %d or %d!\n",
					i, CR_CHAN(chanlist[i]), nowmustbechan,
					CR_CHAN(chanlist[0]));
				return 0;
			}
			/*  well, this is next correct channel in list */
			chansegment[i] = chanlist[i];
		}

		/*  check whole chanlist */
		for (i = 0, segpos = 0; i < n_chan; i++) {
			if (chanlist[i] != chansegment[i % seglen]) {
				dev_dbg(dev->class_dev,
					"bad channel or range number! chanlist[%i]=%d,%d,%d and not %d,%d,%d!\n",
					i, CR_CHAN(chansegment[i]),
					CR_RANGE(chansegment[i]),
					CR_AREF(chansegment[i]),
					CR_CHAN(chanlist[i % seglen]),
					CR_RANGE(chanlist[i % seglen]),
					CR_AREF(chansegment[i % seglen]));
				return 0;	/*  chan/gain list is strange */
			}
		}
	} else {
		seglen = 1;
	}
	return seglen;
}

static void setup_channel_list(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       unsigned int *chanlist, unsigned int n_chan,
			       unsigned int seglen)
{
	struct pcl818_private *devpriv = dev->private;
	int i;

	devpriv->act_chanlist_len = seglen;
	devpriv->act_chanlist_pos = 0;

	for (i = 0; i < seglen; i++) {	/*  store range list to card */
		devpriv->act_chanlist[i] = CR_CHAN(chanlist[i]);
		outb(muxonechan[CR_CHAN(chanlist[i])], dev->iobase + PCL818_MUX);	/* select channel */
		outb(CR_RANGE(chanlist[i]), dev->iobase + PCL818_RANGE);	/* select gain */
	}

	udelay(1);

	/* select channel interval to scan */
	outb(devpriv->act_chanlist[0] | (devpriv->act_chanlist[seglen -
							       1] << 4),
	     dev->iobase + PCL818_MUX);
}

static int check_single_ended(unsigned int port)
{
	if (inb(port + PCL818_STATUS) & 0x20)
		return 1;
	return 0;
}

static int ai_cmdtest(struct comedi_device *dev, struct comedi_subdevice *s,
		      struct comedi_cmd *cmd)
{
	const struct pcl818_board *board = comedi_board(dev);
	struct pcl818_private *devpriv = dev->private;
	int err = 0;
	int tmp;

	/* Step 1 : check if triggers are trivially valid */

	err |= cfc_check_trigger_src(&cmd->start_src, TRIG_NOW);
	err |= cfc_check_trigger_src(&cmd->scan_begin_src, TRIG_FOLLOW);
	err |= cfc_check_trigger_src(&cmd->convert_src, TRIG_TIMER | TRIG_EXT);
	err |= cfc_check_trigger_src(&cmd->scan_end_src, TRIG_COUNT);
	err |= cfc_check_trigger_src(&cmd->stop_src, TRIG_COUNT | TRIG_NONE);

	if (err)
		return 1;

	/* Step 2a : make sure trigger sources are unique */

	err |= cfc_check_trigger_is_unique(cmd->convert_src);
	err |= cfc_check_trigger_is_unique(cmd->stop_src);

	/* Step 2b : and mutually compatible */

	if (err)
		return 2;

	/* Step 3: check if arguments are trivially valid */

	err |= cfc_check_trigger_arg_is(&cmd->start_arg, 0);
	err |= cfc_check_trigger_arg_is(&cmd->scan_begin_arg, 0);

	if (cmd->convert_src == TRIG_TIMER)
		err |= cfc_check_trigger_arg_min(&cmd->convert_arg,
						 board->ns_min);
	else	/* TRIG_EXT */
		err |= cfc_check_trigger_arg_is(&cmd->convert_arg, 0);

	err |= cfc_check_trigger_arg_is(&cmd->scan_end_arg, cmd->chanlist_len);

	if (cmd->stop_src == TRIG_COUNT)
		err |= cfc_check_trigger_arg_min(&cmd->stop_arg, 1);
	else	/* TRIG_NONE */
		err |= cfc_check_trigger_arg_is(&cmd->stop_arg, 0);

	if (err)
		return 3;

	/* step 4: fix up any arguments */

	if (cmd->convert_src == TRIG_TIMER) {
		tmp = cmd->convert_arg;
		i8253_cascade_ns_to_timer(devpriv->i8253_osc_base,
					  &devpriv->divisor1,
					  &devpriv->divisor2,
					  &cmd->convert_arg, cmd->flags);
		if (cmd->convert_arg < board->ns_min)
			cmd->convert_arg = board->ns_min;
		if (tmp != cmd->convert_arg)
			err++;
	}

	if (err)
		return 4;

	/* step 5: complain about special chanlist considerations */

	if (cmd->chanlist) {
		if (!check_channel_list(dev, s, cmd->chanlist,
					cmd->chanlist_len))
			return 5;	/*  incorrect channels list */
	}

	return 0;
}

static int ai_cmd(struct comedi_device *dev, struct comedi_subdevice *s)
{
	struct pcl818_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	int retval;

	devpriv->ai_data_len = s->async->prealloc_bufsz;

	if (cmd->stop_src == TRIG_COUNT)
		devpriv->neverending_ai = 0;
	else
		devpriv->neverending_ai = 1;

	if (cmd->scan_begin_src == TRIG_FOLLOW) {	/*  mode 1, 3 */
		if (cmd->convert_src == TRIG_TIMER) {	/*  mode 1 */
			retval = pcl818_ai_cmd_mode(1, dev, s);
			return retval;
		}
		if (cmd->convert_src == TRIG_EXT) {	/*  mode 3 */
			return pcl818_ai_cmd_mode(3, dev, s);
		}
	}

	return -1;
}

static int pcl818_ai_cancel(struct comedi_device *dev,
			    struct comedi_subdevice *s)
{
	struct pcl818_private *devpriv = dev->private;

	if (devpriv->ai_cmd_running) {
		devpriv->irq_was_now_closed = 1;

		switch (devpriv->ai_mode) {
		case INT_TYPE_AI1_DMA:
		case INT_TYPE_AI3_DMA:
			if (devpriv->neverending_ai ||
			    (!devpriv->neverending_ai &&
			     devpriv->ai_act_scan > 0)) {
				/* wait for running dma transfer to end, do cleanup in interrupt */
				goto end;
			}
			disable_dma(devpriv->dma);
		case INT_TYPE_AI1_INT:
		case INT_TYPE_AI3_INT:
		case INT_TYPE_AI1_FIFO:
		case INT_TYPE_AI3_FIFO:
			outb(inb(dev->iobase + PCL818_CONTROL) & 0x73, dev->iobase + PCL818_CONTROL);	/* Stop A/D */
			udelay(1);
			pcl818_start_pacer(dev, false);
			outb(0, dev->iobase + PCL818_AD_LO);
			pcl818_ai_get_sample(dev, s, NULL);
			outb(0, dev->iobase + PCL818_CLRINT);	/* clear INT request */
			outb(0, dev->iobase + PCL818_CONTROL);	/* Stop A/D */
			if (devpriv->usefifo) {	/*  FIFO shutdown */
				outb(0, dev->iobase + PCL818_FI_INTCLR);
				outb(0, dev->iobase + PCL818_FI_FLUSH);
				outb(0, dev->iobase + PCL818_FI_ENABLE);
			}
			devpriv->ai_cmd_running = 0;
			devpriv->neverending_ai = 0;
			devpriv->ai_mode = 0;
			devpriv->irq_was_now_closed = 0;
			break;
		}
	}

end:
	return 0;
}

static void pcl818_reset(struct comedi_device *dev)
{
	const struct pcl818_board *board = comedi_board(dev);
	unsigned long timer_base = dev->iobase + PCL818_TIMER_BASE;

	/* flush and disable the FIFO */
	if (board->has_fifo) {
		outb(0, dev->iobase + PCL818_FI_INTCLR);
		outb(0, dev->iobase + PCL818_FI_FLUSH);
		outb(0, dev->iobase + PCL818_FI_ENABLE);
	}
	outb(0, dev->iobase + PCL818_DA_LO);	/*  DAC=0V */
	outb(0, dev->iobase + PCL818_DA_HI);
	udelay(1);
	outb(0, dev->iobase + PCL818_DO_HI);	/*  DO=$0000 */
	outb(0, dev->iobase + PCL818_DO_LO);
	udelay(1);
	outb(0, dev->iobase + PCL818_CONTROL);
	outb(0, dev->iobase + PCL818_CNTENABLE);
	outb(0, dev->iobase + PCL818_MUX);
	outb(0, dev->iobase + PCL818_CLRINT);

	/* Stop pacer */
	i8254_set_mode(timer_base, 0, 2, I8254_MODE0 | I8254_BINARY);
	i8254_set_mode(timer_base, 0, 1, I8254_MODE0 | I8254_BINARY);
	i8254_set_mode(timer_base, 0, 0, I8254_MODE0 | I8254_BINARY);

	if (board->is_818) {
		outb(0, dev->iobase + PCL818_RANGE);
	} else {
		outb(0, dev->iobase + PCL718_DA2_LO);
		outb(0, dev->iobase + PCL718_DA2_HI);
	}
}

static void pcl818_set_ai_range_table(struct comedi_device *dev,
				      struct comedi_subdevice *s,
				      struct comedi_devconfig *it)
{
	const struct pcl818_board *board = comedi_board(dev);

	/* default to the range table from the boardinfo */
	s->range_table = board->ai_range_type;

	/* now check the user config option based on the boardtype */
	if (board->is_818) {
		if (it->options[4] == 1 || it->options[4] == 10) {
			/* secondary range list jumper selectable */
			s->range_table = &range_pcl818l_h_ai;
		}
	} else {
		switch (it->options[4]) {
		case 0:
			s->range_table = &range_bipolar10;
			break;
		case 1:
			s->range_table = &range_bipolar5;
			break;
		case 2:
			s->range_table = &range_bipolar2_5;
			break;
		case 3:
			s->range_table = &range718_bipolar1;
			break;
		case 4:
			s->range_table = &range718_bipolar0_5;
			break;
		case 6:
			s->range_table = &range_unipolar10;
			break;
		case 7:
			s->range_table = &range_unipolar5;
			break;
		case 8:
			s->range_table = &range718_unipolar2;
			break;
		case 9:
			s->range_table = &range718_unipolar1;
			break;
		default:
			s->range_table = &range_unknown;
			break;
		}
	}
}

static int pcl818_attach(struct comedi_device *dev, struct comedi_devconfig *it)
{
	const struct pcl818_board *board = comedi_board(dev);
	struct pcl818_private *devpriv;
	struct comedi_subdevice *s;
	int ret;
	int i;

	devpriv = comedi_alloc_devpriv(dev, sizeof(*devpriv));
	if (!devpriv)
		return -ENOMEM;

	ret = comedi_request_region(dev, it->options[0],
				    board->has_fifo ? 0x20 : 0x10);
	if (ret)
		return ret;

	/* we can use IRQ 2-7 for async command support */
	if (it->options[1] >= 2 && it->options[1] <= 7) {
		ret = request_irq(it->options[1], interrupt_pcl818, 0,
				  dev->board_name, dev);
		if (ret == 0)
			dev->irq = it->options[1];
	}

	/* should we use the FIFO? */
	if (dev->irq && board->has_fifo && it->options[2] == -1)
		devpriv->usefifo = 1;

	/* we need an IRQ to do DMA on channel 3 or 1 */
	if (dev->irq && board->has_dma &&
	    (it->options[2] == 3 || it->options[2] == 1)) {
		ret = request_dma(it->options[2], dev->board_name);
		if (ret) {
			dev_err(dev->class_dev,
				"unable to request DMA channel %d\n",
				it->options[2]);
			return -EBUSY;
		}
		devpriv->dma = it->options[2];

		devpriv->dmapages = 2;	/* we need 16KB */
		devpriv->hwdmasize = (1 << devpriv->dmapages) * PAGE_SIZE;

		for (i = 0; i < 2; i++) {
			unsigned long dmabuf;

			dmabuf = __get_dma_pages(GFP_KERNEL, devpriv->dmapages);
			if (!dmabuf)
				return -ENOMEM;

			devpriv->dmabuf[i] = dmabuf;
			devpriv->hwdmaptr[i] = virt_to_bus((void *)dmabuf);
		}
	}

	ret = comedi_alloc_subdevices(dev, 4);
	if (ret)
		return ret;

	s = &dev->subdevices[0];
	s->type		= COMEDI_SUBD_AI;
	s->subdev_flags	= SDF_READABLE;
	if (check_single_ended(dev->iobase)) {
		s->n_chan	= 16;
		s->subdev_flags	|= SDF_COMMON | SDF_GROUND;
	} else {
		s->n_chan	= 8;
		s->subdev_flags	|= SDF_DIFF;
	}
	s->maxdata	= 0x0fff;

	pcl818_set_ai_range_table(dev, s, it);

	s->insn_read	= pcl818_ai_insn_read;
	if (dev->irq) {
		dev->read_subdev = s;
		s->subdev_flags	|= SDF_CMD_READ;
		s->len_chanlist	= s->n_chan;
		s->do_cmdtest	= ai_cmdtest;
		s->do_cmd	= ai_cmd;
		s->cancel	= pcl818_ai_cancel;
	}

	s = &dev->subdevices[1];
	if (!board->n_aochan) {
		s->type = COMEDI_SUBD_UNUSED;
	} else {
		s->type = COMEDI_SUBD_AO;
		s->subdev_flags = SDF_WRITABLE | SDF_GROUND;
		s->n_chan = board->n_aochan;
		s->maxdata = 0x0fff;
		s->range_table = &range_unipolar5;
		s->insn_read = pcl818_ao_insn_read;
		s->insn_write = pcl818_ao_insn_write;
		if (board->is_818) {
			if ((it->options[4] == 1) || (it->options[4] == 10))
				s->range_table = &range_unipolar10;
			if (it->options[4] == 2)
				s->range_table = &range_unknown;
		} else {
			if ((it->options[5] == 1) || (it->options[5] == 10))
				s->range_table = &range_unipolar10;
			if (it->options[5] == 2)
				s->range_table = &range_unknown;
		}
	}

	/* Digital Input subdevice */
	s = &dev->subdevices[2];
	s->type		= COMEDI_SUBD_DI;
	s->subdev_flags	= SDF_READABLE;
	s->n_chan	= 16;
	s->maxdata	= 1;
	s->range_table	= &range_digital;
	s->insn_bits	= pcl818_di_insn_bits;

	/* Digital Output subdevice */
	s = &dev->subdevices[3];
	s->type		= COMEDI_SUBD_DO;
	s->subdev_flags	= SDF_WRITABLE;
	s->n_chan	= 16;
	s->maxdata	= 1;
	s->range_table	= &range_digital;
	s->insn_bits	= pcl818_do_insn_bits;

	/* select 1/10MHz oscilator */
	if ((it->options[3] == 0) || (it->options[3] == 10))
		devpriv->i8253_osc_base = I8254_OSC_BASE_10MHZ;
	else
		devpriv->i8253_osc_base = I8254_OSC_BASE_1MHZ;

	/* max sampling speed */
	devpriv->ns_min = board->ns_min;

	if (!board->is_818) {
		if ((it->options[6] == 1) || (it->options[6] == 100))
			devpriv->ns_min = 10000;	/* extended PCL718 to 100kHz DAC */
	}

	pcl818_reset(dev);

	return 0;
}

static void pcl818_detach(struct comedi_device *dev)
{
	struct pcl818_private *devpriv = dev->private;

	if (devpriv) {
		pcl818_ai_cancel(dev, dev->read_subdev);
		pcl818_reset(dev);
		if (devpriv->dma)
			free_dma(devpriv->dma);
		if (devpriv->dmabuf[0])
			free_pages(devpriv->dmabuf[0], devpriv->dmapages);
		if (devpriv->dmabuf[1])
			free_pages(devpriv->dmabuf[1], devpriv->dmapages);
	}
	comedi_legacy_detach(dev);
}

static struct comedi_driver pcl818_driver = {
	.driver_name	= "pcl818",
	.module		= THIS_MODULE,
	.attach		= pcl818_attach,
	.detach		= pcl818_detach,
	.board_name	= &boardtypes[0].name,
	.num_names	= ARRAY_SIZE(boardtypes),
	.offset		= sizeof(struct pcl818_board),
};
module_comedi_driver(pcl818_driver);

MODULE_AUTHOR("Comedi http://www.comedi.org");
MODULE_DESCRIPTION("Comedi low-level driver");
MODULE_LICENSE("GPL");
