/*
 * Support for Clovertrail PNW Camera Imaging ISP subsystem.
 *
 * Copyright (c) 2013 Intel Corporation. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <media/videobuf-vmalloc.h>
#include <media/v4l2-dev.h>

#include "mmu/isp_mmu.h"
#include "mmu/sh_mmu_mrfld.h"
#include "hmm/hmm_bo_dev.h"
#include "hmm/hmm.h"

#include "atomisp_compat.h"
#include "atomisp_internal.h"
#include "atomisp_cmd.h"
#include "atomisp-regs.h"
#include "atomisp_fops.h"
#include "atomisp_ioctl.h"

#include "hrt/hive_isp_css_mm_hrt.h"

#include <asm/intel-mid.h>

#ifdef CSS21
#include "ia_css_debug.h"
#include "ia_css_isp_param.h"
#else /* CSS21 */
#include "ia_css_accelerate.h"
#include "sh_css_debug.h"
#endif /* CSS21 */

#include <linux/pm_runtime.h>

/* Assume max number of ACC stages */
#define MAX_ACC_STAGES	20

/*
 * to serialize MMIO access , this is due to ISP2400 silicon issue Sighting
 * #4684168, if concurrency access happened, system may hard hang.
 */
static DEFINE_SPINLOCK(mmio_lock);
extern raw_spinlock_t pci_config_lock;

enum frame_info_type {
	ATOMISP_CSS_VF_FRAME,
	ATOMISP_CSS_OUTPUT_FRAME,
	ATOMISP_CSS_RAW_FRAME,
};

struct bayer_ds_factor {
	unsigned int numerator;
	unsigned int denominator;
};

#ifdef CSS21
void atomisp_css_debug_dump_sp_sw_debug_info(void)
{
	ia_css_debug_dump_sp_sw_debug_info();
}

void atomisp_css_debug_dump_debug_info(const char *context)
{
	ia_css_debug_dump_debug_info(context);
}

void atomisp_css_debug_set_dtrace_level(const unsigned int trace_level)
{
	ia_css_debug_set_dtrace_level(trace_level);
}
#else /* CSS21 */
void atomisp_css_debug_dump_sp_sw_debug_info(void)
{
	sh_css_dump_sp_sw_debug_info();
}

void atomisp_css_debug_dump_debug_info(const char *context)
{
	sh_css_dump_debug_info(context);
}

void atomisp_css_debug_set_dtrace_level(const unsigned int trace_level)
{
	sh_css_set_dtrace_level(trace_level);
}
#endif /* CSS21 */

static ia_css_ptr atomisp_css2_mm_alloc(size_t bytes, uint32_t attr)
{
	if (attr & IA_CSS_MEM_ATTR_CACHED)
		return (ia_css_ptr) hrt_isp_css_mm_calloc_cached(bytes);
	else if (attr & IA_CSS_MEM_ATTR_ZEROED)
		return (ia_css_ptr) hrt_isp_css_mm_calloc(bytes);
	else if (attr & IA_CSS_MEM_ATTR_CONTIGUOUS)
		return (ia_css_ptr) hrt_isp_css_mm_calloc_contiguous(bytes);
	else
		return (ia_css_ptr) hrt_isp_css_mm_calloc(bytes);
}

static void atomisp_css2_mm_free(ia_css_ptr ptr)
{
	hrt_isp_css_mm_free(ptr);
}

static int atomisp_css2_mm_load(ia_css_ptr ptr, void *data, size_t bytes)
{
	return hrt_isp_css_mm_load(ptr, data, bytes);
}

static int atomisp_css2_mm_store(ia_css_ptr ptr, const void *data, size_t bytes)
{
	return hrt_isp_css_mm_store(ptr, data, bytes);
}

static int atomisp_css2_mm_set(ia_css_ptr ptr, int c, size_t bytes)
{
	return hrt_isp_css_mm_set(ptr, c, bytes);
}

static ia_css_ptr atomisp_css2_mm_mmap(const void *ptr, const size_t size,
		   uint16_t attribute, void *context)
{
	struct hrt_userbuffer_attr *userbuffer_attr = context;
	return hrt_isp_css_mm_alloc_user_ptr(
			size, (void *)ptr, userbuffer_attr->pgnr,
			userbuffer_attr->type,
			attribute & HRT_BUF_FLAG_CACHED);
}

#ifdef CSS21
static void *atomisp_css21_hrt_vaddr_to_host_vaddr(ia_css_ptr ptr)
{
	return hmm_isp_vaddr_to_host_vaddr(ptr);
}

static ia_css_ptr atomisp_css21_host_vaddr_to_hrt_vaddr(const void *ptr)
{
	return hmm_host_vaddr_to_hrt_vaddr(ptr);
}
#endif

void atomisp_css2_hw_store_8(hrt_address addr, uint8_t data)
{
	unsigned long flags;

	spin_lock_irqsave(&mmio_lock, flags);
	raw_spin_lock(&pci_config_lock);
	_hrt_master_port_store_8(addr, data);
	raw_spin_unlock(&pci_config_lock);
	spin_unlock_irqrestore(&mmio_lock, flags);
}

static void atomisp_css2_hw_store_16(hrt_address addr, uint16_t data)
{
	unsigned long flags;

	spin_lock_irqsave(&mmio_lock, flags);
	raw_spin_lock(&pci_config_lock);
	_hrt_master_port_store_16(addr, data);
	raw_spin_unlock(&pci_config_lock);
	spin_unlock_irqrestore(&mmio_lock, flags);
}

static void atomisp_css2_hw_store_32(hrt_address addr, uint32_t data)
{
	unsigned long flags;

	spin_lock_irqsave(&mmio_lock, flags);
	raw_spin_lock(&pci_config_lock);
	_hrt_master_port_store_32(addr, data);
	raw_spin_unlock(&pci_config_lock);
	spin_unlock_irqrestore(&mmio_lock, flags);
}

static uint8_t atomisp_css2_hw_load_8(hrt_address addr)
{
	unsigned long flags;
	uint8_t ret;

	spin_lock_irqsave(&mmio_lock, flags);
	raw_spin_lock(&pci_config_lock);
	ret = _hrt_master_port_load_8(addr);
	raw_spin_unlock(&pci_config_lock);
	spin_unlock_irqrestore(&mmio_lock, flags);
	return ret;
}

uint16_t atomisp_css2_hw_load_16(hrt_address addr)
{
	unsigned long flags;
	uint16_t ret;

	spin_lock_irqsave(&mmio_lock, flags);
	raw_spin_lock(&pci_config_lock);
	ret = _hrt_master_port_load_16(addr);
	raw_spin_unlock(&pci_config_lock);
	spin_unlock_irqrestore(&mmio_lock, flags);
	return ret;
}
uint32_t atomisp_css2_hw_load_32(hrt_address addr)
{
	unsigned long flags;
	uint32_t ret;

	spin_lock_irqsave(&mmio_lock, flags);
	raw_spin_lock(&pci_config_lock);
	ret = _hrt_master_port_load_32(addr);
	raw_spin_unlock(&pci_config_lock);
	spin_unlock_irqrestore(&mmio_lock, flags);
	return ret;
}

static void atomisp_css2_hw_store(hrt_address addr,
				  const void *from, uint32_t n)
{
	unsigned long flags;
	unsigned i;
	unsigned int _to = (unsigned int)addr;
	const char *_from = (const char *)from;

	spin_lock_irqsave(&mmio_lock, flags);
	raw_spin_lock(&pci_config_lock);
	for (i = 0; i < n; i++, _to++, _from++)
		_hrt_master_port_store_8(_to , *_from);
	raw_spin_unlock(&pci_config_lock);
	spin_unlock_irqrestore(&mmio_lock, flags);
}

static void atomisp_css2_hw_load(hrt_address addr, void *to, uint32_t n)
{
	unsigned long flags;
	unsigned i;
	char *_to = (char *)to;
	unsigned int _from = (unsigned int)addr;

	spin_lock_irqsave(&mmio_lock, flags);
	raw_spin_lock(&pci_config_lock);
	for (i = 0; i < n; i++, _to++, _from++)
		*_to = _hrt_master_port_load_8(_from);
	raw_spin_unlock(&pci_config_lock);
	spin_unlock_irqrestore(&mmio_lock, flags);
}

static int atomisp_css2_dbg_print(const char *fmt, va_list args)
{
	if (dbg_level > 5)
		vprintk(fmt, args);
	return 0;
}

static int atomisp_css2_err_print(const char *fmt, va_list args)
{
	vprintk(fmt, args);
	return 0;
}

void atomisp_store_uint32(hrt_address addr, uint32_t data)
{
	atomisp_css2_hw_store_32(addr, data);
}

void atomisp_load_uint32(hrt_address addr, uint32_t *data)
{
	*data = atomisp_css2_hw_load_32(addr);
}
static int hmm_get_mmu_base_addr(unsigned int *mmu_base_addr)
{
	if (sh_mmu_mrfld.get_pd_base == NULL) {
		dev_err(atomisp_dev, "get mmu base address failed.\n");
		return -EINVAL;
	}

	*mmu_base_addr = sh_mmu_mrfld.get_pd_base(&bo_device.mmu,
					bo_device.mmu.base_address);
	return 0;
}

static void atomisp_isp_parameters_clean_up(
				struct atomisp_css_isp_config *config)
{
	if (config->morph_table)
		ia_css_morph_table_free(config->morph_table);

	/*
	 * Set NULL to configs pointer to avoid they are set into isp again when
	 * some configs are changed and need to be updated later.
	 */
	memset(config, 0, sizeof(*config));
}

static void __dump_pipe_config(struct atomisp_sub_device *asd,
			       struct atomisp_stream_env *stream_env,
			       unsigned int pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	if (stream_env->pipes[pipe_id]) {
		struct ia_css_pipe_config *p_config;
		struct ia_css_pipe_extra_config *pe_config;
		p_config = &stream_env->pipe_configs[pipe_id];
		pe_config = &stream_env->pipe_extra_configs[pipe_id];
		dev_dbg(isp->dev, "dumping pipe[%d] config:\n", pipe_id);
		dev_dbg(isp->dev,
			 "pipe_config.pipe_mode:%d.\n", p_config->mode);
		dev_dbg(isp->dev,
			 "pipe_config.output_info w=%d, h=%d.\n",
			 p_config->output_info.res.width,
			 p_config->output_info.res.height);
#ifndef CSS21
		dev_dbg(isp->dev,
			 "pipe_config.bin_out w=%d, h=%d.\n",
			 p_config->bin_out_res.width,
			 p_config->bin_out_res.height);
#else
		dev_dbg(isp->dev,
			 "pipe_config.vf_pp_in_res w=%d, h=%d.\n",
			 p_config->vf_pp_in_res.width,
			 p_config->vf_pp_in_res.height);
		dev_dbg(isp->dev,
			 "pipe_config.capt_pp_in_res w=%d, h=%d.\n",
			 p_config->capt_pp_in_res.width,
			 p_config->capt_pp_in_res.height);
#endif /* CSS21 */
		dev_dbg(isp->dev,
			 "pipe_config.output.padded w=%d.\n",
			 p_config->output_info.padded_width);
		dev_dbg(isp->dev,
			 "pipe_config.vf_output_info w=%d, h=%d.\n",
			 p_config->vf_output_info.res.width,
			 p_config->vf_output_info.res.height);
		dev_dbg(isp->dev,
			 "pipe_config.bayer_ds_out_res w=%d, h=%d.\n",
			 p_config->bayer_ds_out_res.width,
			 p_config->bayer_ds_out_res.height);
		dev_dbg(isp->dev,
			 "pipe_config.envelope w=%d, h=%d.\n",
			 p_config->dvs_envelope.width,
			 p_config->dvs_envelope.height);
		dev_dbg(isp->dev,
			 "pipe_config.isp_pipe_version:%d.\n",
			p_config->isp_pipe_version);
		dev_dbg(isp->dev,
			 "pipe_config.default_capture_config.capture_mode=%d.\n",
			 p_config->default_capture_config.mode);
		dev_dbg(isp->dev,
#ifndef CSS21
			 "pipe_config.default_capture_config.enable_capture_pp=%d.\n",
			 p_config->default_capture_config.enable_capture_pp);
#else
			 "pipe_config.enable_dz=%d.\n",
			 p_config->enable_dz);
#endif
		dev_dbg(isp->dev,
			 "pipe_config.default_capture_config.enable_xnr=%d.\n",
			 p_config->default_capture_config.enable_xnr);
		dev_dbg(isp->dev,
			 "dumping pipe[%d] extra config:\n", pipe_id);
		dev_dbg(isp->dev,
			 "pipe_extra_config.enable_raw_binning:%d.\n",
			 pe_config->enable_raw_binning);
		dev_dbg(isp->dev,
			 "pipe_extra_config.enable_yuv_ds:%d.\n",
			 pe_config->enable_yuv_ds);
		dev_dbg(isp->dev,
			 "pipe_extra_config.enable_high_speed:%d.\n",
			 pe_config->enable_high_speed);
		dev_dbg(isp->dev,
			 "pipe_extra_config.enable_dvs_6axis:%d.\n",
			 pe_config->enable_dvs_6axis);
		dev_dbg(isp->dev,
			 "pipe_extra_config.enable_reduced_pipe:%d.\n",
			 pe_config->enable_reduced_pipe);
		dev_dbg(isp->dev,
			 "pipe_(extra_)config.enable_dz:%d.\n",
#ifdef CSS21
			 p_config
#else
			 pe_config
#endif
			 ->enable_dz);
		dev_dbg(isp->dev,
			 "pipe_extra_config.disable_vf_pp:%d.\n",
			 pe_config->disable_vf_pp);
#ifndef CSS21
		dev_dbg(isp->dev,
			 "pipe_extra_config.disable_capture_pp:%d.\n",
			 pe_config->disable_capture_pp);
#endif
	}
}

static void __dump_stream_config(struct atomisp_sub_device *asd)
{
	struct atomisp_device *isp = asd->isp;
	struct ia_css_stream_config *s_config;
	int i, j;
	bool valid_stream;

	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		valid_stream = false;
		for (j = 0; j < IA_CSS_PIPE_ID_NUM; j++) {
			if (asd->stream_env[i].pipes[j]) {
				__dump_pipe_config(asd, &asd->stream_env[i], j);
				valid_stream = true;
			}
		}
		if (!valid_stream)
			continue;
		s_config = &asd->stream_env[i].stream_config;
		dev_dbg(isp->dev,
			"dumping stream config[%d]:\n", i);
		dev_dbg(isp->dev,
			"stream_config.mode=%d.\n",
			s_config->mode);
		dev_dbg(isp->dev,
			"stream_config.input_res w=%d, h=%d.\n",
			s_config->input_res.width,
			s_config->input_res.height);
		dev_dbg(isp->dev,
			"stream_config.effective_res w=%d, h=%d.\n",
			s_config->effective_res.width,
			s_config->effective_res.height);
		dev_dbg(isp->dev,
			"stream_config.format=%d.\n",
			s_config->format);
		dev_dbg(isp->dev,
			"stream_config.bayer_order=%d.\n",
			s_config->bayer_order);
		dev_dbg(isp->dev,
			"stream_config.2ppc=%d.\n",
			s_config->two_pixels_per_clock);
		dev_dbg(isp->dev,
			"stream_config.online=%d.\n",
			s_config->online);
		dev_dbg(isp->dev,
			"stream_config.continuous=%d.\n",
			s_config->continuous);
		dev_dbg(isp->dev,
			"stream_config.channel_id=%d.\n",
			s_config->channel_id);
#ifdef CSS21
		dev_dbg(isp->dev,
			"stream_config.init_num_cont_raw_buf=%d.\n",
			s_config->init_num_cont_raw_buf);
		dev_dbg(isp->dev,
			"stream_config.target_num_cont_raw_buf=%d.\n",
			s_config->target_num_cont_raw_buf);
		dev_dbg(isp->dev,
			"stream_config.left_padding=%d.\n",
			s_config->left_padding);
#endif
	}
}

static int __destroy_stream(struct atomisp_sub_device *asd,
			struct atomisp_stream_env *stream_env, bool force)
{
	struct atomisp_device *isp = asd->isp;
	int i;

	if (!stream_env->stream)
		return 0;

	if (!force) {
		for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++)
			if (stream_env->update_pipe[i])
				break;

		if (i == IA_CSS_PIPE_ID_NUM)
			return 0;
	}

	if (stream_env->stream_state == CSS_STREAM_STARTED
	    && ia_css_stream_stop(stream_env->stream) != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "stop stream failed.\n");
		return -EINVAL;
	}
	stream_env->stream_state = CSS_STREAM_STOPPED;

	if (ia_css_stream_destroy(stream_env->stream) != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "destroy stream failed.\n");
		return -EINVAL;
	}
	stream_env->stream_state = CSS_STREAM_UNINIT;
	stream_env->stream = NULL;

	return 0;
}

static int __destroy_streams(struct atomisp_sub_device *asd, bool force)
{
	int ret, i;
	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		ret = __destroy_stream(asd, &asd->stream_env[i], force);
		if (ret)
			return ret;
	}
	return 0;
}
static int __create_stream(struct atomisp_sub_device *asd,
			   struct atomisp_stream_env *stream_env)
{
	int pipe_index = 0, i;
	struct ia_css_pipe *multi_pipes[IA_CSS_PIPE_ID_NUM];

	for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++) {
		if (stream_env->pipes[i])
			multi_pipes[pipe_index++] = stream_env->pipes[i];
	}
	if (pipe_index == 0)
		return 0;

#ifdef CSS21
	stream_env->stream_config.target_num_cont_raw_buf =
		asd->continuous_raw_buffer_size->val;
#endif /* CSS21 */
	stream_env->stream_config.channel_id = stream_env->ch_id;
	if (ia_css_stream_create(&stream_env->stream_config,
	    pipe_index, multi_pipes, &stream_env->stream) != IA_CSS_SUCCESS)
		return -EINVAL;
	stream_env->stream_state = CSS_STREAM_CREATED;
	return 0;
}

static int __create_streams(struct atomisp_sub_device *asd)
{
	int ret, i;
	__dump_stream_config(asd);
	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		ret = __create_stream(asd, &asd->stream_env[i]);
		if (ret)
			goto rollback;
	}
	return 0;
rollback:
	for (i--; i >= 0; i--)
		__destroy_stream(asd, &asd->stream_env[i], true);
	return ret;
}

static int __destroy_stream_pipes(struct atomisp_sub_device *asd,
				  struct atomisp_stream_env *stream_env,
				  bool force)
{
	struct atomisp_device *isp = asd->isp;
	int ret, i;
	for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++) {
		if (!stream_env->pipes[i] ||
		    !(force || stream_env->update_pipe[i]))
			continue;
		if (ia_css_pipe_destroy(stream_env->pipes[i])
		    != IA_CSS_SUCCESS) {
			dev_err(isp->dev,
				"destroy pipe[%d]failed.cannot recover.\n", i);
			ret = -EINVAL;
		}
		stream_env->pipes[i] = NULL;
		stream_env->update_pipe[i] = false;
	}
	return 0;
}

static int __destroy_pipes(struct atomisp_sub_device *asd, bool force)
{
	struct atomisp_device *isp = asd->isp;
	int i;
	int ret = 0;

	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		if (asd->stream_env[i].stream) {

			dev_err(isp->dev,
				"cannot destroy css pipes for stream[%d].\n",
				i);
			continue;
		}

		ret = __destroy_stream_pipes(asd, &asd->stream_env[i], force);
		if (ret)
			return ret;
	}

	return 0;
}

void atomisp_destroy_pipes_stream_force(struct atomisp_sub_device *asd)
{
	__destroy_streams(asd, true);
	__destroy_pipes(asd, true);
}

static void __apply_additional_pipe_config(
				struct atomisp_sub_device *asd,
				struct atomisp_stream_env *stream_env,
				enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;

	if (pipe_id < 0 || pipe_id >= IA_CSS_PIPE_ID_NUM) {
		dev_err(isp->dev,
			 "wrong pipe_id for additional pipe config.\n");
		return;
	}

	/* apply default pipe config */
	stream_env->pipe_configs[pipe_id].isp_pipe_version = 2;
	/* apply isp 2.2 specific config for baytrail*/
	switch (pipe_id) {
	case IA_CSS_PIPE_ID_CAPTURE:
		/* enable capture pp/dz manually or digital zoom would
		 * fail*/
		if (stream_env->pipe_configs[pipe_id].
			default_capture_config.mode != CSS_CAPTURE_MODE_RAW)
			stream_env->pipe_configs[pipe_id]
#ifndef CSS21
			    .default_capture_config.enable_capture_pp = true;
#else
			    .enable_dz = true;
#endif
		/*
		 * FIXME!
		 * For ISP2401 legacy input system, online still image pipe
		 * would cause a watchdog timeout.
		 * With pipe config, dz=0, image capture could be success.
		 *
		 * VIED BZ 1369 on tracking this.
		 */
		if (asd->isp->media_dev.hw_revision ==
		    ATOMISP_HW_REVISION_ISP2401_LEGACY << ATOMISP_HW_REVISION_SHIFT) {
#ifdef CSS21
			stream_env->pipe_configs[pipe_id].enable_dz = false;
#endif
			dev_dbg(isp->dev,
				"pipe config enable_dz is overrided for ISP2401 legacy.\n");
		}
		break;
	case IA_CSS_PIPE_ID_VIDEO:
		/* enable reduced pipe to have binary
		 * video_dz_2_min selected*/
		stream_env->pipe_extra_configs[pipe_id]
		    .enable_reduced_pipe = true;
#ifdef CSS21
		stream_env->pipe_configs[pipe_id]
#else
		stream_env->pipe_extra_configs[pipe_id]
#endif
		    .enable_dz = false;
#ifdef CSS20
		if (asd->params.video_dis_en) {
			stream_env->pipe_extra_configs[pipe_id]
				.enable_dvs_6axis = true;
			stream_env->pipe_configs[pipe_id]
				.dvs_frame_delay = 2;
		}
#endif
		break;
	case IA_CSS_PIPE_ID_PREVIEW:
	case IA_CSS_PIPE_ID_COPY:
	case IA_CSS_PIPE_ID_ACC:
		break;
	default:
		break;
	}
}

static int __create_pipe(struct atomisp_sub_device *asd,
			 struct atomisp_stream_env *stream_env,
			 enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	struct ia_css_pipe_extra_config extra_config;
	enum ia_css_err ret;
	if (!stream_env->pipe_configs[pipe_id].output_info.res.width)
		return 0;

	ia_css_pipe_extra_config_defaults(&extra_config);

	__apply_additional_pipe_config(asd, stream_env, pipe_id);
	if (!memcmp(&extra_config,
		    &stream_env->pipe_extra_configs[pipe_id],
		    sizeof(extra_config)))
		ret = ia_css_pipe_create(
			&stream_env->pipe_configs[pipe_id],
			&stream_env->pipes[pipe_id]);
	else
		ret = ia_css_pipe_create_extra(
			&stream_env->pipe_configs[pipe_id],
			&stream_env->pipe_extra_configs[pipe_id],
			&stream_env->pipes[pipe_id]);
	if (ret != IA_CSS_SUCCESS)
		dev_err(isp->dev, "create pipe[%d] error.\n", pipe_id);
	return ret;
}

static int __create_pipes(struct atomisp_sub_device *asd)
{
	enum ia_css_err ret;
	int i, j;

	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		for (j = 0; j < IA_CSS_PIPE_ID_NUM; j++) {
			ret = __create_pipe(asd, &asd->stream_env[i], j);
			if (ret != IA_CSS_SUCCESS)
				break;
		}
		if (j < IA_CSS_PIPE_ID_NUM)
			goto pipe_err;
	}
	return 0;
pipe_err:
	for (; i >= 0; i--) {
		for (j--; j >= 0; j--) {
			if (asd->stream_env[i].pipes[j]) {
				ia_css_pipe_destroy(asd->stream_env[i].pipes[j]);
				asd->stream_env[i].pipes[j] = NULL;
			}
		}
		j = IA_CSS_PIPE_ID_NUM;
	}
	return -EINVAL;
}

int atomisp_css_update_stream(struct atomisp_sub_device *asd)
{
	int ret;
	struct atomisp_device *isp = asd->isp;

	if (__destroy_streams(asd, true) != IA_CSS_SUCCESS)
		dev_warn(isp->dev, "destroy stream failed.\n");

	if (__destroy_pipes(asd, true) != IA_CSS_SUCCESS)
		dev_warn(isp->dev, "destroy pipe failed.\n");

	ret = __create_pipes(asd);
	if (ret != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "create pipe failed %d.\n", ret);
		return -EIO;
	}

	ret = __create_streams(asd);
	if (ret != IA_CSS_SUCCESS) {
		dev_warn(isp->dev, "create stream failed %d.\n", ret);
		__destroy_pipes(asd, true);
		return -EIO;
	}

	return 0;
}

int atomisp_css_init(struct atomisp_device *isp)
{
	unsigned int mmu_base_addr;
	int ret;
	enum ia_css_err err;

	ret = hmm_get_mmu_base_addr(&mmu_base_addr);
	if (ret)
		return ret;

	/* Init ISP */
	err = ia_css_init(&isp->css_env.isp_css_env, NULL,
			  (uint32_t)mmu_base_addr, IA_CSS_IRQ_TYPE_PULSE);
	if (err != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "css init failed --- bad firmware?\n");
		return -EINVAL;
	}

	dev_dbg(isp->dev, "sh_css_init success\n");

	return 0;
}

int atomisp_css_load_firmware(struct atomisp_device *isp)
{
	enum ia_css_err err;

	/* set css env */
	isp->css_env.isp_css_fw.data = (void *)isp->firmware->data;
	isp->css_env.isp_css_fw.bytes = isp->firmware->size;

	isp->css_env.isp_css_env.cpu_mem_env.alloc = atomisp_kernel_zalloc;
	isp->css_env.isp_css_env.cpu_mem_env.free = atomisp_kernel_free;

	isp->css_env.isp_css_env.css_mem_env.alloc = atomisp_css2_mm_alloc;
	isp->css_env.isp_css_env.css_mem_env.free = atomisp_css2_mm_free;
	isp->css_env.isp_css_env.css_mem_env.load = atomisp_css2_mm_load;
	isp->css_env.isp_css_env.css_mem_env.store = atomisp_css2_mm_store;
	isp->css_env.isp_css_env.css_mem_env.set = atomisp_css2_mm_set;
	isp->css_env.isp_css_env.css_mem_env.mmap = atomisp_css2_mm_mmap;
#ifdef CSS21
	isp->css_env.isp_css_env.css_mem_env.hrt_vaddr_to_host_vaddr =
					atomisp_css21_hrt_vaddr_to_host_vaddr;
	isp->css_env.isp_css_env.css_mem_env.host_vaddr_to_hrt_vaddr =
					atomisp_css21_host_vaddr_to_hrt_vaddr;
#endif

	isp->css_env.isp_css_env.hw_access_env.store_8 =
							atomisp_css2_hw_store_8;
	isp->css_env.isp_css_env.hw_access_env.store_16 =
						atomisp_css2_hw_store_16;
	isp->css_env.isp_css_env.hw_access_env.store_32 =
						atomisp_css2_hw_store_32;

	isp->css_env.isp_css_env.hw_access_env.load_8 = atomisp_css2_hw_load_8;
	isp->css_env.isp_css_env.hw_access_env.load_16 =
							atomisp_css2_hw_load_16;
	isp->css_env.isp_css_env.hw_access_env.load_32 =
							atomisp_css2_hw_load_32;

	isp->css_env.isp_css_env.hw_access_env.load = atomisp_css2_hw_load;
	isp->css_env.isp_css_env.hw_access_env.store = atomisp_css2_hw_store;

	isp->css_env.isp_css_env.print_env.debug_print = atomisp_css2_dbg_print;
	isp->css_env.isp_css_env.print_env.error_print = atomisp_css2_err_print;

	/* load isp fw into ISP memory */
	err = ia_css_load_firmware(&isp->css_env.isp_css_env,
				   &isp->css_env.isp_css_fw);
	if (err != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "css load fw failed.\n");
		return -EINVAL;
	}

	return 0;
}

void atomisp_css_unload_firmware(struct atomisp_device *isp)
{
	ia_css_unload_firmware();
}

void atomisp_css_uninit(struct atomisp_device *isp)
{
	struct atomisp_sub_device *asd;
	unsigned int i;

	for (i = 0; i < isp->num_of_streams; i++) {
		asd = &isp->asd[i];
		atomisp_isp_parameters_clean_up(&asd->params.config);
		asd->params.css_update_params_needed = false;
	}

	ia_css_uninit();
}

void atomisp_css_suspend(void)
{
	ia_css_uninit();
}

int atomisp_css_resume(struct atomisp_device *isp)
{
	unsigned int mmu_base_addr;
	int ret;

	ret = hmm_get_mmu_base_addr(&mmu_base_addr);
	if (ret) {
		dev_err(isp->dev, "get base address error.\n");
		return -EINVAL;
	}

	ret = ia_css_init(&isp->css_env.isp_css_env, NULL,
			  mmu_base_addr, IA_CSS_IRQ_TYPE_PULSE);
	if (ret) {
		dev_err(isp->dev, "re-init css failed.\n");
		return -EINVAL;
	}

	return 0;
}

int atomisp_css_irq_translate(struct atomisp_device *isp,
			      unsigned int *infos)
{
	int err;

	err = ia_css_irq_translate(infos);
	if (err != IA_CSS_SUCCESS) {
		dev_warn(isp->dev,
			  "%s:failed to translate irq (err = %d,infos = %d)\n",
			  __func__, err, *infos);
		return -EINVAL;
	}

	return 0;
}

void atomisp_css_rx_get_irq_info(unsigned int *infos)
{
#ifndef ISP2401_NEW_INPUT_SYSTEM
	ia_css_rx_get_irq_info(infos);
#else
	*infos = 0;
#endif
}

void atomisp_css_rx_clear_irq_info(unsigned int infos)
{
#ifndef ISP2401_NEW_INPUT_SYSTEM
	ia_css_rx_clear_irq_info(infos);
#endif
}

void atomisp_css_rx_clear_irq_status(unsigned int infos)
{
       return;
}

int atomisp_css_irq_enable(struct atomisp_device *isp,
			    enum atomisp_css_irq_info info, bool enable)
{
	if (ia_css_irq_enable(info, enable) != IA_CSS_SUCCESS) {
		dev_warn(isp->dev, "%s:Invalid irq info.\n", __func__);
		return -EINVAL;
	}

	return 0;
}

void atomisp_css_init_struct(struct atomisp_sub_device *asd)
{
	int i, j;
	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		asd->stream_env[i].stream = NULL;
		for (j = 0; j < IA_CSS_PIPE_MODE_NUM; j++) {
			asd->stream_env[i].pipes[j] = NULL;
			asd->stream_env[i].update_pipe[j] = false;
			ia_css_pipe_config_defaults(
				&asd->stream_env[i].pipe_configs[j]);
			ia_css_pipe_extra_config_defaults(
				&asd->stream_env[i].pipe_extra_configs[j]);
		}
		ia_css_stream_config_defaults(&asd->stream_env[i].stream_config);
	}
}

int atomisp_q_video_buffer_to_css(struct atomisp_sub_device *asd,
			struct videobuf_vmalloc_memory *vm_mem,
			enum atomisp_input_stream_id stream_id,
			enum atomisp_css_buffer_type css_buf_type,
			enum atomisp_css_pipe_id css_pipe_id)
{
	struct atomisp_stream_env *stream_env = &asd->stream_env[stream_id];
	struct ia_css_buffer css_buf = {0};
	enum ia_css_err err;

	css_buf.type = css_buf_type;
	css_buf.data.frame = vm_mem->vaddr;

	err = ia_css_pipe_enqueue_buffer(
				stream_env->pipes[css_pipe_id], &css_buf);
	if (err != IA_CSS_SUCCESS)
		return -EINVAL;

	return 0;
}

int atomisp_q_s3a_buffer_to_css(struct atomisp_sub_device *asd,
			struct atomisp_s3a_buf *s3a_buf,
			enum atomisp_input_stream_id stream_id,
			enum atomisp_css_pipe_id css_pipe_id)
{
	struct atomisp_stream_env *stream_env = &asd->stream_env[stream_id];
	struct ia_css_buffer buffer = {0};
	struct atomisp_device *isp = asd->isp;

	buffer.type = IA_CSS_BUFFER_TYPE_3A_STATISTICS;
	buffer.data.stats_3a = s3a_buf->s3a_data;
	if (ia_css_pipe_enqueue_buffer(
				stream_env->pipes[css_pipe_id],
				&buffer)) {
		dev_dbg(isp->dev, "failed to q s3a stat buffer\n");
		return -EINVAL;
	}

	return 0;
}

int atomisp_q_dis_buffer_to_css(struct atomisp_sub_device *asd,
			struct atomisp_dis_buf *dis_buf,
			enum atomisp_input_stream_id stream_id,
			enum atomisp_css_pipe_id css_pipe_id)
{
	struct atomisp_stream_env *stream_env = &asd->stream_env[stream_id];
	struct ia_css_buffer buffer = {0};
	struct atomisp_device *isp = asd->isp;

	buffer.type = IA_CSS_BUFFER_TYPE_DIS_STATISTICS;
	buffer.data.stats_dvs = dis_buf->dis_data;
	if (ia_css_pipe_enqueue_buffer(
				stream_env->pipes[css_pipe_id],
				&buffer)) {
		dev_dbg(isp->dev, "failed to q dvs stat buffer\n");
		return -EINVAL;
	}

	return 0;
}

void atomisp_css_mmu_invalidate_cache(void)
{
	ia_css_mmu_invalidate_cache();
}

void atomisp_css_mmu_invalidate_tlb(void)
{
	ia_css_mmu_invalidate_cache();
}

void atomisp_css_mmu_set_page_table_base_index(unsigned long base_index)
{
}

/*
 * Check whether currently running MIPI buffer size fulfill
 * the requirement of the stream to be run
 */
bool __need_realloc_mipi_buffer(struct atomisp_device *isp)
{
	unsigned int i;

	for (i = 0; i < isp->num_of_streams; i++) {
		struct atomisp_sub_device *asd = &isp->asd[i];

		if (asd->streaming !=
				ATOMISP_DEVICE_STREAMING_ENABLED)
			continue;
		if (asd->mipi_frame_size < isp->mipi_frame_size)
			return true;
	}

	return false;
}

int atomisp_css_start(struct atomisp_sub_device *asd,
			enum atomisp_css_pipe_id pipe_id, bool in_reset)
{
	struct atomisp_device *isp = asd->isp;
	int ret = 0, i = 0;
	if (in_reset) {
		if (__destroy_streams(asd, true))
			dev_warn(isp->dev, "destroy stream failed.\n");

		if (__destroy_pipes(asd, true))
			dev_warn(isp->dev, "destroy pipe failed.\n");

		if (__create_pipes(asd)) {
			dev_err(isp->dev, "create pipe error.\n");
			return -EINVAL;
		}
		if (__create_streams(asd)) {
			dev_err(isp->dev, "create stream error.\n");
			ret = -EINVAL;
			goto stream_err;
		}
	}

	/*
	 * SP can only be started one time
	 * if atomisp_subdev_streaming_count() tell there already has some
	 * subdev at streamming, then SP should already be started previously,
	 * so need to skip start sp procedure
	 */
	if (atomisp_streaming_count(isp)) {
		dev_dbg(isp->dev, "skip start sp\n");
		/*
		 * FIXME! VIED BZ 1439:
		 * ISP timeout in start second stream due to incorrect MIPI
		 * Buffer size.
		 *
		 * This is due to MIPI buffers are allocated once and shared
		 * by all streams. So if the first sensor start running, MIPI
		 * buffer is allocated with size conrresponding to the sensor
		 * output frame size; when start second sensor, whose output
		 * resolution requires more MIPI buffers, the previous
		 * allocated MIPI buffer could not fulfill the requirement and
		 * hense get ISP timeout or other unexpected behavor.
		 *
		 * Workaround here is to reset ISP which will stop previous
		 * running stream, re-allocate mipi buffer, and start again.
		 */
		if (__need_realloc_mipi_buffer(isp)) {
			dev_warn(isp->dev, "Need to reallocate mipi buffer.\n");
			/* destroy stream/pipe for this stream */
			if (__destroy_streams(asd, true))
				dev_warn(isp->dev, "destroy stream failed.\n");

			if (__destroy_pipes(asd, true))
				dev_warn(isp->dev, "destroy pipe failed.\n");

			/*
			 * reset running stream which will reset mipi buffer
			 */
			atomisp_css_flush(isp);

			/* recreate stream/pipe for this stream */
			if (__create_pipes(asd)) {
				dev_err(isp->dev, "create pipe error.\n");
				return -EINVAL;
			}
			if (__create_streams(asd)) {
				dev_err(isp->dev, "create stream error.\n");
				ret = -EINVAL;
				goto stream_err;
			}
		}
	} else if (ia_css_start_sp() != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "start sp error.\n");
		ret = -EINVAL;
		goto start_err;
	}

	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		if (asd->stream_env[i].stream) {
			if (ia_css_stream_start(asd->stream_env[i]
						.stream) != IA_CSS_SUCCESS) {
				dev_err(isp->dev, "stream[%d] start error.\n", i);
				ret = -EINVAL;
				goto start_err;
			} else {
				asd->stream_env[i].stream_state = CSS_STREAM_STARTED;
				dev_dbg(isp->dev, "stream[%d] started.\n", i);
			}
		}
	}

	return 0;

start_err:
	__destroy_streams(asd, true);
stream_err:
	__destroy_pipes(asd, true);

	/* css 2.0 API limitation: ia_css_stop_sp() could be only called after
	 * destroy all pipes
	 */
	/*
	 * SP can not be stop if other streams are in use
	 */
	if (atomisp_streaming_count(isp))
		dev_dbg(isp->dev, "skip stop sp\n");
	else if (ia_css_isp_has_started() &&
		 ia_css_stop_sp() != IA_CSS_SUCCESS)
		dev_err(isp->dev, "stop sp failed.\n");

	return ret;
}

void atomisp_css_update_isp_params(struct atomisp_sub_device *asd)
{
	/*
	 * FIXME!
	 * for ISP2401 new input system, this api is under development.
	 * Calling it would cause kernel panic.
	 *
	 * VIED BZ: 1458
	 *
	 * Check if it is Cherry Trail and also new input system
	 */
	if ((asd->isp->media_dev.hw_revision & ATOMISP_HW_REVISION_MASK) ==
	    (ATOMISP_HW_REVISION_ISP2401 << ATOMISP_HW_REVISION_SHIFT)) {
		dev_warn(asd->isp->dev, "%s: ia_css_stream_set_isp_config() not supported!.\n",
				__func__);
		return;
	}

	ia_css_stream_set_isp_config(
			asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
			&asd->params.config);
	atomisp_isp_parameters_clean_up(&asd->params.config);
}

int atomisp_css_queue_buffer(struct atomisp_sub_device *asd,
			     enum atomisp_input_stream_id stream_id,
			     enum atomisp_css_pipe_id pipe_id,
			     enum atomisp_css_buffer_type buf_type,
			     struct atomisp_css_buffer *isp_css_buffer)
{
	if (ia_css_pipe_enqueue_buffer(
		asd->stream_env[stream_id].pipes[pipe_id],
					&isp_css_buffer->css_buffer)
					!= IA_CSS_SUCCESS)
		return -EINVAL;

	return 0;
}

int atomisp_css_dequeue_buffer(struct atomisp_sub_device *asd,
				enum atomisp_input_stream_id stream_id,
				enum atomisp_css_pipe_id pipe_id,
				enum atomisp_css_buffer_type buf_type,
				struct atomisp_css_buffer *isp_css_buffer)
{
	struct atomisp_device *isp = asd->isp;
	enum ia_css_err err;

	err = ia_css_pipe_dequeue_buffer(
		asd->stream_env[stream_id].pipes[pipe_id],
					&isp_css_buffer->css_buffer);
	if (err != IA_CSS_SUCCESS) {
		dev_err(isp->dev,
			"ia_css_pipe_dequeue_buffer failed: 0x%x\n", err);
		return -EINVAL;
	}

	return 0;
}

int atomisp_css_allocate_3a_dis_bufs(struct atomisp_sub_device *asd,
				struct atomisp_s3a_buf *s3a_buf,
				struct atomisp_dis_buf *dis_buf)
{
	struct atomisp_device *isp = asd->isp;
	if (asd->params.curr_grid_info.s3a_grid.enable) {
		s3a_buf->s3a_data = ia_css_isp_3a_statistics_allocate(
				&asd->params.curr_grid_info.s3a_grid);
		if (!s3a_buf->s3a_data) {
			dev_err(isp->dev, "3a buf allocation failed.\n");
			return -EINVAL;
		}
	}

	if (asd->params.curr_grid_info.dvs_grid.enable) {
		dis_buf->dis_data = ia_css_isp_dvs2_statistics_allocate(
				&asd->params.curr_grid_info.dvs_grid);
		if (!dis_buf->dis_data) {
			dev_err(isp->dev, "dvs buf allocation failed.\n");
			ia_css_isp_3a_statistics_free(s3a_buf->s3a_data);
			return -EINVAL;
		}
	}

	return 0;
}

void atomisp_css_free_3a_buffers(struct atomisp_s3a_buf *s3a_buf)
{
	ia_css_isp_3a_statistics_free(s3a_buf->s3a_data);
}

void atomisp_css_free_dis_buffers(struct atomisp_dis_buf *dis_buf)
{
	ia_css_isp_dvs2_statistics_free(dis_buf->dis_data);
}

void atomisp_css_free_3a_dis_buffers(struct atomisp_sub_device *asd)
{
	struct atomisp_s3a_buf *s3a_buf, *_s3a_buf;
	struct atomisp_dis_buf *dis_buf, *_dis_buf;

	/* 3A statistics use vmalloc, DIS use kmalloc */
	if (asd->params.curr_grid_info.dvs_grid.enable) {
		ia_css_dvs2_coefficients_free(asd->params.dvs_coeff);
		ia_css_dvs2_statistics_free(asd->params.dvs_stat);
		asd->params.dvs_coeff = NULL;
		asd->params.dvs_stat = NULL;
		asd->params.dvs_hor_proj_bytes = 0;
		asd->params.dvs_ver_proj_bytes = 0;
		asd->params.dvs_hor_coef_bytes = 0;
		asd->params.dvs_ver_coef_bytes = 0;
		asd->params.dis_proj_data_valid = false;
		list_for_each_entry_safe(dis_buf, _dis_buf,
						&asd->dis_stats, list) {
			ia_css_isp_dvs2_statistics_free(dis_buf->dis_data);
			list_del(&dis_buf->list);
			kfree(dis_buf);
		}
	}
	if (asd->params.curr_grid_info.s3a_grid.enable) {
		ia_css_3a_statistics_free(asd->params.s3a_user_stat);
		asd->params.s3a_user_stat = NULL;
		asd->params.s3a_buf_data_valid = false;
		asd->params.s3a_output_bytes = 0;
		list_for_each_entry_safe(s3a_buf, _s3a_buf,
						&asd->s3a_stats, list) {
			ia_css_isp_3a_statistics_free(s3a_buf->s3a_data);
			list_del(&s3a_buf->list);
			kfree(s3a_buf);
		}
	}

	if (asd->params.dvs_6axis) {
		ia_css_dvs2_6axis_config_free(asd->params.dvs_6axis);
		asd->params.dvs_6axis = NULL;
	}
}

int atomisp_css_get_grid_info(struct atomisp_sub_device *asd,
				enum atomisp_css_pipe_id pipe_id,
				int source_pad)
{
	struct ia_css_pipe_info p_info;
	struct ia_css_grid_info old_info;
	struct atomisp_device *isp = asd->isp;
	int stream_index = atomisp_source_pad_to_stream_id(asd, source_pad);

	memset(&p_info, 0, sizeof(struct ia_css_pipe_info));
	memset(&old_info, 0, sizeof(struct ia_css_grid_info));

	if (ia_css_pipe_get_info(
		asd->stream_env[stream_index].pipes[pipe_id],
		&p_info) != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "ia_css_pipe_get_info failed\n");
		return -EINVAL;
	}

	memcpy(&old_info, &asd->params.curr_grid_info,
					sizeof(struct ia_css_grid_info));
	memcpy(&asd->params.curr_grid_info, &p_info.grid_info,
					sizeof(struct ia_css_grid_info));

	/* If the grid info has not changed and the buffers for 3A and
	 * DIS statistics buffers are allocated or buffer size would be zero
	 * then no need to do anything. */
	if ((!memcmp(&old_info, &asd->params.curr_grid_info, sizeof(old_info))
	    && asd->params.s3a_user_stat && asd->params.dvs_stat)
	    || asd->params.curr_grid_info.s3a_grid.width == 0
	    || asd->params.curr_grid_info.s3a_grid.height == 0) {
		dev_dbg(isp->dev,
			"grid info change escape. memcmp=%d, s3a_user_stat=%d,"
			"dvs_stat=%d, s3a.width=%d, s3a.height=%d\n",
			!memcmp(&old_info, &asd->params.curr_grid_info,
				 sizeof(old_info)),
			 !!asd->params.s3a_user_stat, !!asd->params.dvs_stat,
			 asd->params.curr_grid_info.s3a_grid.width,
			 asd->params.curr_grid_info.s3a_grid.height);
		return -EINVAL;
	}

	return 0;
}

int atomisp_alloc_3a_output_buf(struct atomisp_sub_device *asd)
{
	asd->params.s3a_user_stat = ia_css_3a_statistics_allocate(
				&asd->params.curr_grid_info.s3a_grid);
	if (!asd->params.s3a_user_stat)
		return -ENOMEM;
	/* 3A statistics. These can be big, so we use vmalloc. */
	asd->params.s3a_output_bytes =
	    asd->params.curr_grid_info.s3a_grid.width *
	    asd->params.curr_grid_info.s3a_grid.height *
	    sizeof(*asd->params.s3a_user_stat->data);

	asd->params.s3a_buf_data_valid = false;

	return 0;
}

int atomisp_alloc_dis_coef_buf(struct atomisp_sub_device *asd)
{
	if (!asd->params.curr_grid_info.dvs_grid.enable) {
		dev_err(asd->isp->dev, "%s: dvs_grid not enabled.\n", __func__);
		return 0;
	}

	/* DIS coefficients. */
	asd->params.dvs_coeff = ia_css_dvs2_coefficients_allocate(
				&asd->params.curr_grid_info.dvs_grid);
	if (!asd->params.dvs_coeff)
		return -ENOMEM;

	asd->params.dvs_hor_coef_bytes =
		asd->params.curr_grid_info.dvs_grid.num_hor_coefs *
		sizeof(*asd->params.dvs_coeff->hor_coefs.odd_real);

	asd->params.dvs_ver_coef_bytes =
		asd->params.curr_grid_info.dvs_grid.num_ver_coefs *
		sizeof(*asd->params.dvs_coeff->ver_coefs.odd_real);

	/* DIS projections. */
	asd->params.dis_proj_data_valid = false;
	asd->params.dvs_stat = ia_css_dvs2_statistics_allocate(
				&asd->params.curr_grid_info.dvs_grid);
	if (!asd->params.dvs_stat)
		return -ENOMEM;

	asd->params.dvs_hor_proj_bytes =
		asd->params.curr_grid_info.dvs_grid.aligned_height *
		asd->params.curr_grid_info.dvs_grid.aligned_width *
		sizeof(*asd->params.dvs_stat->hor_prod.odd_real);

	asd->params.dvs_ver_proj_bytes =
		asd->params.curr_grid_info.dvs_grid.aligned_height *
		asd->params.curr_grid_info.dvs_grid.aligned_width *
		sizeof(*asd->params.dvs_stat->ver_prod.odd_real);

	return 0;
}

int atomisp_css_get_3a_statistics(struct atomisp_sub_device *asd,
				  struct atomisp_css_buffer *isp_css_buffer)
{
	if (asd->params.s3a_user_stat && asd->params.s3a_output_bytes) {
		/* To avoid racing with atomisp_3a_stat() */
		ia_css_get_3a_statistics(asd->params.s3a_user_stat,
				 isp_css_buffer->css_buffer.data.stats_3a);
		asd->params.s3a_buf_data_valid = true;
	}

	return 0;
}

void atomisp_css_get_dis_statistics(struct atomisp_sub_device *asd,
				    struct atomisp_css_buffer *isp_css_buffer)
{
	if (asd->params.dvs_stat) {
		ia_css_get_dvs2_statistics(asd->params.dvs_stat,
				  isp_css_buffer->css_buffer.data.stats_dvs);
		asd->params.exp_id = isp_css_buffer->css_buffer.exp_id;
		asd->params.dis_proj_data_valid = true;
	}
}

int atomisp_css_dequeue_event(struct atomisp_css_event *current_event)
{
	if (ia_css_dequeue_event(&current_event->event) != IA_CSS_SUCCESS)
		return -EINVAL;

	return 0;
}

void atomisp_css_temp_pipe_to_pipe_id(struct atomisp_css_event *current_event)
{
	ia_css_temp_pipe_to_pipe_id(current_event->event.pipe,
						&current_event->pipe);
}

int atomisp_css_input_set_resolution(struct atomisp_sub_device *asd,
				enum atomisp_input_stream_id stream_id,
				struct v4l2_mbus_framefmt *ffmt)
{
	asd->stream_env[stream_id]
	    .stream_config.input_res.width = ffmt->width;
	asd->stream_env[stream_id]
	    .stream_config.input_res.height = ffmt->height;
	return 0;
}

void atomisp_css_input_set_binning_factor(struct atomisp_sub_device *asd,
					enum atomisp_input_stream_id stream_id,
					unsigned int bin_factor)
{
	asd->stream_env[stream_id]
	    .stream_config.sensor_binning_factor = bin_factor;
}

void atomisp_css_input_set_bayer_order(struct atomisp_sub_device *asd,
				enum atomisp_input_stream_id stream_id,
				enum atomisp_css_bayer_order bayer_order)
{
	asd->stream_env[stream_id]
	    .stream_config.bayer_order = bayer_order;
}

void atomisp_css_input_set_format(struct atomisp_sub_device *asd,
					enum atomisp_input_stream_id stream_id,
					enum atomisp_css_stream_format format)
{
	asd->stream_env[stream_id]
	    .stream_config.format = format;
}

int atomisp_css_input_set_effective_resolution(
					struct atomisp_sub_device *asd,
					enum atomisp_input_stream_id stream_id,
					unsigned int width, unsigned int height)
{
	asd->stream_env[stream_id]
	    .stream_config.effective_res.width = width;
	asd->stream_env[stream_id]
	    .stream_config.effective_res.height = height;

	return 0;
}

void atomisp_css_video_set_dis_envelope(struct atomisp_sub_device *asd,
					unsigned int dvs_w, unsigned int dvs_h)
{
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.pipe_configs[IA_CSS_PIPE_ID_VIDEO].dvs_envelope.width = dvs_w;
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.pipe_configs[IA_CSS_PIPE_ID_VIDEO].dvs_envelope.height = dvs_h;
}

void atomisp_css_input_set_two_pixels_per_clock(
					struct atomisp_sub_device *asd,
					bool two_ppc)
{
	int i;

	if (asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.stream_config.two_pixels_per_clock == !!two_ppc)
		return;

	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.stream_config.two_pixels_per_clock = !!two_ppc;
	for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++)
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.update_pipe[i] = true;
}

void atomisp_css_enable_raw_binning(struct atomisp_sub_device *asd,
					bool enable)
{
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	unsigned int pipe;

	if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO)
		pipe = IA_CSS_PIPE_ID_VIDEO;
	else
		pipe = IA_CSS_PIPE_ID_PREVIEW;

	stream_env->pipe_extra_configs[pipe].enable_raw_binning = enable;
	stream_env->update_pipe[pipe] = true;
	if (enable) {
#ifndef CSS21
		stream_env->pipe_configs[pipe].bin_out_res.width =
		    stream_env->stream_config.effective_res.width;
		stream_env->pipe_configs[pipe].bin_out_res.height =
		    stream_env->stream_config.effective_res.height;
#endif /* CSS21 */
		stream_env->pipe_configs[pipe].output_info.padded_width =
		    stream_env->stream_config.effective_res.width;
	}
}

void atomisp_css_enable_dz(struct atomisp_sub_device *asd, bool enable)
{
	int i;
	for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++)
#ifdef CSS21
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
			.pipe_configs[i].enable_dz = enable;
#else
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
			.pipe_extra_configs[i].enable_dz = enable;
#endif
}

void atomisp_css_capture_set_mode(struct atomisp_sub_device *asd,
				enum atomisp_css_capture_mode mode)
{
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];

	if (stream_env->pipe_configs[IA_CSS_PIPE_ID_CAPTURE]
		.default_capture_config.mode == mode)
		return;

	stream_env->pipe_configs[IA_CSS_PIPE_ID_CAPTURE].
					default_capture_config.mode = mode;
	stream_env->update_pipe[IA_CSS_PIPE_ID_CAPTURE] = true;
}

void atomisp_css_input_set_mode(struct atomisp_sub_device *asd,
				enum atomisp_css_input_mode mode)
{
	int i;
	struct atomisp_device *isp = asd->isp;
	unsigned int size_mem_words, total_size_mem_words = 0;
	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++)
		asd->stream_env[i].stream_config.mode = mode;

	if (isp->inputs[asd->input_curr].type == TEST_PATTERN) {
		struct ia_css_stream_config *s_config =
		    &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream_config;
		s_config->mode = IA_CSS_INPUT_MODE_TPG;
		s_config->source.tpg.mode = IA_CSS_TPG_MODE_CHECKERBOARD;
		s_config->source.tpg.x_mask = (1 << 4) - 1;
		s_config->source.tpg.x_delta = -2;
		s_config->source.tpg.y_mask = (1 << 4) - 1;
		s_config->source.tpg.y_delta = 3;
		s_config->source.tpg.xy_mask = (1 << 8) - 1;
		return;
	}

	if (mode != IA_CSS_INPUT_MODE_BUFFERED_SENSOR)
		return;

	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		/*
		 * TODO: sensor needs to export the embedded_data_size_words
		 * information to atomisp for each setting.
		 * Here using a large safe value.
		 */
		struct ia_css_stream_config *s_config =
			&asd->stream_env[i].stream_config;
		if (s_config->input_res.width == 0)
			continue;
		if (ia_css_mipi_frame_calculate_size(s_config->input_res.width,
					s_config->input_res.height,
					s_config->format,
					true,
					0x13000,
					&size_mem_words) != IA_CSS_SUCCESS) {
			if (intel_mid_identify_cpu() ==
				INTEL_MID_CPU_CHIP_TANGIER)
				size_mem_words = CSS_MIPI_FRAME_BUFFER_SIZE_2;
			else
				size_mem_words = CSS_MIPI_FRAME_BUFFER_SIZE_1;
			dev_warn(asd->isp->dev,
				"ia_css_mipi_frame_calculate_size failed,"
				"applying pre-defined MIPI buffer size %u.\n",
				size_mem_words);
		}
		total_size_mem_words += size_mem_words;
	}

	if (total_size_mem_words > asd->isp->mipi_frame_size)
		asd->isp->mipi_frame_size = size_mem_words;

	asd->mipi_frame_size = asd->isp->mipi_frame_size;

	ia_css_mipi_frame_specify(asd->isp->mipi_frame_size, false);
}

void atomisp_css_capture_enable_online(struct atomisp_sub_device *asd,
							bool enable)
{
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];

	if (stream_env->stream_config.online == !!enable)
		return;

	stream_env->stream_config.online = !!enable;
	stream_env->update_pipe[IA_CSS_PIPE_ID_CAPTURE] = true;
}

void atomisp_css_preview_enable_online(struct atomisp_sub_device *asd,
							bool enable)
{
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	int i;

	if (stream_env->stream_config.online != !!enable) {
		stream_env->stream_config.online = !!enable;
		for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++)
			stream_env->update_pipe[i] = true;
	}
}

void atomisp_css_enable_continuous(struct atomisp_sub_device *asd,
							bool enable)
{
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	int i;

	if (stream_env->stream_config.continuous != !!enable) {
		stream_env->stream_config.continuous = !!enable;
#ifdef CSS21
		stream_env->stream_config.pack_raw_pixels = true;
#endif
		for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++)
			stream_env->update_pipe[i] = true;
	}
}

void atomisp_css_enable_cont_capt(bool enable, bool stop_copy_preview)
{
	sh_css_enable_cont_capt(enable, stop_copy_preview);
}

int atomisp_css_input_configure_port(struct atomisp_sub_device *asd,
					mipi_port_ID_t port,
					unsigned int num_lanes,
					unsigned int timeout)
{
	int i;
	struct atomisp_stream_env *stream_env;
	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
		stream_env = &asd->stream_env[i];
		stream_env->stream_config.source.port.port = port;
		stream_env->stream_config.source.port.num_lanes = num_lanes;
		stream_env->stream_config.source.port.timeout = timeout;
	}

	return 0;
}

int atomisp_css_frame_allocate(struct atomisp_css_frame **frame,
				unsigned int width, unsigned int height,
				enum atomisp_css_frame_format format,
				unsigned int padded_width,
				unsigned int raw_bit_depth)
{
	if (ia_css_frame_allocate(frame, width, height, format,
			padded_width, raw_bit_depth) != IA_CSS_SUCCESS)
		return -ENOMEM;

	return 0;
}

int atomisp_css_frame_allocate_from_info(struct atomisp_css_frame **frame,
				const struct atomisp_css_frame_info *info)
{
	if (ia_css_frame_allocate_from_info(frame, info) != IA_CSS_SUCCESS)
		return -ENOMEM;

	return 0;
}

void atomisp_css_frame_free(struct atomisp_css_frame *frame)
{
	ia_css_frame_free(frame);
}

int atomisp_css_frame_map(struct atomisp_css_frame **frame,
				const struct atomisp_css_frame_info *info,
				const void *data, uint16_t attribute,
				void *context)
{
	if (ia_css_frame_map(frame, info, data, attribute, context)
	    != IA_CSS_SUCCESS)
		return -ENOMEM;

	return 0;
}

int atomisp_css_set_black_frame(struct atomisp_sub_device *asd,
				const struct atomisp_css_frame *raw_black_frame)
{
	if (sh_css_set_black_frame(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		raw_black_frame) != IA_CSS_SUCCESS)
		return -ENOMEM;

	return 0;
}

int atomisp_css_allocate_continuous_frames(bool init_time,
				struct atomisp_sub_device *asd)
{
	if (ia_css_alloc_continuous_frame_remain(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream)
			!= IA_CSS_SUCCESS)
		return -EINVAL;
	return 0;
}

void atomisp_css_update_continuous_frames(struct atomisp_sub_device *asd)
{
	ia_css_update_continuous_frames(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream);
}

int atomisp_css_stop(struct atomisp_sub_device *asd,
			enum atomisp_css_pipe_id pipe_id, bool in_reset)
{
	struct atomisp_device *isp = asd->isp;
	/* if is called in atomisp_reset(), force destroy stream */
	if (__destroy_streams(asd, true))
		dev_err(isp->dev, "destroy stream failed.\n");

	/* if is called in atomisp_reset(), force destroy all pipes */
	if (__destroy_pipes(asd, true))
		dev_err(isp->dev, "destroy pipes failed.\n");
	/*
	 * SP can not be stop if other streams are in use
	 */
	if (atomisp_streaming_count(isp)) {
		dev_dbg(isp->dev, "skip stop sp\n");
	} else if (ia_css_isp_has_started() &&
		   (ia_css_stop_sp() != IA_CSS_SUCCESS)) {
		dev_err(isp->dev, "stop sp failed. stop css fatal error.\n");
		return -EINVAL;
	}

	if (!in_reset) {
		struct atomisp_stream_env *stream_env;
		int i, j;
		for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++) {
			stream_env = &asd->stream_env[i];
			for (j = 0; j < IA_CSS_PIPE_ID_NUM; j++) {
				ia_css_pipe_config_defaults(
					&stream_env->pipe_configs[j]);
				ia_css_pipe_extra_config_defaults(
					&stream_env->pipe_extra_configs[j]);
			}
			ia_css_stream_config_defaults(
				&stream_env->stream_config);
		}
		atomisp_isp_parameters_clean_up(&asd->params.config);
		asd->params.css_update_params_needed = false;
	}

	return 0;
}

int atomisp_css_continuous_set_num_raw_frames(
					struct atomisp_sub_device *asd,
					int num_frames)
{
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.stream_config.init_num_cont_raw_buf =
		ATOMISP_CSS2_NUM_OFFLINE_INIT_CONTINUOUS_FRAMES;
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.stream_config.target_num_cont_raw_buf = num_frames;
	return 0;
}

void atomisp_css_disable_vf_pp(struct atomisp_sub_device *asd,
			       bool disable)
{
	int i;

	for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++)
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.pipe_extra_configs[i].disable_vf_pp = !!disable;
}

static enum ia_css_pipe_mode __pipe_id_to_pipe_mode(
					enum ia_css_pipe_id pipe_id)
{
	switch (pipe_id) {
#ifdef CSS21
	case IA_CSS_PIPE_ID_COPY:
		return IA_CSS_PIPE_MODE_COPY;
#endif
	case IA_CSS_PIPE_ID_PREVIEW:
		return IA_CSS_PIPE_MODE_PREVIEW;
	case IA_CSS_PIPE_ID_CAPTURE:
		return IA_CSS_PIPE_MODE_CAPTURE;
	case IA_CSS_PIPE_ID_VIDEO:
		return IA_CSS_PIPE_MODE_VIDEO;
	case IA_CSS_PIPE_ID_ACC:
		return IA_CSS_PIPE_MODE_ACC;
	default:
		WARN_ON(1);
		return IA_CSS_PIPE_MODE_PREVIEW;
	}

}

static void __configure_output(struct atomisp_sub_device *asd,
			       unsigned int stream_index,
			       unsigned int width, unsigned int height,
			       unsigned int min_width,
			       enum ia_css_frame_format format,
			       enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[stream_index];

	stream_env->pipe_configs[pipe_id].mode =
		__pipe_id_to_pipe_mode(pipe_id);
	stream_env->update_pipe[pipe_id] = true;

	stream_env->pipe_configs[pipe_id].output_info.res.width = width;
	stream_env->pipe_configs[pipe_id].output_info.res.height = height;
	stream_env->pipe_configs[pipe_id].output_info.format = format;
	stream_env->pipe_configs[pipe_id].output_info.padded_width = min_width;

	/* isp binary 2.2 specific setting*/
	if (width > stream_env->stream_config.effective_res.width ||
	    height > stream_env->stream_config.effective_res.height) {
		stream_env->stream_config.effective_res.width = width;
		stream_env->stream_config.effective_res.height = height;
	}

	dev_dbg(isp->dev, "configuring pipe[%d] output info w=%d.h=%d.f=%d.\n",
		pipe_id, width, height, format);
}

/*
 * CSS2.1 and Old CSS2.0 has different parameters for pp input configuration.
 */
#ifndef CSS21
/*
 * For old CSS2.0, preview pipe and capture pipe all use bayer_ds_out_res to
 * configure YUV Downscaling input resolution
 */
static void __configure_pp_input(struct atomisp_sub_device *asd,
				 unsigned int width, unsigned int height,
				 enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	if (width == 0 && height == 0)
		return;

	if (width * 9 / 10 <
	    stream_env->pipe_configs[pipe_id].
	    output_info.res.width ||
	    height * 9 / 10 <
	    stream_env->pipe_configs[pipe_id].
	    output_info.res.height
	   )
		return;
	stream_env->pipe_configs[pipe_id].mode =
					__pipe_id_to_pipe_mode(pipe_id);
	stream_env->update_pipe[pipe_id] = true;

	stream_env->pipe_extra_configs[pipe_id].enable_yuv_ds = true;
	stream_env->pipe_configs[pipe_id].bayer_ds_out_res.width =
	    stream_env->stream_config.effective_res.width;
	stream_env->pipe_configs[pipe_id].bayer_ds_out_res.height =
	    stream_env->stream_config.effective_res.height;
	dev_dbg(isp->dev, "configuring pipe[%d]capture pp input w=%d.h=%d.\n",
		pipe_id, width, height);
}
#else
/*
 * For CSS2.1, capture pipe uses capture_pp_in_res to configure yuv
 * downscaling input resolution.
 */
static void __configure_capture_pp_input(struct atomisp_sub_device *asd,
				 unsigned int width, unsigned int height,
				 enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	struct ia_css_stream_config *stream_config = &stream_env->stream_config;
	struct ia_css_pipe_config *pipe_configs =
		&stream_env->pipe_configs[pipe_id];
	struct ia_css_pipe_extra_config *pipe_extra_configs =
		&stream_env->pipe_extra_configs[pipe_id];

	if (width == 0 && height == 0)
		return;

	if (width * 9 / 10 < pipe_configs->output_info.res.width ||
	    height * 9 / 10 < pipe_configs->output_info.res.height)
		return;

	pipe_configs->mode = __pipe_id_to_pipe_mode(pipe_id);
	stream_env->update_pipe[pipe_id] = true;

	pipe_extra_configs->enable_yuv_ds = true;
	pipe_configs->capt_pp_in_res.width = stream_config->effective_res.width;
	pipe_configs->capt_pp_in_res.height =
		stream_config->effective_res.height;
	dev_dbg(isp->dev, "configuring pipe[%d]capture pp input w=%d.h=%d.\n",
		pipe_id, width, height);
}

/*
 * For CSS2.1, preview pipe could support bayer decimation, yuv decimation and
 * yuv downscaling, which needs addtional configurations.
 */
static void __configure_preview_pp_input(struct atomisp_sub_device *asd,
				 unsigned int width, unsigned int height,
				 enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
#if 0
	/*
	 * FIXME, this is workaround to disable YUV decimation which caused
	 * vied BZ:1075
	 */
	int out_width, out_height, yuv_ds_in_width, yuv_ds_in_height;
#else
	int out_width, out_height;
#endif
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	struct ia_css_stream_config *stream_config = &stream_env->stream_config;
	struct ia_css_pipe_config *pipe_configs =
		&stream_env->pipe_configs[pipe_id];
	struct ia_css_pipe_extra_config *pipe_extra_configs =
		&stream_env->pipe_extra_configs[pipe_id];
#if 0
	/*
	 * FIXME, this is workaround to disable bayer downscaling which caused
	 * vied BZ:1075
	 */
	struct ia_css_resolution *bayer_ds_out_res =
		&pipe_configs->bayer_ds_out_res;
#endif
	struct ia_css_resolution *vf_pp_in_res =
		&pipe_configs->vf_pp_in_res;
	struct ia_css_resolution  *effective_res =
		&stream_config->effective_res;

	if (width == 0 && height == 0)
		return;

	if (width * 9 / 10 < pipe_configs->output_info.res.width ||
	    height * 9 / 10 < pipe_configs->output_info.res.height)
		return;
	pipe_configs->mode = __pipe_id_to_pipe_mode(pipe_id);
	stream_env->update_pipe[pipe_id] = true;

	pipe_extra_configs->enable_yuv_ds = true;

	out_width = pipe_configs->output_info.res.width;
	out_height = pipe_configs->output_info.res.height;

	/*
	 * The ISP could do bayer decimation, yuv decimation and yuv
	 * downscaling:
	 * 1: Bayer Decimimation: between effective resolution and
	 * bayer_ds_res_out;
	 * 2: YUV Decimation: between bayer_ds_res_out and vf_pp_in_res;
	 * 3: YUV downscaling: between vf_pp_in_res and final vf output
	 *
	 * Rule for Bayer Decimation: support factor 2, 1.5 and 1.25
	 * Rule for YUV Decimation: support factor 1,2,4
	 * Rule for YUV Downscaling: arbitary value below 2
	 *
	 * General rule of factor distribution among these stages:
	 * 1: try to do 2, 1.5, 1.25 in order for Bayer decimation if not in
	 * online mode.
	 * 2: try to do maximum of 2 for YUV downscaling
	 * 3: the remainling for YUV decimation
	 *
	 * Note:
	 * Do not configure bayer_ds_out_res if:
	 * online == 1 or continuous == 0 or raw_binning = 0
	 */

	/*
	 * calculate bayer decimate factor:
	 * 1: only 2, 1.5, 1.25 get supported
	 * 2: Do not configure bayer_ds_out_res if:
	 * online == 1 or continuous == 0 or raw_binning = 0
	 */
#if 0
	/*
	 * FIXME, this is workaround to disable bayer downscaling which caused
	 * vied BZ:1075
	 */
	if (stream_config->online || !stream_config->continuous ||
			!pipe_extra_configs->enable_raw_binning) {
		bayer_ds_out_res->width = 0;
		bayer_ds_out_res->height = 0;
	} else if (effective_res->width > out_width * 2 &&
			effective_res->height > out_height * 2) {
		bayer_ds_out_res->width = effective_res->width / 2;
		bayer_ds_out_res->height = effective_res->height / 2;
	} else if (effective_res->width > out_width * 3 / 2 &&
			effective_res->height > out_height * 3 / 2) {
		bayer_ds_out_res->width = effective_res->width * 2 / 3;
		bayer_ds_out_res->height = effective_res->height * 2 / 3;
	} else if (effective_res->width > out_width * 5 / 4 &&
			effective_res->height > out_height * 5 / 4) {
		bayer_ds_out_res->width = effective_res->width * 4 / 5;
		bayer_ds_out_res->height = effective_res->height * 4 / 5;

	} else {
		bayer_ds_out_res->width = effective_res->width;
		bayer_ds_out_res->height = effective_res->height;
	}
#endif
	/*
	 * calculate YUV Decimation, YUV downscaling facor:
	 * YUV Downscaling factor must not exceed 2.
	 * YUV Decimation factor could be 1, 2 ,4.
	 */
	/* first decide the yuv_ds input resolution */
#if 0
	/*
	 * FIXME, this is workaround to disable YUV decimation which caused
	 * vied BZ:1075
	 */
	if (bayer_ds_out_res->width == 0) {
		yuv_ds_in_width = effective_res->width;
		yuv_ds_in_height = effective_res->height;
	} else {
		yuv_ds_in_width = bayer_ds_out_res->width;
		yuv_ds_in_height = bayer_ds_out_res->height;
	}
	if (yuv_ds_in_width > out_width * 4 &&
			yuv_ds_in_height > out_height * 4) {
		/* YUV Decimation factor 4 */
		vf_pp_in_res->width = yuv_ds_in_width / 4;
		vf_pp_in_res->height = yuv_ds_in_height / 4;
	} else 	if (yuv_ds_in_width > out_width * 2 &&
			yuv_ds_in_height > out_height * 2) {
		/* YUV Decimation factor 2 */
		vf_pp_in_res->width = yuv_ds_in_width / 2;
		vf_pp_in_res->height = yuv_ds_in_height / 2;
	} else {
		/* YUV Decimation not needed */
		vf_pp_in_res->width = yuv_ds_in_width;
		vf_pp_in_res->height = yuv_ds_in_height;
	}
#else
	vf_pp_in_res->width = effective_res->width;
	vf_pp_in_res->height = effective_res->height;
#endif
	dev_dbg(isp->dev, "configuring pipe[%d]capture pp input w=%d.h=%d.\n",
		pipe_id, width, height);
}

/*
 * For CSS2.1, offline video pipe could support bayer decimation, and
 * yuv downscaling, which needs addtional configurations.
 */
static void __configure_video_pp_input(struct atomisp_sub_device *asd,
				 unsigned int width, unsigned int height,
				 enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	int out_width, out_height;
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	struct ia_css_stream_config *stream_config = &stream_env->stream_config;
	struct ia_css_pipe_config *pipe_configs =
		&stream_env->pipe_configs[pipe_id];
	struct ia_css_pipe_extra_config *pipe_extra_configs =
		&stream_env->pipe_extra_configs[pipe_id];
	struct ia_css_resolution *bayer_ds_out_res =
		&pipe_configs->bayer_ds_out_res;
	struct ia_css_resolution  *effective_res =
		&stream_config->effective_res;
	const struct bayer_ds_factor bds_factors[] =
		{{8, 1}, {4, 1}, {2, 1}, {3, 2}, {5, 4}};
	unsigned int i;

	if (width == 0 && height == 0)
		return;

	if (width * 9 / 10 < pipe_configs->output_info.res.width ||
	    height * 9 / 10 < pipe_configs->output_info.res.height)
		return;

	pipe_configs->mode = __pipe_id_to_pipe_mode(pipe_id);
	stream_env->update_pipe[pipe_id] = true;

	pipe_extra_configs->enable_yuv_ds = false;

	/*
	 * If DVS is enabled,  video binary will take care the dvs envelope
	 * and usually the bayer_ds_out_res should be larger than 120% of
	 * destination resolution, the extra 20% will be cropped as DVS
	 * envelope. But,  if the bayer_ds_out_res is less than 120% of the
	 * destination. The ISP can still work,  but DVS quality is not good.
	 */
	/* taking at least 18% as envelope */
	if (asd->params.video_dis_en) {
		out_width = pipe_configs->output_info.res.width * 118 / 100;
		out_height = pipe_configs->output_info.res.height * 118 / 100;
	} else {
		out_width = pipe_configs->output_info.res.width;
		out_height = pipe_configs->output_info.res.height;
	}

	/*
	 * calculate bayer decimate factor:
	 * 1: only 1.25, 1.5, 2, 4 and 8 get supported
	 * 2: Do not configure bayer_ds_out_res if:
	 *    online == 1 or continuous == 0 or raw_binning = 0
	 */
	if (stream_config->online || !stream_config->continuous ||
	    !pipe_extra_configs->enable_raw_binning) {
		bayer_ds_out_res->width = 0;
		bayer_ds_out_res->height = 0;
		goto done;
	}

	bayer_ds_out_res->width = effective_res->width;
	bayer_ds_out_res->height = effective_res->height;

	for (i = 0; i < sizeof(bds_factors) / sizeof(struct bayer_ds_factor);
	     i++) {
		if (effective_res->width >= out_width *
		    bds_factors[i].numerator / bds_factors[i].denominator &&
		    effective_res->height >= out_height *
		    bds_factors[i].numerator / bds_factors[i].denominator) {
			bayer_ds_out_res->width = effective_res->width *
			    bds_factors[i].denominator /
			    bds_factors[i].numerator;
			bayer_ds_out_res->height = effective_res->height *
			    bds_factors[i].denominator /
			    bds_factors[i].numerator;
			break;
		}
	}

done:
	stream_config->left_padding = 12;
	dev_dbg(isp->dev, "configuring pipe[%d]video pp input w=%d.h=%d.\n",
		pipe_id, width, height);
}
#endif

static void __configure_vf_output(struct atomisp_sub_device *asd,
				  unsigned int width, unsigned int height,
				  unsigned int min_width,
				  enum atomisp_css_frame_format format,
				  enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	stream_env->pipe_configs[pipe_id].mode =
					__pipe_id_to_pipe_mode(pipe_id);
	stream_env->update_pipe[pipe_id] = true;

	stream_env->pipe_configs[pipe_id].vf_output_info.res.width = width;
	stream_env->pipe_configs[pipe_id].vf_output_info.res.height = height;
	stream_env->pipe_configs[pipe_id].vf_output_info.format = format;
	stream_env->pipe_configs[pipe_id].vf_output_info.padded_width =
		min_width;
	dev_dbg(isp->dev,
		"configuring pipe[%d] vf output info w=%d.h=%d.f=%d.\n",
		 pipe_id, width, height, format);
}

static int __get_frame_info(struct atomisp_sub_device *asd,
				unsigned int stream_index,
				struct atomisp_css_frame_info *info,
				enum frame_info_type type,
				enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	enum ia_css_err ret;
	struct ia_css_pipe_info p_info;

	/* FIXME! No need to destroy/recreate all streams */
	if (__destroy_streams(asd, true))
		dev_warn(isp->dev, "destroy stream failed.\n");

	if (__destroy_pipes(asd, true))
		dev_warn(isp->dev, "destroy pipe failed.\n");

	if (__create_pipes(asd))
		return -EINVAL;

	if (__create_streams(asd))
		goto stream_err;

	ret = ia_css_pipe_get_info(
		asd->stream_env[stream_index]
		.pipes[pipe_id], &p_info);
	if (ret == IA_CSS_SUCCESS) {
		switch (type) {
		case ATOMISP_CSS_VF_FRAME:
			*info = p_info.vf_output_info;
			dev_dbg(isp->dev, "getting vf frame info.\n");
			break;
		case ATOMISP_CSS_OUTPUT_FRAME:
			*info = p_info.output_info;
			dev_dbg(isp->dev, "getting main frame info.\n");
			break;
		case ATOMISP_CSS_RAW_FRAME:
			*info = p_info.raw_output_info;
			dev_dbg(isp->dev, "getting raw frame info.\n");
		}
		dev_dbg(isp->dev, "get frame info: w=%d, h=%d.\n",
			info->res.width, info->res.height);
		return 0;
	}

stream_err:
	__destroy_pipes(asd, true);
	return -EINVAL;
}

unsigned int atomisp_get_pipe_index(struct atomisp_sub_device *asd,
					uint16_t source_pad)
{
	struct atomisp_device *isp = asd->isp;
	if (isp->inputs[asd->input_curr].camera_caps->
			sensor[asd->sensor_curr].stream_num > 1)
		return IA_CSS_PIPE_ID_COPY;

	switch (source_pad) {
	case ATOMISP_SUBDEV_PAD_SOURCE_VIDEO:
		if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO
		    || asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER)
			return IA_CSS_PIPE_ID_VIDEO;
		else
			return IA_CSS_PIPE_ID_CAPTURE;
	case ATOMISP_SUBDEV_PAD_SOURCE_CAPTURE:
		return IA_CSS_PIPE_ID_CAPTURE;
	case ATOMISP_SUBDEV_PAD_SOURCE_VF:
		if (!atomisp_is_mbuscode_raw(
		    asd->fmt[asd->capture_pad].fmt.code))
			return IA_CSS_PIPE_ID_CAPTURE;
	case ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW:
		if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO)
			return IA_CSS_PIPE_ID_VIDEO;
		else
			return IA_CSS_PIPE_ID_PREVIEW;
	}

	dev_warn(isp->dev,
		 "invalid source pad:%d, return default preview pipe index.\n",
		 source_pad);
	return IA_CSS_PIPE_ID_PREVIEW;
}

int atomisp_get_css_frame_info(struct atomisp_sub_device *asd,
				uint16_t source_pad,
				struct atomisp_css_frame_info *frame_info)
{
	struct ia_css_pipe_info info;
	int pipe_index = atomisp_get_pipe_index(asd, source_pad);
	int stream_index = atomisp_source_pad_to_stream_id(asd, source_pad);

	ia_css_pipe_get_info(asd->stream_env[stream_index]
		.pipes[pipe_index], &info);
	switch (source_pad) {
	case ATOMISP_SUBDEV_PAD_SOURCE_CAPTURE:
	case ATOMISP_SUBDEV_PAD_SOURCE_VIDEO:
		*frame_info = info.output_info;
		break;
	case ATOMISP_SUBDEV_PAD_SOURCE_VF:
		if (stream_index == ATOMISP_INPUT_STREAM_POSTVIEW)
			*frame_info = info.output_info;
		else
			*frame_info = info.vf_output_info;
		break;
	case ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW:
		if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO)
			*frame_info = info.vf_output_info;
		else
			*frame_info = info.output_info;
		break;
	default:
		frame_info = NULL;
		break;
	}
	return frame_info ? 0 : -EINVAL;
}

#if defined(CSS21) && defined(ISP2401_NEW_INPUT_SYSTEM)
int atomisp_css_copy_configure_output(struct atomisp_sub_device *asd,
				unsigned int stream_index,
				unsigned int width, unsigned int height,
				enum atomisp_css_frame_format format)
{
	asd->stream_env[stream_index].pipe_configs[IA_CSS_PIPE_ID_COPY].
					default_capture_config.mode =
					CSS_CAPTURE_MODE_RAW;

	__configure_output(asd, stream_index, width, height, width, format,
			   IA_CSS_PIPE_ID_COPY);
	return 0;
}
#endif

int atomisp_css_preview_configure_output(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum atomisp_css_frame_format format)
{
	__configure_output(asd, ATOMISP_INPUT_STREAM_GENERAL, width, height,
			   min_width, format, IA_CSS_PIPE_ID_PREVIEW);
	return 0;
}

int atomisp_css_capture_configure_output(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum atomisp_css_frame_format format)
{
	__configure_output(asd, ATOMISP_INPUT_STREAM_GENERAL, width, height,
			   min_width, format, IA_CSS_PIPE_ID_CAPTURE);
	return 0;
}

int atomisp_css_video_configure_output(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum atomisp_css_frame_format format)
{
	__configure_output(asd, ATOMISP_INPUT_STREAM_GENERAL, width, height,
			   min_width, format, IA_CSS_PIPE_ID_VIDEO);
	return 0;
}

int atomisp_css_video_configure_viewfinder(
				struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum atomisp_css_frame_format format)
{
	__configure_vf_output(asd, width, height, min_width, format,
			      IA_CSS_PIPE_ID_VIDEO);
	return 0;
}

int atomisp_css_capture_configure_viewfinder(
				struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum atomisp_css_frame_format format)
{
	__configure_vf_output(asd, width, height, min_width, format,
			      IA_CSS_PIPE_ID_CAPTURE);
	return 0;
}

int atomisp_css_video_get_viewfinder_frame_info(
					struct atomisp_sub_device *asd,
					struct atomisp_css_frame_info *info)
{
	return __get_frame_info(asd, ATOMISP_INPUT_STREAM_GENERAL, info,
			ATOMISP_CSS_VF_FRAME, IA_CSS_PIPE_ID_VIDEO);
}

int atomisp_css_capture_get_viewfinder_frame_info(
					struct atomisp_sub_device *asd,
					struct atomisp_css_frame_info *info)
{
	return __get_frame_info(asd, ATOMISP_INPUT_STREAM_GENERAL, info,
			ATOMISP_CSS_VF_FRAME, IA_CSS_PIPE_ID_CAPTURE);
}

int atomisp_css_capture_get_output_raw_frame_info(
					struct atomisp_sub_device *asd,
					struct atomisp_css_frame_info *info)
{
	return __get_frame_info(asd, ATOMISP_INPUT_STREAM_GENERAL, info,
			ATOMISP_CSS_RAW_FRAME, IA_CSS_PIPE_ID_CAPTURE);
}

#if defined(CSS21) && defined(ISP2401_NEW_INPUT_SYSTEM)
int atomisp_css_copy_get_output_frame_info(
					struct atomisp_sub_device *asd,
					unsigned int stream_index,
					struct atomisp_css_frame_info *info)
{
	return __get_frame_info(asd, stream_index, info,
			ATOMISP_CSS_OUTPUT_FRAME, IA_CSS_PIPE_ID_COPY);
}
#endif

int atomisp_css_preview_get_output_frame_info(
					struct atomisp_sub_device *asd,
					struct atomisp_css_frame_info *info)
{
	return __get_frame_info(asd, ATOMISP_INPUT_STREAM_GENERAL, info,
			ATOMISP_CSS_OUTPUT_FRAME, IA_CSS_PIPE_ID_PREVIEW);
}

int atomisp_css_capture_get_output_frame_info(
					struct atomisp_sub_device *asd,
					struct atomisp_css_frame_info *info)
{
	return __get_frame_info(asd, ATOMISP_INPUT_STREAM_GENERAL, info,
			ATOMISP_CSS_OUTPUT_FRAME, IA_CSS_PIPE_ID_CAPTURE);
}

int atomisp_css_video_get_output_frame_info(
					struct atomisp_sub_device *asd,
					struct atomisp_css_frame_info *info)
{
	return __get_frame_info(asd, ATOMISP_INPUT_STREAM_GENERAL, info,
			ATOMISP_CSS_OUTPUT_FRAME, IA_CSS_PIPE_ID_VIDEO);
}

int atomisp_css_preview_configure_pp_input(
				struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height)
{
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
#ifdef CSS21
	__configure_preview_pp_input(asd, width, height,
			IA_CSS_PIPE_ID_PREVIEW);

	if (width > stream_env->pipe_configs[IA_CSS_PIPE_ID_CAPTURE].
					capt_pp_in_res.width)
		__configure_capture_pp_input(asd,
				     width, height, IA_CSS_PIPE_ID_CAPTURE);
#else
	if (stream_env->pipe_extra_configs[IA_CSS_PIPE_ID_PREVIEW].
					enable_raw_binning == false)
		__configure_pp_input(asd, width, height,
					IA_CSS_PIPE_ID_PREVIEW);

	if (width > stream_env->pipe_configs[IA_CSS_PIPE_ID_CAPTURE].
					bayer_ds_out_res.width)
		__configure_pp_input(asd,
				     width, height, IA_CSS_PIPE_ID_CAPTURE);
#endif
	return 0;
}

int atomisp_css_capture_configure_pp_input(
				struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height)
{
#ifdef CSS21
	__configure_capture_pp_input(asd, width, height, IA_CSS_PIPE_ID_CAPTURE);
#else
	__configure_pp_input(asd, width, height, IA_CSS_PIPE_ID_CAPTURE);
#endif
	return 0;
}

int atomisp_css_video_configure_pp_input(
				struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height)
{
#ifdef CSS21
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];

	__configure_video_pp_input(asd, width, height,
			IA_CSS_PIPE_ID_VIDEO);

	if (width > stream_env->pipe_configs[IA_CSS_PIPE_ID_CAPTURE].
					capt_pp_in_res.width)
		__configure_capture_pp_input(asd,
				     width, height, IA_CSS_PIPE_ID_CAPTURE);
#endif
	return 0;
}

int atomisp_css_offline_capture_configure(struct atomisp_sub_device *asd,
			int num_captures, unsigned int skip, int offset)
{
	enum ia_css_err ret;

	ret = ia_css_stream_capture(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		num_captures, skip, offset);
	if (ret != IA_CSS_SUCCESS)
		return -EINVAL;

	return 0;
}

int atomisp_css_capture_enable_xnr(struct atomisp_sub_device *asd,
				   bool enable)
{
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.pipe_configs[IA_CSS_PIPE_ID_CAPTURE]
		.default_capture_config.enable_xnr = enable;
	asd->params.capture_config.enable_xnr = enable;
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.update_pipe[IA_CSS_PIPE_ID_CAPTURE] = true;

	return 0;
}

void atomisp_css_send_input_frame(struct atomisp_sub_device *asd,
				  unsigned short *data, unsigned int width,
				  unsigned int height)
{
	ia_css_stream_send_input_frame(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		data, width, height);
}

bool atomisp_css_isp_has_started(void)
{
	return ia_css_isp_has_started();
}

void atomisp_css_request_flash(struct atomisp_sub_device *asd)
{
	ia_css_stream_request_flash(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream);
}

void atomisp_css_set_wb_config(struct atomisp_sub_device *asd,
			struct atomisp_css_wb_config *wb_config)
{
	asd->params.config.wb_config = wb_config;
}

void atomisp_css_set_ob_config(struct atomisp_sub_device *asd,
			struct atomisp_css_ob_config *ob_config)
{
	asd->params.config.ob_config = ob_config;
}

void atomisp_css_set_dp_config(struct atomisp_sub_device *asd,
			struct atomisp_css_dp_config *dp_config)
{
	asd->params.config.dp_config = dp_config;
}

void atomisp_css_set_de_config(struct atomisp_sub_device *asd,
			struct atomisp_css_de_config *de_config)
{
	asd->params.config.de_config = de_config;
}

void atomisp_css_set_default_de_config(struct atomisp_sub_device *asd)
{
	asd->params.config.de_config = NULL;
}

void atomisp_css_set_ce_config(struct atomisp_sub_device *asd,
			struct atomisp_css_ce_config *ce_config)
{
	asd->params.config.ce_config = ce_config;
}

void atomisp_css_set_nr_config(struct atomisp_sub_device *asd,
			struct atomisp_css_nr_config *nr_config)
{
	asd->params.config.nr_config = nr_config;
}

void atomisp_css_set_ee_config(struct atomisp_sub_device *asd,
			struct atomisp_css_ee_config *ee_config)
{
	asd->params.config.ee_config = ee_config;
}

void atomisp_css_set_tnr_config(struct atomisp_sub_device *asd,
			struct atomisp_css_tnr_config *tnr_config)
{
	asd->params.config.tnr_config = tnr_config;
}

void atomisp_css_set_cc_config(struct atomisp_sub_device *asd,
			struct atomisp_css_cc_config *cc_config)
{
	asd->params.config.cc_config = cc_config;
}

void atomisp_css_set_macc_table(struct atomisp_sub_device *asd,
			struct atomisp_css_macc_table *macc_table)
{
	asd->params.config.macc_table = macc_table;
}

void atomisp_css_set_macc_config(struct atomisp_sub_device *asd,
			struct atomisp_css_macc_config *macc_config)
{
	asd->params.config.macc_config = macc_config;
}

void atomisp_css_set_ecd_config(struct atomisp_sub_device *asd,
			struct atomisp_css_ecd_config *ecd_config)
{
	asd->params.config.ecd_config = ecd_config;
}

void atomisp_css_set_ynr_config(struct atomisp_sub_device *asd,
			struct atomisp_css_ynr_config *ynr_config)
{
	asd->params.config.ynr_config = ynr_config;
}

void atomisp_css_set_fc_config(struct atomisp_sub_device *asd,
			struct atomisp_css_fc_config *fc_config)
{
	asd->params.config.fc_config = fc_config;
}

void atomisp_css_set_ctc_config(struct atomisp_sub_device *asd,
			struct atomisp_css_ctc_config *ctc_config)
{
	asd->params.config.ctc_config = ctc_config;
}

void atomisp_css_set_cnr_config(struct atomisp_sub_device *asd,
			struct atomisp_css_cnr_config *cnr_config)
{
	asd->params.config.cnr_config = cnr_config;
}

void atomisp_css_set_aa_config(struct atomisp_sub_device *asd,
			struct atomisp_css_aa_config *aa_config)
{
	asd->params.config.aa_config = aa_config;
}

void atomisp_css_set_baa_config(struct atomisp_sub_device *asd,
			struct atomisp_css_baa_config *baa_config)
{
	asd->params.config.baa_config = baa_config;
}

void atomisp_css_set_anr_config(struct atomisp_sub_device *asd,
			struct atomisp_css_anr_config *anr_config)
{
	asd->params.config.anr_config = anr_config;
}

void atomisp_css_set_xnr_config(struct atomisp_sub_device *asd,
			struct atomisp_css_xnr_config *xnr_config)
{
	asd->params.config.xnr_config = xnr_config;
}

void atomisp_css_set_yuv2rgb_cc_config(struct atomisp_sub_device *asd,
			struct atomisp_css_cc_config *yuv2rgb_cc_config)
{
	asd->params.config.yuv2rgb_cc_config = yuv2rgb_cc_config;
}

void atomisp_css_set_rgb2yuv_cc_config(struct atomisp_sub_device *asd,
			struct atomisp_css_cc_config *rgb2yuv_cc_config)
{
	asd->params.config.rgb2yuv_cc_config = rgb2yuv_cc_config;
}

void atomisp_css_set_xnr_table(struct atomisp_sub_device *asd,
			struct atomisp_css_xnr_table *xnr_table)
{
	asd->params.config.xnr_table = xnr_table;
}

void atomisp_css_set_r_gamma_table(struct atomisp_sub_device *asd,
			struct atomisp_css_rgb_gamma_table *r_gamma_table)
{
	asd->params.config.r_gamma_table = r_gamma_table;
}

void atomisp_css_set_g_gamma_table(struct atomisp_sub_device *asd,
			struct atomisp_css_rgb_gamma_table *g_gamma_table)
{
	asd->params.config.g_gamma_table = g_gamma_table;
}

void atomisp_css_set_b_gamma_table(struct atomisp_sub_device *asd,
			struct atomisp_css_rgb_gamma_table *b_gamma_table)
{
	asd->params.config.b_gamma_table = b_gamma_table;
}

void atomisp_css_set_gamma_table(struct atomisp_sub_device *asd,
			struct atomisp_css_gamma_table *gamma_table)
{
	asd->params.config.gamma_table = gamma_table;
}

void atomisp_css_set_ctc_table(struct atomisp_sub_device *asd,
			struct atomisp_css_ctc_table *ctc_table)
{
	int i;
	uint16_t *vamem_ptr = ctc_table->data.vamem_1;
	int data_size = IA_CSS_VAMEM_1_CTC_TABLE_SIZE;
	bool valid = false;

	/* workaround: if ctc_table is all 0, do not apply it */
	if (ctc_table->vamem_type == IA_CSS_VAMEM_TYPE_2) {
		vamem_ptr = ctc_table->data.vamem_2;
		data_size = IA_CSS_VAMEM_2_CTC_TABLE_SIZE;
	}

	for (i = 0; i < data_size; i++) {
		if (*(vamem_ptr + i)) {
			valid = true;
			break;
		}
	}

	if (valid)
		asd->params.config.ctc_table = ctc_table;
	else
		dev_warn(asd->isp->dev, "Bypass the invalid ctc_table.\n");
}

void atomisp_css_set_anr_thres(struct atomisp_sub_device *asd,
			struct atomisp_css_anr_thres *anr_thres)
{
	asd->params.config.anr_thres = anr_thres;
}

void atomisp_css_set_dvs_6axis(struct atomisp_sub_device *asd,
			struct atomisp_css_dvs_6axis *dvs_6axis)
{
	asd->params.config.dvs_6axis_config = dvs_6axis;
}

void atomisp_css_set_gc_config(struct atomisp_sub_device *asd,
			struct atomisp_css_gc_config *gc_config)
{
	asd->params.config.gc_config = gc_config;
}

void atomisp_css_set_3a_config(struct atomisp_sub_device *asd,
			struct atomisp_css_3a_config *s3a_config)
{
	asd->params.config.s3a_config = s3a_config;
}

void atomisp_css_video_set_dis_vector(struct atomisp_sub_device *asd,
				struct atomisp_dis_vector *vector)
{
	if (!asd->params.config.motion_vector)
		asd->params.config.motion_vector = &asd->params.motion_vector;

	memset(asd->params.config.motion_vector,
			0, sizeof(struct ia_css_vector));
	asd->params.motion_vector.x = vector->x;
	asd->params.motion_vector.y = vector->y;
}

static int atomisp_compare_dvs_grid(struct atomisp_sub_device *asd,
				struct atomisp_dvs_grid_info *atomgrid)
{
	struct atomisp_css_dvs_grid_info *cur =
	    &asd->params.curr_grid_info.dvs_grid;

	if (sizeof(*cur) != sizeof(*atomgrid)) {
		dev_err(asd->isp->dev, "dvs grid mis-match!\n");
		return -EINVAL;
	}

	if (!cur->enable) {
		dev_err(asd->isp->dev, "dvs not enabled!\n");
		return -EINVAL;
	}

	return memcmp(atomgrid, cur, sizeof(*cur));
}

int atomisp_css_set_dis_coefs(struct atomisp_sub_device *asd,
			  struct atomisp_dis_coefficients *coefs)
{
	if (atomisp_compare_dvs_grid(asd, &coefs->grid_info) != 0)
		/* If the grid info in the argument differs from the current
		   grid info, we tell the caller to reset the grid size and
		   try again. */
		return -EAGAIN;

	if (coefs->hor_coefs.odd_real == NULL ||
	    coefs->hor_coefs.odd_imag == NULL ||
	    coefs->hor_coefs.even_real == NULL ||
	    coefs->hor_coefs.even_imag == NULL ||
	    coefs->ver_coefs.odd_real == NULL ||
	    coefs->ver_coefs.odd_imag == NULL ||
	    coefs->ver_coefs.even_real == NULL ||
	    coefs->ver_coefs.even_imag == NULL ||
	    asd->params.dvs_coeff->hor_coefs.odd_real == NULL ||
	    asd->params.dvs_coeff->hor_coefs.odd_imag == NULL ||
	    asd->params.dvs_coeff->hor_coefs.even_real == NULL ||
	    asd->params.dvs_coeff->hor_coefs.even_imag == NULL ||
	    asd->params.dvs_coeff->ver_coefs.odd_real == NULL ||
	    asd->params.dvs_coeff->ver_coefs.odd_imag == NULL ||
	    asd->params.dvs_coeff->ver_coefs.even_real == NULL ||
	    asd->params.dvs_coeff->ver_coefs.even_imag == NULL)
		return -EINVAL;

	if (copy_from_user(asd->params.dvs_coeff->hor_coefs.odd_real,
	    coefs->hor_coefs.odd_real, asd->params.dvs_hor_coef_bytes))
		return -EFAULT;
	if (copy_from_user(asd->params.dvs_coeff->hor_coefs.odd_imag,
	    coefs->hor_coefs.odd_imag, asd->params.dvs_hor_coef_bytes))
		return -EFAULT;
	if (copy_from_user(asd->params.dvs_coeff->hor_coefs.even_real,
	    coefs->hor_coefs.even_real, asd->params.dvs_hor_coef_bytes))
		return -EFAULT;
	if (copy_from_user(asd->params.dvs_coeff->hor_coefs.even_imag,
	    coefs->hor_coefs.even_imag, asd->params.dvs_hor_coef_bytes))
		return -EFAULT;

	if (copy_from_user(asd->params.dvs_coeff->ver_coefs.odd_real,
	    coefs->ver_coefs.odd_real, asd->params.dvs_ver_coef_bytes))
		return -EFAULT;
	if (copy_from_user(asd->params.dvs_coeff->ver_coefs.odd_imag,
	    coefs->ver_coefs.odd_imag, asd->params.dvs_ver_coef_bytes))
		return -EFAULT;
	if (copy_from_user(asd->params.dvs_coeff->ver_coefs.even_real,
	    coefs->ver_coefs.even_real, asd->params.dvs_ver_coef_bytes))
		return -EFAULT;
	if (copy_from_user(asd->params.dvs_coeff->ver_coefs.even_imag,
	    coefs->ver_coefs.even_imag, asd->params.dvs_ver_coef_bytes))
		return -EFAULT;

	asd->params.config.dvs2_coefs = asd->params.dvs_coeff;
	/* FIXME! */
/*	asd->params.dis_proj_data_valid = false; */
	asd->params.css_update_params_needed = true;

	return 0;
}

void atomisp_css_set_zoom_factor(struct atomisp_sub_device *asd,
					unsigned int zoom)
{
	struct atomisp_device *isp = asd->isp;

	if (!asd->params.config.dz_config)
		asd->params.config.dz_config = &asd->params.dz_config;

	if (zoom == asd->params.config.dz_config->dx &&
		 zoom == asd->params.config.dz_config->dy) {
		dev_dbg(isp->dev, "same zoom scale. skipped.\n");
		return;
	}

	memset(asd->params.config.dz_config, 0,
		sizeof(struct ia_css_dz_config));
	asd->params.dz_config.dx = zoom;
	asd->params.dz_config.dy = zoom;
}

int atomisp_css_get_wb_config(struct atomisp_sub_device *asd,
			struct atomisp_wb_config *config)
{
	struct atomisp_css_wb_config wb_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&wb_config, 0, sizeof(struct atomisp_css_wb_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.wb_config = &wb_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, &wb_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_ob_config(struct atomisp_sub_device *asd,
			struct atomisp_ob_config *config)
{
	struct atomisp_css_ob_config ob_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&ob_config, 0, sizeof(struct atomisp_css_ob_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.ob_config = &ob_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, &ob_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_dp_config(struct atomisp_sub_device *asd,
			struct atomisp_dp_config *config)
{
	struct atomisp_css_dp_config dp_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&dp_config, 0, sizeof(struct atomisp_css_dp_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.dp_config = &dp_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, &dp_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_de_config(struct atomisp_sub_device *asd,
			struct atomisp_de_config *config)
{
	struct atomisp_css_de_config de_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&de_config, 0, sizeof(struct atomisp_css_de_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.de_config = &de_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, &de_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_nr_config(struct atomisp_sub_device *asd,
			struct atomisp_nr_config *config)
{
	struct atomisp_css_nr_config nr_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&nr_config, 0, sizeof(struct atomisp_css_nr_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));

	isp_config.nr_config = &nr_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, &nr_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_ee_config(struct atomisp_sub_device *asd,
			struct atomisp_ee_config *config)
{
	struct atomisp_css_ee_config ee_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			 __func__);
		return -EINVAL;
	}
	memset(&ee_config, 0, sizeof(struct atomisp_css_ee_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.ee_config = &ee_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, &ee_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_tnr_config(struct atomisp_sub_device *asd,
			struct atomisp_tnr_config *config)
{
	struct atomisp_css_tnr_config tnr_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&tnr_config, 0, sizeof(struct atomisp_css_tnr_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.tnr_config = &tnr_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, &tnr_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_ctc_table(struct atomisp_sub_device *asd,
			struct atomisp_ctc_table *config)
{
	struct atomisp_css_ctc_table *tab;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}

	tab = vzalloc(sizeof(struct atomisp_css_ctc_table));
	if (!tab)
		return -ENOMEM;

	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.ctc_table = tab;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, tab, sizeof(*tab));
	vfree(tab);

	return 0;
}

int atomisp_css_get_gamma_table(struct atomisp_sub_device *asd,
			struct atomisp_gamma_table *config)
{
	struct atomisp_css_gamma_table *tab;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}

	tab = vzalloc(sizeof(struct atomisp_css_gamma_table));
	if (!tab)
		return -ENOMEM;

	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.gamma_table = tab;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	memcpy(config, tab, sizeof(*tab));
	vfree(tab);

	return 0;
}

int atomisp_css_get_gc_config(struct atomisp_sub_device *asd,
			struct atomisp_gc_config *config)
{
	struct atomisp_css_gc_config gc_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&gc_config, 0, sizeof(struct atomisp_css_gc_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.gc_config = &gc_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	/* Get gamma correction params from current setup */
	memcpy(config, &gc_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_3a_config(struct atomisp_sub_device *asd,
			struct atomisp_3a_config *config)
{
	struct atomisp_css_3a_config s3a_config;
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&s3a_config, 0, sizeof(struct atomisp_css_3a_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.s3a_config = &s3a_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	/* Get white balance from current setup */
	memcpy(config, &s3a_config, sizeof(*config));

	return 0;
}

int atomisp_css_get_zoom_factor(struct atomisp_sub_device *asd,
					unsigned int *zoom)
{
	struct ia_css_dz_config dz_config;  /**< Digital Zoom */
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev, "%s called after streamoff, skipping.\n",
			__func__);
		return -EINVAL;
	}
	memset(&dz_config, 0, sizeof(struct ia_css_dz_config));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.dz_config = &dz_config;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
	*zoom = dz_config.dx;

	return 0;
}


/*
 * Function to set/get image stablization statistics
 */
int atomisp_css_get_dis_stat(struct atomisp_sub_device *asd,
			 struct atomisp_dis_statistics *stats)
{
	struct atomisp_device *isp = asd->isp;
	unsigned long flags;

	if (asd->params.dvs_stat->hor_prod.odd_real == NULL ||
	    asd->params.dvs_stat->hor_prod.odd_imag == NULL ||
	    asd->params.dvs_stat->hor_prod.even_real == NULL ||
	    asd->params.dvs_stat->hor_prod.even_imag == NULL ||
	    asd->params.dvs_stat->ver_prod.odd_real == NULL ||
	    asd->params.dvs_stat->ver_prod.odd_imag == NULL ||
	    asd->params.dvs_stat->ver_prod.even_real == NULL ||
	    asd->params.dvs_stat->ver_prod.even_imag == NULL)
		return -EINVAL;

	/* isp needs to be streaming to get DIS statistics */
	spin_lock_irqsave(&isp->lock, flags);
	if (asd->streaming != ATOMISP_DEVICE_STREAMING_ENABLED) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&isp->lock, flags);

	if (atomisp_compare_dvs_grid(asd, &stats->dvs2_stat.grid_info) != 0)
		/* If the grid info in the argument differs from the current
		   grid info, we tell the caller to reset the grid size and
		   try again. */
		return -EAGAIN;

	if (!asd->params.dis_proj_data_valid)
		return -EBUSY;

	stats->exp_id = asd->params.exp_id;
	if (copy_to_user(stats->dvs2_stat.ver_prod.odd_real,
			 asd->params.dvs_stat->ver_prod.odd_real,
			 asd->params.dvs_ver_proj_bytes))
		return -EFAULT;
	if (copy_to_user(stats->dvs2_stat.ver_prod.odd_imag,
			 asd->params.dvs_stat->ver_prod.odd_imag,
			 asd->params.dvs_ver_proj_bytes))
		return -EFAULT;
	if (copy_to_user(stats->dvs2_stat.ver_prod.even_real,
			 asd->params.dvs_stat->ver_prod.even_real,
			 asd->params.dvs_ver_proj_bytes))
		return -EFAULT;
	if (copy_to_user(stats->dvs2_stat.ver_prod.even_imag,
			 asd->params.dvs_stat->ver_prod.even_imag,
			 asd->params.dvs_ver_proj_bytes))
		return -EFAULT;
	if (copy_to_user(stats->dvs2_stat.hor_prod.odd_real,
			 asd->params.dvs_stat->hor_prod.odd_real,
			 asd->params.dvs_hor_proj_bytes))
		return -EFAULT;
	if (copy_to_user(stats->dvs2_stat.hor_prod.odd_imag,
			 asd->params.dvs_stat->hor_prod.odd_imag,
			 asd->params.dvs_hor_proj_bytes))
		return -EFAULT;
	if (copy_to_user(stats->dvs2_stat.hor_prod.even_real,
			 asd->params.dvs_stat->hor_prod.even_real,
			 asd->params.dvs_hor_proj_bytes))
		return -EFAULT;
	if (copy_to_user(stats->dvs2_stat.hor_prod.even_imag,
			 asd->params.dvs_stat->hor_prod.even_imag,
			 asd->params.dvs_hor_proj_bytes))
		return -EFAULT;

	return 0;

}

struct atomisp_css_shading_table *atomisp_css_shading_table_alloc(
				unsigned int width, unsigned int height)
{
	return ia_css_shading_table_alloc(width, height);
}

void atomisp_css_set_shading_table(struct atomisp_sub_device *asd,
			struct atomisp_css_shading_table *table)
{
	asd->params.config.shading_table = table;
}

void atomisp_css_shading_table_free(struct atomisp_css_shading_table *table)
{
	ia_css_shading_table_free(table);
}

struct atomisp_css_morph_table *atomisp_css_morph_table_allocate(
				unsigned int width, unsigned int height)
{
	return ia_css_morph_table_allocate(width, height);
}

void atomisp_css_set_morph_table(struct atomisp_sub_device *asd,
					struct atomisp_css_morph_table *table)
{
	asd->params.config.morph_table = table;
}

void atomisp_css_get_morph_table(struct atomisp_sub_device *asd,
				struct atomisp_css_morph_table *table)
{
	struct ia_css_isp_config isp_config;
	struct atomisp_device *isp = asd->isp;

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(isp->dev,
			"%s called after streamoff, skipping.\n", __func__);
		return;
	}
	memset(table, 0, sizeof(struct atomisp_css_morph_table));
	memset(&isp_config, 0, sizeof(struct ia_css_isp_config));
	isp_config.morph_table = table;
	ia_css_stream_get_isp_config(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
		&isp_config);
}

void atomisp_css_morph_table_free(struct atomisp_css_morph_table *table)
{
	ia_css_morph_table_free(table);
}

void atomisp_css_set_cont_prev_start_time(struct atomisp_device *isp,
					unsigned int overlap)
{
	/* CSS 2.0 doesn't support this API. */
	dev_info(isp->dev, "set cont prev start time is not supported.\n");
	return;
}

void atomisp_css_acc_done(struct atomisp_sub_device *asd)
{
	complete(&asd->isp->acc.acc_done);
}

int atomisp_css_wait_acc_finish(struct atomisp_sub_device *asd)
{
	int ret = 0;
	struct atomisp_device *isp = asd->isp;

	/* Unlock the isp mutex taken in IOCTL handler before sleeping! */
	mutex_unlock(&isp->mutex);
	if (wait_for_completion_interruptible_timeout(&isp->acc.acc_done,
					ATOMISP_ISP_TIMEOUT_DURATION) == 0) {
		unsigned int old_dbglevel = dbg_level;

		dbg_level = 6;
		dev_err(isp->dev, "<%s: completion timeout\n", __func__);
		atomisp_css_debug_set_dtrace_level(
						CSS_DTRACE_VERBOSITY_TIMEOUT);
		atomisp_css_debug_dump_sp_sw_debug_info();
		atomisp_css_debug_dump_debug_info(__func__);
		atomisp_css_debug_set_dtrace_level(CSS_DTRACE_VERBOSITY_LEVEL);
		dbg_level = old_dbglevel;
		ret = -EIO;
	}
	mutex_lock(&isp->mutex);

	return ret;
}

/* Set the ACC binary arguments */
int atomisp_css_set_acc_parameters(struct atomisp_acc_fw *acc_fw)
{
#ifndef CSS21
	struct ia_css_data sec;
#endif /* !CSS21 */
	unsigned int mem;

	for (mem = 0; mem < ATOMISP_ACC_NR_MEMORY; mem++) {
		if (acc_fw->args[mem].length == 0)
			continue;

#ifndef CSS21
		sec.address = acc_fw->args[mem].css_ptr;
		sec.size = acc_fw->args[mem].length;
		if (sh_css_acc_set_firmware_parameters(acc_fw->fw, mem, sec)
			!= IA_CSS_SUCCESS)
			return -EIO;
#else /* !CSS21 */
		ia_css_isp_param_set_css_mem_init(&acc_fw->fw->mem_initializers,
						IA_CSS_PARAM_CLASS_PARAM, mem,
						acc_fw->args[mem].css_ptr,
						acc_fw->args[mem].length);
#endif /* !CSS21 */
	}

	return 0;
}

/* Load acc binary extension */
int atomisp_css_load_acc_extension(struct atomisp_sub_device *asd,
					struct atomisp_css_fw_info *fw,
					enum atomisp_css_pipe_id pipe_id,
					unsigned int type)
{
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.pipe_configs[pipe_id].acc_extension = fw;
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.update_pipe[pipe_id] = true;
	return 0;
}

/* Unload acc binary extension */
void atomisp_css_unload_acc_extension(struct atomisp_sub_device *asd,
					struct atomisp_css_fw_info *fw,
					enum atomisp_css_pipe_id pipe_id)
{
	asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.pipe_configs[pipe_id].acc_extension = NULL;
}

int atomisp_css_create_acc_pipe(struct atomisp_sub_device *asd)
{
	struct atomisp_device *isp = asd->isp;
	struct ia_css_pipe_config *pipe_config;
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];

	if (stream_env->acc_stream) {
		if (stream_env->acc_stream_state == CSS_STREAM_STARTED) {
			if (ia_css_stream_stop(stream_env->acc_stream)
				!= IA_CSS_SUCCESS) {
				dev_err(isp->dev, "stop acc_stream failed.\n");
				return -EBUSY;
			}
		}

		if (ia_css_stream_destroy(stream_env->acc_stream)
			!= IA_CSS_SUCCESS) {
			dev_err(isp->dev, "destroy acc_stream failed.\n");
			return -EBUSY;
		}
		stream_env->acc_stream = NULL;
	}

	pipe_config = &stream_env->pipe_configs[CSS_PIPE_ID_ACC];
	ia_css_pipe_config_defaults(pipe_config);
	isp->acc.acc_stages = kzalloc(MAX_ACC_STAGES *
				sizeof(void *), GFP_KERNEL);
	if (!isp->acc.acc_stages)
		return -ENOMEM;
	pipe_config->acc_stages = isp->acc.acc_stages;
	pipe_config->mode = IA_CSS_PIPE_MODE_ACC;
	pipe_config->num_acc_stages = 0;

	/*
	 * We delay the ACC pipeline creation to atomisp_css_start_acc_pipe,
	 * because pipe configuration will soon be changed by
	 * atomisp_css_load_acc_binary()
	 */
	return 0;
}

int atomisp_css_start_acc_pipe(struct atomisp_sub_device *asd)
{
	struct atomisp_device *isp = asd->isp;
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	struct ia_css_pipe_config *pipe_config =
			&stream_env->pipe_configs[IA_CSS_PIPE_ID_ACC];

	if (ia_css_pipe_create(pipe_config,
		&stream_env->pipes[IA_CSS_PIPE_ID_ACC]) != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "%s: ia_css_pipe_create failed\n",
				__func__);
		return -EBADE;
	}

	memset(&stream_env->acc_stream_config, 0,
		sizeof(struct ia_css_stream_config));
	if (ia_css_stream_create(&stream_env->acc_stream_config, 1,
				&stream_env->pipes[IA_CSS_PIPE_ID_ACC],
				&stream_env->acc_stream) != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "%s: create acc_stream error.\n", __func__);
		return -EINVAL;
	}
	stream_env->acc_stream_state = CSS_STREAM_CREATED;

	init_completion(&isp->acc.acc_done);
	isp->acc.pipeline = stream_env->pipes[IA_CSS_PIPE_ID_ACC];

	atomisp_freq_scaling(isp, ATOMISP_DFS_MODE_MAX);

	if (ia_css_start_sp() != IA_CSS_SUCCESS) {
		dev_err(isp->dev, "start sp error.\n");
		return -EIO;
	}

	if (ia_css_stream_start(stream_env->acc_stream)
		!= IA_CSS_SUCCESS) {
		dev_err(isp->dev, "acc_stream start error.\n");
		return -EIO;
	}

	stream_env->acc_stream_state = CSS_STREAM_STARTED;
	return 0;
}

int atomisp_css_stop_acc_pipe(struct atomisp_sub_device *asd)
{
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	if (stream_env->acc_stream_state == CSS_STREAM_STARTED) {
		ia_css_stream_stop(stream_env->acc_stream);
		stream_env->acc_stream_state = CSS_STREAM_STOPPED;
	}
	return 0;
}

void atomisp_css_destroy_acc_pipe(struct atomisp_sub_device *asd)
{
	struct atomisp_stream_env *stream_env =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	if (stream_env->acc_stream) {
		if (ia_css_stream_destroy(stream_env->acc_stream)
		    != IA_CSS_SUCCESS)
			dev_warn(asd->isp->dev,
				"destroy acc_stream failed.\n");
		stream_env->acc_stream = NULL;
	}

	if (stream_env->pipes[IA_CSS_PIPE_ID_ACC]) {
		if (ia_css_pipe_destroy(stream_env->pipes[IA_CSS_PIPE_ID_ACC])
			!= IA_CSS_SUCCESS)
			dev_warn(asd->isp->dev,
				"destroy ACC pipe failed.\n");
		stream_env->pipes[IA_CSS_PIPE_ID_ACC] = NULL;
		stream_env->update_pipe[IA_CSS_PIPE_ID_ACC] = false;
		ia_css_pipe_config_defaults(
			&stream_env->pipe_configs[IA_CSS_PIPE_ID_ACC]);
		ia_css_pipe_extra_config_defaults(
			&stream_env->pipe_extra_configs[IA_CSS_PIPE_ID_ACC]);
	}
	asd->isp->acc.pipeline = NULL;

	/* css 2.0 API limitation: ia_css_stop_sp() could be only called after
	 * destroy all pipes
	 */
	if (ia_css_isp_has_started())
		ia_css_stop_sp();

	kfree(asd->isp->acc.acc_stages);
	asd->isp->acc.acc_stages = NULL;

	atomisp_freq_scaling(asd->isp, ATOMISP_DFS_MODE_LOW);

	/* Force power cycling when binary finished */
	atomisp_css_suspend();
	if (pm_runtime_put_sync(asd->isp->dev) < 0)
		dev_err(asd->isp->dev, "can not disable ISP power\n");
	else if (pm_runtime_get_sync(asd->isp->dev) < 0)
		dev_err(asd->isp->dev, "can not enable ISP power\n");
	atomisp_css_resume(asd->isp);
}

int atomisp_css_load_acc_binary(struct atomisp_sub_device *asd,
					struct atomisp_css_fw_info *fw,
					unsigned int index)
{
	struct ia_css_pipe_config *pipe_config =
			&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
			.pipe_configs[IA_CSS_PIPE_ID_ACC];

	if (index >= MAX_ACC_STAGES) {
		dev_dbg(asd->isp->dev, "%s: index(%d) out of range\n",
				__func__, index);
		return -ENOMEM;
	}

	pipe_config->acc_stages[index] = fw;
	pipe_config->num_acc_stages = index + 1;
#ifdef CSS21
	pipe_config->acc_num_execs = 1;
#endif

	return 0;
}

static struct atomisp_sub_device *__get_atomisp_subdev(
					struct ia_css_pipe *css_pipe,
					struct atomisp_device *isp,
					enum atomisp_input_stream_id *stream_id) {
	int i, j, k;
	struct atomisp_sub_device *asd;
	struct atomisp_stream_env *stream_env;

	for (i = 0; i < isp->num_of_streams; i++) {
		asd = &isp->asd[i];
		if (asd->streaming == ATOMISP_DEVICE_STREAMING_DISABLED &&
		    !isp->acc.pipeline)
			continue;
		for (j = 0; j < ATOMISP_INPUT_STREAM_NUM; j++) {
			stream_env = &asd->stream_env[j];
			for (k = 0; k < IA_CSS_PIPE_ID_NUM; k++) {
				if (stream_env->pipes[k] &&
					stream_env->pipes[k] == css_pipe) {
						*stream_id = j;
						return asd;
					}
				}
		}
	}

	return NULL;
}

int atomisp_css_isr_thread(struct atomisp_device *isp,
			   bool *frame_done_found,
			   bool *css_pipe_done,
			   bool *reset_wdt_timer)
{
	enum atomisp_input_stream_id stream_id = 0;
	struct atomisp_css_event current_event;
	struct atomisp_sub_device *asd = &isp->asd[0];

	while (!atomisp_css_dequeue_event(&current_event)) {
		atomisp_css_temp_pipe_to_pipe_id(&current_event);
		asd = __get_atomisp_subdev(current_event.event.pipe,
						  isp, &stream_id);
		if (!asd) {
			/* EOF Event does not have the css_pipe returned */
			if (current_event.event.type !=
			    IA_CSS_EVENT_TYPE_PORT_EOF) {
				dev_err(isp->dev, "%s:no subdev. event:%d",
					 __func__, current_event.event.type);
				return -EINVAL;
			}
		}

		switch (current_event.event.type) {
		case CSS_EVENT_OUTPUT_FRAME_DONE:
			frame_done_found[asd->index] = true;
			atomisp_buf_done(asd, 0, CSS_BUFFER_TYPE_OUTPUT_FRAME,
					 current_event.pipe, true, stream_id);
			*reset_wdt_timer = true; /* ISP running */
			break;
		case CSS_EVENT_3A_STATISTICS_DONE:
			atomisp_buf_done(asd, 0,
					 CSS_BUFFER_TYPE_3A_STATISTICS,
					 current_event.pipe,
					 css_pipe_done[asd->index], stream_id);
			break;
		case CSS_EVENT_VF_OUTPUT_FRAME_DONE:
			atomisp_buf_done(asd, 0,
					 CSS_BUFFER_TYPE_VF_OUTPUT_FRAME,
					 current_event.pipe, true, stream_id);
			*reset_wdt_timer = true; /* ISP running */
			break;
		case CSS_EVENT_DIS_STATISTICS_DONE:
			atomisp_buf_done(asd, 0,
					 CSS_BUFFER_TYPE_DIS_STATISTICS,
					 current_event.pipe,
					 css_pipe_done[asd->index], stream_id);
			break;
		case CSS_EVENT_PIPELINE_DONE:
			css_pipe_done[asd->index] = true;
			break;
		case CSS_EVENT_PORT_EOF:
			break;
		default:
			dev_err(isp->dev, "unhandled css stored event: 0x%x\n",
					current_event.event.type);
			break;
		}
	}
	return 0;
}

void atomisp_set_stop_timeout(unsigned int timeout)
{
	return;
}
