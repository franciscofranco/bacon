/*
 * aac audio input device
 *
 * Copyright (C) 2008 Google, Inc.
 * Copyright (C) 2008 HTC Corporation
 * Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/atomic.h>
#include <asm/ioctls.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>
#include <linux/msm_audio_aac.h>
#include <linux/memory_alloc.h>
#include <mach/msm_memtypes.h>

#include <mach/msm_adsp.h>
#include <mach/iommu.h>
#include <mach/iommu_domains.h>
#include <mach/qdsp5v2/qdsp5audreccmdi.h>
#include <mach/qdsp5v2/qdsp5audrecmsg.h>
#include <mach/qdsp5v2/audpreproc.h>
#include <mach/qdsp5v2/audio_dev_ctl.h>
#include <mach/debug_mm.h>
#include <mach/socinfo.h>

/* FRAME_NUM must be a power of two */
#define FRAME_NUM		(8)
#define FRAME_SIZE		(772 * 2) /* 1536 bytes data */
#define NT_FRAME_SIZE	(780 * 2) /* 1536 bytes data  + 24 meta field*/
#define AAC_FRAME_SIZE	1536
#define DMASZ 			(FRAME_SIZE * FRAME_NUM)
#define OUT_FRAME_NUM	(2)
#define META_OUT_SIZE	(24)
#define META_IN_SIZE	(14)
#define OUT_BUFFER_SIZE (32 * 1024 + META_OUT_SIZE)
#define BUFFER_SIZE		(OUT_BUFFER_SIZE * OUT_FRAME_NUM)

#define AUDPREPROC_AAC_EOS_FLG_OFFSET 0x0A /* Offset from beginning of buffer */
#define AUDPREPROC_AAC_EOS_FLG_MASK 0x01
#define AUDPREPROC_AAC_EOS_NONE 0x0 /* No EOS detected */
#define AUDPREPROC_AAC_EOS_SET 0x1 /* EOS set in meta field */

#define PCM_CONFIG_UPDATE_FLAG_ENABLE -1
#define PCM_CONFIG_UPDATE_FLAG_DISABLE	0

#define ENABLE_FLAG_VALUE	-1
#define DISABLE_FLAG_VALUE	0

struct buffer {
	void *data;
	uint32_t size;
	uint32_t read;
	uint32_t addr;
	uint32_t used;
	uint32_t mfield_sz;
};

struct audio_in {
	struct buffer in[FRAME_NUM];

	spinlock_t dsp_lock;

	atomic_t in_bytes;
	atomic_t in_samples;

	struct mutex lock;
	struct mutex read_lock;
	wait_queue_head_t wait;
	wait_queue_head_t wait_enable;
	/*write section*/
	struct buffer out[OUT_FRAME_NUM];

	uint8_t out_head;
	uint8_t out_tail;
	uint8_t out_needed;	/* number of buffers the dsp is waiting for */
	uint32_t out_count;

	struct mutex write_lock;
	wait_queue_head_t write_wait;
	int32_t out_phys; /* physical address of write buffer */
	char *out_data;
	void *map_v_read;
	void *map_v_write;

	int mfield; /* meta field embedded in data */
	int wflush; /*write flush */
	int rflush; /*read flush*/
	int out_frame_cnt;

	struct msm_adsp_module *audrec;

	/* configuration to use on next enable */
	uint32_t buffer_size; /* Frame size (36 bytes) */
	uint32_t samp_rate;
	uint32_t channel_mode;
	uint32_t bit_rate; /* bit rate for AAC */
	uint32_t record_quality; /* record quality (bits/sample/channel) */
	uint32_t enc_type;

	uint32_t dsp_cnt;
	uint32_t in_head; /* next buffer dsp will write */
	uint32_t in_tail; /* next buffer read() will read */
	uint32_t in_count; /* number of buffers available to read() */
	uint32_t mode;
	uint32_t eos_ack;
	uint32_t flush_ack;

	const char *module_name;
	unsigned queue_ids;
	uint16_t enc_id;

	struct audrec_session_info session_info; /*audrec session info*/
	uint16_t source; /* Encoding source bit mask */
	uint32_t device_events; /* device events interested in */
	uint32_t dev_cnt;
	spinlock_t dev_lock;

	/* data allocated for various buffers */
	char *data;
	dma_addr_t phys;

	int opened;
	int enabled;
	int running;
	int stopped; /* set when stopped, cleared on flush */
	int abort; /* set when error, like sample rate mismatch */
	char *build_id;
};

struct audio_frame {
	uint16_t frame_count_lsw;
	uint16_t frame_count_msw;
	uint16_t frame_length;
	uint16_t erased_pcm;
	unsigned char raw_bitstream[]; /* samples */
} __attribute__((packed));

struct audio_frame_nt {
	uint16_t metadata_len;
	uint16_t frame_count_lsw;
	uint16_t frame_count_msw;
	uint16_t frame_length;
	uint16_t erased_pcm;
	uint16_t reserved;
	uint16_t time_stamp_dword_lsw;
	uint16_t time_stamp_dword_msw;
	uint16_t time_stamp_lsw;
	uint16_t time_stamp_msw;
	uint16_t nflag_lsw;
	uint16_t nflag_msw;
	unsigned char raw_bitstream[]; /* samples */
} __attribute__((packed));

struct aac_encoded_meta_in {
	uint16_t metadata_len;
	uint16_t time_stamp_dword_lsw;
	uint16_t time_stamp_dword_msw;
	uint16_t time_stamp_lsw;
	uint16_t time_stamp_msw;
	uint16_t nflag_lsw;
	uint16_t nflag_msw;
};

/* Audrec Queue command sent macro's */
#define audrec_send_bitstreamqueue(audio, cmd, len) \
	msm_adsp_write(audio->audrec, ((audio->queue_ids & 0xFFFF0000) >> 16),\
			cmd, len)

#define audrec_send_audrecqueue(audio, cmd, len) \
	msm_adsp_write(audio->audrec, (audio->queue_ids & 0x0000FFFF),\
			cmd, len)

/* DSP command send functions */
static int audaac_in_enc_config(struct audio_in *audio, int enable);
static int audaac_in_param_config(struct audio_in *audio);
static int audaac_in_mem_config(struct audio_in *audio);
static int audaac_in_record_config(struct audio_in *audio, int enable);
static int audaac_dsp_read_buffer(struct audio_in *audio, uint32_t read_cnt);

static void audaac_in_get_dsp_frames(struct audio_in *audio);
static int audpcm_config(struct audio_in *audio);
static void audaac_out_flush(struct audio_in *audio);
static int audpreproc_cmd_cfg_routing_mode(struct audio_in *audio);
static void audpreproc_pcm_send_data(struct audio_in *audio, unsigned needed);
static void audaac_nt_in_get_dsp_frames(struct audio_in *audio);

static void audaac_in_flush(struct audio_in *audio);

static void aac_in_listener(u32 evt_id, union auddev_evt_data *evt_payload,
				void *private_data)
{
	struct audio_in *audio = (struct audio_in *) private_data;
	unsigned long flags;

	MM_DBG("evt_id = 0x%8x\n", evt_id);
	switch (evt_id) {
	case AUDDEV_EVT_DEV_RDY: {
		MM_DBG("AUDDEV_EVT_DEV_RDY\n");
		spin_lock_irqsave(&audio->dev_lock, flags);
		audio->dev_cnt++;
		audio->source |= (0x1 << evt_payload->routing_id);
		spin_unlock_irqrestore(&audio->dev_lock, flags);

		if ((audio->running == 1) && (audio->enabled == 1) &&
			(audio->mode == MSM_AUD_ENC_MODE_TUNNEL))
			audaac_in_record_config(audio, 1);

		break;
	}
	case AUDDEV_EVT_DEV_RLS: {
		MM_DBG("AUDDEV_EVT_DEV_RLS\n");
		spin_lock_irqsave(&audio->dev_lock, flags);
		audio->dev_cnt--;
		audio->source &= ~(0x1 << evt_payload->routing_id);
		spin_unlock_irqrestore(&audio->dev_lock, flags);

		if ((!audio->running) || (!audio->enabled))
			break;

		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
			/* Turn of as per source */
			if (audio->source)
				audaac_in_record_config(audio, 1);
			else
			/* Turn off all */
				audaac_in_record_config(audio, 0);
		}
		break;
	}
	case AUDDEV_EVT_FREQ_CHG: {
		MM_DBG("Encoder Driver got sample rate change event\n");
		MM_DBG("sample rate %d\n", evt_payload->freq_info.sample_rate);
		MM_DBG("dev_type %d\n", evt_payload->freq_info.dev_type);
		MM_DBG("acdb_dev_id %d\n", evt_payload->freq_info.acdb_dev_id);
		if ((audio->running == 1) && (audio->enabled == 1)) {
			/* Stop Recording sample rate does not match
			   with device sample rate */
			if (evt_payload->freq_info.sample_rate !=
				audio->samp_rate) {
				if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)
					audaac_in_record_config(audio, 0);
				audio->abort = 1;
				wake_up(&audio->wait);
			}
		}
		break;
	}
	default:
		MM_ERR("wrong event %d\n", evt_id);
		break;
	}
}

/* Convert Bit Rate to Record Quality field of DSP */
static unsigned int bitrate_to_record_quality(unsigned int sample_rate,
		unsigned int channel, unsigned int bit_rate) {
	unsigned int temp;

	temp = sample_rate * channel;
	MM_DBG(" sample rate *  channel = %d \n", temp);
	/* To represent in Q12 fixed format */
	temp = (bit_rate * 4096) / temp;
	MM_DBG(" Record Quality = 0x%8x \n", temp);
	return temp;
}

/* ------------------- dsp preproc event handler--------------------- */
static void audpreproc_dsp_event(void *data, unsigned id,  void *msg)
{
	struct audio_in *audio = data;

	switch (id) {
	case AUDPREPROC_ERROR_MSG: {
		struct audpreproc_err_msg *err_msg = msg;

		MM_ERR("ERROR_MSG: stream id %d err idx %d\n",
		err_msg->stream_id, err_msg->aud_preproc_err_idx);
		/* Error case */
		wake_up(&audio->wait_enable);
		break;
	}
	case AUDPREPROC_CMD_CFG_DONE_MSG: {
		MM_DBG("CMD_CFG_DONE_MSG \n");
		break;
	}
	case AUDPREPROC_CMD_ENC_CFG_DONE_MSG: {
		struct audpreproc_cmd_enc_cfg_done_msg *enc_cfg_msg = msg;

		MM_DBG("CMD_ENC_CFG_DONE_MSG: stream id %d enc type \
			0x%8x\n", enc_cfg_msg->stream_id,
			enc_cfg_msg->rec_enc_type);
		/* Encoder enable success */
		if (enc_cfg_msg->rec_enc_type & ENCODE_ENABLE) {
			if(audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL) {
				MM_DBG("routing command\n");
				audpreproc_cmd_cfg_routing_mode(audio);
			} else {
				audaac_in_param_config(audio);
			}
		} else { /* Encoder disable success */
			audio->running = 0;
			if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)
				audaac_in_record_config(audio, 0);
			else
				wake_up(&audio->wait_enable);
		}
		break;
	}
	case AUDPREPROC_CMD_ENC_PARAM_CFG_DONE_MSG: {
		MM_DBG("CMD_ENC_PARAM_CFG_DONE_MSG\n");
		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)
			audaac_in_mem_config(audio);
		else
			audpcm_config(audio);
		break;
	}
	case AUDPREPROC_CMD_ROUTING_MODE_DONE_MSG: {
		struct audpreproc_cmd_routing_mode_done\
				*routing_cfg_done_msg = msg;
		if (routing_cfg_done_msg->configuration == 0) {
			MM_INFO("routing configuration failed\n");
			audio->running = 0;
		} else
			audaac_in_param_config(audio);
		break;
	}
	case AUDPREPROC_AFE_CMD_AUDIO_RECORD_CFG_DONE_MSG: {
		MM_DBG("AFE_CMD_AUDIO_RECORD_CFG_DONE_MSG\n");
		wake_up(&audio->wait_enable);
		break;
	}
	default:
		MM_ERR("Unknown Event id %d\n", id);
	}
}

/* ------------------- dsp audrec event handler--------------------- */
static void audrec_dsp_event(void *data, unsigned id, size_t len,
			    void (*getevent)(void *ptr, size_t len))
{
	struct audio_in *audio = data;

	switch (id) {
	case AUDREC_CMD_MEM_CFG_DONE_MSG: {
		MM_DBG("CMD_MEM_CFG_DONE MSG DONE\n");
		audio->running = 1;
		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
			if (audio->dev_cnt > 0)
				audaac_in_record_config(audio, 1);
		} else {
			audpreproc_pcm_send_data(audio, 1);
			wake_up(&audio->wait_enable);
		}
		break;
	}
	case AUDREC_FATAL_ERR_MSG: {
		struct audrec_fatal_err_msg fatal_err_msg;

		getevent(&fatal_err_msg, AUDREC_FATAL_ERR_MSG_LEN);
		MM_ERR("FATAL_ERR_MSG: err id %d\n",
				fatal_err_msg.audrec_err_id);
		/* Error stop the encoder */
		audio->stopped = 1;
		wake_up(&audio->wait);
		if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL)
			wake_up(&audio->write_wait);
		break;
	}
	case AUDREC_UP_PACKET_READY_MSG: {
		struct audrec_up_pkt_ready_msg pkt_ready_msg;

		getevent(&pkt_ready_msg, AUDREC_UP_PACKET_READY_MSG_LEN);
		MM_DBG("UP_PACKET_READY_MSG: write cnt lsw  %d \
		write cnt msw %d read cnt lsw %d  read cnt msw %d \n",\
		pkt_ready_msg.audrec_packet_write_cnt_lsw, \
		pkt_ready_msg.audrec_packet_write_cnt_msw, \
		pkt_ready_msg.audrec_up_prev_read_cnt_lsw, \
		pkt_ready_msg.audrec_up_prev_read_cnt_msw);

		audaac_in_get_dsp_frames(audio);
		break;
	}
	case AUDREC_CMD_PCM_BUFFER_PTR_UPDATE_ARM_TO_ENC_MSG: {
		MM_DBG("ptr_update recieved from DSP\n");
		audpreproc_pcm_send_data(audio, 1);
		break;
	}
	case AUDREC_CMD_PCM_CFG_ARM_TO_ENC_DONE_MSG: {
		MM_ERR("AUDREC_CMD_PCM_CFG_ARM_TO_ENC_DONE_MSG");
		audaac_in_mem_config(audio);
		break;
	}
	case AUDREC_UP_NT_PACKET_READY_MSG: {
		struct audrec_up_nt_packet_ready_msg pkt_ready_msg;

		getevent(&pkt_ready_msg, AUDREC_UP_NT_PACKET_READY_MSG_LEN);
		MM_DBG("UP_NT_PACKET_READY_MSG: write cnt lsw  %d \
		write cnt msw %d read cnt lsw %d  read cnt msw %d \n",\
		pkt_ready_msg.audrec_packetwrite_cnt_lsw, \
		pkt_ready_msg.audrec_packetwrite_cnt_msw, \
		pkt_ready_msg.audrec_upprev_readcount_lsw, \
		pkt_ready_msg.audrec_upprev_readcount_msw);

		audaac_nt_in_get_dsp_frames(audio);
		break;
	}
	case AUDREC_CMD_EOS_ACK_MSG: {
		MM_DBG("eos ack recieved\n");
		break;
	}
	case AUDREC_CMD_FLUSH_DONE_MSG: {
		audio->wflush = 0;
		audio->rflush = 0;
		audio->flush_ack = 1;
		wake_up(&audio->write_wait);
		MM_DBG("flush ack recieved\n");
		break;
	}
	case ADSP_MESSAGE_ID: {
		MM_DBG("Received ADSP event:module audrectask\n");
		break;
	}
	default:
		MM_ERR("Unknown Event id %d\n", id);
	}
}

static void audaac_in_get_dsp_frames(struct audio_in *audio)
{
	struct audio_frame *frame;
	uint32_t index;
	unsigned long flags;

	MM_DBG("head = %d\n", audio->in_head);
	index = audio->in_head;

	frame = (void *) (((char *)audio->in[index].data) - \
			 sizeof(*frame));

	spin_lock_irqsave(&audio->dsp_lock, flags);
	audio->in[index].size = frame->frame_length;

	/* statistics of read */
	atomic_add(audio->in[index].size, &audio->in_bytes);
	atomic_add(1, &audio->in_samples);

	audio->in_head = (audio->in_head + 1) & (FRAME_NUM - 1);

	/* If overflow, move the tail index foward. */
	if (audio->in_head == audio->in_tail) {
		MM_ERR("Error! not able to keep up the read\n");
		audio->in_tail = (audio->in_tail + 1) & (FRAME_NUM - 1);
	} else
		audio->in_count++;

	audaac_dsp_read_buffer(audio, audio->dsp_cnt++);
	spin_unlock_irqrestore(&audio->dsp_lock, flags);

	wake_up(&audio->wait);
}

static void audaac_nt_in_get_dsp_frames(struct audio_in *audio)
{
	struct audio_frame_nt *nt_frame;
	uint32_t index;
	unsigned long flags;
	MM_DBG("head = %d\n", audio->in_head);
	index = audio->in_head;
	nt_frame = (void *) (((char *)audio->in[index].data) - \
				sizeof(struct audio_frame_nt));
	spin_lock_irqsave(&audio->dsp_lock, flags);
	audio->in[index].size = nt_frame->frame_length;
	/* statistics of read */
	atomic_add(audio->in[index].size, &audio->in_bytes);
	atomic_add(1, &audio->in_samples);

	audio->in_head = (audio->in_head + 1) & (FRAME_NUM - 1);

	/* If overflow, move the tail index foward. */
	if (audio->in_head == audio->in_tail)
		MM_DBG("Error! not able to keep up the read\n");
	else
		audio->in_count++;

	spin_unlock_irqrestore(&audio->dsp_lock, flags);
	wake_up(&audio->wait);
}


struct msm_adsp_ops audrec_aac_adsp_ops = {
	.event = audrec_dsp_event,
};

static int audpreproc_pcm_buffer_ptr_refresh(struct audio_in *audio,
				       unsigned idx, unsigned len)
{
	struct audrec_cmd_pcm_buffer_ptr_refresh_arm_enc cmd;

	if (len ==  META_OUT_SIZE)
		len = len / 2;
	else
		len = (len + META_OUT_SIZE) / 2;
	MM_DBG("len = %d\n", len);
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_PCM_BUFFER_PTR_REFRESH_ARM_TO_ENC;
	cmd.num_buffers = 1;
	if (cmd.num_buffers == 1) {
		cmd.buf_address_length[0] = (audio->out[idx].addr &
							0xffff0000) >> 16;
		cmd.buf_address_length[1] = (audio->out[idx].addr &
							0x0000ffff);
		cmd.buf_address_length[2] = (len & 0xffff0000) >> 16;
		cmd.buf_address_length[3] = (len & 0x0000ffff);
	}
	audio->out_frame_cnt++;
	return audrec_send_audrecqueue(audio, (void *)&cmd,
					(unsigned int)sizeof(cmd));
}


static int audpcm_config(struct audio_in *audio)
{
	struct audrec_cmd_pcm_cfg_arm_to_enc cmd;
	MM_DBG("\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_PCM_CFG_ARM_TO_ENC;
	cmd.config_update_flag = PCM_CONFIG_UPDATE_FLAG_ENABLE;
	cmd.enable_flag = ENABLE_FLAG_VALUE;
	cmd.sampling_freq = audio->samp_rate;
	if (!audio->channel_mode)
		cmd.channels = 1;
	else
		cmd.channels = 2;
	cmd.frequency_of_intimation = 1;
	cmd.max_number_of_buffers = OUT_FRAME_NUM;
	return audrec_send_audrecqueue(audio, (void *)&cmd,
					(unsigned int)sizeof(cmd));
}


static int audpreproc_cmd_cfg_routing_mode(struct audio_in *audio)
{
	struct audpreproc_audrec_cmd_routing_mode cmd;

	MM_DBG("\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDPREPROC_AUDREC_CMD_ROUTING_MODE;
	cmd.stream_id = audio->enc_id;
	if (audio->mode == MSM_ADSP_ENC_MODE_NON_TUNNEL)
		cmd.routing_mode = 1;
	return audpreproc_send_audreccmdqueue(&cmd, sizeof(cmd));
}



static int audaac_in_enc_config(struct audio_in *audio, int enable)
{
	struct audpreproc_audrec_cmd_enc_cfg cmd;
	memset(&cmd, 0, sizeof(cmd));
	if (audio->build_id[17] == '1') {
		cmd.cmd_id = AUDPREPROC_AUDREC_CMD_ENC_CFG_2;
		MM_ERR("sending AUDPREPROC_AUDREC_CMD_ENC_CFG_2 command");
	} else {
		cmd.cmd_id = AUDPREPROC_AUDREC_CMD_ENC_CFG;
		MM_ERR("sending AUDPREPROC_AUDREC_CMD_ENC_CFG command");
	}
	cmd.stream_id = audio->enc_id;

	if (enable)
		cmd.audrec_enc_type = audio->enc_type | ENCODE_ENABLE;
	else
		cmd.audrec_enc_type &= ~(ENCODE_ENABLE);

	return audpreproc_send_audreccmdqueue(&cmd, sizeof(cmd));
}

static int audaac_in_param_config(struct audio_in *audio)
{
	struct audpreproc_audrec_cmd_parm_cfg_aac cmd;

	MM_DBG("\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.common.cmd_id = AUDPREPROC_AUDREC_CMD_PARAM_CFG;
	cmd.common.stream_id = audio->enc_id;

	cmd.aud_rec_samplerate_idx = audio->samp_rate;
	cmd.aud_rec_stereo_mode = audio->channel_mode;
	cmd.recording_quality = audio->record_quality;

	return audpreproc_send_audreccmdqueue(&cmd, sizeof(cmd));
}

/* To Do: msm_snddev_route_enc(audio->enc_id); */
static int audaac_in_record_config(struct audio_in *audio, int enable)
{
	struct audpreproc_afe_cmd_audio_record_cfg cmd;
	MM_DBG("\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDPREPROC_AFE_CMD_AUDIO_RECORD_CFG;
	cmd.stream_id = audio->enc_id;
	if (enable)
		cmd.destination_activity = AUDIO_RECORDING_TURN_ON;
	else
		cmd.destination_activity = AUDIO_RECORDING_TURN_OFF;

	cmd.source_mix_mask = audio->source;
	if (audio->enc_id == 2) {
		if ((cmd.source_mix_mask & INTERNAL_CODEC_TX_SOURCE_MIX_MASK) ||
			(cmd.source_mix_mask & AUX_CODEC_TX_SOURCE_MIX_MASK) ||
			(cmd.source_mix_mask & VOICE_UL_SOURCE_MIX_MASK) ||
			(cmd.source_mix_mask & VOICE_DL_SOURCE_MIX_MASK)) {
			cmd.pipe_id = SOURCE_PIPE_1;
		}
		if (cmd.source_mix_mask &
				AUDPP_A2DP_PIPE_SOURCE_MIX_MASK)
			cmd.pipe_id |= SOURCE_PIPE_0;
	}
	return audpreproc_send_audreccmdqueue(&cmd, sizeof(cmd));
}

static int audaac_in_mem_config(struct audio_in *audio)
{
	struct audrec_cmd_arecmem_cfg cmd;
	uint16_t *data = (void *) audio->data;
	int n;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_MEM_CFG_CMD;
	cmd.audrec_up_pkt_intm_count = 1;
	cmd.audrec_ext_pkt_start_addr_msw = audio->phys >> 16;
	cmd.audrec_ext_pkt_start_addr_lsw = audio->phys;
	cmd.audrec_ext_pkt_buf_number = FRAME_NUM;
	MM_DBG("audio->phys = %x\n", audio->phys);
	/* prepare buffer pointers:
	 * 1536 bytes aac packet + 4 halfword header
	 */
	for (n = 0; n < FRAME_NUM; n++) {
		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
			audio->in[n].data = data + 4;
			data += (FRAME_SIZE/2);
			MM_DBG("0x%8x\n", (int)(audio->in[n].data - 8));
		} else  {
			audio->in[n].data = data + 12;
			data += ((AAC_FRAME_SIZE) / 2) + 12;
			MM_DBG("0x%8x\n", (int)(audio->in[n].data - 24));
		}
	}
	return audrec_send_audrecqueue(audio, &cmd, sizeof(cmd));
}

static int audaac_dsp_read_buffer(struct audio_in *audio, uint32_t read_cnt)
{
	struct up_audrec_packet_ext_ptr cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = UP_AUDREC_PACKET_EXT_PTR;
	cmd.audrec_up_curr_read_count_msw = read_cnt >> 16;
	cmd.audrec_up_curr_read_count_lsw = read_cnt;

	return audrec_send_bitstreamqueue(audio, &cmd, sizeof(cmd));
}
static int audaac_flush_command(struct audio_in *audio)
{
	struct audrec_cmd_flush cmd;
	MM_DBG("\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_FLUSH;
	return audrec_send_audrecqueue(audio, &cmd, sizeof(cmd));
}

/* must be called with audio->lock held */
static int audaac_in_enable(struct audio_in *audio)
{
	if (audio->enabled)
		return 0;

	if (audpreproc_enable(audio->enc_id, &audpreproc_dsp_event, audio)) {
		MM_ERR("msm_adsp_enable(audpreproc) failed\n");
		return -ENODEV;
	}

	if (msm_adsp_enable(audio->audrec)) {
		MM_ERR("msm_adsp_enable(audrec) failed\n");
		audpreproc_disable(audio->enc_id, audio);
		return -ENODEV;
	}
	audio->enabled = 1;
	audaac_in_enc_config(audio, 1);

	return 0;
}

/* must be called with audio->lock held */
static int audaac_in_disable(struct audio_in *audio)
{
	if (audio->enabled) {
		audio->enabled = 0;
		audaac_in_enc_config(audio, 0);
		wake_up(&audio->wait);
		wait_event_interruptible_timeout(audio->wait_enable,
				audio->running == 0, 1*HZ);
		msm_adsp_disable(audio->audrec);
		audpreproc_disable(audio->enc_id, audio);
	}
	return 0;
}

static void audaac_ioport_reset(struct audio_in *audio)
{
	/* Make sure read/write thread are free from
	 * sleep and knowing that system is not able
	 * to process io request at the moment
	 */
	wake_up(&audio->write_wait);
	mutex_lock(&audio->write_lock);
	audaac_in_flush(audio);
	mutex_unlock(&audio->write_lock);
	wake_up(&audio->wait);
	mutex_lock(&audio->read_lock);
	audaac_out_flush(audio);
	mutex_unlock(&audio->read_lock);
}

static void audaac_in_flush(struct audio_in *audio)
{
	int i;

	audio->dsp_cnt = 0;
	audio->in_head = 0;
	audio->in_tail = 0;
	audio->in_count = 0;
	audio->eos_ack = 0;
	for (i = 0; i < FRAME_NUM; i++) {
		audio->in[i].size = 0;
		audio->in[i].read = 0;
	}
	MM_DBG("in_bytes %d\n", atomic_read(&audio->in_bytes));
	MM_DBG("in_samples %d\n", atomic_read(&audio->in_samples));
	atomic_set(&audio->in_bytes, 0);
	atomic_set(&audio->in_samples, 0);
}

static void audaac_out_flush(struct audio_in *audio)
{
	int i;

	audio->out_head = 0;
	audio->out_tail = 0;
	audio->out_count = 0;
	for (i = 0; i < OUT_FRAME_NUM; i++) {
		audio->out[i].size = 0;
		audio->out[i].read = 0;
		audio->out[i].used = 0;
	}
}

/* ------------------- device --------------------- */
static long audaac_in_ioctl(struct file *file,
				unsigned int cmd, unsigned long arg)
{
	struct audio_in *audio = file->private_data;
	int rc = 0;

	if (cmd == AUDIO_GET_STATS) {
		struct msm_audio_stats stats;
		memset(&stats, 0, sizeof(stats));
		stats.byte_count = atomic_read(&audio->in_bytes);
		stats.sample_count = atomic_read(&audio->in_samples);
		if (copy_to_user((void *) arg, &stats, sizeof(stats)))
			return -EFAULT;
		return rc;
	}

	mutex_lock(&audio->lock);
	switch (cmd) {
	case AUDIO_START: {
		uint32_t freq;
		/* Poll at 48KHz always */
		freq = 48000;
		MM_DBG("AUDIO_START\n");
		rc = msm_snddev_request_freq(&freq, audio->enc_id,
					SNDDEV_CAP_TX, AUDDEV_CLNT_ENC);
		MM_DBG("sample rate configured %d sample rate requested %d\n",
				freq, audio->samp_rate);
		if (rc < 0) {
			MM_DBG(" Sample rate can not be set, return code %d\n",
								 rc);
			msm_snddev_withdraw_freq(audio->enc_id,
					SNDDEV_CAP_TX, AUDDEV_CLNT_ENC);
			MM_DBG("msm_snddev_withdraw_freq\n");
			break;
		}
		/*update aurec session info in audpreproc layer*/
		audio->session_info.session_id = audio->enc_id;
		audio->session_info.sampling_freq = audio->samp_rate;
		audpreproc_update_audrec_info(&audio->session_info);
		rc = audaac_in_enable(audio);
		if (!rc) {
			rc =
			wait_event_interruptible_timeout(audio->wait_enable,
				audio->running != 0, 1*HZ);
			MM_DBG("state %d rc = %d\n", audio->running, rc);

			if (audio->running == 0)
				rc = -ENODEV;
			else
				rc = 0;
		}
		audio->stopped = 0;
		break;
	}
	case AUDIO_STOP: {
		audio->session_info.sampling_freq = 0;
		audpreproc_update_audrec_info(&audio->session_info);
		rc = audaac_in_disable(audio);
		rc = msm_snddev_withdraw_freq(audio->enc_id,
					SNDDEV_CAP_TX, AUDDEV_CLNT_ENC);
		MM_DBG("msm_snddev_withdraw_freq\n");
		audio->stopped = 1;
		audio->abort = 0;
		break;
	}
	case AUDIO_FLUSH:
		MM_DBG("AUDIO_FLUSH\n");
		audio->rflush = 1;
		audio->wflush = 1;
		audaac_ioport_reset(audio);
		if (audio->running) {
			audaac_flush_command(audio);
			rc = wait_event_interruptible(audio->write_wait,
				!audio->wflush);
			if (rc < 0) {
				MM_ERR("AUDIO_FLUSH interrupted\n");
				rc = -EINTR;
			}
		} else {
			audio->rflush = 0;
			audio->wflush = 0;
		}
		break;
	case AUDIO_GET_STREAM_CONFIG: {
		struct msm_audio_stream_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.buffer_size = audio->buffer_size;
		cfg.buffer_count = FRAME_NUM;
		if (copy_to_user((void *)arg, &cfg, sizeof(cfg)))
			rc = -EFAULT;
		break;
	}
	case AUDIO_SET_STREAM_CONFIG: {
		struct msm_audio_stream_config cfg;
		if (copy_from_user(&cfg, (void *)arg, sizeof(cfg))) {
			rc = -EFAULT;
			break;
		}
		/* Allow only single frame */
		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
			if (cfg.buffer_size != (FRAME_SIZE - 8)) {
				rc = -EINVAL;
				break;
			}
		} else {
			if (cfg.buffer_size != (NT_FRAME_SIZE - 24)) {
				rc = -EINVAL;
				break;
			}
		}
		audio->buffer_size = cfg.buffer_size;
		break;
	}
	case AUDIO_GET_CONFIG: {
		struct msm_audio_pcm_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.buffer_size = OUT_BUFFER_SIZE;
		cfg.buffer_count = OUT_FRAME_NUM;
		if (copy_to_user((void *)arg, &cfg, sizeof(cfg)))
			rc = -EFAULT;
		break;
	}
	case AUDIO_GET_AAC_ENC_CONFIG: {
		struct msm_audio_aac_enc_config cfg;
		if (audio->channel_mode == AUDREC_CMD_MODE_MONO)
			cfg.channels = 1;
		else
			cfg.channels = 2;
		cfg.sample_rate = audio->samp_rate;
		cfg.bit_rate = audio->bit_rate;
		cfg.stream_format = AUDIO_AAC_FORMAT_RAW;
		if (copy_to_user((void *)arg, &cfg, sizeof(cfg)))
			rc = -EFAULT;
		break;
	}
	case AUDIO_SET_AAC_ENC_CONFIG: {
		struct msm_audio_aac_enc_config cfg;
		unsigned int record_quality;
		if (copy_from_user(&cfg, (void *)arg, sizeof(cfg))) {
			rc = -EFAULT;
			break;
		}
		if (cfg.stream_format != AUDIO_AAC_FORMAT_RAW) {
			MM_ERR("unsupported AAC format\n");
			rc = -EINVAL;
			break;
		}
		record_quality = bitrate_to_record_quality(cfg.sample_rate,
					cfg.channels, cfg.bit_rate);
		/* Range of Record Quality Supported by DSP, Q12 format */
		if ((record_quality < 0x800) || (record_quality > 0x4000)) {
			MM_ERR("Unsupported bit rate \n");
			rc = -EINVAL;
			break;
		}
		MM_DBG("channels = %d\n", cfg.channels);
		if (cfg.channels == 1) {
			cfg.channels = AUDREC_CMD_MODE_MONO;
		} else if (cfg.channels == 2) {
			cfg.channels = AUDREC_CMD_MODE_STEREO;
		} else {
			rc = -EINVAL;
			break;
		}
		MM_DBG("channels = %d\n", cfg.channels);
		audio->samp_rate = cfg.sample_rate;
		audio->channel_mode = cfg.channels;
		audio->bit_rate = cfg.bit_rate;
		audio->record_quality = record_quality;
		MM_DBG(" Record Quality = 0x%8x \n", audio->record_quality);
		break;
	}
	case AUDIO_GET_SESSION_ID: {
		if (copy_to_user((void *) arg, &audio->enc_id,
			sizeof(unsigned short))) {
			rc = -EFAULT;
		}
		break;
	}
	default:
		rc = -EINVAL;
	}
	mutex_unlock(&audio->lock);
	return rc;
}

static ssize_t audaac_in_read(struct file *file,
				char __user *buf,
				size_t count, loff_t *pos)
{
	struct audio_in *audio = file->private_data;
	unsigned long flags;
	const char __user *start = buf;
	void *data;
	uint32_t index;
	uint32_t size;
	int rc = 0;
	struct aac_encoded_meta_in meta_field;
	struct audio_frame_nt *nt_frame;
	MM_DBG(" count = %d\n", count);
	mutex_lock(&audio->read_lock);
	while (count > 0) {
		rc = wait_event_interruptible(
			audio->wait, (audio->in_count > 0) || audio->stopped ||
				audio->abort || audio->rflush);

		if (rc < 0)
			break;

		if (audio->rflush) {
			rc = -EBUSY;
			break;
		}

		if (audio->stopped && !audio->in_count) {
			MM_DBG("Driver in stop state, No more buffer to read");
			rc = 0;/* End of File */
			break;
		}

		if (audio->abort) {
			rc = -EPERM; /* Not permitted due to abort */
			break;
		}

		index = audio->in_tail;
		data = (uint8_t *) audio->in[index].data;
		size = audio->in[index].size;

		if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL) {
			nt_frame = (struct audio_frame_nt *)(data -
					sizeof(struct audio_frame_nt));
			memcpy((char *)&meta_field.time_stamp_dword_lsw,
				(char *)&nt_frame->time_stamp_dword_lsw, 12);
			meta_field.metadata_len =
					sizeof(struct aac_encoded_meta_in);
			if (copy_to_user((char *)start, (char *)&meta_field,
					sizeof(struct aac_encoded_meta_in))) {
				rc = -EFAULT;
				break;
			}
			if (nt_frame->nflag_lsw & 0x0001) {
				MM_ERR("recieved EOS in read call\n");
				audio->eos_ack = 1;
			}
			buf += sizeof(struct aac_encoded_meta_in);
			count -= sizeof(struct aac_encoded_meta_in);
		}
		if (count >= size) {
			if (copy_to_user(buf, data, size)) {
				rc = -EFAULT;
				break;
			}
			spin_lock_irqsave(&audio->dsp_lock, flags);
			if (index != audio->in_tail) {
				/* overrun -- data is
				 * invalid and we need to retry */
				spin_unlock_irqrestore(&audio->dsp_lock, flags);
				continue;
			}
			audio->in[index].size = 0;
			audio->in_tail = (audio->in_tail + 1) & (FRAME_NUM - 1);
			audio->in_count--;
			spin_unlock_irqrestore(&audio->dsp_lock, flags);
			count -= size;
			buf += size;
			if ((audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL) &&
						(!audio->eos_ack)) {
				MM_DBG("sending read ptr command %d %d\n",
							audio->dsp_cnt,
							audio->in_tail);
				audaac_dsp_read_buffer(audio,
							audio->dsp_cnt++);
				break;
			}
		} else {
			MM_ERR("short read\n");
			break;
		}
		break;
	}
	mutex_unlock(&audio->read_lock);
	if (buf > start)
		return buf - start;

	return rc;
}

static void audpreproc_pcm_send_data(struct audio_in *audio, unsigned needed)
{
	struct buffer *frame;
	unsigned long flags;
	MM_DBG("\n");
	spin_lock_irqsave(&audio->dsp_lock, flags);
	if (!audio->running)
		goto done;

	if (needed && !audio->wflush) {
		/* We were called from the callback because the DSP
		 * requested more data.  Note that the DSP does want
		 * more data, and if a buffer was in-flight, mark it
		 * as available (since the DSP must now be done with
		 * it).
		 */
		audio->out_needed = 1;
		frame = audio->out + audio->out_tail;
		if (frame->used == 0xffffffff) {
			MM_DBG("frame %d free\n", audio->out_tail);
			frame->used = 0;
			audio->out_tail ^= 1;
			wake_up(&audio->write_wait);
		}
	}

	if (audio->out_needed) {
		/* If the DSP currently wants data and we have a
		 * buffer available, we will send it and reset
		 * the needed flag.  We'll mark the buffer as in-flight
		 * so that it won't be recycled until the next buffer
		 * is requested
		 */

		frame = audio->out + audio->out_tail;
		if (frame->used) {
			BUG_ON(frame->used == 0xffffffff);
			MM_DBG("frame %d busy\n", audio->out_tail);
			audpreproc_pcm_buffer_ptr_refresh(audio,
						 audio->out_tail,
						    frame->used);
			frame->used = 0xffffffff;
			audio->out_needed = 0;
		}
	}
 done:
	spin_unlock_irqrestore(&audio->dsp_lock, flags);
}


static int audaac_in_fsync(struct file *file, loff_t ppos1, loff_t ppos2, int datasync)

{
	struct audio_in *audio = file->private_data;
	int rc = 0;

	MM_DBG("\n"); /* Macro prints the file name and function */
	if (!audio->running || (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)) {
		rc = -EINVAL;
		goto done_nolock;
	}

	mutex_lock(&audio->write_lock);

	rc = wait_event_interruptible(audio->write_wait,
			audio->wflush);
	MM_DBG("waked on by some event audio->wflush = %d\n", audio->wflush);

	if (rc < 0)
		goto done;
	else if (audio->wflush) {
		rc = -EBUSY;
		goto done;
	}
done:
	mutex_unlock(&audio->write_lock);
done_nolock:
	return rc;

}

 int audpreproc_aac_process_eos(struct audio_in *audio,
		const char __user *buf_start, unsigned short mfield_size)
{
	struct buffer *frame;
	int rc = 0;

	frame = audio->out + audio->out_head;

	rc = wait_event_interruptible(audio->write_wait,
		(audio->out_needed &&
		audio->out[0].used == 0 &&
		audio->out[1].used == 0)
		|| (audio->stopped)
		|| (audio->wflush));

	if (rc < 0)
		goto done;
	if (audio->stopped || audio->wflush) {
		rc = -EBUSY;
		goto done;
	}
	if (copy_from_user(frame->data, buf_start, mfield_size)) {
		rc = -EFAULT;
		goto done;
	}

	frame->mfield_sz = mfield_size;
	audio->out_head ^= 1;
	frame->used = mfield_size;
	MM_DBG("copying meta_out frame->used = %d\n", frame->used);
	audpreproc_pcm_send_data(audio, 0);
done:
	return rc;
}

static ssize_t audaac_in_write(struct file *file,
				const char __user *buf,
				size_t count, loff_t *pos)
{
	struct audio_in *audio = file->private_data;
	const char __user *start = buf;
	struct buffer *frame;
	char *cpy_ptr;
	int rc = 0, eos_condition = AUDPREPROC_AAC_EOS_NONE;
	unsigned short mfield_size = 0;
	int write_count = count;
	MM_DBG("cnt=%d\n", count);

	if (count & 1)
		return -EINVAL;

	mutex_lock(&audio->write_lock);
	frame = audio->out + audio->out_head;
	cpy_ptr = frame->data;
	rc = wait_event_interruptible(audio->write_wait,
				      (frame->used == 0)
					|| (audio->stopped)
					|| (audio->wflush));
	if (rc < 0)
		goto error;

	if (audio->stopped || audio->wflush) {
		rc = -EBUSY;
		goto error;
	}
	if (audio->mfield) {
		if (buf == start) {
			/* Processing beginning of user buffer */
			if (__get_user(mfield_size,
				(unsigned short __user *) buf)) {
				rc = -EFAULT;
				goto error;
			} else if (mfield_size > count) {
				rc = -EINVAL;
				goto error;
			}
			MM_DBG("mf offset_val %x\n", mfield_size);
			if (copy_from_user(cpy_ptr, buf, mfield_size)) {
				rc = -EFAULT;
				goto error;
			}
			/* Check if EOS flag is set and buffer has
			 * contains just meta field
			 */
			if (cpy_ptr[AUDPREPROC_AAC_EOS_FLG_OFFSET] &
					AUDPREPROC_AAC_EOS_FLG_MASK) {
				MM_DBG("EOS SET\n");
				eos_condition = AUDPREPROC_AAC_EOS_SET;
				if (mfield_size == count) {
					buf += mfield_size;
					if (audio->mode ==
						MSM_AUD_ENC_MODE_NONTUNNEL) {
						eos_condition = 0;
						goto exit;
					}
					goto error;
				} else
				cpy_ptr[AUDPREPROC_AAC_EOS_FLG_OFFSET] &=
					~AUDPREPROC_AAC_EOS_FLG_MASK;
			}
			cpy_ptr += mfield_size;
			count -= mfield_size;
			buf += mfield_size;
		} else {
			mfield_size = 0;
			MM_DBG("continuous buffer\n");
		}
		frame->mfield_sz = mfield_size;
	}
	MM_DBG("copying the stream count = %d\n", count);
	if (copy_from_user(cpy_ptr, buf, count)) {
		rc = -EFAULT;
		goto error;
	}
exit:
	frame->used = count;
	audio->out_head ^= 1;
	if (!audio->flush_ack)
		audpreproc_pcm_send_data(audio, 0);
	else {
		audpreproc_pcm_send_data(audio, 1);
		audio->flush_ack = 0;
	}
	if (eos_condition == AUDPREPROC_AAC_EOS_SET)
		rc = audpreproc_aac_process_eos(audio, start, mfield_size);
	mutex_unlock(&audio->write_lock);
	return write_count;
error:
	mutex_unlock(&audio->write_lock);
	return rc;
}

static int audaac_in_release(struct inode *inode, struct file *file)
{
	struct audio_in *audio = file->private_data;

	mutex_lock(&audio->lock);
	/* with draw frequency for session
	   incase not stopped the driver */
	msm_snddev_withdraw_freq(audio->enc_id, SNDDEV_CAP_TX,
					AUDDEV_CLNT_ENC);
	auddev_unregister_evt_listner(AUDDEV_CLNT_ENC, audio->enc_id);
	/*reset the sampling frequency information at audpreproc layer*/
	audio->session_info.sampling_freq = 0;
	audpreproc_update_audrec_info(&audio->session_info);
	audaac_in_disable(audio);
	audaac_in_flush(audio);
	msm_adsp_put(audio->audrec);
	audpreproc_aenc_free(audio->enc_id);
	audio->audrec = NULL;
	audio->opened = 0;
	if (audio->data) {
		iounmap(audio->map_v_read);
		free_contiguous_memory_by_paddr(audio->phys);
		audio->data = NULL;
	}
	if (audio->out_data) {
		iounmap(audio->map_v_write);
		free_contiguous_memory_by_paddr(audio->out_phys);
		audio->out_data = NULL;
	}
	mutex_unlock(&audio->lock);
	return 0;
}

struct audio_in the_audio_aac_in;

static int audaac_in_open(struct inode *inode, struct file *file)
{
	struct audio_in *audio = &the_audio_aac_in;
	int rc;
	int encid;

	mutex_lock(&audio->lock);
	if (audio->opened) {
		rc = -EBUSY;
		goto done;
	}
	audio->phys = allocate_contiguous_ebi_nomap(DMASZ, SZ_4K);
	if (audio->phys) {
		audio->map_v_read = ioremap(audio->phys, DMASZ);
		if (IS_ERR(audio->map_v_read)) {
			MM_ERR("could not map DMA buffers\n");
			rc = -ENOMEM;
			free_contiguous_memory_by_paddr(audio->phys);
			goto done;
		}
		audio->data = audio->map_v_read;
	} else {
		MM_ERR("could not allocate DMA buffers\n");
		rc = -ENOMEM;
		goto done;
	}
	MM_DBG("Memory addr = 0x%8x  phy addr = 0x%8x\n",\
		(int) audio->data, (int) audio->phys);
	if ((file->f_mode & FMODE_WRITE) &&
				(file->f_mode & FMODE_READ)) {
		audio->mode = MSM_AUD_ENC_MODE_NONTUNNEL;
	} else if (!(file->f_mode & FMODE_WRITE) &&
					(file->f_mode & FMODE_READ)) {
		audio->mode = MSM_AUD_ENC_MODE_TUNNEL;
		MM_DBG("Opened for tunnel mode encoding\n");
	} else {
		rc = -EACCES;
		goto done;
	}

	/* Settings will be re-config at AUDIO_SET_CONFIG,
	 * but at least we need to have initial config
	 */
	 if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL)
			audio->buffer_size = (NT_FRAME_SIZE - 24);
	else
			audio->buffer_size = (FRAME_SIZE - 8);
	audio->enc_type = ENC_TYPE_AAC | audio->mode;
	audio->samp_rate = 8000;
	audio->channel_mode = AUDREC_CMD_MODE_MONO;
	/* For AAC, bit rate hard coded, default settings is
	 * sample rate (8000) x channel count (1) x recording quality (1.75)
	 * = 14000 bps  */
	audio->bit_rate = 14000;
	audio->record_quality = 0x1c00;
	MM_DBG("enc_type = %x\n", audio->enc_type);
	encid = audpreproc_aenc_alloc(audio->enc_type, &audio->module_name,
			&audio->queue_ids);
	if (encid < 0) {
		MM_ERR("No free encoder available\n");
		rc = -ENODEV;
		goto done;
	}
	audio->enc_id = encid;

	rc = msm_adsp_get(audio->module_name, &audio->audrec,
			   &audrec_aac_adsp_ops, audio);

	if (rc) {
		audpreproc_aenc_free(audio->enc_id);
		goto done;
	}

	audio->stopped = 0;
	audio->source = 0;
	audio->abort = 0;
	audio->wflush = 0;
	audio->rflush = 0;
	audio->flush_ack = 0;

	audaac_in_flush(audio);
	audaac_out_flush(audio);

	audio->out_phys = allocate_contiguous_ebi_nomap(BUFFER_SIZE, SZ_4K);
	if (!audio->out_phys) {
		MM_ERR("could not allocate write buffers\n");
		rc = -ENOMEM;
		goto evt_error;
	} else {
		audio->map_v_write = ioremap(
					audio->out_phys, BUFFER_SIZE);
		if (IS_ERR(audio->map_v_write)) {
			MM_ERR("could not map write phys address\n");
			rc = -ENOMEM;
			free_contiguous_memory_by_paddr(audio->out_phys);
			goto evt_error;
		}
		audio->out_data = audio->map_v_write;
		MM_DBG("write buf: phy addr 0x%08x kernel addr 0x%08x\n",
				audio->out_phys, (int)audio->out_data);
	}
	audio->build_id = socinfo_get_build_id();
	MM_DBG("Modem build id = %s\n", audio->build_id);

		/* Initialize buffer */
	audio->out[0].data = audio->out_data + 0;
	audio->out[0].addr = audio->out_phys + 0;
	audio->out[0].size = OUT_BUFFER_SIZE;

	audio->out[1].data = audio->out_data + OUT_BUFFER_SIZE;
	audio->out[1].addr = audio->out_phys + OUT_BUFFER_SIZE;
	audio->out[1].size = OUT_BUFFER_SIZE;

	MM_DBG("audio->out[0].data = %d  audio->out[1].data = %d",
					(unsigned int)audio->out[0].data,
					(unsigned int)audio->out[1].data);
	audio->device_events = AUDDEV_EVT_DEV_RDY | AUDDEV_EVT_DEV_RLS |
				AUDDEV_EVT_FREQ_CHG;

	rc = auddev_register_evt_listner(audio->device_events,
					AUDDEV_CLNT_ENC, audio->enc_id,
					aac_in_listener, (void *) audio);
	if (rc) {
		MM_ERR("failed to register device event listener\n");
		iounmap(audio->map_v_write);
		free_contiguous_memory_by_paddr(audio->out_phys);
		goto evt_error;
	}
	audio->mfield = META_OUT_SIZE;
	file->private_data = audio;
	audio->opened = 1;
	audio->out_frame_cnt++;
	rc = 0;
done:
	mutex_unlock(&audio->lock);
	return rc;
evt_error:
	msm_adsp_put(audio->audrec);
	audpreproc_aenc_free(audio->enc_id);
	mutex_unlock(&audio->lock);
	return rc;
}

static const struct file_operations audio_in_fops = {
	.owner		= THIS_MODULE,
	.open		= audaac_in_open,
	.release	= audaac_in_release,
	.read		= audaac_in_read,
	.write		= audaac_in_write,
	.fsync		= audaac_in_fsync,
	.unlocked_ioctl	= audaac_in_ioctl,
};

struct miscdevice audio_aac_in_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "msm_aac_in",
	.fops	= &audio_in_fops,
};

static int __init audaac_in_init(void)
{
	mutex_init(&the_audio_aac_in.lock);
	mutex_init(&the_audio_aac_in.read_lock);
	spin_lock_init(&the_audio_aac_in.dsp_lock);
	spin_lock_init(&the_audio_aac_in.dev_lock);
	init_waitqueue_head(&the_audio_aac_in.wait);
	init_waitqueue_head(&the_audio_aac_in.wait_enable);
	mutex_init(&the_audio_aac_in.write_lock);
	init_waitqueue_head(&the_audio_aac_in.write_wait);

	return misc_register(&audio_aac_in_misc);
}

device_initcall(audaac_in_init);
