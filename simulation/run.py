# -*- coding: utf-8 -*-

import argparse
import os
import sys


CONFIG_TEMPLATE = """ENABLE_QCN {enable_qcn}
USE_DYNAMIC_PFC_THRESHOLD 0

PACKET_PAYLOAD_SIZE 1500

TOPOLOGY_FILE mix/{topo}.txt
FLOW_FILE mix/{trace}.txt
TRACE_FILE mix/trace.txt
TRACE_OUTPUT_FILE {trace_output}
FCT_OUTPUT_FILE {fct_output}
PFC_OUTPUT_FILE {pfc_output}
CONTROL_OUTPUT_FILE {control_output}

SIMULATOR_STOP_TIME 4.00

CC_MODE {mode}
ALPHA_RESUME_INTERVAL {t_alpha}
RATE_DECREASE_INTERVAL {t_dec}
CLAMP_TARGET_RATE 0
RP_TIMER {t_inc}
EWMA_GAIN {g}
FAST_RECOVERY_TIMES 1
RATE_AI {ai}Mb/s
RATE_HAI {hai}Mb/s
MIN_RATE 1000Mb/s
DCTCP_RATE_AI {dctcp_ai}Mb/s

ERROR_RATE_PER_LINK 0.0000
L2_CHUNK_SIZE 4000
L2_ACK_INTERVAL 1
L2_BACK_TO_ZERO 0

HAS_WIN {has_win}
GLOBAL_T 1
VAR_WIN {vwin}
FAST_REACT {us}
U_TARGET {u_tgt}
MI_THRESH {mi}
INT_MULTI {int_multi}
MULTI_RATE 0
SAMPLE_FEEDBACK 0
PINT_LOG_BASE {pint_log_base}
PINT_PROB {pint_prob}

RATE_BOUND 1

ACK_HIGH_PRIO {ack_prio}

LINK_DOWN {link_down}

ENABLE_TRACE {enable_tr}

ENABLE_MMU_STABILITY_GUARD {enable_mmu_stability_guard}
MMU_STABILITY_EWMA_ALPHA {mmu_stability_ewma_alpha}
MMU_STABILITY_ENTER_SAMPLES {mmu_stability_enter_samples}
MMU_STABILITY_EXIT_SAMPLES {mmu_stability_exit_samples}

ENABLE_BROADCAST_DAMPENING {enable_broadcast_dampening}
BROADCAST_STABLE_ROUNDS {broadcast_stable_rounds}
MIN_BROADCAST_INTERVAL_NS {min_broadcast_interval_ns}
ENABLE_CONGESTION_LABEL_COMPRESSION {enable_congestion_label_compression}

KMAX_MAP {kmax_map}
KMIN_MAP {kmin_map}
PMAX_MAP {pmax_map}
BUFFER_SIZE {buffer_size}
QLEN_MON_FILE {qlen_output}
QLEN_MON_START 2000000000
QLEN_MON_END 3000000000
"""


def build_cc_params(args, bw):
	cc = args.cc
	params = {
		"mode": 1,
		"t_alpha": 1,
		"t_dec": 4,
		"t_inc": 300,
		"g": 0.00390625,
		"ai": max(1, int(5 * bw // 25)),
		"hai": max(1, int(50 * bw // 25)),
		"dctcp_ai": 1000,
		"has_win": 0,
		"vwin": 0,
		"us": 0,
		"int_multi": 1,
		"ack_prio": 1,
	}

	tag = "base"
	if cc == "none":
		params.update({
			"mode": 20,
			"has_win": 0,
			"vwin": 0,
			"us": 0,
			"ack_prio": 1,
		})
		tag = "none"
	elif cc == "dcqcn":
		tag = "dcqcn"
	elif cc == "dctcp":
		params.update({
			"mode": 8,
			"g": 0.0625,
			"ai": 10,
			"hai": 10,
			"dctcp_ai": 615,
			"has_win": 1,
			"vwin": 1,
			"ack_prio": 0,
		})
		tag = "dctcp"
	elif cc == "timely":
		params.update({
			"mode": 7,
			"ai": max(1, int(10 * bw // 10)),
			"hai": max(1, int(50 * bw // 10)),
			"has_win": 0,
			"vwin": 0,
			"ack_prio": 1,
		})
		tag = "timely"
	elif cc == "hp":
		ai = max(1, int(10 * bw // 25))
		if args.hpai > 0:
			ai = args.hpai
		params.update({
			"mode": 3,
			"ai": ai,
			"hai": ai,
			"has_win": 1,
			"vwin": 1,
			"us": 1,
			"int_multi": max(1, int(bw // 25)),
			"ack_prio": 0,
		})
		tag = "hp{}{}{}".format(
			args.utgt,
			"mi{}".format(args.mi) if args.mi > 0 else "",
			"ai{}".format(ai) if args.hpai > 0 else "",
		)
	elif cc == "hpccPint":
		ai = max(1, int(10 * bw // 25))
		if args.hpai > 0:
			ai = args.hpai
		params.update({
			"mode": 10,
			"ai": ai,
			"hai": ai,
			"has_win": 1,
			"vwin": 1,
			"us": 1,
			"int_multi": max(1, int(bw // 25)),
			"ack_prio": 0,
		})
		tag = "hpccPint{}{}{}log{:.3f}p{:.3f}".format(
			args.utgt,
			"mi{}".format(args.mi) if args.mi > 0 else "",
			"ai{}".format(ai) if args.hpai > 0 else "",
			args.pint_log_base,
			args.pint_prob,
		)
	else:
		print("unknown cc:", cc)
		sys.exit(1)

	if args.switchnodecc >= 0:
		params["mode"] = args.switchnodecc
		tag += "_sw{}".format(args.switchnodecc)

	return params, tag


def build_ecn_params(cc, bw):
	if cc == "dctcp":
		kmax = "2 {} {} {} {}".format(
			bw * 1000000000,
			int(30 * bw // 10),
			bw * 4 * 1000000000,
			int(30 * bw * 4 // 10),
		)
		kmin = kmax
		pmax = "2 {} {:.2f} {} {:.2f}".format(
			bw * 1000000000,
			1.0,
			bw * 4 * 1000000000,
			1.0,
		)
	else:
		kmax = "2 {} {} {} {}".format(
			bw * 1000000000,
			int(400 * bw // 25),
			bw * 4 * 1000000000,
			int(400 * bw * 4 // 25),
		)
		kmin = "2 {} {} {} {}".format(
			bw * 1000000000,
			int(100 * bw // 25),
			bw * 4 * 1000000000,
			int(100 * bw * 4 // 25),
		)
		pmax = "2 {} {:.2f} {} {:.2f}".format(
			bw * 1000000000,
			0.2,
			bw * 4 * 1000000000,
			0.2,
		)
	return kmax, kmin, pmax


def _fmt_tag_value(value):
	if isinstance(value, float):
		text = ("{:.3f}".format(value)).rstrip("0").rstrip(".")
	else:
		text = str(value)
	return text.replace(".", "p")


def build_param_tag(args, bw):
	parts = [
		"q{}".format(args.enable_qcn),
		"bw{}".format(bw),
		"mmu{}".format(args.enable_mmu_stability_guard),
		"ea{}".format(_fmt_tag_value(args.mmu_stability_ewma_alpha)),
		"es{}".format(args.mmu_stability_enter_samples),
		"xs{}".format(args.mmu_stability_exit_samples),
		"bd{}".format(args.enable_broadcast_dampening),
		"br{}".format(args.broadcast_stable_rounds),
		"bi{}".format(args.min_broadcast_interval_ns),
		"clc{}".format(args.enable_congestion_label_compression),
	]
	return "_" + "_".join(parts)


if __name__ == "__main__":
	parser = argparse.ArgumentParser(description="一键生成并运行 third2 的 config")
	parser.add_argument("--enable_qcn", type=int, default=1, help="enable QCN: 1 on, 0 off")
	parser.add_argument("--cc", default="none", help="none/dcqcn/dctcp/timely/hp/hpccPint")
	parser.add_argument("--trace", default="flow", help="flow file name in mix/")
	parser.add_argument("--bw", type=int, default=50, help="NIC bandwidth (Gbps)")
	parser.add_argument("--down", default="0 0 0", help="link down event: 'time A B'")
	parser.add_argument("--topo", default="fat", help="topology file name in mix/")

	parser.add_argument("--utgt", type=int, default=95, help="HPCC eta")
	parser.add_argument("--mi", type=int, default=0, help="MI_THRESH")
	parser.add_argument("--hpai", type=int, default=0, help="AI for HPCC")
	parser.add_argument("--pint_log_base", type=float, default=1.01, help="PINT log base")
	parser.add_argument("--pint_prob", type=float, default=1.0, help="PINT sampling probability")
	parser.add_argument("--enable_tr", type=int, default=0, help="enable packet-level trace dump")

	parser.add_argument("--switchnodecc", type=int, default=-1, help="override CC_MODE for switch/host (e.g. 8 for dctcp)")

	parser.add_argument("--enable_mmu_stability_guard", type=int, default=1)
	parser.add_argument("--mmu_stability_ewma_alpha", type=float, default=0.25)
	parser.add_argument("--mmu_stability_enter_samples", type=int, default=2)
	parser.add_argument("--mmu_stability_exit_samples", type=int, default=3)

	parser.add_argument("--enable_broadcast_dampening", type=int, default=1)
	parser.add_argument("--broadcast_stable_rounds", type=int, default=2)
	parser.add_argument("--min_broadcast_interval_ns", type=int, default=50000)
	parser.add_argument("--enable_congestion_label_compression", type=int, default=0, help="enable congestion-label prefix compression: 1 on, 0 off")

	parser.add_argument("--run", type=int, default=1, help="1=generate+run, 0=only generate")

	args = parser.parse_args()

	topo = args.topo
	trace = args.trace
	bw = int(args.bw)
	failure = "_down" if args.down != "0 0 0" else ""

	cc_params, cc_tag = build_cc_params(args, bw)
	kmax_map, kmin_map, pmax_map = build_ecn_params(args.cc, bw)
	param_tag = build_param_tag(args, bw)

	u_tgt = args.utgt / 100.0
	bfsz = 1024

	file_tag = "{}_{}{}".format(cc_tag, param_tag, failure)
	config_name = "mix/config_{}_{}_{}.txt".format(topo, trace, file_tag)
	trace_output = "mix/mix_{}_{}_{}.tr".format(topo, trace, file_tag)
	fct_output = "mix/fct_{}_{}_{}.txt".format(topo, trace, file_tag)
	pfc_output = "mix/pfc_{}_{}_{}.txt".format(topo, trace, file_tag)
	control_output = "mix/control_{}_{}_{}.txt".format(topo, trace, file_tag)
	qlen_output = "mix/qlen_{}_{}_{}.txt".format(topo, trace, file_tag)

	config = CONFIG_TEMPLATE.format(
		enable_qcn=args.enable_qcn,
		topo=topo,
		trace=trace,
		trace_output=trace_output,
		fct_output=fct_output,
		pfc_output=pfc_output,
		control_output=control_output,
		qlen_output=qlen_output,
		mode=cc_params["mode"],
		t_alpha=cc_params["t_alpha"],
		t_dec=cc_params["t_dec"],
		t_inc=cc_params["t_inc"],
		g=cc_params["g"],
		ai=cc_params["ai"],
		hai=cc_params["hai"],
		dctcp_ai=cc_params["dctcp_ai"],
		has_win=cc_params["has_win"],
		vwin=cc_params["vwin"],
		us=cc_params["us"],
		u_tgt=u_tgt,
		mi=args.mi,
		int_multi=cc_params["int_multi"],
		pint_log_base=args.pint_log_base,
		pint_prob=args.pint_prob,
		ack_prio=cc_params["ack_prio"],
		link_down=args.down,
		enable_tr=args.enable_tr,
		enable_mmu_stability_guard=args.enable_mmu_stability_guard,
		mmu_stability_ewma_alpha=args.mmu_stability_ewma_alpha,
		mmu_stability_enter_samples=args.mmu_stability_enter_samples,
		mmu_stability_exit_samples=args.mmu_stability_exit_samples,
		enable_broadcast_dampening=args.enable_broadcast_dampening,
		broadcast_stable_rounds=args.broadcast_stable_rounds,
		min_broadcast_interval_ns=args.min_broadcast_interval_ns,
		enable_congestion_label_compression=args.enable_congestion_label_compression,
		kmax_map=kmax_map,
		kmin_map=kmin_map,
		pmax_map=pmax_map,
		buffer_size=bfsz,
	)

	with open(config_name, "w") as file:
		file.write(config)

	print("CONFIG:", config_name)
	print("FCT:", fct_output)
	print("CONTROL:", control_output)

	if args.run:
		waf_cmd = "waf.bat" if os.name == "nt" else "./waf"
		cmd = '{} --run "scratch/third2 {}"'.format(waf_cmd, config_name)
		os.system(cmd)
