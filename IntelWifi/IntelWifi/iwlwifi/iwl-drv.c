/******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2007 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110,
 * USA
 *
 * The full GNU General Public License is included in this distribution
 * in the file called COPYING.
 *
 * Contact Information:
 *  Intel Linux Wireless <linuxwifi@intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 * BSD LICENSE
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/
#include <linux/module.h>

#include "iwl-drv.h"
#include "iwl-csr.h"
//#include "iwl-debug.h"
#include "iwl-trans.h"
#include "iwl-op-mode.h"
#include "iwl-agn-hw.h"

#include "iwl-config.h"
#include "iwl-modparams.h"

#include "firmware_file.h"


struct firmware {
    size_t size;
    const u8 *data;
    
    /* firmware loader private fields */
    void *priv;
};


/******************************************************************************
 *
 * module boiler plate
 *
 ******************************************************************************/

#define DRV_DESCRIPTION	"Intel(R) Wireless WiFi driver for Linux"
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_COPYRIGHT " " DRV_AUTHOR);
MODULE_LICENSE("GPL");

#ifdef CONFIG_IWLWIFI_DEBUGFS
static struct dentry *iwl_dbgfs_root;
#endif



enum {
	DVM_OP_MODE,
	MVM_OP_MODE,
};

/* Protects the table contents, i.e. the ops pointer & drv list */
//static struct mutex iwlwifi_opmode_table_mtx;
static IOLock *iwlwifi_opmode_table_mtx;
static struct iwlwifi_opmode_table {
	const char *name;			/* name: iwldvm, iwlmvm, etc */
	const struct iwl_op_mode_ops *ops;	/* pointer to op_mode ops */
    STAILQ_HEAD(, iwl_drv) drv; /* list of devices using this op_mode */
} iwlwifi_opmode_table[] = {		/* ops set when driver is initialized */
	[DVM_OP_MODE] = { .name = "iwldvm", .ops = NULL },
	[MVM_OP_MODE] = { .name = "iwlmvm", .ops = NULL },
};

#define IWL_DEFAULT_SCAN_CHANNELS 40

/*
 * struct fw_sec: Just for the image parsing process.
 * For the fw storage we are using struct fw_desc.
 */
struct fw_sec {
	const void *data;		/* the sec data */
	size_t size;			/* section size */
	u32 offset;			/* offset of writing in the device */
};

static void iwl_free_fw_desc(struct iwl_drv *drv, struct fw_desc *desc)
{
    IOFree(desc->data, desc->len);
	desc->data = NULL;
	desc->len = 0;
}

static void iwl_free_fw_img(struct iwl_drv *drv, struct fw_img *img)
{
    int i;
    for (i = 0; i < img->num_sec; i++)
        iwl_free_fw_desc(drv, &img->sec[i]);
    IOFree(img->sec, sizeof(*img->sec) * img->num_sec);
}

static void iwl_dealloc_ucode(struct iwl_drv *drv)
{
	int i;

    IOFree(drv->fw.dbg_dest_tlv, sizeof(struct iwl_fw_dbg_dest_tlv));
    for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_conf_tlv); i++){
        IOFree(drv->fw.dbg_conf_tlv[i], sizeof(struct iwl_fw_dbg_conf_tlv));
    }
		
    for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_trigger_tlv); i++){
        IOFree(drv->fw.dbg_trigger_tlv[i], sizeof(struct iwl_fw_dbg_trigger_tlv));
    }

    IOFree(drv->fw.dbg_mem_tlv, sizeof(struct iwl_fw_dbg_mem_seg_tlv));

	for (i = 0; i < IWL_UCODE_TYPE_MAX; i++)
		iwl_free_fw_img(drv, drv->fw.img + i);
}

static int iwl_alloc_fw_desc(struct iwl_drv *drv, struct fw_desc *desc,
			     struct fw_sec *sec)
{
	void *data;

	desc->data = NULL;

	if (!sec || !sec->size)
		return -EINVAL;

    data = IOMalloc(sec->size);
	if (!data)
		return -ENOMEM;

	desc->len = sec->size;
	desc->offset = sec->offset;
	memcpy(data, sec->data, desc->len);
	desc->data = data;

	return 0;
}

static void iwl_req_fw_callback(const struct firmware *ucode_raw,
				void *context);

static int request_firmware_nowait(void *context,
                                   void (*cont)(const struct firmware *fw, void *context)) {
    struct firmware* fw = (struct firmware *)IOMalloc(sizeof(struct firmware));
    fw->size = iwlwifi_6000g2b_6_ucode_len;
    fw->data = iwlwifi_6000g2b_6_ucode;
    
    cont(fw, context);
    
    return 0;
}


static int iwl_request_firmware(struct iwl_drv *drv, bool first)
{
	const struct iwl_cfg *cfg = drv->trans->cfg;
	char tag[8];
	const char *fw_pre_name;

	if (drv->trans->cfg->device_family == IWL_DEVICE_FAMILY_9000 &&
	    (CSR_HW_REV_STEP(drv->trans->hw_rev) == SILICON_B_STEP ||
	     CSR_HW_REV_STEP(drv->trans->hw_rev) == SILICON_C_STEP))
		fw_pre_name = cfg->fw_name_pre_b_or_c_step;
	else if (drv->trans->cfg->integrated &&
		 CSR_HW_RFID_STEP(drv->trans->hw_rf_id) == SILICON_B_STEP &&
		 cfg->fw_name_pre_rf_next_step)
		fw_pre_name = cfg->fw_name_pre_rf_next_step;
	else
		fw_pre_name = cfg->fw_name_pre;

	if (first) {
		drv->fw_index = cfg->ucode_api_max;
		snprintf(tag, 8, "%d", drv->fw_index);
	} else {
		drv->fw_index--;
		snprintf(tag, 8, "%d", drv->fw_index);
	}

	if (drv->fw_index < cfg->ucode_api_min) {
		IWL_ERR(drv, "no suitable firmware found!\n");

		if (cfg->ucode_api_min == cfg->ucode_api_max) {
			IWL_ERR(drv, "%s%d is required\n", fw_pre_name,
				cfg->ucode_api_max);
		} else {
			IWL_ERR(drv, "minimum version required: %s%d\n",
				fw_pre_name,
				cfg->ucode_api_min);
			IWL_ERR(drv, "maximum version supported: %s%d\n",
				fw_pre_name,
				cfg->ucode_api_max);
		}

		IWL_ERR(drv,
			"check git://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git\n");
		return -ENOENT;
	}

	snprintf(drv->firmware_name, sizeof(drv->firmware_name), "%s%s.ucode",
		 fw_pre_name, tag);

    IWL_DEBUG_INFO(drv, "attempting to load firmware '%s'\n",
               drv->firmware_name);

	return request_firmware_nowait(drv, iwl_req_fw_callback);
}



struct fw_img_parsing {
	struct fw_sec *sec;
	int sec_counter;
};

/*
 * struct fw_sec_parsing: to extract fw section and it's offset from tlv
 */
struct fw_sec_parsing {
	__le32 offset;
	const u8 data[];
} __packed;

/**
 * struct iwl_tlv_calib_data - parse the default calib data from TLV
 *
 * @ucode_type: the uCode to which the following default calib relates.
 * @calib: default calibrations.
 */
struct iwl_tlv_calib_data {
	__le32 ucode_type;
	struct iwl_tlv_calib_ctrl calib;
} __packed;

struct iwl_firmware_pieces {
	struct fw_img_parsing img[IWL_UCODE_TYPE_MAX];

	u32 init_evtlog_ptr, init_evtlog_size, init_errlog_ptr;
	u32 inst_evtlog_ptr, inst_evtlog_size, inst_errlog_ptr;

	/* FW debug data parsed for driver usage */
	struct iwl_fw_dbg_dest_tlv *dbg_dest_tlv;
	struct iwl_fw_dbg_conf_tlv *dbg_conf_tlv[FW_DBG_CONF_MAX];
	size_t dbg_conf_tlv_len[FW_DBG_CONF_MAX];
	struct iwl_fw_dbg_trigger_tlv *dbg_trigger_tlv[FW_DBG_TRIGGER_MAX];
	size_t dbg_trigger_tlv_len[FW_DBG_TRIGGER_MAX];
	struct iwl_fw_dbg_mem_seg_tlv *dbg_mem_tlv;
	size_t n_dbg_mem_tlv;
};

/*
 * These functions are just to extract uCode section data from the pieces
 * structure.
 */
static struct fw_sec *get_sec(struct iwl_firmware_pieces *pieces,
			      enum iwl_ucode_type type,
			      int  sec)
{
	return &pieces->img[type].sec[sec];
}

static void alloc_sec_data(struct iwl_firmware_pieces *pieces,
			   enum iwl_ucode_type type,
			   int sec)
{
    struct fw_img_parsing *img = &pieces->img[type];
    int size = sec + 1;
    size_t alloc_size = sizeof(struct fw_sec) * size;

    if (img->sec && img->sec_counter >= size)
        return;

    struct fw_sec *sec_memory = IOMalloc(alloc_size);

    if (!sec_memory) {
        IOLog("ALLOC FAILED!!!!");
        return;
    }
    

    if (img->sec && img->sec_counter > 0)
        memcpy(sec_memory, img->sec, sec * sizeof(struct fw_sec));

    img->sec = sec_memory;
    img->sec_counter = size;
}

static void set_sec_data(struct iwl_firmware_pieces *pieces,
			 enum iwl_ucode_type type,
			 int sec,
			 const void *data)
{
    alloc_sec_data(pieces, type, sec);
    pieces->img[type].sec[sec].data = data;
}

static void set_sec_size(struct iwl_firmware_pieces *pieces,
			 enum iwl_ucode_type type,
			 int sec,
			 size_t size)
{
    alloc_sec_data(pieces, type, sec);
    pieces->img[type].sec[sec].size = size;
}

static size_t get_sec_size(struct iwl_firmware_pieces *pieces,
			   enum iwl_ucode_type type,
			   int sec)
{
	return pieces->img[type].sec[sec].size;
}

static void set_sec_offset(struct iwl_firmware_pieces *pieces,
			   enum iwl_ucode_type type,
			   int sec,
			   u32 offset)
{
    alloc_sec_data(pieces, type, sec);
    pieces->img[type].sec[sec].offset = offset;
}

static int iwl_store_cscheme(struct iwl_fw *fw, const u8 *data, const u32 len)
{
	int i, j;
	struct iwl_fw_cscheme_list *l = (struct iwl_fw_cscheme_list *)data;
	struct iwl_fw_cipher_scheme *fwcs;

	if (len < sizeof(*l) ||
	    len < sizeof(l->size) + l->size * sizeof(l->cs[0]))
		return -EINVAL;

	for (i = 0, j = 0; i < IWL_UCODE_MAX_CS && i < l->size; i++) {
		fwcs = &l->cs[j];

		/* we skip schemes with zero cipher suite selector */
		if (!fwcs->cipher)
			continue;

		fw->cs[j++] = *fwcs;
	}

	return 0;
}

static void iwl_store_gscan_capa(struct iwl_fw *fw, const u8 *data,
				 const u32 len)
{
	struct iwl_fw_gscan_capabilities *fw_capa = (void *)data;
	struct iwl_gscan_capabilities *capa = &fw->gscan_capa;

	capa->max_scan_cache_size = le32_to_cpu(fw_capa->max_scan_cache_size);
	capa->max_scan_buckets = le32_to_cpu(fw_capa->max_scan_buckets);
	capa->max_ap_cache_per_scan =
		le32_to_cpu(fw_capa->max_ap_cache_per_scan);
	capa->max_rssi_sample_size = le32_to_cpu(fw_capa->max_rssi_sample_size);
	capa->max_scan_reporting_threshold =
		le32_to_cpu(fw_capa->max_scan_reporting_threshold);
	capa->max_hotlist_aps = le32_to_cpu(fw_capa->max_hotlist_aps);
	capa->max_significant_change_aps =
		le32_to_cpu(fw_capa->max_significant_change_aps);
	capa->max_bssid_history_entries =
		le32_to_cpu(fw_capa->max_bssid_history_entries);
	capa->max_hotlist_ssids = le32_to_cpu(fw_capa->max_hotlist_ssids);
	capa->max_number_epno_networks =
		le32_to_cpu(fw_capa->max_number_epno_networks);
	capa->max_number_epno_networks_by_ssid =
		le32_to_cpu(fw_capa->max_number_epno_networks_by_ssid);
	capa->max_number_of_white_listed_ssid =
		le32_to_cpu(fw_capa->max_number_of_white_listed_ssid);
	capa->max_number_of_black_listed_ssid =
		le32_to_cpu(fw_capa->max_number_of_black_listed_ssid);
}

/*
 * Gets uCode section from tlv.
 */
static int iwl_store_ucode_sec(struct iwl_firmware_pieces *pieces,
			       const void *data, enum iwl_ucode_type type,
			       int size)
{
	struct fw_img_parsing *img;
	struct fw_sec *sec;
	struct fw_sec_parsing *sec_parse;
	size_t alloc_size;

	if (WARN_ON(!pieces || !data || type >= IWL_UCODE_TYPE_MAX))
		return -1;

	sec_parse = (struct fw_sec_parsing *)data;

	img = &pieces->img[type];

    alloc_size = sizeof(*img->sec) * (img->sec_counter + 1);
    sec = IOMalloc(alloc_size);
    memcpy(sec, img->sec, sizeof(*img->sec) * img->sec_counter);

    if (!sec)
        return -ENOMEM;
    img->sec = sec;

	sec = &img->sec[img->sec_counter];

	sec->offset = le32_to_cpu(sec_parse->offset);
	sec->data = sec_parse->data;
	sec->size = size - sizeof(sec_parse->offset);

	++img->sec_counter;

	return 0;
}

static int iwl_set_default_calib(struct iwl_drv *drv, const u8 *data)
{
	struct iwl_tlv_calib_data *def_calib =
					(struct iwl_tlv_calib_data *)data;
	u32 ucode_type = le32_to_cpu(def_calib->ucode_type);
	if (ucode_type >= IWL_UCODE_TYPE_MAX) {
		IWL_ERR(drv, "Wrong ucode_type %u for default calibration.\n",
			ucode_type);
		return -EINVAL;
	}
	drv->fw.default_calib[ucode_type].flow_trigger =
		def_calib->calib.flow_trigger;
	drv->fw.default_calib[ucode_type].event_trigger =
		def_calib->calib.event_trigger;

	return 0;
}

static void iwl_set_ucode_api_flags(struct iwl_drv *drv, const u8 *data,
				    struct iwl_ucode_capabilities *capa)
{
	const struct iwl_ucode_api *ucode_api = (void *)data;
	u32 api_index = le32_to_cpu(ucode_api->api_index);
	u32 api_flags = le32_to_cpu(ucode_api->api_flags);
	int i;

	if (api_index >= DIV_ROUND_UP(NUM_IWL_UCODE_TLV_API, 32)) {
        IWL_WARN(drv,
             "api flags index %d larger than supported by driver\n",
             api_index);
		return;
	}

	for (i = 0; i < 32; i++) {
		if (api_flags & BIT(i))
			set_bit(i + 32 * api_index, capa->_api);
	}
}

static void iwl_set_ucode_capabilities(struct iwl_drv *drv, const u8 *data,
				       struct iwl_ucode_capabilities *capa)
{
	const struct iwl_ucode_capa *ucode_capa = (void *)data;
	u32 api_index = le32_to_cpu(ucode_capa->api_index);
	u32 api_flags = le32_to_cpu(ucode_capa->api_capa);
	int i;

	if (api_index >= DIV_ROUND_UP(NUM_IWL_UCODE_TLV_CAPA, 32)) {
        IWL_WARN(drv,
             "capa flags index %d larger than supported by driver\n",
             api_index);
		return;
	}

	for (i = 0; i < 32; i++) {
		if (api_flags & BIT(i))
			set_bit(i + 32 * api_index, capa->_capa);
	}
}

static int iwl_parse_v1_v2_firmware(struct iwl_drv *drv,
				    const struct firmware *ucode_raw,
				    struct iwl_firmware_pieces *pieces)
{
	struct iwl_ucode_header *ucode = (void *)ucode_raw->data;
	u32 api_ver, hdr_size, build;
	char buildstr[25];
	const u8 *src;

	drv->fw.ucode_ver = le32_to_cpu(ucode->ver);
	api_ver = IWL_UCODE_API(drv->fw.ucode_ver);

	switch (api_ver) {
	default:
		hdr_size = 28;
		if (ucode_raw->size < hdr_size) {
			IWL_ERR(drv, "File size too small!\n");
			return -EINVAL;
		}
		build = le32_to_cpu(ucode->u.v2.build);
		set_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST,
			     le32_to_cpu(ucode->u.v2.inst_size));
		set_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA,
			     le32_to_cpu(ucode->u.v2.data_size));
		set_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST,
			     le32_to_cpu(ucode->u.v2.init_size));
		set_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA,
			     le32_to_cpu(ucode->u.v2.init_data_size));
		src = ucode->u.v2.data;
		break;
	case 0:
	case 1:
	case 2:
		hdr_size = 24;
		if (ucode_raw->size < hdr_size) {
			IWL_ERR(drv, "File size too small!\n");
			return -EINVAL;
		}
		build = 0;
		set_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST,
			     le32_to_cpu(ucode->u.v1.inst_size));
		set_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA,
			     le32_to_cpu(ucode->u.v1.data_size));
		set_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST,
			     le32_to_cpu(ucode->u.v1.init_size));
		set_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA,
			     le32_to_cpu(ucode->u.v1.init_data_size));
		src = ucode->u.v1.data;
		break;
	}

	if (build)
		snprintf(buildstr, 25, " build %u", build);
	else
		buildstr[0] = '\0';

	snprintf(drv->fw.fw_version,
		 sizeof(drv->fw.fw_version),
		 "%u.%u.%u.%u%s",
		 IWL_UCODE_MAJOR(drv->fw.ucode_ver),
		 IWL_UCODE_MINOR(drv->fw.ucode_ver),
		 IWL_UCODE_API(drv->fw.ucode_ver),
		 IWL_UCODE_SERIAL(drv->fw.ucode_ver),
		 buildstr);

	/* Verify size of file vs. image size info in file's header */

	if (ucode_raw->size != hdr_size +
	    get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST) +
	    get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA) +
	    get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST) +
	    get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA)) {

		IWL_ERR(drv,
			"uCode file size %d does not match expected size\n",
			(int)ucode_raw->size);
		return -EINVAL;
	}


	set_sec_data(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST, src);
	src += get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST);
	set_sec_offset(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST,
		       IWLAGN_RTC_INST_LOWER_BOUND);
	set_sec_data(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA, src);
	src += get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA);
	set_sec_offset(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA,
		       IWLAGN_RTC_DATA_LOWER_BOUND);
	set_sec_data(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST, src);
	src += get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST);
	set_sec_offset(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST,
		       IWLAGN_RTC_INST_LOWER_BOUND);
	set_sec_data(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA, src);
	src += get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA);
	set_sec_offset(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA,
		       IWLAGN_RTC_DATA_LOWER_BOUND);
	return 0;
}

static int iwl_parse_tlv_firmware(struct iwl_drv *drv,
				const struct firmware *ucode_raw,
				struct iwl_firmware_pieces *pieces,
				struct iwl_ucode_capabilities *capa,
				bool *usniffer_images)
{
	struct iwl_tlv_ucode_header *ucode = (void *)ucode_raw->data;
	struct iwl_ucode_tlv *tlv;
	size_t len = ucode_raw->size;
	const u8 *data;
	u32 tlv_len;
	u32 usniffer_img;
	enum iwl_ucode_tlv_type tlv_type;
	const u8 *tlv_data;
	char buildstr[25];
	u32 build, paging_mem_size;
	int num_of_cpus;
	bool usniffer_req = false;
	bool gscan_capa = false;

	if (len < sizeof(*ucode)) {
		IWL_ERR(drv, "uCode has invalid length: %zd\n", len);
		return -EINVAL;
	}

	if (ucode->magic != cpu_to_le32(IWL_TLV_UCODE_MAGIC)) {
		IWL_ERR(drv, "invalid uCode magic: 0X%x\n",
			le32_to_cpu(ucode->magic));
		return -EINVAL;
	}

	drv->fw.ucode_ver = le32_to_cpu(ucode->ver);
	memcpy(drv->fw.human_readable, ucode->human_readable,
	       sizeof(drv->fw.human_readable));
	build = le32_to_cpu(ucode->build);

	if (build)
		snprintf(buildstr, 25, " build %u", build);
	else
		buildstr[0] = '\0';

	snprintf(drv->fw.fw_version,
		 sizeof(drv->fw.fw_version),
		 "%u.%u.%u.%u%s",
		 IWL_UCODE_MAJOR(drv->fw.ucode_ver),
		 IWL_UCODE_MINOR(drv->fw.ucode_ver),
		 IWL_UCODE_API(drv->fw.ucode_ver),
		 IWL_UCODE_SERIAL(drv->fw.ucode_ver),
		 buildstr);

	data = ucode->data;

	len -= sizeof(*ucode);

	while (len >= sizeof(*tlv)) {
		len -= sizeof(*tlv);
		tlv = (void *)data;

		tlv_len = le32_to_cpu(tlv->length);
		tlv_type = le32_to_cpu(tlv->type);
		tlv_data = tlv->data;

		if (len < tlv_len) {
			IWL_ERR(drv, "invalid TLV len: %zd/%u\n",
				len, tlv_len);
			return -EINVAL;
		}
		len -= ALIGN(tlv_len, 4);
		data += sizeof(*tlv) + ALIGN(tlv_len, 4);

		switch (tlv_type) {
		case IWL_UCODE_TLV_INST:
			set_sec_data(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_INST, tlv_data);
			set_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_INST, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_REGULAR,
				       IWL_UCODE_SECTION_INST,
				       IWLAGN_RTC_INST_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_DATA:
			set_sec_data(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_DATA, tlv_data);
			set_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_DATA, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_REGULAR,
				       IWL_UCODE_SECTION_DATA,
				       IWLAGN_RTC_DATA_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_INIT:
			set_sec_data(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_INST, tlv_data);
			set_sec_size(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_INST, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_INIT,
				       IWL_UCODE_SECTION_INST,
				       IWLAGN_RTC_INST_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_INIT_DATA:
			set_sec_data(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_DATA, tlv_data);
			set_sec_size(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_DATA, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_INIT,
				       IWL_UCODE_SECTION_DATA,
				       IWLAGN_RTC_DATA_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_BOOT:
			IWL_ERR(drv, "Found unexpected BOOT ucode\n");
			break;
		case IWL_UCODE_TLV_PROBE_MAX_LEN:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			capa->max_probe_length =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_PAN:
			if (tlv_len)
				goto invalid_tlv_len;
			capa->flags |= IWL_UCODE_TLV_FLAGS_PAN;
			break;
		case IWL_UCODE_TLV_FLAGS:
			/* must be at least one u32 */
			if (tlv_len < sizeof(u32))
				goto invalid_tlv_len;
			/* and a proper number of u32s */
			if (tlv_len % sizeof(u32))
				goto invalid_tlv_len;
			/*
			 * This driver only reads the first u32 as
			 * right now no more features are defined,
			 * if that changes then either the driver
			 * will not work with the new firmware, or
			 * it'll not take advantage of new features.
			 */
			capa->flags = le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_API_CHANGES_SET:
			if (tlv_len != sizeof(struct iwl_ucode_api))
				goto invalid_tlv_len;
			iwl_set_ucode_api_flags(drv, tlv_data, capa);
			break;
		case IWL_UCODE_TLV_ENABLED_CAPABILITIES:
			if (tlv_len != sizeof(struct iwl_ucode_capa))
				goto invalid_tlv_len;
			iwl_set_ucode_capabilities(drv, tlv_data, capa);
			break;
		case IWL_UCODE_TLV_INIT_EVTLOG_PTR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->init_evtlog_ptr =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_INIT_EVTLOG_SIZE:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->init_evtlog_size =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_INIT_ERRLOG_PTR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->init_errlog_ptr =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_RUNT_EVTLOG_PTR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->inst_evtlog_ptr =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_RUNT_EVTLOG_SIZE:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->inst_evtlog_size =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_RUNT_ERRLOG_PTR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->inst_errlog_ptr =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_ENHANCE_SENS_TBL:
			if (tlv_len)
				goto invalid_tlv_len;
			drv->fw.enhance_sensitivity_table = true;
			break;
		case IWL_UCODE_TLV_WOWLAN_INST:
			set_sec_data(pieces, IWL_UCODE_WOWLAN,
				     IWL_UCODE_SECTION_INST, tlv_data);
			set_sec_size(pieces, IWL_UCODE_WOWLAN,
				     IWL_UCODE_SECTION_INST, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_WOWLAN,
				       IWL_UCODE_SECTION_INST,
				       IWLAGN_RTC_INST_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_WOWLAN_DATA:
			set_sec_data(pieces, IWL_UCODE_WOWLAN,
				     IWL_UCODE_SECTION_DATA, tlv_data);
			set_sec_size(pieces, IWL_UCODE_WOWLAN,
				     IWL_UCODE_SECTION_DATA, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_WOWLAN,
				       IWL_UCODE_SECTION_DATA,
				       IWLAGN_RTC_DATA_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_PHY_CALIBRATION_SIZE:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			capa->standard_phy_calibration_size =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		 case IWL_UCODE_TLV_SEC_RT:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_REGULAR,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_SEC_INIT:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_INIT,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_SEC_WOWLAN:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_WOWLAN,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_DEF_CALIB:
			if (tlv_len != sizeof(struct iwl_tlv_calib_data))
				goto invalid_tlv_len;
			if (iwl_set_default_calib(drv, tlv_data))
				goto tlv_error;
			break;
		case IWL_UCODE_TLV_PHY_SKU:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			drv->fw.phy_config = le32_to_cpup((__le32 *)tlv_data);
			drv->fw.valid_tx_ant = (drv->fw.phy_config &
						FW_PHY_CFG_TX_CHAIN) >>
						FW_PHY_CFG_TX_CHAIN_POS;
			drv->fw.valid_rx_ant = (drv->fw.phy_config &
						FW_PHY_CFG_RX_CHAIN) >>
						FW_PHY_CFG_RX_CHAIN_POS;
			break;
		 case IWL_UCODE_TLV_SECURE_SEC_RT:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_REGULAR,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_SECURE_SEC_INIT:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_INIT,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_SECURE_SEC_WOWLAN:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_WOWLAN,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_NUM_OF_CPU:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			num_of_cpus =
				le32_to_cpup((__le32 *)tlv_data);

			if (num_of_cpus == 2) {
				drv->fw.img[IWL_UCODE_REGULAR].is_dual_cpus =
					true;
				drv->fw.img[IWL_UCODE_INIT].is_dual_cpus =
					true;
				drv->fw.img[IWL_UCODE_WOWLAN].is_dual_cpus =
					true;
			} else if ((num_of_cpus > 2) || (num_of_cpus < 1)) {
				IWL_ERR(drv, "Driver support upto 2 CPUs\n");
				return -EINVAL;
			}
			break;
		case IWL_UCODE_TLV_CSCHEME:
			if (iwl_store_cscheme(&drv->fw, tlv_data, tlv_len))
				goto invalid_tlv_len;
			break;
		case IWL_UCODE_TLV_N_SCAN_CHANNELS:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			capa->n_scan_channels =
				le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_FW_VERSION: {
			__le32 *ptr = (void *)tlv_data;
			u32 major, minor;
			u8 local_comp;

			if (tlv_len != sizeof(u32) * 3)
				goto invalid_tlv_len;

			major = le32_to_cpup(ptr++);
			minor = le32_to_cpup(ptr++);
			local_comp = le32_to_cpup(ptr);

			snprintf(drv->fw.fw_version,
				 sizeof(drv->fw.fw_version), "%u.%u.%u",
				 major, minor, local_comp);
			break;
			}
		case IWL_UCODE_TLV_FW_DBG_DEST: {
			struct iwl_fw_dbg_dest_tlv *dest = (void *)tlv_data;

			if (pieces->dbg_dest_tlv) {
				IWL_ERR(drv,
					"dbg destination ignored, already exists\n");
				break;
			}

			pieces->dbg_dest_tlv = dest;
            IWL_INFO(drv, "Found debug destination: %s\n",
                 get_fw_dbg_mode_string(dest->monitor_mode));

			drv->fw.dbg_dest_reg_num =
				tlv_len - offsetof(struct iwl_fw_dbg_dest_tlv,
						   reg_ops);
			drv->fw.dbg_dest_reg_num /=
				sizeof(drv->fw.dbg_dest_tlv->reg_ops[0]);

			break;
			}
		case IWL_UCODE_TLV_FW_DBG_CONF: {
			struct iwl_fw_dbg_conf_tlv *conf = (void *)tlv_data;

			if (!pieces->dbg_dest_tlv) {
				IWL_ERR(drv,
					"Ignore dbg config %d - no destination configured\n",
					conf->id);
				break;
			}

			if (conf->id >= ARRAY_SIZE(drv->fw.dbg_conf_tlv)) {
				IWL_ERR(drv,
					"Skip unknown configuration: %d\n",
					conf->id);
				break;
			}

			if (pieces->dbg_conf_tlv[conf->id]) {
				IWL_ERR(drv,
					"Ignore duplicate dbg config %d\n",
					conf->id);
				break;
			}

			if (conf->usniffer)
				usniffer_req = true;

            IWL_INFO(drv, "Found debug configuration: %d\n",
                 conf->id);

			pieces->dbg_conf_tlv[conf->id] = conf;
			pieces->dbg_conf_tlv_len[conf->id] = tlv_len;
			break;
			}
		case IWL_UCODE_TLV_FW_DBG_TRIGGER: {
			struct iwl_fw_dbg_trigger_tlv *trigger =
				(void *)tlv_data;
			u32 trigger_id = le32_to_cpu(trigger->id);

			if (trigger_id >= ARRAY_SIZE(drv->fw.dbg_trigger_tlv)) {
				IWL_ERR(drv,
					"Skip unknown trigger: %u\n",
					trigger->id);
				break;
			}

			if (pieces->dbg_trigger_tlv[trigger_id]) {
				IWL_ERR(drv,
					"Ignore duplicate dbg trigger %u\n",
					trigger->id);
				break;
			}

            IWL_INFO(drv, "Found debug trigger: %u\n", trigger->id);

			pieces->dbg_trigger_tlv[trigger_id] = trigger;
			pieces->dbg_trigger_tlv_len[trigger_id] = tlv_len;
			break;
			}
		case IWL_UCODE_TLV_SEC_RT_USNIFFER:
			*usniffer_images = true;
			iwl_store_ucode_sec(pieces, tlv_data,
					    IWL_UCODE_REGULAR_USNIFFER,
					    tlv_len);
			break;
		case IWL_UCODE_TLV_PAGING:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			paging_mem_size = le32_to_cpup((__le32 *)tlv_data);

            IWL_DEBUG_FW(drv,
                     "Paging: paging enabled (size = %u bytes)\n",
                     paging_mem_size);

			if (paging_mem_size > MAX_PAGING_IMAGE_SIZE) {
				IWL_ERR(drv,
					"Paging: driver supports up to %lu bytes for paging image\n",
					MAX_PAGING_IMAGE_SIZE);
				return -EINVAL;
			}

			if (paging_mem_size & (FW_PAGING_SIZE - 1)) {
				IWL_ERR(drv,
					"Paging: image isn't multiple %lu\n",
					FW_PAGING_SIZE);
				return -EINVAL;
			}

			drv->fw.img[IWL_UCODE_REGULAR].paging_mem_size =
				paging_mem_size;
			usniffer_img = IWL_UCODE_REGULAR_USNIFFER;
			drv->fw.img[usniffer_img].paging_mem_size =
				paging_mem_size;
			break;
		case IWL_UCODE_TLV_SDIO_ADMA_ADDR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			drv->fw.sdio_adma_addr =
				le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_FW_GSCAN_CAPA:
			/*
			 * Don't return an error in case of a shorter tlv_len
			 * to enable loading of FW that has an old format
			 * of GSCAN capabilities TLV.
			 */
			if (tlv_len < sizeof(struct iwl_fw_gscan_capabilities))
				break;

			iwl_store_gscan_capa(&drv->fw, tlv_data, tlv_len);
			gscan_capa = true;
			break;
		case IWL_UCODE_TLV_FW_MEM_SEG: {
			struct iwl_fw_dbg_mem_seg_tlv *dbg_mem =
				(void *)tlv_data;
			u32 type;
			size_t size;
			struct iwl_fw_dbg_mem_seg_tlv *n;

			if (tlv_len != (sizeof(*dbg_mem)))
				goto invalid_tlv_len;

			type = le32_to_cpu(dbg_mem->data_type);

            IWL_DEBUG_INFO(drv, "Found debug memory segment: %u\n",
                       dbg_mem->data_type);

			switch (type & FW_DBG_MEM_TYPE_MASK) {
			case FW_DBG_MEM_TYPE_REGULAR:
			case FW_DBG_MEM_TYPE_PRPH:
				/* we know how to handle these */
				break;
			default:
				IWL_ERR(drv,
					"Found debug memory segment with invalid type: 0x%x\n",
					type);
				return -EINVAL;
			}

			size = sizeof(*pieces->dbg_mem_tlv) * (pieces->n_dbg_mem_tlv + 1);
            n = IOMalloc(size);
            memcpy(n, pieces->dbg_mem_tlv, sizeof(*pieces->dbg_mem_tlv) *
                   (pieces->n_dbg_mem_tlv));
			if (!n)
				return -ENOMEM;
			pieces->dbg_mem_tlv = n;
			pieces->dbg_mem_tlv[pieces->n_dbg_mem_tlv] = *dbg_mem;
			pieces->n_dbg_mem_tlv++;
			break;
			}
		default:
            IWL_DEBUG_INFO(drv, "unknown TLV: %d\n", tlv_type);
			break;
		}
	}

	if (!fw_has_capa(capa, IWL_UCODE_TLV_CAPA_USNIFFER_UNIFIED) &&
	    usniffer_req && !*usniffer_images) {
		IWL_ERR(drv,
			"user selected to work with usniffer but usniffer image isn't available in ucode package\n");
		return -EINVAL;
	}

	if (len) {
		IWL_ERR(drv, "invalid TLV after parsing: %zd\n", len);
//        iwl_print_hex_dump(drv, IWL_DL_FW, (u8 *)data, len);
		return -EINVAL;
	}

	/*
	 * If ucode advertises that it supports GSCAN but GSCAN
	 * capabilities TLV is not present, or if it has an old format,
	 * warn and continue without GSCAN.
	 */
	if (fw_has_capa(capa, IWL_UCODE_TLV_CAPA_GSCAN_SUPPORT) && !gscan_capa) {
        IWL_DEBUG_INFO(drv, "GSCAN is supported but capabilities TLV is unavailable\n");
		clear_bit((__force long)IWL_UCODE_TLV_CAPA_GSCAN_SUPPORT, capa->_capa);
	}

	return 0;

 invalid_tlv_len:
	IWL_ERR(drv, "TLV %d has invalid size: %u\n", tlv_type, tlv_len);
 tlv_error:
//    iwl_print_hex_dump(drv, IWL_DL_FW, tlv_data, tlv_len);

	return -EINVAL;
}

static int iwl_alloc_ucode(struct iwl_drv *drv,
			   struct iwl_firmware_pieces *pieces,
			   enum iwl_ucode_type type)
{
	int i;
	struct fw_desc *sec;
    
    vm_size_t size = sizeof(*sec) * pieces->img[type].sec_counter;
    sec = IOMalloc(size);
    
    if (!sec) {
        IOLog("IOMalloc fail");
        return -ENOMEM;
    }
    bzero(sec, size);
		
	drv->fw.img[type].sec = sec;
	drv->fw.img[type].num_sec = pieces->img[type].sec_counter;

    for (i = 0; i < pieces->img[type].sec_counter; i++) {
        if (iwl_alloc_fw_desc(drv, &sec[i], get_sec(pieces, type, i))) {
            IOLog("alloc fw_desc fail");
            return -ENOMEM;
        }
    }

	return 0;
}

static int validate_sec_sizes(struct iwl_drv *drv,
			      struct iwl_firmware_pieces *pieces,
			      const struct iwl_cfg *cfg)
{
    IWL_DEBUG_INFO(drv, "f/w package hdr runtime inst size = %zd\n",
        get_sec_size(pieces, IWL_UCODE_REGULAR,
                 IWL_UCODE_SECTION_INST));
    IWL_DEBUG_INFO(drv, "f/w package hdr runtime data size = %zd\n",
        get_sec_size(pieces, IWL_UCODE_REGULAR,
                 IWL_UCODE_SECTION_DATA));
    IWL_DEBUG_INFO(drv, "f/w package hdr init inst size = %zd\n",
        get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST));
    IWL_DEBUG_INFO(drv, "f/w package hdr init data size = %zd\n",
        get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA));

	/* Verify that uCode images will fit in card's SRAM. */
	if (get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST) >
	    cfg->max_inst_size) {
		IWL_ERR(drv, "uCode instr len %zd too large to fit in\n",
			get_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_INST));
		return -1;
	}

	if (get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA) >
	    cfg->max_data_size) {
		IWL_ERR(drv, "uCode data len %zd too large to fit in\n",
			get_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_DATA));
		return -1;
	}

	if (get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST) >
	     cfg->max_inst_size) {
		IWL_ERR(drv, "uCode init instr len %zd too large to fit in\n",
			get_sec_size(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_INST));
		return -1;
	}

	if (get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA) >
	    cfg->max_data_size) {
		IWL_ERR(drv, "uCode init data len %zd too large to fit in\n",
			get_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_DATA));
		return -1;
	}
	return 0;
}

static struct iwl_op_mode *
_iwl_op_mode_start(struct iwl_drv *drv, struct iwlwifi_opmode_table *op)
{
	const struct iwl_op_mode_ops *ops = op->ops;
	struct iwl_op_mode *op_mode = NULL;
	op_mode = ops->start(drv->trans, drv->trans->cfg, &drv->fw);

#ifdef CONFIG_IWLWIFI_DEBUGFS
	if (!op_mode) {
		debugfs_remove_recursive(drv->dbgfs_op_mode);
		drv->dbgfs_op_mode = NULL;
	}
#endif

	return op_mode;
}

static void _iwl_op_mode_stop(struct iwl_drv *drv)
{
	/* op_mode can be NULL if its start failed */
	if (drv->op_mode) {
		iwl_op_mode_stop(drv->op_mode);
		drv->op_mode = NULL;

#ifdef CONFIG_IWLWIFI_DEBUGFS
		debugfs_remove_recursive(drv->dbgfs_op_mode);
		drv->dbgfs_op_mode = NULL;
#endif
	}
}

/**
 * iwl_req_fw_callback - callback when firmware was loaded
 *
 * If loaded successfully, copies the firmware into buffers
 * for the card to fetch (via DMA).
 */
static void iwl_req_fw_callback(const struct firmware *ucode_raw, void *context)
{
	struct iwl_drv *drv = context;
	struct iwl_fw *fw = &drv->fw;
	struct iwl_ucode_header *ucode;
	struct iwlwifi_opmode_table *op;
	int err;
	struct iwl_firmware_pieces *pieces;
	const unsigned int api_max = drv->trans->cfg->ucode_api_max;
	const unsigned int api_min = drv->trans->cfg->ucode_api_min;
	size_t trigger_tlv_sz[FW_DBG_TRIGGER_MAX];
	u32 api_ver;
	int i;
	bool load_module = false;
	bool usniffer_images = false;

	fw->ucode_capa.max_probe_length = IWL_DEFAULT_MAX_PROBE_LENGTH;
	fw->ucode_capa.standard_phy_calibration_size =
			IWL_DEFAULT_STANDARD_PHY_CALIBRATE_TBL_SIZE;
	fw->ucode_capa.n_scan_channels = IWL_DEFAULT_SCAN_CHANNELS;
    
    pieces = IOMalloc(sizeof(*pieces));
    
    if (!pieces)
        goto out_free_fw;
    bzero(pieces, sizeof(*pieces));

    if (!ucode_raw)
        goto try_again;

    IWL_DEBUG_INFO(drv, "Loaded firmware file '%s' (%zd bytes).\n",
               drv->firmware_name, ucode_raw->size);

	/* Make sure that we got at least the API version number */
	if (ucode_raw->size < 4) {
		IWL_ERR(drv, "File size way too small!\n");
		goto try_again;
	}

	/* Data from ucode file:  header followed by uCode images */
	ucode = (struct iwl_ucode_header *)ucode_raw->data;

	if (ucode->ver)
		err = iwl_parse_v1_v2_firmware(drv, ucode_raw, pieces);
	else
		err = iwl_parse_tlv_firmware(drv, ucode_raw, pieces,
					     &fw->ucode_capa, &usniffer_images);

    if (err)
        goto try_again;

	if (fw_has_api(&drv->fw.ucode_capa, IWL_UCODE_TLV_API_NEW_VERSION))
		api_ver = drv->fw.ucode_ver;
	else
		api_ver = IWL_UCODE_API(drv->fw.ucode_ver);

	/*
	 * api_ver should match the api version forming part of the
	 * firmware filename ... but we don't check for that and only rely
	 * on the API version read from firmware header from here on forward
	 */
    if (api_ver < api_min || api_ver > api_max) {
        IWL_ERR(drv,
            "Driver unable to support your firmware API. "
            "Driver supports v%u, firmware is v%u.\n",
            api_max, api_ver);
        goto try_again;
    }

	/*
	 * In mvm uCode there is no difference between data and instructions
	 * sections.
	 */
    if (fw->type == IWL_FW_DVM && validate_sec_sizes(drv, pieces, drv->trans->cfg))
        goto try_again;
    

	/* Allocate ucode buffers for card's bus-master loading ... */

	/* Runtime instructions and 2 copies of data:
	 * 1) unmodified from disk
	 * 2) backup cache for save/restore during power-downs */
    for (i = 0; i < IWL_UCODE_TYPE_MAX; i++)
        if (iwl_alloc_ucode(drv, pieces, i))
            goto out_free_fw;
        
    if (pieces->dbg_dest_tlv) {
        memcpy(drv->fw.dbg_dest_tlv,
               pieces->dbg_dest_tlv,
               sizeof(*pieces->dbg_dest_tlv) +
               sizeof(pieces->dbg_dest_tlv->reg_ops[0]) *
               drv->fw.dbg_dest_reg_num);
        
        if (!drv->fw.dbg_dest_tlv)
            goto out_free_fw;
    }

    for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_conf_tlv); i++) {
        if (pieces->dbg_conf_tlv[i]) {
            drv->fw.dbg_conf_tlv_len[i] = pieces->dbg_conf_tlv_len[i];
            memcpy(drv->fw.dbg_conf_tlv[i], pieces->dbg_conf_tlv[i], drv->fw.dbg_conf_tlv_len[i]);
            
            if (!drv->fw.dbg_conf_tlv[i])
                goto out_free_fw;
        }
    }

    memset(&trigger_tlv_sz, 0xff, sizeof(trigger_tlv_sz));

    trigger_tlv_sz[FW_DBG_TRIGGER_MISSED_BEACONS] =
        sizeof(struct iwl_fw_dbg_trigger_missed_bcon);
    trigger_tlv_sz[FW_DBG_TRIGGER_CHANNEL_SWITCH] = 0;
    trigger_tlv_sz[FW_DBG_TRIGGER_FW_NOTIF] =
        sizeof(struct iwl_fw_dbg_trigger_cmd);
    trigger_tlv_sz[FW_DBG_TRIGGER_MLME] =
        sizeof(struct iwl_fw_dbg_trigger_mlme);
    trigger_tlv_sz[FW_DBG_TRIGGER_STATS] =
        sizeof(struct iwl_fw_dbg_trigger_stats);
    trigger_tlv_sz[FW_DBG_TRIGGER_RSSI] =
        sizeof(struct iwl_fw_dbg_trigger_low_rssi);
    trigger_tlv_sz[FW_DBG_TRIGGER_TXQ_TIMERS] =
        sizeof(struct iwl_fw_dbg_trigger_txq_timer);
    trigger_tlv_sz[FW_DBG_TRIGGER_TIME_EVENT] =
        sizeof(struct iwl_fw_dbg_trigger_time_event);
    trigger_tlv_sz[FW_DBG_TRIGGER_BA] =
        sizeof(struct iwl_fw_dbg_trigger_ba);
    trigger_tlv_sz[FW_DBG_TRIGGER_TDLS] =
        sizeof(struct iwl_fw_dbg_trigger_tdls);

    for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_trigger_tlv); i++) {
        if (pieces->dbg_trigger_tlv[i]) {
            /*
             * If the trigger isn't long enough, WARN and exit.
             * Someone is trying to debug something and he won't
             * be able to catch the bug he is trying to chase.
             * We'd better be noisy to be sure he knows what's
             * going on.
             */
            if (WARN_ON(pieces->dbg_trigger_tlv_len[i] <
                    (trigger_tlv_sz[i] +
                     sizeof(struct iwl_fw_dbg_trigger_tlv))))
                goto out_free_fw;
            drv->fw.dbg_trigger_tlv_len[i] = pieces->dbg_trigger_tlv_len[i];
            memcpy(drv->fw.dbg_trigger_tlv[i], pieces->dbg_trigger_tlv[i], drv->fw.dbg_trigger_tlv_len[i]);
            
            if (!drv->fw.dbg_trigger_tlv[i])
                goto out_free_fw;
        }
    }

	/* Now that we can no longer fail, copy information */

    drv->fw.dbg_mem_tlv = pieces->dbg_mem_tlv;
    pieces->dbg_mem_tlv = NULL;
    drv->fw.n_dbg_mem_tlv = pieces->n_dbg_mem_tlv;

    /*
     * The (size - 16) / 12 formula is based on the information recorded
     * for each event, which is of mode 1 (including timestamp) for all
     * new microcodes that include this information.
     */
    fw->init_evtlog_ptr = pieces->init_evtlog_ptr;
    if (pieces->init_evtlog_size)
        fw->init_evtlog_size = (pieces->init_evtlog_size - 16)/12;
    else
        fw->init_evtlog_size =
            drv->trans->cfg->base_params->max_event_log_size;
    fw->init_errlog_ptr = pieces->init_errlog_ptr;
    fw->inst_evtlog_ptr = pieces->inst_evtlog_ptr;
    if (pieces->inst_evtlog_size)
        fw->inst_evtlog_size = (pieces->inst_evtlog_size - 16)/12;
    else
        fw->inst_evtlog_size =
            drv->trans->cfg->base_params->max_event_log_size;
    fw->inst_errlog_ptr = pieces->inst_errlog_ptr;

    /*
     * figure out the offset of chain noise reset and gain commands
     * base on the size of standard phy calibration commands table size
     */
    if (fw->ucode_capa.standard_phy_calibration_size >
        IWL_MAX_PHY_CALIBRATE_TBL_SIZE)
        fw->ucode_capa.standard_phy_calibration_size =
            IWL_MAX_STANDARD_PHY_CALIBRATE_TBL_SIZE;

    /* We have our copies now, allow OS release its copies */
    //release_firmware(ucode_raw);
    //IOFree(ucode_raw, sizeof(*ucode_raw));

    IOLockLock(iwlwifi_opmode_table_mtx);
    
	switch (fw->type) {
	case IWL_FW_DVM:
        op = &iwlwifi_opmode_table[DVM_OP_MODE];
		break;
	default:
		//WARN(1, "Invalid fw type %d\n", fw->type);
	case IWL_FW_MVM:
        op = &iwlwifi_opmode_table[MVM_OP_MODE];
		break;
	}

    IWL_INFO(drv, "loaded firmware version %s op_mode %s\n",
         drv->fw.fw_version, op->name);

	/* add this device to the list of devices using this op_mode */
//    list_add_tail(&drv->list, &op->drv);

    if (op->ops) {
        drv->op_mode = _iwl_op_mode_start(drv, op);

        if (!drv->op_mode) {
            IOLockUnlock(iwlwifi_opmode_table_mtx);
            goto out_unbind;
        }
    } else {
        load_module = true;
    }

    IOLockUnlock(iwlwifi_opmode_table_mtx);

	/*
	 * Complete the firmware request last so that
	 * a driver unbind (stop) doesn't run while we
	 * are doing the start() above.
	 */
	//complete(&drv->request_firmware_complete);

	/*
	 * Load the module last so we don't block anything
	 * else from proceeding if the module fails to load
	 * or hangs loading.
	 */
	if (load_module) {
		//request_module("%s", op->name);
#ifdef CONFIG_IWLWIFI_OPMODE_MODULAR
        if (err)
            IWL_ERR(drv,
                "failed to load module %s (error %d), is dynamic loading enabled?\n",
                op->name, err);
#endif
	}
	goto free;

 try_again:
	/* try next, if any */
    
//    release_firmware(ucode_raw);
//    IOFree(ucode_raw, sizeof(*ucode_raw));
	if (iwl_request_firmware(drv, false))
		goto out_unbind;
	goto free;

 out_free_fw:
    IOLog("out_free_fw");
    iwl_dealloc_ucode(drv);
	//release_firmware(ucode_raw);
//    IOFree(ucode_raw, sizeof(*ucode_raw));
 out_unbind:
	//complete(&drv->request_firmware_complete);
	//device_release_driver(drv->trans->dev);
 free:
	if (pieces) {
        for (i = 0; i < ARRAY_SIZE(pieces->img); i++) {
            IOFree(pieces->img[i].sec, sizeof(*pieces->img[i].sec) * pieces->img[i].sec_counter);
        }
			
        IOFree(pieces->dbg_mem_tlv, sizeof(*pieces->dbg_mem_tlv));
        IOFree(pieces, sizeof(*pieces));
	}
}

struct iwl_drv *iwl_drv_start(struct iwl_trans *trans)
{
    iwlwifi_opmode_table_mtx = IOLockAlloc();
    
	struct iwl_drv *drv;
	int ret;

    drv = IOMalloc(sizeof(*drv));
    
	if (!drv) {
		ret = -ENOMEM;
		goto err;
	}
    bzero(drv, sizeof(*drv));

	drv->trans = trans;
	drv->dev = trans->dev;

	//init_completion(&drv->request_firmware_complete);
	//INIT_LIST_HEAD(&drv->list);
    STAILQ_INIT(&drv->list);

#ifdef CONFIG_IWLWIFI_DEBUGFS
	/* Create the device debugfs entries. */
	drv->dbgfs_drv = debugfs_create_dir(dev_name(trans->dev),
					    iwl_dbgfs_root);

	if (!drv->dbgfs_drv) {
		IWL_ERR(drv, "failed to create debugfs directory\n");
		ret = -ENOMEM;
		goto err_free_drv;
	}

	/* Create transport layer debugfs dir */
	drv->trans->dbgfs_dir = debugfs_create_dir("trans", drv->dbgfs_drv);

	if (!drv->trans->dbgfs_dir) {
		IWL_ERR(drv, "failed to create transport debugfs directory\n");
		ret = -ENOMEM;
		goto err_free_dbgfs;
	}
#endif

	ret = iwl_request_firmware(drv, true);
	if (ret) {
		IWL_ERR(trans, "Couldn't request the fw\n");
		goto err_fw;
	}

	return drv;

err_fw:
#ifdef CONFIG_IWLWIFI_DEBUGFS
err_free_dbgfs:
	debugfs_remove_recursive(drv->dbgfs_drv);
err_free_drv:
#endif
	//kfree(drv);
    IOFree(drv, sizeof(*drv));
err:
	return ERR_PTR(ret);
}

void iwl_drv_stop(struct iwl_drv *drv)
{
	//wait_for_completion(&drv->request_firmware_complete);

	//_iwl_op_mode_stop(drv);

	iwl_dealloc_ucode(drv);

    IOLockLock(iwlwifi_opmode_table_mtx);
	/*
	 * List is empty (this item wasn't added)
	 * when firmware loading failed -- in that
	 * case we can't remove it from any list.
	 */
//    if (!STAILQ_EMPTY(&drv->list))
//
//        STAILQ_REMOVE(&drv->list, <#elm#>, <#type#>, <#field#>)
//        //STAILQ_REMOVE_HEAD(&drv->list, drv);
//        list_del(&drv->list);

    IOLockUnlock(iwlwifi_opmode_table_mtx);

#ifdef CONFIG_IWLWIFI_DEBUGFS
	debugfs_remove_recursive(drv->dbgfs_drv);
#endif
    
    IOLockFree(iwlwifi_opmode_table_mtx);

    IOFree(drv, sizeof(*drv));
}


/* shared module parameters */
struct iwl_mod_params iwlwifi_mod_params = {
	.fw_restart = true,
	.bt_coex_active = true,
	.power_level = IWL_POWER_INDEX_1,
	.d0i3_disable = true,
	.d0i3_timeout = 1000,
	.uapsd_disable = IWL_DISABLE_UAPSD_BSS | IWL_DISABLE_UAPSD_P2P_CLIENT,
    .debug_level = 0xFFFFFFFF,
	/* the rest are 0 by default */
};
IWL_EXPORT_SYMBOL(iwlwifi_mod_params);

int iwl_opmode_register(const char *name, const struct iwl_op_mode_ops *ops)
{
	int i;
	struct iwl_drv *drv;
	struct iwlwifi_opmode_table *op;
    
    IOLockLock(iwlwifi_opmode_table_mtx);
	for (i = 0; i < ARRAY_SIZE(iwlwifi_opmode_table); i++) {
		op = &iwlwifi_opmode_table[i];
		if (strcmp(op->name, name))
			continue;
		op->ops = ops;
		/* TODO: need to handle exceptional case */
        STAILQ_FOREACH(drv, &op->drv, list)
			drv->op_mode = _iwl_op_mode_start(drv, op);

        IOLockUnlock(iwlwifi_opmode_table_mtx);
		return 0;
	}

    IOLockUnlock(iwlwifi_opmode_table_mtx);
	return -EIO;
}
IWL_EXPORT_SYMBOL(iwl_opmode_register);

void iwl_opmode_deregister(const char *name)
{
	int i;
	struct iwl_drv *drv;

    IOLockLock(iwlwifi_opmode_table_mtx);
	for (i = 0; i < ARRAY_SIZE(iwlwifi_opmode_table); i++) {
		if (strcmp(iwlwifi_opmode_table[i].name, name))
			continue;
		iwlwifi_opmode_table[i].ops = NULL;

		/* call the stop routine for all devices */
        STAILQ_FOREACH(drv, &iwlwifi_opmode_table[i].drv, list)
			_iwl_op_mode_stop(drv);

        IOLockUnlock(iwlwifi_opmode_table_mtx);
		return;
	}

    IOLockUnlock(iwlwifi_opmode_table_mtx);
}
IWL_EXPORT_SYMBOL(iwl_opmode_deregister);

static int __unused iwl_drv_init(void)
{
	int i;

    iwlwifi_opmode_table_mtx = IOLockAlloc();

	for (i = 0; i < ARRAY_SIZE(iwlwifi_opmode_table); i++)
		STAILQ_INIT(&iwlwifi_opmode_table[i].drv);

	pr_info(DRV_DESCRIPTION "\n");
	pr_info(DRV_COPYRIGHT "\n");

#ifdef CONFIG_IWLWIFI_DEBUGFS
	/* Create the root of iwlwifi debugfs subsystem. */
	iwl_dbgfs_root = debugfs_create_dir(DRV_NAME, NULL);

	if (!iwl_dbgfs_root)
		return -EFAULT;
#endif

	return iwl_pci_register_driver();
}
module_init(iwl_drv_init);

static void __unused iwl_drv_exit(void)
{
    IOLockFree(iwlwifi_opmode_table_mtx);
    
	iwl_pci_unregister_driver();

#ifdef CONFIG_IWLWIFI_DEBUGFS
	debugfs_remove_recursive(iwl_dbgfs_root);
#endif
}
module_exit(iwl_drv_exit);

#if DISABLED_CODE

#ifdef CONFIG_IWLWIFI_DEBUG
module_param_named(debug, iwlwifi_mod_params.debug_level, uint,
		   S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "debug output mask");
#endif

module_param_named(swcrypto, iwlwifi_mod_params.swcrypto, int, S_IRUGO);
MODULE_PARM_DESC(swcrypto, "using crypto in software (default 0 [hardware])");
module_param_named(11n_disable, iwlwifi_mod_params.disable_11n, uint, S_IRUGO);
MODULE_PARM_DESC(11n_disable,
	"disable 11n functionality, bitmap: 1: full, 2: disable agg TX, 4: disable agg RX, 8 enable agg TX");
module_param_named(amsdu_size, iwlwifi_mod_params.amsdu_size,
		   int, S_IRUGO);
MODULE_PARM_DESC(amsdu_size,
		 "amsdu size 0: 12K for multi Rx queue devices, 4K for other devices 1:4K 2:8K 3:12K (default 0)");
module_param_named(fw_restart, iwlwifi_mod_params.fw_restart, bool, S_IRUGO);
MODULE_PARM_DESC(fw_restart, "restart firmware in case of error (default true)");

module_param_named(antenna_coupling, iwlwifi_mod_params.antenna_coupling,
		   int, S_IRUGO);
MODULE_PARM_DESC(antenna_coupling,
		 "specify antenna coupling in dB (default: 0 dB)");

module_param_named(nvm_file, iwlwifi_mod_params.nvm_file, charp, S_IRUGO);
MODULE_PARM_DESC(nvm_file, "NVM file name");

module_param_named(d0i3_disable, iwlwifi_mod_params.d0i3_disable,
		   bool, S_IRUGO);
MODULE_PARM_DESC(d0i3_disable, "disable d0i3 functionality (default: Y)");

module_param_named(lar_disable, iwlwifi_mod_params.lar_disable,
		   bool, S_IRUGO);
MODULE_PARM_DESC(lar_disable, "disable LAR functionality (default: N)");

module_param_named(uapsd_disable, iwlwifi_mod_params.uapsd_disable,
		   uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(uapsd_disable,
		 "disable U-APSD functionality bitmap 1: BSS 2: P2P Client (default: 3)");

/*
 * set bt_coex_active to true, uCode will do kill/defer
 * every time the priority line is asserted (BT is sending signals on the
 * priority line in the PCIx).
 * set bt_coex_active to false, uCode will ignore the BT activity and
 * perform the normal operation
 *
 * User might experience transmit issue on some platform due to WiFi/BT
 * co-exist problem. The possible behaviors are:
 *   Able to scan and finding all the available AP
 *   Not able to associate with any AP
 * On those platforms, WiFi communication can be restored by set
 * "bt_coex_active" module parameter to "false"
 *
 * default: bt_coex_active = true (BT_COEX_ENABLE)
 */
module_param_named(bt_coex_active, iwlwifi_mod_params.bt_coex_active,
		bool, S_IRUGO);
MODULE_PARM_DESC(bt_coex_active, "enable wifi/bt co-exist (default: enable)");

module_param_named(led_mode, iwlwifi_mod_params.led_mode, int, S_IRUGO);
MODULE_PARM_DESC(led_mode, "0=system default, "
		"1=On(RF On)/Off(RF Off), 2=blinking, 3=Off (default: 0)");

module_param_named(power_save, iwlwifi_mod_params.power_save,
		bool, S_IRUGO);
MODULE_PARM_DESC(power_save,
		 "enable WiFi power management (default: disable)");

module_param_named(power_level, iwlwifi_mod_params.power_level,
		int, S_IRUGO);
MODULE_PARM_DESC(power_level,
		 "default power save level (range from 1 - 5, default: 1)");

module_param_named(fw_monitor, iwlwifi_mod_params.fw_monitor, bool, S_IRUGO);
MODULE_PARM_DESC(fw_monitor,
		 "firmware monitor - to debug FW (default: false - needs lots of memory)");

module_param_named(d0i3_timeout, iwlwifi_mod_params.d0i3_timeout,
		   uint, S_IRUGO);
MODULE_PARM_DESC(d0i3_timeout, "Timeout to D0i3 entry when idle (ms)");

module_param_named(disable_11ac, iwlwifi_mod_params.disable_11ac, bool,
		   S_IRUGO);
MODULE_PARM_DESC(disable_11ac, "Disable VHT capabilities (default: false)");
#endif
