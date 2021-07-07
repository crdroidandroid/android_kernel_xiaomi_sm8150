/* Copyright (c) 2015 Cirrus Logic, Inc.
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef MSM_CIRRUS_PLAYBACK_H
#define MSM_CIRRUS_PLAYBACK_H
#include <linux/slab.h>
#include <linux/list.h>
#include <sound/soc.h>
#include <dsp/apr_audio-v2.h>
#include <uapi/sound/msm-cirrus-playback.h>


#define VOL_ATTN_MAX		0x7FFFFFFF
#define VOL_ATTN_18DB		257698038
#define VOL_ATTN_24DB		128849019
#define CONFIG_FILE_SIZE	128
#define PAYLOAD_FOLLOWS_CONFIG	4
#define MAX_TUNING_CONFIGS	4
#define MIN_CHAN_SWAP_SAMPLES	48
#define MAX_CHAN_SWAP_SAMPLES	9600
#define CRUS_MAX_BUFFER_SIZE	384
#define CRUS_RES_OPEN_CIRCUIT	65535
#define CRUS_BUF_SIZE 256
#define INVALID_COPPID (-1)

/* Payload struct for getting or setting one integer value from/to the DSP
 * module
 */
struct crus_single_data_t {
	int32_t	value;
};

/* Payload struct for getting or setting two integer values from/to the DSP
 * module
 */
struct crus_dual_data_t {
	int32_t data1;
	int32_t data2;
};

/* Payload struct for getting or setting three integer values from/to the DSP
 * module
 */
struct crus_triple_data_t {
	int32_t data1;
	int32_t data2;
	int32_t data3;
};

/* Payload struct for setting the RX and TX use cases */
struct crus_rx_run_case_ctrl_t {
	int32_t value;
	int32_t status_l;
	int32_t checksum_l;
	int32_t z_l;
	int32_t status_r;
	int32_t checksum_r;
	int32_t z_r;
	int32_t atemp;
};

#define APR_CHUNK_SIZE		256
#define TUNING_MAX_SIZE 2048

/* Payload struct for sending an external configuration string to the DSP
 * module
 */
struct crus_external_config_t {
	uint32_t total_size;
	uint32_t chunk_size;
	int32_t done;
	int32_t reserved;
	int32_t config;
	char data[APR_CHUNK_SIZE];
};
/* Payload struct for sending an external tuning transition string to the DSP
 * module
 */
struct crus_delta_config_t {
	uint32_t total_size;
	uint32_t chunk_size;
	int32_t done;
	int32_t index;
	int32_t reserved;
	int32_t config;
	char data[APR_CHUNK_SIZE];
};

struct crus_rx_temperature_t {
	uint32_t cal_status_l;
	uint32_t temp_r;
	uint32_t z_r;
	uint32_t temp_l;
	uint32_t z_l;
	uint32_t cal_status_r;
	uint32_t amb_temp_l;
	uint32_t excur_model_r;
	uint32_t excur_model_l;
	uint32_t cksum_r;
	uint32_t amb_temp_r;
	uint32_t cksum_l;
	uint32_t hp_status_l;
	uint32_t full_status_l;
	uint32_t hp_status_r;
	uint32_t full_status_r;
};

struct crus_resistance_t {
	uint32_t status_l;
	uint32_t status_r;
	uint32_t r_l;
	uint32_t r_r;
};

struct cache_block {
	struct snd_kcontrol kcontrol;
	struct snd_ctl_elem_value ucontrol;
	snd_kcontrol_put_t *put;
	void (*destroy)(void *block);
	struct list_head entry;
};

typedef struct cache_block cache_block_t;

struct crus_control_t {
	struct device *device;
	int32_t module_id;
	int32_t fb_port;
	int32_t ff_port;
	bool qdsp_start;
	int32_t sp_enable;
	bool cache_execute;
	bool prot_en;
	int32_t copp_idx;
	int instance_id;
	int copp_ids[3];
	atomic_t qdsp_start_count[3];
	int32_t vol_atten;
	int32_t usecase;
	int32_t location;
	int32_t swap_duration;
	int32_t swap;
	int32_t usecase_dt_count;
    bool resistance_en;
    bool fail_det;
	atomic_t ioctl_count;
	int32_t conf_index;
	int32_t delta_index;
	int32_t apr_payload[APR_CHUNK_SIZE];
	int32_t tuning[TUNING_MAX_SIZE];
    bool initialized;
};

int crus_afe_get_params(u16 port_id, uint32_t module_id, uint32_t param_id,
         void* data, int size);
int crus_afe_set_params(u16 port_id, uint32_t module_id, uint32_t param_id,
        void* data, int size);
int crus_adm_set_params(int port_id, int copp_idx, int instance_id,
        uint32_t module_id, uint32_t param_id, char *params,
        uint32_t length);
int crus_adm_get_params(int port_id, int copp_idx, int instance_id,
        uint32_t module_id, uint32_t param_id, char *params,
        uint32_t length, uint32_t client_id);
int crus_qdsp_port_start(u16 port_id, int copp_id);
int crus_qdsp_port_close(u16 port_id, int copp_id);
int crus_afe_callback(uint32_t opcode, void* payload, int size);

void msm_crus_pb_add_controls(struct snd_soc_component *component);
#endif /* _MSM_CIRRUS_PLAYBACK_H */

