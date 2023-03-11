// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include <linux/math64.h>
#include "en/aso.h"
#include "meter.h"
#include "en/tc_priv.h"
#include "en/tc/post_act.h"

#define START_COLOR_SHIFT 28
#define METER_MODE_SHIFT 24
#define CBS_EXP_SHIFT 24
#define CBS_MAN_SHIFT 16
#define CIR_EXP_SHIFT 8

/* cir = 8*(10^9)*cir_mantissa/(2^cir_exponent)) bits/s */
#define CONST_CIR 8000000000ULL
#define CALC_CIR(m, e)  ((CONST_CIR * (m)) >> (e))
#define MAX_CIR ((CONST_CIR * 0x100) - 1)

/* cbs = cbs_mantissa*2^cbs_exponent */
#define CALC_CBS(m, e)  ((m) << (e))
#define MAX_CBS ((0x100ULL << 0x1F) - 1)
#define MAX_HW_CBS 0x7FFFFFFF

struct mlx5e_flow_meters {
	struct mlx5_core_dev *mdev;
	enum mlx5_flow_namespace_type ns_type;
	struct mlx5e_aso *aso;
	int log_granularity;

	DECLARE_HASHTABLE(hashtbl, 8);

	struct mutex sync_lock; /* protect flow meter operations */
	struct list_head partial_list;
	struct list_head full_list;

	struct mlx5e_post_act *post_act;
};

static void
mlx5e_flow_meter_cir_calc(u64 cir, u8 *man, u8 *exp)
{
	s64 _cir, _delta, delta = S64_MAX;
	u8 e, _man = 0, _exp = 0;
	u64 m;

	for (e = 0; e <= 0x1F; e++) { /* exp width 5bit */
		m = cir << e;
		if ((s64)m < 0) /* overflow */
			break;
		m = div64_u64(m, CONST_CIR);
		if (m > 0xFF) /* man width 8 bit */
			continue;
		_cir = CALC_CIR(m, e);
		_delta = cir - _cir;
		if (_delta < delta) {
			_man = m;
			_exp = e;
			if (!_delta)
				goto found;
			delta = _delta;
		}
	}

found:
	*man = _man;
	*exp = _exp;
}

static void
mlx5e_flow_meter_cbs_calc(u64 cbs, u8 *man, u8 *exp)
{
	s64 _cbs, _delta, delta = S64_MAX;
	u8 e, _man = 0, _exp = 0;
	u64 m;

	for (e = 0; e <= 0x1F; e++) { /* exp width 5bit */
		m = cbs >> e;
		if (m > 0xFF) /* man width 8 bit */
			continue;
		_cbs = CALC_CBS(m, e);
		_delta = cbs - _cbs;
		if (_delta < delta) {
			_man = m;
			_exp = e;
			if (!_delta)
				goto found;
			delta = _delta;
		}
	}

found:
	*man = _man;
	*exp = _exp;
}

int
mlx5e_flow_meter_send(struct mlx5_core_dev *mdev,
		      struct mlx5e_flow_meter_handle *meter,
		      struct mlx5e_flow_meter_params *meter_params)
{
	struct mlx5e_aso_ctrl_param param = {};
	struct mlx5_wqe_aso_data_seg *aso_data;
	struct mlx5e_flow_meters *flow_meters;
	u8 cir_man, cir_exp, cbs_man, cbs_exp;
	struct mlx5e_aso_wqe_data *aso_wqe;
	u16 pi, contig_wqebbs_room;
	struct mlx5e_asosq *sq;
	struct mlx5_wq_cyc *wq;
	struct mlx5e_aso *aso;
	u64 rate, burst;
	int err = 0;

	flow_meters = meter->flow_meters;
	aso = flow_meters->aso;
	sq = &aso->sq;
	wq = &sq->wq;

	rate = meter_params->rate;
	burst = meter_params->burst;
	/* HW treats each packet as 128 bytes in PPS mode */
	if (meter_params->mode == MLX5_RATE_LIMIT_PPS) {
		rate <<= 10;
		burst <<= 7;
	}

	if (!rate || rate > MAX_CIR || !burst || burst > MAX_CBS)
		return -EINVAL;

	/* HW has limitation of total 31 bits for cbs */
	if (burst > MAX_HW_CBS) {
		mlx5_core_warn(mdev,
			       "burst(%lld) is too large, use HW allowed value(%d)\n",
			       burst, MAX_HW_CBS);
		burst = MAX_HW_CBS;
	}

	mlx5_core_dbg(mdev, "meter mode=%d\n", meter_params->mode);
	mlx5e_flow_meter_cir_calc(rate, &cir_man, &cir_exp);
	mlx5_core_dbg(mdev, "rate=%lld, cir=%lld, exp=%d, man=%d\n",
		      rate, CALC_CIR(cir_man, cir_exp), cir_exp, cir_man);
	mlx5e_flow_meter_cbs_calc(burst, &cbs_man, &cbs_exp);
	mlx5_core_dbg(mdev, "burst=%lld, cbs=%lld, exp=%d, man=%d\n",
		      burst, CALC_CBS((u64)cbs_man, cbs_exp), cbs_exp, cbs_man);

	if (!cir_man || !cbs_man)
		return -EINVAL;

	mutex_lock(&aso->priv->aso_lock);
	pi = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	contig_wqebbs_room = mlx5_wq_cyc_get_contig_wqebbs(wq, pi);

	if (unlikely(contig_wqebbs_room < MLX5E_ASO_WQEBBS_DATA)) {
		mlx5e_fill_asosq_frag_edge(sq, wq, pi, contig_wqebbs_room);
		pi = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	}

	aso_wqe = mlx5_wq_cyc_get_wqe(wq, pi);
	param.data_mask_mode = ASO_DATA_MASK_MODE_BYTEWISE_64BYTE;
	param.condition_operand = LOGICAL_OR;
	param.condition_0_operand = ALWAYS_TRUE;
	param.condition_1_operand = ALWAYS_TRUE;
	param.data_mask = 0x80FFFFFFULL << (meter->idx ? 0 : 32);
	mlx5e_build_aso_wqe(aso, sq,
			    DIV_ROUND_UP(sizeof(*aso_wqe), MLX5_SEND_WQE_DS),
			    &aso_wqe->ctrl, &aso_wqe->aso_ctrl, meter->obj_id,
			    MLX5_ACCESS_ASO_OPC_MOD_FLOW_METER, &param);

	aso_data = &aso_wqe->aso_data;
	memset(aso_data, 0, sizeof(*aso_data));
	aso_data->bytewise_data[meter->idx * 8] = cpu_to_be32((0x1 << 31) | /* valid */
					(MLX5_FLOW_METER_COLOR_GREEN << START_COLOR_SHIFT));
	if (meter_params->mode == MLX5_RATE_LIMIT_PPS)
		aso_data->bytewise_data[meter->idx * 8] |=
			cpu_to_be32(MLX5_FLOW_METER_MODE_NUM_PACKETS << METER_MODE_SHIFT);
	else
		aso_data->bytewise_data[meter->idx * 8] |=
			cpu_to_be32(MLX5_FLOW_METER_MODE_BYTES_IP_LENGTH << METER_MODE_SHIFT);

	aso_data->bytewise_data[meter->idx * 8 + 2] = cpu_to_be32((cbs_exp << CBS_EXP_SHIFT) |
								  (cbs_man << CBS_MAN_SHIFT) |
								  (cir_exp << CIR_EXP_SHIFT) |
								  cir_man);

	sq->db.aso_wqe[pi].opcode = MLX5_OPCODE_ACCESS_ASO;
	sq->db.aso_wqe[pi].with_data = true;
	sq->pc += MLX5E_ASO_WQEBBS_DATA;
	sq->doorbell_cseg = &aso_wqe->ctrl;

	mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, sq->doorbell_cseg);

	/* Ensure doorbell is written on uar_page before poll_cq */
	WRITE_ONCE(sq->doorbell_cseg, NULL);

	err = mlx5e_poll_aso_cq(&sq->cq);
	mutex_unlock(&aso->priv->aso_lock);

	return err;
}

static int
mlx5e_flow_meter_create_aso_obj(struct mlx5_core_dev *dev,
				struct mlx5e_flow_meters *flow_meters, int *obj_id)
{
	u32 in[MLX5_ST_SZ_DW(create_flow_meter_aso_obj_in)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];
	void *obj;
	int err;

	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type,
		 MLX5_GENERAL_OBJECT_TYPES_FLOW_METER_ASO);
	MLX5_SET(general_obj_in_cmd_hdr, in, log_obj_range, flow_meters->log_granularity);

	obj = MLX5_ADDR_OF(create_flow_meter_aso_obj_in, in, flow_meter_aso_obj);
	MLX5_SET(flow_meter_aso_obj, obj, meter_aso_access_pd, flow_meters->aso->pdn);

	err = mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
	if (!err) {
		*obj_id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);
		mlx5_core_dbg(dev, "flow meter aso obj(0x%x) created\n", *obj_id);
	}

	return err;
}

static void
mlx5e_flow_meter_destroy_aso_obj(struct mlx5_core_dev *dev, u32 obj_id)
{
	u32 in[MLX5_ST_SZ_DW(general_obj_in_cmd_hdr)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];

	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_DESTROY_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type,
		 MLX5_GENERAL_OBJECT_TYPES_FLOW_METER_ASO);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_id, obj_id);

	mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
	mlx5_core_dbg(dev, "flow meter aso obj(0x%x) destroyed\n", obj_id);
}

static struct mlx5e_flow_meter_handle *
__mlx5e_flow_meter_alloc(struct mlx5e_flow_meters *flow_meters)
{
	struct mlx5_core_dev *mdev = flow_meters->mdev;
	struct mlx5e_flow_meter_aso_obj *meters_obj;
	struct mlx5e_flow_meter_handle *meter;
	struct mlx5_fc *counter;
	int err, pos, total;
	u32 id;

	meter = kzalloc(sizeof(*meter), GFP_KERNEL);
	if (!meter)
		return ERR_PTR(-ENOMEM);

	counter = mlx5_fc_create(mdev, true);
	if (IS_ERR(counter)) {
		err = PTR_ERR(counter);
		goto err_red_counter;
	}
	meter->red_counter = counter;

	counter = mlx5_fc_create(mdev, true);
	if (IS_ERR(counter)) {
		err = PTR_ERR(counter);
		goto err_green_counter;
	}
	meter->green_counter = counter;

	meters_obj = list_first_entry_or_null(&flow_meters->partial_list,
					      struct mlx5e_flow_meter_aso_obj,
					      entry);
	/* 2 meters in one object */
	total = 1 << (flow_meters->log_granularity + 1);
	if (!meters_obj) {
		err = mlx5e_flow_meter_create_aso_obj(mdev, flow_meters, &id);
		if (err) {
			mlx5_core_err(mdev, "Failed to create flow meter ASO object\n");
			goto err_create;
		}

		meters_obj = kzalloc(sizeof(*meters_obj) + BITS_TO_BYTES(total),
				     GFP_KERNEL);
		if (!meters_obj) {
			err = -ENOMEM;
			goto err_mem;
		}

		meters_obj->base_id = id;
		meters_obj->total_meters = total;
		list_add(&meters_obj->entry, &flow_meters->partial_list);
		pos = 0;
	} else {
		pos = find_first_zero_bit(meters_obj->meters_map, total);
		if (bitmap_weight(meters_obj->meters_map, total) == total - 1) {
			list_del(&meters_obj->entry);
			list_add(&meters_obj->entry, &flow_meters->full_list);
		}
	}

	bitmap_set(meters_obj->meters_map, pos, 1);
	meter->flow_meters = flow_meters;
	meter->meters_obj = meters_obj;
	meter->obj_id = meters_obj->base_id + pos / 2;
	meter->idx = pos % 2;

	mlx5_core_dbg(mdev, "flow meter allocated, obj_id=0x%x, index=%d\n",
		      meter->obj_id, meter->idx);

	return meter;

err_mem:
	mlx5e_flow_meter_destroy_aso_obj(mdev, id);
err_create:
	mlx5_fc_destroy(mdev, meter->green_counter);
err_green_counter:
	mlx5_fc_destroy(mdev, meter->red_counter);
err_red_counter:
	kfree(meter);
	return ERR_PTR(err);
}

static void
__mlx5e_flow_meter_free(struct mlx5e_flow_meter_handle *meter)
{
	struct mlx5e_flow_meters *flow_meters = meter->flow_meters;
	struct mlx5_core_dev *mdev = flow_meters->mdev;
	struct mlx5e_flow_meter_aso_obj *meters_obj;
	int n, pos;

	mlx5_fc_destroy(mdev, meter->green_counter);
	mlx5_fc_destroy(mdev, meter->red_counter);

	meters_obj = meter->meters_obj;
	pos = (meter->obj_id - meters_obj->base_id) * 2 + meter->idx;
	bitmap_clear(meters_obj->meters_map, pos, 1);
	n = bitmap_weight(meters_obj->meters_map, meters_obj->total_meters);
	if (n == 0) {
		list_del(&meters_obj->entry);
		mlx5e_flow_meter_destroy_aso_obj(mdev, meters_obj->base_id);
		kfree(meters_obj);
	} else if (n == meters_obj->total_meters - 1) {
		list_del(&meters_obj->entry);
		list_add(&meters_obj->entry, &flow_meters->partial_list);
	}

	mlx5_core_dbg(mdev, "flow meter freed, obj_id=0x%x, index=%d\n",
		      meter->obj_id, meter->idx);
	kfree(meter);
}

static struct mlx5e_flow_meter_handle *
__mlx5e_tc_meter_get(struct mlx5e_flow_meters *flow_meters, u32 index)
{
	struct mlx5e_flow_meter_handle *meter;

	hash_for_each_possible(flow_meters->hashtbl, meter, hlist, index)
		if (meter->params.index == index)
			goto add_ref;

	return ERR_PTR(-ENOENT);

add_ref:
	meter->refcnt++;

	return meter;
}

struct mlx5e_flow_meter_handle *
mlx5e_tc_meter_get(struct mlx5_core_dev *mdev, struct mlx5e_flow_meter_params *params)
{
	struct mlx5e_flow_meters *flow_meters;
	struct mlx5e_flow_meter_handle *meter;

	flow_meters = mlx5e_get_flow_meters(mdev);
	if (!flow_meters)
		return ERR_PTR(-EOPNOTSUPP);

	meter = __mlx5e_tc_meter_get(flow_meters, params->index);
	mutex_unlock(&flow_meters->sync_lock);

	return meter;
}

static void
__mlx5e_tc_meter_put(struct mlx5e_flow_meter_handle *meter)
{
	if (--meter->refcnt == 0) {
		hash_del(&meter->hlist);
		__mlx5e_flow_meter_free(meter);
	}
}

void
mlx5e_tc_meter_put(struct mlx5e_flow_meter_handle *meter)
{
	struct mlx5e_flow_meters *flow_meters = meter->flow_meters;

	mutex_lock(&flow_meters->sync_lock);
	__mlx5e_tc_meter_put(meter);
	mutex_unlock(&flow_meters->sync_lock);
}

static struct mlx5e_flow_meter_handle *
mlx5e_tc_meter_alloc(struct mlx5e_flow_meters *flow_meters,
		     struct mlx5e_flow_meter_params *params)
{
	struct mlx5e_flow_meter_handle *meter;

	meter = __mlx5e_flow_meter_alloc(flow_meters);
	if (IS_ERR(meter))
		return meter;

	hash_add(flow_meters->hashtbl, &meter->hlist, params->index);
	meter->params.index = params->index;
	meter->refcnt++;

	return meter;
}

static int
__mlx5e_tc_meter_update(struct mlx5e_flow_meter_handle *meter,
			struct mlx5e_flow_meter_params *params)
{
	struct mlx5_core_dev *mdev = meter->flow_meters->mdev;
	int err = 0;

	if (meter->params.mode != params->mode || meter->params.rate != params->rate ||
	    meter->params.burst != params->burst) {
		err = mlx5e_flow_meter_send(mdev, meter, params);
		if (err)
			goto out;

		meter->params.mode = params->mode;
		meter->params.rate = params->rate;
		meter->params.burst = params->burst;
	}

out:
	return err;
}

int
mlx5e_tc_meter_update(struct mlx5e_flow_meter_handle *meter,
		      struct mlx5e_flow_meter_params *params)
{
	struct mlx5_core_dev *mdev = meter->flow_meters->mdev;
	struct mlx5e_flow_meters *flow_meters;
	int err;

	flow_meters = mlx5e_get_flow_meters(mdev);
	if (!flow_meters)
		return -EOPNOTSUPP;

	mutex_lock(&flow_meters->sync_lock);
	err = __mlx5e_tc_meter_update(meter, params);
	mutex_unlock(&flow_meters->sync_lock);
	return err;
}

struct mlx5e_flow_meter_handle *
mlx5e_tc_meter_replace(struct mlx5_core_dev *mdev, struct mlx5e_flow_meter_params *params)
{
	struct mlx5e_flow_meters *flow_meters;
	struct mlx5e_flow_meter_handle *meter;
	int err;

	flow_meters = mlx5e_get_flow_meters(mdev);
	if (!flow_meters)
		return ERR_PTR(-EOPNOTSUPP);

	mutex_lock(&flow_meters->sync_lock);
	meter = __mlx5e_tc_meter_get(flow_meters, params->index);
	if (IS_ERR(meter)) {
		meter = mlx5e_tc_meter_alloc(flow_meters, params);
		if (IS_ERR(meter)) {
			err = PTR_ERR(meter);
			goto err_get;
		}
	}

	err = __mlx5e_tc_meter_update(meter, params);
	if (err)
		goto err_update;

	mutex_unlock(&flow_meters->sync_lock);
	return meter;

err_update:
	__mlx5e_tc_meter_put(meter);
err_get:
	mutex_unlock(&flow_meters->sync_lock);
	return ERR_PTR(err);
}

enum mlx5_flow_namespace_type
mlx5e_tc_meter_get_namespace(struct mlx5e_flow_meters *flow_meters)
{
	return flow_meters->ns_type;
}

struct mlx5e_flow_meters *
mlx5e_flow_meters_init(struct mlx5e_priv *priv,
		       enum mlx5_flow_namespace_type ns_type,
		       struct mlx5e_post_act *post_act)
{
	struct mlx5e_flow_meters *flow_meters;

	if (!(MLX5_CAP_GEN_64(priv->mdev, general_obj_types) &
	      MLX5_HCA_CAP_GENERAL_OBJECT_TYPES_FLOW_METER_ASO))
		return NULL;

	flow_meters = kzalloc(sizeof(*flow_meters), GFP_KERNEL);
	if (!flow_meters)
		return NULL;

	if (IS_ERR_OR_NULL(post_act)) {
		netdev_dbg(priv->netdev,
			   "flow meter offload is not supported, post action is missing\n");
		goto errout;
	}

	flow_meters->aso = mlx5e_aso_get(priv);
	if (!flow_meters->aso) {
		mlx5_core_warn(priv->mdev, "Failed to create aso wqe for flow meter\n");
		goto errout;
	}

	flow_meters->ns_type = ns_type;
	flow_meters->mdev = priv->mdev;
	flow_meters->post_act = post_act;
	flow_meters->log_granularity = min_t(int, 6,
					     MLX5_CAP_QOS(priv->mdev, log_meter_aso_max_alloc));
	mutex_init(&flow_meters->sync_lock);
	INIT_LIST_HEAD(&flow_meters->partial_list);
	INIT_LIST_HEAD(&flow_meters->full_list);

	return flow_meters;

errout:
	kfree(flow_meters);
	return NULL;
}

void
mlx5e_flow_meters_cleanup(struct mlx5e_flow_meters *flow_meters)
{
	if (!flow_meters)
		return;

	mlx5e_aso_put(flow_meters->aso->priv);
	kfree(flow_meters);
}

int
mlx5e_aso_send_flow_meter_aso(struct mlx5_core_dev *mdev,
			      struct mlx5e_flow_meter_handle *meter,
			      struct mlx5e_flow_meter_params *meter_params)
{
	struct mlx5e_aso_ctrl_param param = {};
	struct mlx5_wqe_aso_data_seg *aso_data;
	struct mlx5e_flow_meters *flow_meters;
	u8 cir_man, cir_exp, cbs_man, cbs_exp;
	struct mlx5e_aso_wqe_data *aso_wqe;
	u16 pi, contig_wqebbs_room;
	struct mlx5e_asosq *sq;
	struct mlx5_wq_cyc *wq;
	struct mlx5e_aso *aso;
	u64 rate, burst;
	int err = 0;

	flow_meters = meter->flow_meters;
	aso = flow_meters->aso;
	sq = &aso->sq;
	wq = &sq->wq;

	rate = meter_params->rate;
	burst = meter_params->burst;
	/* HW treats each packet as 128 bytes in PPS mode */
	if (meter_params->mode == MLX5_RATE_LIMIT_PPS) {
		rate <<= 10;
		burst <<= 7;
	}

	if (!rate || rate > MAX_CIR || !burst || burst > MAX_CBS)
		return -EINVAL;

	/* HW has limitation of total 31 bits for cbs */
	if (burst > MAX_HW_CBS) {
		mlx5_core_warn(mdev,
			       "burst(%lld) is too large, use HW allowed value(%d)\n",
			       burst, MAX_HW_CBS);
		burst = MAX_HW_CBS;
	}

	mlx5_core_dbg(mdev, "meter mode=%d\n", meter_params->mode);
	mlx5e_flow_meter_cir_calc(rate, &cir_man, &cir_exp);
	mlx5_core_dbg(mdev, "rate=%lld, cir=%lld, exp=%d, man=%d\n",
		      rate, CALC_CIR(cir_man, cir_exp), cir_exp, cir_man);
	mlx5e_flow_meter_cbs_calc(burst, &cbs_man, &cbs_exp);
	mlx5_core_dbg(mdev, "burst=%lld, cbs=%lld, exp=%d, man=%d\n",
		      burst, CALC_CBS((u64)cbs_man, cbs_exp), cbs_exp, cbs_man);

	if (!cir_man || !cbs_man)
		return -EINVAL;

	mutex_lock(&aso->priv->aso_lock);
	pi = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	contig_wqebbs_room = mlx5_wq_cyc_get_contig_wqebbs(wq, pi);

	if (unlikely(contig_wqebbs_room < MLX5E_ASO_WQEBBS_DATA)) {
		mlx5e_fill_asosq_frag_edge(sq, wq, pi, contig_wqebbs_room);
		pi = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	}

	aso_wqe = mlx5_wq_cyc_get_wqe(wq, pi);
	param.data_mask_mode = ASO_DATA_MASK_MODE_BYTEWISE_64BYTE;
	param.condition_operand = LOGICAL_OR;
	param.condition_0_operand = ALWAYS_TRUE;
	param.condition_1_operand = ALWAYS_TRUE;
	param.data_mask = 0x80FFFFFFULL << (meter->idx ? 0 : 32);
	mlx5e_build_aso_wqe(aso, sq,
			    DIV_ROUND_UP(sizeof(*aso_wqe), MLX5_SEND_WQE_DS),
			    &aso_wqe->ctrl, &aso_wqe->aso_ctrl, meter->obj_id,
			    MLX5_ACCESS_ASO_OPC_MOD_FLOW_METER, &param);

	aso_data = &aso_wqe->aso_data;
	memset(aso_data, 0, sizeof(*aso_data));
	aso_data->bytewise_data[meter->idx * 8] = cpu_to_be32((0x1 << 31) | /* valid */
					(MLX5_FLOW_METER_COLOR_GREEN << START_COLOR_SHIFT));
	if (meter_params->mode == MLX5_RATE_LIMIT_PPS)
		aso_data->bytewise_data[meter->idx * 8] |=
			cpu_to_be32(MLX5_FLOW_METER_MODE_NUM_PACKETS << METER_MODE_SHIFT);
	else
		aso_data->bytewise_data[meter->idx * 8] |=
			cpu_to_be32(MLX5_FLOW_METER_MODE_BYTES_IP_LENGTH << METER_MODE_SHIFT);

	aso_data->bytewise_data[meter->idx * 8 + 2] = cpu_to_be32((cbs_exp << CBS_EXP_SHIFT) |
								  (cbs_man << CBS_MAN_SHIFT) |
								  (cir_exp << CIR_EXP_SHIFT) |
								  cir_man);

	sq->db.aso_wqe[pi].opcode = MLX5_OPCODE_ACCESS_ASO;
	sq->db.aso_wqe[pi].with_data = true;
	sq->pc += MLX5E_ASO_WQEBBS_DATA;
	sq->doorbell_cseg = &aso_wqe->ctrl;

	mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, sq->doorbell_cseg);

	/* Ensure doorbell is written on uar_page before poll_cq */
	WRITE_ONCE(sq->doorbell_cseg, NULL);

	err = mlx5e_poll_aso_cq(&sq->cq);
	mutex_unlock(&aso->priv->aso_lock);

	return err;
}

static int
mlx5e_create_flow_meter_aso_obj(struct mlx5_core_dev *dev,
				struct mlx5e_flow_meters *flow_meters, int *obj_id)
{
	u32 in[MLX5_ST_SZ_DW(create_flow_meter_aso_obj_in)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];
	void *obj;
	int err;

	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type,
		 MLX5_GENERAL_OBJECT_TYPES_FLOW_METER_ASO);
	MLX5_SET(general_obj_in_cmd_hdr, in, log_obj_range, flow_meters->log_granularity);

	obj = MLX5_ADDR_OF(create_flow_meter_aso_obj_in, in, flow_meter_aso_obj);
	MLX5_SET(flow_meter_aso_obj, obj, meter_aso_access_pd, flow_meters->aso->pdn);

	err = mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
	if (!err) {
		*obj_id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);
		mlx5_core_dbg(dev, "flow meter aso obj(0x%x) created\n", *obj_id);
	}

	return err;
}

static void
mlx5e_destroy_flow_meter_aso_obj(struct mlx5_core_dev *dev, u32 obj_id)
{
	u32 in[MLX5_ST_SZ_DW(general_obj_in_cmd_hdr)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];

	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_DESTROY_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type,
		 MLX5_GENERAL_OBJECT_TYPES_FLOW_METER_ASO);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_id, obj_id);

	mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
	mlx5_core_dbg(dev, "flow meter aso obj(0x%x) destroyed\n", obj_id);
}

static struct mlx5e_flow_meter_handle *
__mlx5e_alloc_flow_meter(struct mlx5_core_dev *dev,
			 struct mlx5e_flow_meters *flow_meters)
{
	struct mlx5e_flow_meter_aso_obj *meters_obj;
	struct mlx5e_flow_meter_handle *meter;
	int err, pos, total;
	u32 id;

	meter = kzalloc(sizeof(*meter), GFP_KERNEL);
	if (!meter)
		return ERR_PTR(-ENOMEM);

	meters_obj = list_first_entry_or_null(&flow_meters->partial_list,
					      struct mlx5e_flow_meter_aso_obj,
					      entry);
	/* 2 meters in one object */
	total = 1 << (flow_meters->log_granularity + 1);
	if (!meters_obj) {
		err = mlx5e_create_flow_meter_aso_obj(dev, flow_meters, &id);
		if (err) {
			mlx5_core_err(dev, "Failed to create flow meter ASO object\n");
			goto err_create;
		}

		meters_obj = kzalloc(sizeof(*meters_obj) + BITS_TO_BYTES(total),
				     GFP_KERNEL);
		if (!meters_obj) {
			err = -ENOMEM;
			goto err_mem;
		}

		meters_obj->base_id = id;
		meters_obj->total_meters = total;
		list_add(&meters_obj->entry, &flow_meters->partial_list);
		pos = 0;
	} else {
		pos = find_first_zero_bit(meters_obj->meters_map, total);
		if (bitmap_weight(meters_obj->meters_map, total) == total - 1) {
			list_del(&meters_obj->entry);
			list_add(&meters_obj->entry, &flow_meters->full_list);
		}
	}

	bitmap_set(meters_obj->meters_map, pos, 1);
	meter->flow_meters = flow_meters;
	meter->meters_obj = meters_obj;
	meter->obj_id = meters_obj->base_id + pos / 2;
	meter->idx = pos % 2;

	mlx5_core_dbg(dev, "flow meter allocated, obj_id=0x%x, index=%d\n",
		      meter->obj_id, meter->idx);

	return meter;

err_mem:
	mlx5e_destroy_flow_meter_aso_obj(dev, id);
err_create:
	kfree(meter);
	return ERR_PTR(err);
}

static void
__mlx5e_free_flow_meter(struct mlx5_core_dev *dev,
			struct mlx5e_flow_meters *flow_meters,
			struct mlx5e_flow_meter_handle *meter)
{
	struct mlx5e_flow_meter_aso_obj *meters_obj;
	int n, pos;

	meters_obj = meter->meters_obj;
	pos = (meter->obj_id - meters_obj->base_id) * 2 + meter->idx;
	bitmap_clear(meters_obj->meters_map, pos, 1);
	n = bitmap_weight(meters_obj->meters_map, meters_obj->total_meters);
	if (n == 0) {
		list_del(&meters_obj->entry);
		mlx5e_destroy_flow_meter_aso_obj(dev, meters_obj->base_id);
		kfree(meters_obj);
	} else if (n == meters_obj->total_meters - 1) {
		list_del(&meters_obj->entry);
		list_add(&meters_obj->entry, &flow_meters->partial_list);
	}

	mlx5_core_dbg(dev, "flow meter freed, obj_id=0x%x, index=%d\n",
		      meter->obj_id, meter->idx);
	kfree(meter);
}

struct mlx5e_flow_meter_handle *
mlx5e_alloc_flow_meter(struct mlx5_core_dev *dev)
{
	struct mlx5e_flow_meters *flow_meters;
	struct mlx5e_flow_meter_handle *meter;

	flow_meters = mlx5e_get_flow_meters(dev);
	if (!flow_meters)
		return ERR_PTR(-EOPNOTSUPP);

	mutex_lock(&flow_meters->sync_lock);
	meter = __mlx5e_alloc_flow_meter(dev, flow_meters);
	mutex_unlock(&flow_meters->sync_lock);

	return meter;
}

void
mlx5e_free_flow_meter(struct mlx5_core_dev *dev, struct mlx5e_flow_meter_handle *meter)
{
	struct mlx5e_flow_meters *flow_meters;

	flow_meters = meter->flow_meters;
	mutex_lock(&flow_meters->sync_lock);
	__mlx5e_free_flow_meter(dev, flow_meters, meter);
	mutex_unlock(&flow_meters->sync_lock);
}

void
mlx5e_tc_meter_get_stats(struct mlx5e_flow_meter_handle *meter,
			 u64 *bytes, u64 *packets, u64 *drops, u64 *lastuse)
{
	u64 bytes1, packets1, lastuse1;
	u64 bytes2, packets2, lastuse2;

	mlx5_fc_query_cached(meter->green_counter, &bytes1, &packets1, &lastuse1);
	mlx5_fc_query_cached(meter->red_counter, &bytes2, &packets2, &lastuse2);

	*bytes = bytes1 + bytes2;
	*packets = packets1 + packets2;
	*drops = packets2;
	*lastuse = max_t(u64, lastuse1, lastuse2);
}