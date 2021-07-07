/* Copyright (c) 2015, The Linux Foundation. All rights reserved.
* Copyright (C) 2021 XiaoMi, Inc.
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 and
* only version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/


#include <linux/firmware.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/compat.h>
#include <linux/acpi.h>
#include <linux/string.h>
#include <linux/list.h>

#include <dsp/msm-cirrus-playback-v2.h>

#undef CONFIG_OF
#define CRUS_AMP_FACTOR		71498
#define CRUS_MATERIAL_FACTOR	250
#define CRUS_T_SCALE_FACTOR	100000
#define CRUS_Z_SCALE_FACTOR	100000000
#define CIRRUS_NONE 0
#define CIRRUS_COPP 1
#define CIRRUS_AFE 2

#undef pr_info
#undef pr_err
#undef pr_debug
#define pr_debug(fmt, args...) printk(KERN_INFO "[CSPL] " pr_fmt(fmt), ##args)
#define pr_info(fmt, args...) printk(KERN_INFO "[CSPL] " pr_fmt(fmt), ##args)
#define pr_err(fmt, args...) printk(KERN_ERR "[CSPL] " pr_fmt(fmt), ##args)
#define CRUS_TX_CONFIG "crus_sp_tx%d.bin"
#define CRUS_RX_CONFIG "crus_sp_rx%d.bin"

#define CIRRUS_RX_GET_IODATA 0x00A1AF09
#define CIRRUS_TX_GET_IODATA 0x00A1BF09

static struct crus_sp_ioctl_header crus_sp_hdr;
#define xCACHE_COMMANDS
static struct crus_control_t this = {
    .module_id = CIRRUS_SP,
	.fb_port = AFE_PORT_ID_QUATERNARY_TDM_TX,
	.ff_port = AFE_PORT_ID_QUATERNARY_TDM_RX,
	.usecase_dt_count = MAX_TUNING_CONFIGS,
	.usecase = 0,
	.conf_index = 0,
	.delta_index = 0,
	.swap_duration = 30,
	.location = CIRRUS_COPP,
	.sp_enable = 1, // enable by default
	.vol_atten = 0,
    .resistance_en = true,
    .prot_en = false,
	.copp_idx = 0,
    .copp_ids={INVALID_COPPID, INVALID_COPPID, INVALID_COPPID},

	.swap = 0,
    .initialized = false,
};


#define CHAR_BIT 8
unsigned int crc32(int *message) { /* Only supports 32-bit for now */
    int i, j;
    unsigned int crc;

    crc = ~0;
    for(i = 0; i < message[0] - 1; i++) /* Loop through the array, excluding the CRC */
    {
        crc = crc ^ message[i];
        for (j = 0; j < sizeof(*message) * CHAR_BIT; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

static int crc_check(int *params, int size)
{
    int crc, crc_idx, ret = 0, i = 0;

    for (i = 0; i < size; i += params[i]) {
        crc_idx = i+params[i]-1;
        crc = crc32(&params[i]);
        if(crc!=params[crc_idx]) {
            pr_err("crc failed: %d != %d\n", crc, params[crc_idx]);
            ret = -1;
        }
    }

    return ret;
}

#define IS_DIGIT(number) ((number) >='0' && (number) <= '9')
#define IS_PARAM_CHAR(number) ((number) == '-' || (IS_DIGIT(number)))
static bool is_params_str(char *str)
{
    int i = 0, start = 0;
    bool ret = true;

    if(strlen(str)<=0)
        return -1;

    if(str[0] == '-')
        start = 1;
    for (i = start; i < strlen(str); i ++) {
        if (!IS_DIGIT(str[start])) {
            ret = false;
            break;
        }
    }

    return ret;
}

static int tuning_parser(const char *bytes, int size_byte, int *params, int *size)
{
    int i = 0, n, ret = 0;
    char buf[24] = {0,};

    *size = 0;
    for (i = 0; bytes[i] != '\0' && i < size_byte; i ++ ) {
        if(!IS_PARAM_CHAR(bytes[i]))
            continue;
        n = 0;
        for(; bytes[i] != ',' && bytes[i] != '\n' && bytes[i] != '\0'; i++) {
            if(!IS_PARAM_CHAR(bytes[i]))
                break;
            buf[n++] = bytes[i];
        }

        buf[n] = '\0';
        if(!is_params_str(buf))
            continue;

        if(kstrtos32(buf, 10, params++) < 0)
            continue;

        *size += 1;
        pr_info("%d: %s\n", *size, buf);
    }

    return ret;
}

#ifdef CACHE_COMMANDS
LIST_HEAD(cache_list);
static void cache_block_destroy(void* block)
{
    kfree(block);
}

static struct cache_block* crus_block_initial(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol, snd_kcontrol_put_t *put)
{
    struct cache_block *db = kzalloc(sizeof(struct cache_block), GFP_KERNEL);

    if(db==NULL) {
        pr_info("%s: Fail to alloc for: '%s'\n", __func__, kcontrol->id.name);
        return NULL;
    }

    memcpy(&db->kcontrol, kcontrol, sizeof(struct snd_kcontrol));
    memcpy(&db->ucontrol, ucontrol, sizeof(struct snd_ctl_elem_value));
    db->destroy = cache_block_destroy;

    return db;
}

static bool crus_cache_push(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol, snd_kcontrol_put_t *put)
{
	struct cache_block *db;
	struct list_head *cursor, *head;
	
    if (atomic_read(&this.qdsp_start_count[this.location]) == 0) {
        pr_info("%s: port started, no need cache: '%s'\n",
                __func__, kcontrol->id.name);
        return false;
    }

	list_for_each_safe(cursor, head, &cache_list) {
		db = list_entry(cursor, struct cache_block, entry);
        if(db==NULL)
            continue;
		if(!strcmp(db->kcontrol.id.name, kcontrol->id.name)) {
            pr_info("%s: cache block remove duplicated one: '%s'\n",
                    __func__, kcontrol->id.name);
			list_del(cursor);
            cache_block_destroy(db);
			break;
		}
	}

	db = crus_block_initial(kcontrol, ucontrol, put);
    if(db==NULL)
        return false;

	pr_info("%s: cache block: %p, '%s'\n", __func__, db, kcontrol->id.name);
    db->put = put;
	list_add_tail(&db->entry, &cache_list);

	return true;
}

static void crus_cache_pop_execute(void)
{
	struct cache_block *db;
	struct list_head *cursor, *head;

	if(list_empty(&cache_list))
		return;

	list_for_each_safe(cursor, head, &cache_list) {
		db = list_last_entry(&cache_list, struct cache_block, entry);
		if(db==NULL)
			continue;

		pr_info("%s:%p - put '%s'\n", __func__, db, db->kcontrol.id.name);

		if(db->put) {
			this.cache_execute = true;
			db->put(&db->kcontrol, &db->ucontrol);
			this.cache_execute = false;
		}

		list_del(cursor);
        cache_block_destroy(db);
		db=NULL;
	}

	INIT_LIST_HEAD(&cache_list);
}
#endif
static int crus_get_copp_id(void)
{
    int i = 0;
    for(i = 0; i < 3; i++) {
        if(this.copp_ids[i] != INVALID_COPPID)
            return this.copp_ids[i];
    }

    return 0;
}
static int crus_get_param(int port_id, int param_id, void *data, int length)
{
    int copp_idx = 0;

    if (atomic_read(&this.qdsp_start_count[this.location]) == 0) {
        pr_info("%s: port '0x%x' not yet start!\n", __func__, port_id);
        return 0;
    }

    switch(this.location) {
    case CIRRUS_AFE:
        crus_afe_get_params(port_id, this.module_id, param_id, data, length);
        break;
    case CIRRUS_COPP:
        copp_idx = crus_get_copp_id();
        crus_adm_get_params(port_id, copp_idx, this.instance_id,
                this.module_id, param_id, data, length, 0);
        break;
    default:
        break;
    }
    return 0;
}
static int crus_set_param(int port_id, int param_id, void* data, int length)
{
    int copp_idx = 0;
    if (atomic_read(&this.qdsp_start_count[this.location]) == 0) {
        pr_info("%s: port '0x%x' not yet start!\n", __func__, port_id);
        return 0;
    }

    switch(this.location) {
    case CIRRUS_AFE:
        crus_afe_set_params(port_id, this.module_id, param_id, data, length);
        break;
    case CIRRUS_COPP:
        copp_idx = crus_get_copp_id();
        crus_adm_set_params(port_id, copp_idx, this.instance_id,
                this.module_id, param_id, data,length);
        break;
    default:
        break;
    }
    return 0;
}

#define CONF_HEADER(buf, total_bytes, chunk_bytes, done) \
do{\
buf[0]=(total_bytes)*sizeof(int);\
buf[1]=(chunk_bytes)*sizeof(int);\
buf[2]=(done);\
buf[3]=0;\
buf[4]=4;} while(0)

static int crus_send_config(const char *data, int32_t length, int32_t port)
{
    int i = 0, ret = 0, chunk = 90, head_size = 5;
    int rest, last_index, total_size;
    int *payload = this.apr_payload;
    int *params = this.tuning;
    int param_id, payload_size;

    pr_err("Wing: Invalid port %d\n", port);
    /* Destination settings for message */
    if (port == this.ff_port)
        param_id = CRUS_PARAM_RX_SET_EXT_CONFIG;
    else if (port == this.fb_port)
        param_id = CRUS_PARAM_TX_SET_EXT_CONFIG;
    else {
        pr_err("Invalid port %d\n", port);
        return -EINVAL;
    }

    memset(params, 0x00, TUNING_MAX_SIZE * sizeof(int));
    memset(payload, 0x00, APR_CHUNK_SIZE * sizeof(int));

    //firmware_request("delta0_raw_full.txt", &bytes, &size_byte);
    if (tuning_parser(data, length, params, &total_size) < 0)
        return -1;
    if (crc_check(params, total_size) < 0)
        return -1;

    rest = total_size % chunk;
    if (rest == 0) {
        last_index = total_size/chunk -1;
        rest = chunk;
    } else {
        last_index = total_size/chunk;
    }

    pr_info("rest = %d, last_index=%d, total_size=%d\n", rest,
        last_index, total_size);

    payload_size = chunk + head_size;
    for(i = 0; i <= last_index - 1; i++) {
        CONF_HEADER(payload, total_size, chunk, 0);
        memcpy(&payload[head_size], &params[i * chunk], chunk * sizeof(int));
        crus_set_param(port, param_id, payload,
            payload_size * sizeof(int));
    }

    /* send last chunk */
    payload_size = rest + head_size;
    CONF_HEADER(payload, total_size, rest, 1/*done*/);
    memcpy(&payload[head_size], &params[i * chunk],
            rest * sizeof(int));
    crus_set_param(port, param_id, payload,
            payload_size * sizeof(int));

    return ret;
}

#define DELTA_HEADER(buf, total_bytes, chunk_bytes, done, index) \
do{\
buf[0]=(total_bytes)*sizeof(int);\
buf[1]=(chunk_bytes)*sizeof(int);\
buf[2]=(done);\
buf[3]=index;\
buf[4]=0;\
buf[5]=4;\
} while(0)

static int crus_send_delta(const char *data, uint32_t length)
{
    int i = 0, ret = 0, chunk = 90, head_size = 6;
    int rest, last_index, total_size, payload_size;
    int param_id = CRUS_PARAM_RX_SET_DELTA_CONFIG;
    int *payload = this.apr_payload;
    int *params = this.tuning;
    int port = this.ff_port;

    memset(params, 0x00, TUNING_MAX_SIZE * sizeof(int));
    memset(payload, 0x00, APR_CHUNK_SIZE * sizeof(int));
    //firmware_request("delta0_raw_full.txt", &bytes, &size_byte);
    if (tuning_parser(data, length, params, &total_size) < 0)
        return -1;
    if (crc_check(params, total_size) < 0)
        return -1;

    rest = total_size % chunk;
    if (rest == 0) {
        last_index = total_size/chunk -1;
        rest = chunk;
    } else {
        last_index = total_size/chunk;
    }

    pr_info("rest = %d, last_index=%d, total_size=%d\n", rest,
        last_index, total_size);

    payload_size = chunk + head_size;
    for(i = 0; i <= last_index - 1; i++) {
        DELTA_HEADER(payload, total_size, chunk, 0, 0);
        memcpy(&payload[head_size], &params[i * chunk], chunk * sizeof(int));
        crus_set_param(port, param_id, payload, payload_size * sizeof(int));
    }

    /* send last chunk */
    payload_size = rest + head_size;
    DELTA_HEADER(payload, total_size, rest, 1/*done*/, 0);
    memcpy(&payload[head_size], &params[i * chunk], rest * sizeof(int));
    crus_set_param(port, param_id, payload, payload_size * sizeof(int));

    return ret;
}

static int crus_location_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int location = ucontrol->value.integer.value[0];

	pr_info("%s: %d\n", __func__, location);

	switch(location) {
		case CIRRUS_AFE:
		case CIRRUS_COPP:
		case CIRRUS_NONE:
			this.location = location;
			break;
		default:
			break;
	}

	return 0;
}

static int crus_location_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: this.location= %d", __func__, this.location);
	ucontrol->value.integer.value[0] = this.location;
	return 0;
}


static int crus_rx_port_put(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	this.ff_port = ucontrol->value.integer.value[0];
	pr_info("%s: set rx port: %d\n", __func__, this.ff_port);
	return 0;
}

static int crus_rx_port_get(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = this.ff_port;
	return 0;
}
static int crus_tx_port_put(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	this.fb_port = ucontrol->value.integer.value[0];
	pr_info("%s: set tx port: %d\n", __func__, this.fb_port);
	return 0;
}

static int crus_tx_port_get(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = this.fb_port;
	return 0;
}

static int crus_enable_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	this.sp_enable = ucontrol->value.integer.value[0];

	crus_set_param(this.ff_port, CIRRUS_SP_ENABLE, (void *)&this.sp_enable,
			   sizeof(struct crus_single_data_t));

	if (this.prot_en)
		crus_set_param(this.fb_port, CIRRUS_SP_ENABLE, (void *)&this.sp_enable,
				   sizeof(struct crus_single_data_t));

	return 0;
}

static int crus_enable_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = this.sp_enable;
	return 0;
}

static int crus_prot_enable_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_err("Cirrus SP Protection Enable is set via DT\n");
	return 0;
}

static int crus_prot_enable_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = this.prot_en ? 1 : 0;
	return 0;
}

static int crus_usecase_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	const int index = ucontrol->value.integer.value[0];
	struct crus_rx_run_case_ctrl_t case_ctrl;
	struct crus_rx_temperature_t rx_temp;
	int buffer[CRUS_MAX_BUFFER_SIZE / 4];

	if (index >= e->items) {
		pr_err("Config index out of bounds (%d)\n", index);
		return -EINVAL;
	}

	pr_info("%s: '%s' <-- '%s'\n", __func__, kcontrol->id.name, e->texts[index]);

	this.usecase = index;

#ifdef CACHE_COMMANDS
	if(!this.cache_execute &&  crus_cache_push(kcontrol, ucontrol, crus_usecase_put))
		return 0;
#endif

	crus_get_param(this.ff_port, CRUS_PARAM_RX_GET_TEMP, buffer,
			CRUS_MAX_BUFFER_SIZE);

	memcpy(&rx_temp, buffer, sizeof(rx_temp));


	case_ctrl.status_l = 1;
	case_ctrl.status_r = 1;
	case_ctrl.z_l = rx_temp.z_l;
	case_ctrl.z_r = rx_temp.z_r;
	case_ctrl.checksum_l = rx_temp.z_l + 1;
	case_ctrl.checksum_r = rx_temp.z_r + 1;
	case_ctrl.atemp = rx_temp.amb_temp_l;
	case_ctrl.value = this.usecase;

	if (this.prot_en) {
		crus_set_param(this.fb_port, CRUS_PARAM_TX_SET_USECASE,
			(void *)&this.usecase,
            sizeof(this.usecase));
	}


	crus_set_param(this.ff_port, CRUS_PARAM_RX_SET_USECASE,
            (void *)&case_ctrl,sizeof(case_ctrl));

	return 0;
}


static int crus_usecase_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	pr_info("%s: %d\n", __func__, this.usecase);
	ucontrol->value.integer.value[0] = this.usecase;

    return 0;
}
static int crus_load_rx_config_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
    struct soc_enum *soc_enum = (struct soc_enum *)kcontrol->private_value;
    unsigned int i = ucontrol->value.enumerated.item[0];
    const struct firmware *firmware;


    if (i >= soc_enum->items) {
        pr_err("Invalid mixer input (%u)\n", i);
        return -EINVAL;
    }

    if (request_firmware(&firmware, soc_enum->texts[i], component->dev) != 0) {
        pr_err("Request firmware '%s' failed!\n", soc_enum->texts[i]);
        this.delta_index = 0;
        return -EINVAL;
    }

    pr_info("Requestd:%s, value=%d\n", soc_enum->texts[i], soc_enum->values[i]);
    crus_send_config(firmware->data, firmware->size, this.ff_port);
    this.conf_index = i;

    release_firmware(firmware);

    return 0;
}

static int crus_load_rx_config_get(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: %d\n", __func__, this.conf_index);
	ucontrol->value.integer.value[0] = this.conf_index;
    return 0;
}

static int crus_load_delta_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: %d\n", __func__, this.conf_index);
	ucontrol->value.enumerated.item[0] = this.delta_index;

	return 0;
}

static int crus_load_delta_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
    struct soc_enum *soc_enum = (struct soc_enum *)kcontrol->private_value;
    unsigned int i = ucontrol->value.enumerated.item[0];
    const struct firmware *firmware;
	struct crus_single_data_t data;


    if (i >= soc_enum->items) {
        pr_err("Invalid mixer input (%u)\n", i);
        return -EINVAL;
    }

    if (request_firmware(&firmware, soc_enum->texts[i], component->dev) != 0) {
        pr_err("Request firmware '%s' failed!\n", soc_enum->texts[i]);
        this.delta_index = 0;
        return -EINVAL;
    }


    pr_info("Requestd: '%s'\n", soc_enum->texts[i]);
    crus_send_delta(firmware->data, firmware->size);

    this.delta_index = i;
    release_firmware(firmware);

    data.value = 0;
    crus_set_param(this.ff_port, CRUS_PARAM_RX_RUN_DELTA_CONFIG, &data,
            sizeof(struct crus_single_data_t));

    return 0;
}
#if 0
static int crus_vol_attn_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	const int crus_set = ucontrol->value.integer.value[0];
	struct crus_dual_data_t data;

	//mutex_lock(&this.sp_lock);

	this.vol_atten = crus_set;

	switch (crus_set) {
		case 0: /* 0dB */
			data.data1 = VOL_ATTN_MAX;
			data.data2 = VOL_ATTN_MAX;
			break;
		case 1: /* -18dB */
			data.data1 = VOL_ATTN_18DB;
			data.data2 = VOL_ATTN_18DB;
			break;
		case 2: /* -24dB */
			data.data1 = VOL_ATTN_24DB;
			data.data2 = VOL_ATTN_24DB;
			break;
		default:
			return -EINVAL;
	}

	crus_set_param(this.ff_port, CRUS_PARAM_RX_SET_ATTENUATION, &data,
				sizeof(struct crus_dual_data_t));

	//mutex_unlock(&this.sp_lock);

	return 0;
}

static int crus_vol_attn_get(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("Starting Cirrus SP Volume Attenuation Get function call\n");
	ucontrol->value.integer.value[0] = this.vol_atten;

	return 0;
}
#endif

static int crus_chan_swap_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct crus_dual_data_t data;
	const int crus_set = ucontrol->value.integer.value[0];

	switch (crus_set) {
	case 0: /* L/R */
		data.data1 = 1;
		break;
	case 1: /* R/L */
		data.data1 = 2;
		break;
	default:
		return -EINVAL;
	}

	data.data2 = this.swap;

	crus_set_param(this.ff_port, CRUS_PARAM_RX_CHANNEL_SWAP, &data
			   ,sizeof(struct crus_dual_data_t));

	this.swap = !!crus_set;

	return 0;
}
static int crus_chan_swap_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct crus_single_data_t data;
	pr_debug("%s: %d\n", __func__, this.swap);
	crus_get_param(this.ff_port, CRUS_PARAM_RX_GET_CHANNEL_SWAP, &data,
			   sizeof(struct crus_single_data_t));

	ucontrol->value.integer.value[0] = this.swap;

	return 0;
}

static int crus_chan_swap_dur_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	int crus_set = ucontrol->value.integer.value[0];

	if ((crus_set < 0) || (crus_set > MAX_CHAN_SWAP_SAMPLES)) {
		pr_err("Value out of range (%d)\n", crus_set);
		return -EINVAL;
	}

	if (crus_set < MIN_CHAN_SWAP_SAMPLES) {
		pr_info("Received %d, round up to min value %d\n",
			crus_set, MIN_CHAN_SWAP_SAMPLES);
		crus_set = MIN_CHAN_SWAP_SAMPLES;
	}

	this.swap_duration = crus_set;

	return 0;
}

static int crus_chan_swap_dur_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: %d\n", __func__, this.swap_duration);

	ucontrol->value.integer.value[0] = this.swap_duration;

	return 0;
}
static int crus_fail_det_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	this.fail_det = ucontrol->value.integer.value[0];
	return 0;
}

static int crus_fail_det_get(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = this.fail_det;
	return 0;
}


static int crus_resistance_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct crus_single_data_t data;
	const int crus_set = ucontrol->value.integer.value[0];

	if (!this.prot_en) {
		pr_err("CRUS_SP: Need feedback module to enable resistance calculation\n");
		return -EINVAL;
	}

	switch (crus_set) {
	case 0: /* Disable */
		this.resistance_en = false;
		data.value = 0;
		break;
	case 1: /* Enable */
		this.resistance_en = true;
		data.value = 1;
		break;
	default:
		return -EINVAL;
	}

   // crus_set_param(this.fb_port, CRUS_PARAM_TX_RESISTANCE,
   //         &data, sizeof(struct crus_single_data_t));

	return 0;
}

static int crus_resistance_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = this.resistance_en ? 1 : 0;
	return 0;
}

static const char * const crus_en_text[] = {"Disable", "Enable"};
static const char * const crus_prot_en_text[] = {"Disable", "Enable"};
static const char * const crus_usecase_text[] = {"Music", "Game", "Handsfree","Movie"};
static const char * const crus_conf_load_text[] = {"Idle", "Load RX", "Load TX"};
static const char * const crus_conf_load_no_prot_text[] = {"Idle", "Load"};
static const char * const crus_chan_swap_text[] = {"LR", "RL"};
static const char * const crus_resistance_text[] = {"Disable", "Enable"};
static const char * const cirrus_location_text[] = {"NONE", "COPP", "AFE"};

static const struct soc_enum cirrus_location_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(cirrus_location_text), cirrus_location_text),
};
static struct soc_enum crus_usecase_enum =
		SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(crus_usecase_text), crus_usecase_text);
static const char * const crus_dt_text[] = {
	"delta_0.txt", "delta_1.txt", "delta_2.txt", "delta_3.txt", "delta_4.txt",
	"delta_5.txt", "delta_6.txt", "delta_7.txt", "delta_8.txt", "delta_9.txt",
	"delta_10.txt", "delta_11.txt", "delta_12.txt", "delta_13.txt", "delta_14.txt",
	"delta_15.txt", "delta_16.txt", "delta_17.txt", "delta_18.txt", "delta_19.txt",
	"delta_20.txt", "delta_21.txt", "delta_22.txt", "delta_23.txt", "delta_24.txt",
	"delta_25.txt", "delta_26.txt", "delta_27.txt", "delta_28.txt", "delta_29.txt",
};

static struct soc_enum crus_dt_enum =
        SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(crus_dt_text), crus_dt_text);
static const char * const crus_conf_text[] = {
	"config_0.txt", "config_1.txt", "config_2.txt", "config_3.txt", "config_4.txt",
	"config_5.txt", "config_6.txt", "config_7.txt", "config_8.txt", "config_9.txt",
	"config_10.txt", "config_11.txt", "config_12.txt", "config_13.txt", "config_14.txt",
	"config_15.txt", "config_16.txt", "config_17.txt", "config_18.txt", "config_19.txt",
	"config_20.txt", "config_21.txt", "config_22.txt", "config_23.txt", "config_24.txt",
	"config_25.txt", "config_26.txt", "config_27.txt", "config_28.txt", "config_29.txt",
};

static struct soc_enum crus_conf_enum =
        SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(crus_conf_text), crus_conf_text);

static const struct snd_kcontrol_new crus_core_controls[] = {
	SOC_SINGLE_EXT("Cirrus SP Rx Port", SND_SOC_NOPM, 0, 0x7FFFFFF, 0,
		     crus_rx_port_get, crus_rx_port_put),
	SOC_SINGLE_EXT("Cirrus SP Tx Port", SND_SOC_NOPM, 0, 0x7FFFFFF, 0,
		     crus_tx_port_get, crus_tx_port_put),
	SOC_ENUM_EXT("Cirrus SP Module", (struct soc_enum)
		     SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(crus_en_text),
					 crus_en_text),
		     crus_enable_get, crus_enable_put),
	SOC_ENUM_EXT("Cirrus SP Protection", (struct soc_enum)
		     SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(crus_prot_en_text),
					 crus_prot_en_text),
		     crus_prot_enable_get, crus_prot_enable_put),

	SOC_ENUM_EXT("Cirrus SP Channel Swap", (struct soc_enum)
		     SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(crus_chan_swap_text),
					 crus_chan_swap_text),
		     crus_chan_swap_get, crus_chan_swap_put),
	SOC_ENUM_EXT("Cirrus SP Usecase", crus_usecase_enum,
		     crus_usecase_get, crus_usecase_put),
	SOC_SINGLE_EXT("Cirrus SP Channel Swap Duration", SND_SOC_NOPM, 0,
		       MAX_CHAN_SWAP_SAMPLES, 0,
		       crus_chan_swap_dur_get, crus_chan_swap_dur_put),
	SOC_ENUM_EXT("Cirrus SP Resistance", (struct soc_enum)
		     SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(crus_resistance_text),
					 crus_resistance_text),
		     crus_resistance_get, crus_resistance_put),
	SOC_ENUM_EXT("Cirrus SP Location", cirrus_location_enum[0],
	crus_location_get, crus_location_put),
	SOC_ENUM_EXT("Cirrus SP Delta", crus_dt_enum,
            crus_load_delta_get, crus_load_delta_put),
	SOC_ENUM_EXT("Cirrus SP Load Rx Config", crus_conf_enum,
            crus_load_rx_config_get, crus_load_rx_config_put),
};
static const struct snd_kcontrol_new crus_prot_controls[] = {
	SOC_SINGLE_BOOL_EXT("Cirrus SP Failure Detection", 0,
			    crus_fail_det_get, crus_fail_det_put),
};

static const struct snd_kcontrol_new crus_no_prot_controls[] = {
};
#if 0
static int crus_status_dump(int module_id, int param_id)
{
	int port;
	int i = 0;
	short *vmon0, *vmon1, *imon0, *imon1;
	static int param_buf[1024] = { 0 };

	switch (module_id) {
	case CRUS_MODULE_ID_TX:
		port = this.fb_port;
		break;
	case CRUS_MODULE_ID_RX:
		port = this.ff_port;
		break;
	default:
		pr_info("%s: Unrecognized port ID (%d)\n", __func__,
			crus_sp_hdr.module_id);
		port = this.ff_port;
		break;
	}

	memset(param_buf, 0x0, sizeof(param_buf));

	crus_get_param(port, this.module, param_id, param_buf, 96*4);
	vmon0 = (short *)&(param_buf[0]);
	imon1 = (short *)&(param_buf[24]);
	vmon1 = (short *)&(param_buf[24*2]);
	imon0 = (short *)&(param_buf[24*3]);

	for(i = 0; i < 48; i ++) {
		pr_info("%d, %d, %d, %d\n", vmon0[i], imon0[i], vmon1[i], imon1[i]);
	}

	return 0;
}
#endif
void msm_crus_pb_set_copp_idx(int port_id, int copp_idx)
{
	if (port_id != this.ff_port)
		return;

	pr_info("%s: port_id = 0x%x, copp_idx = %d\n", __func__, port_id, copp_idx);
	this.copp_idx = copp_idx;
}
EXPORT_SYMBOL(msm_crus_pb_set_copp_idx);

void msm_crus_pb_add_controls(struct snd_soc_component *component)
{
	pr_err("Wing: %s: \n", __func__);
	snd_soc_add_component_controls(component, crus_core_controls,
				      ARRAY_SIZE(crus_core_controls));
	pr_info("%s: end\n", __func__);
}

EXPORT_SYMBOL(msm_crus_pb_add_controls);

int crus_qdsp_port_start(u16 port_id, int copp_id)
{
	if (port_id != this.ff_port)
		return 0;

    /* copp_id = -1 in AFE*/
    if(copp_id == INVALID_COPPID && this.location == CIRRUS_COPP)
        return 0;

    atomic_inc(&this.qdsp_start_count[this.location]);
    pr_info("%s:port_id = 0x%x, copp_id = %d, count = %d\n",
            __func__, port_id, copp_id,
            this.qdsp_start_count[this.location]);

    if(this.location==CIRRUS_COPP)
        this.copp_ids[copp_id] = copp_id;

#ifdef CACHE_COMMANDS
	crus_cache_pop_execute();
#endif
	return 0;
}
EXPORT_SYMBOL(crus_qdsp_port_start);

int crus_qdsp_port_close(u16 port_id, int copp_id)
{
    if (port_id != this.ff_port)
        return 0;

    /* copp_id = -1 in AFE*/
    if(copp_id == -1 && this.location == CIRRUS_COPP)
        return 0;

    if (atomic_dec_return(&this.qdsp_start_count[this.location]) == 0) {
        pr_info("%s: all stream on '0x%x' closed\n",
                __func__, port_id);
    }

    pr_info("%s: port_id = 0x%x, copp_id = %d, count = %d\n", __func__, port_id,
            copp_id, this.qdsp_start_count[this.location]);

    if(this.location==CIRRUS_COPP) {
        this.copp_ids[copp_id] = INVALID_COPPID;
    }

	return 0;
}
EXPORT_SYMBOL(crus_qdsp_port_close);

static long crus_sp_shared_ioctl(struct file *f, unsigned int cmd,
				 void __user *arg)
{
    int result = 0, port, n;
	uint32_t bufsize = 0, size;
	void *io_data = NULL;


	pr_info("%s\n", __func__);

	if (copy_from_user(&size, arg, sizeof(size))) {
		pr_err("%s: copy_from_user (size) failed\n", __func__);
		result = -EFAULT;
		goto exit;
	}

	/* Copy IOCTL header from usermode */
	if (copy_from_user(&crus_sp_hdr, arg, size)) {
		pr_err("%s: copy_from_user (struct) failed\n", __func__);
		result = -EFAULT;
		goto exit;
		}

    switch (crus_sp_hdr.module_id) {
        case CRUS_MODULE_ID_TX:
            port = this.fb_port;
            break;
        case CRUS_MODULE_ID_RX:
            port = this.ff_port;
            break;
        default:
            pr_info("Unrecognized port ID (%d)\n",
                    crus_sp_hdr.module_id);
            port = this.ff_port;
    }

    bufsize = crus_sp_hdr.data_length;
    io_data = kzalloc(bufsize, GFP_KERNEL);

    switch (cmd) {
        case CRUS_SP_IOCTL_GET:
            crus_get_param(port, crus_sp_hdr.param_id, io_data, bufsize);
            result = copy_to_user(crus_sp_hdr.data, io_data, bufsize);
            if (result) {
                pr_err("copy_to_user failed (%d)\n", result);
                result = -EFAULT;
            } else {
                result = bufsize;
            }
            break;
        case CRUS_SP_IOCTL_SET:
            result = copy_from_user(io_data, (void *)crus_sp_hdr.data,
                    bufsize);
            if (result) {
                pr_err("copy_from_user failed (%d)\n", result);
                result = -EFAULT;
                goto exit_io;
            }
#if 1
            for (n=0; n < bufsize/sizeof(int); n ++) {
                pr_info("iodata[%d]=%d\n", n, ((int*)io_data)[n]);
            }
#endif
            crus_set_param(port, crus_sp_hdr.param_id, io_data, bufsize);
            break;
        default:
            pr_err("Invalid IOCTL, command = %d\n", cmd);
            result = -EINVAL;
            break;
    }

exit_io:
    kfree(io_data);
exit:
    return result;
}

static long crus_sp_ioctl(struct file *f,
		unsigned int cmd, unsigned long arg)
{
	pr_info("%s\n", __func__);

	return crus_sp_shared_ioctl(f, cmd, (void __user *)arg);
}

static long crus_compat_ioctl(struct file *f,
		unsigned int cmd, unsigned long arg)
{
	unsigned int cmd64;

	pr_info("%s\n", __func__);

	switch (cmd) {
	case CRUS_SP_IOCTL_GET32:
		cmd64 = CRUS_SP_IOCTL_GET;
		break;
	case CRUS_SP_IOCTL_SET32:
		cmd64 = CRUS_SP_IOCTL_SET;
		break;
	default:
		pr_err("%s: Invalid IOCTL, command = %d!\n", __func__, cmd);
		return -EINVAL;
	}

	return crus_sp_shared_ioctl(f, cmd64, compat_ptr(arg));
}

static int crus_ioctl_open(struct inode *inode, struct file *f)
{
	pr_debug("%s\n", __func__);

	atomic_inc(&this.ioctl_count);
	return 0;
}

static int crus_ioctl_release(struct inode *inode, struct file *f)
{
	atomic_dec(&this.ioctl_count);

	return 0;
}

static ssize_t temperature_left_show(struct device *dev,
				     struct device_attribute *a, char *buf)
{
	struct crus_rx_temperature_t rx_temp;
	int buffer[CRUS_MAX_BUFFER_SIZE / 4];
	int z, r, t, temp0;

	crus_get_param(this.ff_port, CRUS_PARAM_RX_GET_TEMP,
			   buffer, CRUS_MAX_BUFFER_SIZE);

	memcpy(&rx_temp, buffer, sizeof(rx_temp));

	if ((rx_temp.hp_status_l != 2) || (rx_temp.full_status_l != 2))
		return snprintf(buf, PAGE_SIZE, "Calibration is not done\n");

	z = rx_temp.z_l;
	r = rx_temp.temp_l;
	temp0 = rx_temp.amb_temp_l;

	t = (CRUS_MATERIAL_FACTOR * CRUS_T_SCALE_FACTOR * (r-z) / z) +
	    (temp0 * CRUS_T_SCALE_FACTOR);

	return snprintf(buf, PAGE_SIZE, "%d.%05dc\n", t / CRUS_T_SCALE_FACTOR,
			t % CRUS_T_SCALE_FACTOR);
}

static DEVICE_ATTR_RO(temperature_left);

static ssize_t temperature_right_show(struct device *dev,
				      struct device_attribute *a, char *buf)
{
	struct crus_rx_temperature_t rx_temp;
	int buffer[CRUS_MAX_BUFFER_SIZE / 4];
	int z, r, t, temp0;

	crus_get_param(this.ff_port, CRUS_PARAM_RX_GET_TEMP,
			   buffer, CRUS_MAX_BUFFER_SIZE);

	memcpy(&rx_temp, buffer, sizeof(rx_temp));

	if ((rx_temp.hp_status_r != 2) || (rx_temp.full_status_r != 2))
		return snprintf(buf, PAGE_SIZE, "Calibration is not done\n");

	z = rx_temp.z_r;
	r = rx_temp.temp_r;
	temp0 = rx_temp.amb_temp_r;

	t = (CRUS_MATERIAL_FACTOR * CRUS_T_SCALE_FACTOR * (r-z) / z) +
	    (temp0 * CRUS_T_SCALE_FACTOR);

	return snprintf(buf, PAGE_SIZE, "%d.%05dc\n", t / CRUS_T_SCALE_FACTOR,
			t % CRUS_T_SCALE_FACTOR);
}
static DEVICE_ATTR_RO(temperature_right);
#if 0
static ssize_t resistance_left_show(struct device *dev,
				    struct device_attribute *a, char *buf)
{
	struct crus_rx_temperature_t rx_temp;
	struct crus_resistance_t res;
	int buffer[CRUS_MAX_BUFFER_SIZE / 4];
	int r;

	if (this.resistance_en) {
		crus_get_param(this.fb_port, CRUS_PARAM_TX_RESISTANCE, &res,
                sizeof(struct crus_resistance_t));

		if (!res.status_l)
			return snprintf(buf, PAGE_SIZE,
					"Invalid status for left channel\n");

		if (res.r_l == 0)
			return snprintf(buf, PAGE_SIZE,
					"Short circuit detected\n");

		if (res.r_l >= CRUS_RES_OPEN_CIRCUIT)
			return snprintf(buf, PAGE_SIZE,
					"Open circuit detected\n");

		r = res.r_l * CRUS_AMP_FACTOR;
	} else {
		crus_get_param(this.ff_port, CRUS_PARAM_RX_GET_TEMP,
				   buffer, CRUS_MAX_BUFFER_SIZE);

		memcpy(&rx_temp, buffer, sizeof(rx_temp));

		if ((rx_temp.hp_status_l != 2) || (rx_temp.full_status_l != 2))
			return snprintf(buf, PAGE_SIZE,
					"Calibration is not done\n");

		r = rx_temp.temp_l * CRUS_AMP_FACTOR;
	}

	return snprintf(buf, PAGE_SIZE, "%d.%08d ohms\n",
			r / CRUS_Z_SCALE_FACTOR, r % CRUS_Z_SCALE_FACTOR);
}
static DEVICE_ATTR_RO(resistance_left);

static ssize_t resistance_right_show(struct device *dev,
				     struct device_attribute *a, char *buf)
{
	struct crus_rx_temperature_t rx_temp;
	struct crus_resistance_t res;
	int buffer[CRUS_MAX_BUFFER_SIZE / 4];
	int r;

	if (this.resistance_en) {
		crus_get_param(this.fb_port, CRUS_PARAM_TX_RESISTANCE, &res,
                sizeof(struct crus_resistance_t));

		if (!res.status_r)
			return snprintf(buf, PAGE_SIZE,
					"Invalid status for right channel\n");

		if (res.r_r == 0)
			return snprintf(buf, PAGE_SIZE,
					"Short circuit detected\n");

		if (res.r_r >= CRUS_RES_OPEN_CIRCUIT)
			return snprintf(buf, PAGE_SIZE,
					"Open circuit detected\n");

		r = res.r_r * CRUS_AMP_FACTOR;
	} else {
		crus_get_param(this.ff_port, CRUS_PARAM_RX_GET_TEMP,
				   buffer, CRUS_MAX_BUFFER_SIZE);

		memcpy(&rx_temp, buffer, sizeof(rx_temp));

		if ((rx_temp.hp_status_r != 2) || (rx_temp.full_status_r != 2))
			return snprintf(buf, PAGE_SIZE,
					"Calibration is not done\n");

		r = rx_temp.temp_r * CRUS_AMP_FACTOR;
	}

	return snprintf(buf, PAGE_SIZE, "%d.%08d ohms\n",
			r / CRUS_Z_SCALE_FACTOR, r % CRUS_Z_SCALE_FACTOR);
}
static DEVICE_ATTR_RO(resistance_right);
#endif
static struct attribute *crus_sp_attrs[] = {
	&dev_attr_temperature_left.attr,
	&dev_attr_temperature_right.attr,
#if 0
	&dev_attr_resistance_left.attr,
	&dev_attr_resistance_right.attr,
#endif
	NULL,
};

static const struct attribute_group crus_sp_group = {
	.attrs  = crus_sp_attrs,
};

static const struct attribute_group *crus_sp_groups[] = {
	&crus_sp_group,
	NULL,
};

#ifdef CONFIG_OF
static int msm_cirrus_playback_probe(struct platform_device *pdev)
{
	int i;

	pr_err("Wing: Initializing platform device\n");

	this.usecase_dt_count = of_property_count_strings(pdev->dev.of_node,
							     "usecase-names");
	if (this.usecase_dt_count <= 0) {
		pr_debug("Usecase names not found\n");
		this.usecase_dt_count = 0;
		return 0;
	}

	if ((this.usecase_dt_count > 0) &&
	    (this.usecase_dt_count <= MAX_TUNING_CONFIGS))
		of_property_read_string_array(pdev->dev.of_node,
					      "usecase-names",
					      crus_sp_usecase_dt_text,
					      this.usecase_dt_count);
	else if (this.usecase_dt_count > MAX_TUNING_CONFIGS) {
		pr_err("Max of %d usecase configs allowed\n",
			MAX_TUNING_CONFIGS);
		return -EINVAL;
	}

	for (i = 0; i < this.usecase_dt_count; i++)
		pr_info("Usecase[%d] = %s\n", i,
			 crus_sp_usecase_dt_text[i]);

	this.prot_en = of_property_read_bool(pdev->dev.of_node,
						  "protect-en");

	return 0;
}

static const struct of_device_id msm_cirrus_playback_dt_match[] = {
	{.compatible = "cirrus,msm-cirrus-playback"},
	{}
};
MODULE_DEVICE_TABLE(of, msm_cirrus_playback_dt_match);

static struct platform_driver msm_cirrus_playback_driver = {
	.driver = {
		.name = "msm-cirrus-playback",
		.owner = THIS_MODULE,
		.of_match_table = msm_cirrus_playback_dt_match,
	},
	.probe = msm_cirrus_playback_probe,
};
#endif

static const struct file_operations crus_sp_fops = {
	.owner = THIS_MODULE,
	.open = crus_ioctl_open,
	.release = crus_ioctl_release,
	.unlocked_ioctl = crus_sp_ioctl,
	.compat_ioctl = crus_compat_ioctl,
};

struct miscdevice crus_sp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "msm_cirrus_playback",
	.fops = &crus_sp_fops,
};

int __init crus_sp_init(void)
{
    if(this.initialized)
        return 0;
	pr_info("Initializing misc device\n");

	misc_register(&crus_sp_misc);

	if (sysfs_create_groups(&crus_sp_misc.this_device->kobj,
				crus_sp_groups))
		pr_err("%s: Could not create sysfs groups\n", __func__);

#ifdef CONFIG_OF
	platform_driver_register(&msm_cirrus_playback_driver);
#endif

    this.initialized = true;
	pr_info("Initializing misc device end\n");
	return 0;
}

void __exit crus_sp_exit(void)
{
    if(!this.initialized)
        return;

#ifdef CONFIG_OF
	platform_driver_unregister(&msm_cirrus_playback_driver);
#endif
	sysfs_remove_groups(&crus_sp_misc.this_device->kobj,
				crus_sp_groups);

	misc_deregister(&crus_sp_misc);
}

//MODULE_AUTHOR("Cirrus SP");
//MODULE_DESCRIPTION("Providing Interface to Cirrus SP");
//MODULE_LICENSE("GPL");
