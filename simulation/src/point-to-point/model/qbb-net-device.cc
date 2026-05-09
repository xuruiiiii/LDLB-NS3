/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* Copyright (c) 2006 Georgia Tech Research Corporation, INRIA
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
* Author: Yuliang Li <yuliangli@g.harvard.com>
*/

#define __STDC_LIMIT_MACROS 1
#include <stdint.h>
#include <stdio.h>
#include "ns3/qbb-net-device.h"
#include "ns3/log.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/data-rate.h"
#include "ns3/object-vector.h"
#include "ns3/pause-header.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/assert.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/qbb-channel.h"
#include "ns3/random-variable.h"
#include "ns3/flow-id-tag.h"
#include "ns3/qbb-header.h"
#include "ns3/error-model.h"
#include "ns3/cn-header.h"
#include "ns3/ppp-header.h"
#include "ns3/udp-header.h"
#include "ns3/seq-ts-header.h"
#include "ns3/pointer.h"
#include "ns3/custom-header.h"
#include "switch-node.h"
#include "switch-mmu.h"
#include <vector>
#include <string>
#include <sstream>
#include <iostream>
#include <iomanip> // for std::hex
#include <cstdint> // for uint8_t, uint32_t
#include <chrono>
#include <algorithm>
#include <unordered_map>



NS_LOG_COMPONENT_DEFINE("QbbNetDevice");

namespace ns3 {
	
	uint32_t RdmaEgressQueue::ack_q_idx = 3;
	// RdmaEgressQueue
	TypeId RdmaEgressQueue::GetTypeId (void)
	{
		static TypeId tid = TypeId ("ns3::RdmaEgressQueue")
			.SetParent<Object> ()
			.AddTraceSource ("RdmaEnqueue", "Enqueue a packet in the RdmaEgressQueue.",
					MakeTraceSourceAccessor (&RdmaEgressQueue::m_traceRdmaEnqueue))
			.AddTraceSource ("RdmaDequeue", "Dequeue a packet in the RdmaEgressQueue.",
					MakeTraceSourceAccessor (&RdmaEgressQueue::m_traceRdmaDequeue))
			;
		return tid;
	}

	RdmaEgressQueue::RdmaEgressQueue(){
		m_rrlast = 0;
		m_qlast = 0;
		m_ackQ = CreateObject<DropTailQueue>();
		m_ackQ->SetAttribute("MaxBytes", UintegerValue(0xffffffff)); // queue limit is on a higher level, not here
	}

	Ptr<Packet> RdmaEgressQueue::DequeueQindex(int qIndex){
		if (qIndex == -1){ // high prio
			Ptr<Packet> p = m_ackQ->Dequeue();
			m_qlast = -1;
			m_traceRdmaDequeue(p, 0);
			return p;
		}
		if (qIndex >= 0){ // qp
			Ptr<Packet> p = m_rdmaGetNxtPkt(m_qpGrp->Get(qIndex));
			m_rrlast = qIndex;
			m_qlast = qIndex;
			m_traceRdmaDequeue(p, m_qpGrp->Get(qIndex)->m_pg);
			return p;
		}
		return 0;
	}
	int RdmaEgressQueue::GetNextQindex(bool paused[]){
		bool found = false;
		uint32_t qIndex;
		if (!paused[ack_q_idx] && m_ackQ->GetNPackets() > 0)
			return -1;

		// no pkt in highest priority queue, do rr for each qp
		int res = -1024;
		uint32_t fcount = m_qpGrp->GetN();
		uint32_t min_finish_id = 0xffffffff;
		for (qIndex = 1; qIndex <= fcount; qIndex++){
			uint32_t idx = (qIndex + m_rrlast) % fcount;
			Ptr<RdmaQueuePair> qp = m_qpGrp->Get(idx);
			if (!paused[qp->m_pg] && qp->GetBytesLeft() > 0 && !qp->IsWinBound()){
				if (m_qpGrp->Get(idx)->m_nextAvail.GetTimeStep() > Simulator::Now().GetTimeStep()) //not available now
					continue;
				res = idx;
				break;
			}else if (qp->IsFinished()){
				min_finish_id = idx < min_finish_id ? idx : min_finish_id;
			}
		}

		// clear the finished qp
		if (min_finish_id < 0xffffffff){
			int nxt = min_finish_id;
			auto &qps = m_qpGrp->m_qps;
			for (int i = min_finish_id + 1; i < fcount; i++) if (!qps[i]->IsFinished()){
				if (i == res) // update res to the idx after removing finished qp
					res = nxt;
				qps[nxt] = qps[i];
				nxt++;
			}
			qps.resize(nxt);
		}
		return res;
	}

	int RdmaEgressQueue::GetLastQueue(){
		return m_qlast;
	}

	uint32_t RdmaEgressQueue::GetNBytes(uint32_t qIndex){
		NS_ASSERT_MSG(qIndex < m_qpGrp->GetN(), "RdmaEgressQueue::GetNBytes: qIndex >= m_qpGrp->GetN()");
		return m_qpGrp->Get(qIndex)->GetBytesLeft();
	}

	uint32_t RdmaEgressQueue::GetFlowCount(void){
		return m_qpGrp->GetN();
	}

	Ptr<RdmaQueuePair> RdmaEgressQueue::GetQp(uint32_t i){
		return m_qpGrp->Get(i);
	}
 
	void RdmaEgressQueue::RecoverQueue(uint32_t i){
		NS_ASSERT_MSG(i < m_qpGrp->GetN(), "RdmaEgressQueue::RecoverQueue: qIndex >= m_qpGrp->GetN()");
		m_qpGrp->Get(i)->snd_nxt = m_qpGrp->Get(i)->snd_una;
	}

	void RdmaEgressQueue::EnqueueHighPrioQ(Ptr<Packet> p){
		m_traceRdmaEnqueue(p, 0);
		m_ackQ->Enqueue(p);
	}

	void RdmaEgressQueue::CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb){
		while (m_ackQ->GetNPackets() > 0){
			Ptr<Packet> p = m_ackQ->Dequeue();
			dropCb(p, 0);
		}
	}

	/******************
	 * QbbNetDevice
	 *****************/
	NS_OBJECT_ENSURE_REGISTERED(QbbNetDevice);

	TypeId
		QbbNetDevice::GetTypeId(void)
	{
		static TypeId tid = TypeId("ns3::QbbNetDevice")
			.SetParent<PointToPointNetDevice>()
			.AddConstructor<QbbNetDevice>()
			.AddAttribute("QbbEnabled",
				"Enable the generation of PAUSE packet.",
				BooleanValue(true),
				MakeBooleanAccessor(&QbbNetDevice::m_qbbEnabled),
				MakeBooleanChecker())
			.AddAttribute("QcnEnabled",
				"Enable the generation of PAUSE packet.",
				BooleanValue(false),
				MakeBooleanAccessor(&QbbNetDevice::m_qcnEnabled),
				MakeBooleanChecker())
			.AddAttribute("DynamicThreshold",
				"Enable dynamic threshold.",
				BooleanValue(false),
				MakeBooleanAccessor(&QbbNetDevice::m_dynamicth),
				MakeBooleanChecker())
			.AddAttribute("EnableCongestionLabelCompression",
				"Enable prefix compression for congestion labels.",
				BooleanValue(true),
				MakeBooleanAccessor(&QbbNetDevice::m_enableCongestionLabelCompression),
				MakeBooleanChecker())
			.AddAttribute("PauseTime",
				"Number of microseconds to pause upon congestion",
				UintegerValue(5),
				MakeUintegerAccessor(&QbbNetDevice::m_pausetime),
				MakeUintegerChecker<uint32_t>())
			.AddAttribute ("TxBeQueue", 
					"A queue to use as the transmit queue in the device.",
					PointerValue (),
					MakePointerAccessor (&QbbNetDevice::m_queue),
					MakePointerChecker<Queue> ())
			.AddAttribute ("RdmaEgressQueue", 
					"A queue to use as the transmit queue in the device.",
					PointerValue (),
					MakePointerAccessor (&QbbNetDevice::m_rdmaEQ),
					MakePointerChecker<Object> ())
			.AddTraceSource ("QbbEnqueue", "Enqueue a packet in the QbbNetDevice.",
					MakeTraceSourceAccessor (&QbbNetDevice::m_traceEnqueue))
			.AddTraceSource ("QbbDequeue", "Dequeue a packet in the QbbNetDevice.",
					MakeTraceSourceAccessor (&QbbNetDevice::m_traceDequeue))
			.AddTraceSource ("QbbDrop", "Drop a packet in the QbbNetDevice.",
					MakeTraceSourceAccessor (&QbbNetDevice::m_traceDrop))
			.AddTraceSource ("RdmaQpDequeue", "A qp dequeue a packet.",
					MakeTraceSourceAccessor (&QbbNetDevice::m_traceQpDequeue))
			.AddTraceSource ("QbbPfc", "get a PFC packet. 0: resume, 1: pause",
					MakeTraceSourceAccessor (&QbbNetDevice::m_tracePfc))
			;

		return tid;
	}

	QbbNetDevice::QbbNetDevice()
	{
		NS_LOG_FUNCTION(this);
		m_ecn_source = new std::vector<ECNAccount>;
		m_congestionLabelTxBytes = 0;
		m_congestionLabelPayloadBytes = 0;
		m_congestionLabelTxCount = 0;
		m_congestionLabelGenTxWallNs = 0;
		m_congestionLabelRxWallNs = 0;
		m_congestionLabelRxCount = 0;
		for (uint32_t i = 0; i < qCnt; i++){
			m_paused[i] = false;
		}

		m_rdmaEQ = CreateObject<RdmaEgressQueue>();
	}

	QbbNetDevice::~QbbNetDevice()
	{
		NS_LOG_FUNCTION(this);
	}

	void
		QbbNetDevice::DoDispose()
	{
		NS_LOG_FUNCTION(this);

		PointToPointNetDevice::DoDispose();
	}

	void
		QbbNetDevice::TransmitComplete(void)
	{
		NS_LOG_FUNCTION(this);
		m_txMachineState = BUSY;
		NS_ASSERT_MSG(m_txMachineState == BUSY, "Must be BUSY if transmitting");
		m_txMachineState = READY;
		NS_ASSERT_MSG(m_currentPkt != 0, "QbbNetDevice::TransmitComplete(): m_currentPkt zero");
		m_phyTxEndTrace(m_currentPkt);
		m_currentPkt = 0;
		DequeueAndTransmit();
	}

	void
		QbbNetDevice::DequeueAndTransmit(void)
	{
		NS_LOG_FUNCTION(this);
		if (!m_linkUp) return; // if link is down, return
		if (m_txMachineState == BUSY) return;	// Quit if channel busy
		Ptr<Packet> p;
		if (m_node->GetNodeType() == 0){
			int qIndex = m_rdmaEQ->GetNextQindex(m_paused);
			if (qIndex != -1024){
				if (qIndex == -1){ // high prio
					p = m_rdmaEQ->DequeueQindex(qIndex);
					m_traceDequeue(p, 0);
					TransmitStart(p);
					return;
				}
				// a qp dequeue a packet
				Ptr<RdmaQueuePair> lastQp = m_rdmaEQ->GetQp(qIndex);
				p = m_rdmaEQ->DequeueQindex(qIndex);

				// transmit
				m_traceQpDequeue(p, lastQp);
				TransmitStart(p);

				// update for the next avail time
				m_rdmaPktSent(lastQp, p, m_tInterframeGap);
			}else { // no packet to send
				NS_LOG_INFO("PAUSE prohibits send at node " << m_node->GetId());
				Time t = Simulator::GetMaximumSimulationTime();
				for (uint32_t i = 0; i < m_rdmaEQ->GetFlowCount(); i++){
					Ptr<RdmaQueuePair> qp = m_rdmaEQ->GetQp(i);
					if (qp->GetBytesLeft() == 0)
						continue;
					t = Min(qp->m_nextAvail, t);
				}
				if (m_nextSend.IsExpired() && t < Simulator::GetMaximumSimulationTime() && t > Simulator::Now()){
					m_nextSend = Simulator::Schedule(t - Simulator::Now(), &QbbNetDevice::DequeueAndTransmit, this);
				}
			}
			return;
		}else{   //switch, doesn't care about qcn, just send
			 
			p = m_queue->DequeueRR(m_paused);		//this is round-robin
			if (p != 0){
				m_snifferTrace(p);
				m_promiscSnifferTrace(p);
				Ipv4Header h;
				Ptr<Packet> packet = p->Copy();
				uint16_t protocol = 0;
				ProcessHeader(packet, protocol);
				packet->RemoveHeader(h);
				FlowIdTag t;
				uint32_t qIndex = m_queue->GetLastQueue();
				if (qIndex == 0){//this is a pause or cnp, send it immediately!
					
					m_node->SwitchNotifyDequeue(m_ifIndex, qIndex, p);
					p->RemovePacketTag(t);
					
				}else{
					m_node->SwitchNotifyDequeue(m_ifIndex, qIndex, p);
					p->RemovePacketTag(t);
				}
				m_traceDequeue(p, qIndex);
				TransmitStart(p);
				return;
			}else{ //No queue can deliver any packet
				NS_LOG_INFO("PAUSE prohibits send at node " << m_node->GetId());
				if (m_node->GetNodeType() == 0 && m_qcnEnabled){ //nothing to send, possibly due to qcn flow control, if so reschedule sending
					
					Time t = Simulator::GetMaximumSimulationTime();
					for (uint32_t i = 0; i < m_rdmaEQ->GetFlowCount(); i++){
						Ptr<RdmaQueuePair> qp = m_rdmaEQ->GetQp(i);
						if (qp->GetBytesLeft() == 0)
							continue;
						t = Min(qp->m_nextAvail, t);
					}
					
					if (m_nextSend.IsExpired() && t < Simulator::GetMaximumSimulationTime() && t > Simulator::Now()){
						
						m_nextSend = Simulator::Schedule(t - Simulator::Now(), &QbbNetDevice::DequeueAndTransmit, this);
					}
				}
			}
		}
		return;
	}

	void
		QbbNetDevice::Resume(unsigned qIndex)
	{
		NS_LOG_FUNCTION(this << qIndex);
		NS_ASSERT_MSG(m_paused[qIndex], "Must be PAUSEd");
		m_paused[qIndex] = false;
		NS_LOG_INFO("Node " << m_node->GetId() << " dev " << m_ifIndex << " queue " << qIndex <<
			" resumed at " << Simulator::Now().GetSeconds());
		DequeueAndTransmit();
	}

	void
		QbbNetDevice::Receive(Ptr<Packet> packet)
	{
		NS_LOG_FUNCTION(this << packet);
		if (!m_linkUp){
			m_traceDrop(packet, 0);
			return;
		}

		if (m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet))
		{
			// 
			// If we have an error model and it indicates that it is time to lose a
			// corrupted packet, don't forward this packet up, let it go.
			//
			m_phyRxDropTrace(packet);
			return;
		}

		m_macRxTrace(packet);
		// 先只解析 L2/L3，避免对 0xFF 控制报文误按固定 L4 头反序列化导致越界。
		CustomHeader chL3(CustomHeader::L2_Header | CustomHeader::L3_Header);
		packet->PeekHeader(chL3);

		if (chL3.l3Prot == 0xFF){ // congestion label broadcast
			Ptr<Packet> payloadPkt = packet->Copy();
			uint16_t protocol = 0;
			ProcessHeader(payloadPkt, protocol);
			Ipv4Header ipv4h;
			if (payloadPkt->GetSize() >= ipv4h.GetSerializedSize()){
				payloadPkt->RemoveHeader(ipv4h);
				uint32_t payloadSize = payloadPkt->GetSize();
				if (payloadSize > 0){
					std::vector<uint8_t> payload(payloadSize);
					payloadPkt->CopyData(payload.data(), payloadSize);
					auto rxStart = std::chrono::high_resolution_clock::now();
					DecodeCongestionLabelPayload(payload);
					auto rxEnd = std::chrono::high_resolution_clock::now();
					m_congestionLabelRxWallNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(rxEnd - rxStart).count());
					m_congestionLabelRxCount++;
				}
			}
			return; // control packet consumed locally
		}

		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		ch.getInt = 1; // parse INT header
		packet->PeekHeader(ch);

		if (ch.l3Prot == 0xFE){ // PFC
			

			if (!m_qbbEnabled) return;
			unsigned qIndex = ch.pfc.qIndex;
			if (ch.pfc.time > 0){
				m_tracePfc(1);
				m_paused[qIndex] = true;
			}else{
				m_tracePfc(0);
				Resume(qIndex);
			}
		

		}else { // non-PFC packets (data, ACK, NACK, CNP...)
			if (m_node->GetNodeType() > 0){ // switch
				packet->AddPacketTag(FlowIdTag(m_ifIndex));
				m_node->SwitchReceiveFromDevice(this, packet, ch);
			}else { // NIC
				// send to RdmaHw
				int ret = m_rdmaReceiveCb(packet, ch);
				// TODO we may based on the ret do something
			}
		}
		return;
	}

	bool QbbNetDevice::Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber)
	{
		NS_ASSERT_MSG(false, "QbbNetDevice::Send not implemented yet\n");
		return false;
	}

	bool QbbNetDevice::SwitchSend (uint32_t qIndex, Ptr<Packet> packet, CustomHeader &ch){
		m_macTxTrace(packet);
		m_traceEnqueue(packet, qIndex);
		m_queue->Enqueue(packet, qIndex);
		DequeueAndTransmit();
		return true;
	}

	uint32_t QbbNetDevice::bytes_to_uint32(const uint8_t* buffer) {
		return (static_cast<uint32_t>(buffer[0]) << 24) |
			(static_cast<uint32_t>(buffer[1]) << 16) |
			(static_cast<uint32_t>(buffer[2]) << 8) |
			(static_cast<uint32_t>(buffer[3]));
	}

	uint16_t QbbNetDevice::bytes_to_uint16(const uint8_t* buffer) {
		return static_cast<uint16_t>((static_cast<uint16_t>(buffer[0]) << 8) |
			static_cast<uint16_t>(buffer[1]));
	}

	// 将“受影响目的IP集合”编码为拥塞标签负载。
	// 简化统一格式(基于旧格式扩展):
	// [nodeid:4][prefix_count:4][prefix:4][range_count:4][start:1][end:1]...
	// - prefix 为 /24 前缀(最低 8bit 置 0)
	// - 每个 [start,end] 表示该 prefix 下连续后缀区间
	std::vector<uint8_t> QbbNetDevice::BuildCongestionLabelPayload(uint32_t nodeid, const std::vector<uint32_t>& affectedDestinations) const {
		std::vector<uint32_t> dedupIps = affectedDestinations;
		std::sort(dedupIps.begin(), dedupIps.end());
		dedupIps.erase(std::unique(dedupIps.begin(), dedupIps.end()), dedupIps.end());

		std::vector<uint8_t> bytes;
		auto append_u32 = [&bytes](uint32_t v){
			bytes.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
			bytes.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
			bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
			bytes.push_back(static_cast<uint8_t>(v & 0xFF));
		};
		std::map<uint32_t, std::vector<uint8_t>> prefixToSuffixes;
		for (uint32_t ip : dedupIps){
			uint32_t prefix = ip & 0xFFFFFF00u;
			uint8_t suffix = static_cast<uint8_t>(ip & 0xFFu);
			prefixToSuffixes[prefix].push_back(suffix);
		}

		std::vector<uint32_t> prefixes;
		prefixes.reserve(prefixToSuffixes.size());
		for (const auto& kv : prefixToSuffixes){
			prefixes.push_back(kv.first);
		}
		std::sort(prefixes.begin(), prefixes.end());

		append_u32(nodeid);
		append_u32(static_cast<uint32_t>(prefixes.size()));

		for (size_t pi = 0; pi < prefixes.size(); ++pi){
			uint32_t prefix = prefixes[pi];
			std::vector<uint8_t>& suffixes = prefixToSuffixes[prefix];
			std::sort(suffixes.begin(), suffixes.end());
			suffixes.erase(std::unique(suffixes.begin(), suffixes.end()), suffixes.end());

			// 连续后缀压缩为若干 [start, end] 区间。
			std::vector<std::pair<uint8_t, uint8_t>> ranges;
			if (!suffixes.empty()){
				uint8_t start = suffixes[0];
				uint8_t prev = suffixes[0];
				for (size_t i = 1; i < suffixes.size(); ++i){
					uint8_t cur = suffixes[i];
					if (static_cast<uint16_t>(prev) + 1 == cur){
						prev = cur;
						continue;
					}
					ranges.push_back(std::make_pair(start, prev));
					start = cur;
					prev = cur;
				}
				ranges.push_back(std::make_pair(start, prev));
			}

			append_u32(prefix);
			append_u32(static_cast<uint32_t>(ranges.size()));
			for (size_t r = 0; r < ranges.size(); ++r){
				bytes.push_back(ranges[r].first);
				bytes.push_back(ranges[r].second);
			}
		}
		return bytes;
	}

	// 解析拥塞标签负载，并更新本地阻塞目的地址表。
	// 与 BuildCongestionLabelPayload 对应的统一格式:
	// [nodeid:4][prefix_count:4][prefix:4][range_count:4][start:1][end:1]...
	bool QbbNetDevice::DecodeCongestionLabelPayload(const std::vector<uint8_t>& payload) {
		if (payload.size() < 8){
			return false;
		}

		// 统一格式:
		// [nodeid:4][prefix_count:4][prefix:4][range_count:4][start:1][end:1]...
		size_t offset = 0;
		bool parsedAny = false;
		while (offset + 8 <= payload.size()){
			uint32_t nodeid = bytes_to_uint32(&payload[offset]);
			offset += 4;
			uint32_t prefixCount = bytes_to_uint32(&payload[offset]);
			offset += 4;

			std::vector<uint32_t> decodedIps;
			std::map<uint32_t, std::bitset<256>> prefixBitmap;
			for (uint32_t i = 0; i < prefixCount; ++i){
				if (offset + 8 > payload.size()){
					return false;
				}
				uint32_t prefix = bytes_to_uint32(&payload[offset]) & 0xFFFFFF00u;
				offset += 4;
				uint32_t rangeCount = bytes_to_uint32(&payload[offset]);
				offset += 4;
				if (offset + static_cast<size_t>(rangeCount) * 2 > payload.size()){
					return false;
				}

				std::bitset<256>& bitmap = prefixBitmap[prefix];
				for (uint32_t r = 0; r < rangeCount; ++r){
					uint8_t start = payload[offset++];
					uint8_t end = payload[offset++];
					if (end < start){
						return false;
					}
					for (uint16_t s = start; s <= end; ++s){
						bitmap.set(static_cast<size_t>(s));
						decodedIps.push_back(prefix | static_cast<uint32_t>(s));
					}
				}
			}
			std::sort(decodedIps.begin(), decodedIps.end());
			decodedIps.erase(std::unique(decodedIps.begin(), decodedIps.end()), decodedIps.end());
			Rxspineinformation[nodeid] = decodedIps;
			RxspineinformationCompressed[nodeid] = prefixBitmap;
			parsedAny = true;
		}
		return parsedAny && offset == payload.size();
	}
	// 广播发送当前交换机的拥塞标签(受影响目的IP集合)。
	// L3 协议号 0xFF 用于区分这类控制报文。
    void QbbNetDevice::Sendspineinformation(uint32_t nodeid,std::vector<uint32_t> affectedDestinations){
		auto genTxStart = std::chrono::high_resolution_clock::now();
		// 本地先保存一份 nodeid->destinations 映射，便于调试和后续查询。
		addAffectedDestinations(nodeid,affectedDestinations);

		std::sort(affectedDestinations.begin(), affectedDestinations.end());
		affectedDestinations.erase(std::unique(affectedDestinations.begin(), affectedDestinations.end()), affectedDestinations.end());

		// 统计编码耗时与压缩比，便于评估压缩开销。
		auto t0 = std::chrono::high_resolution_clock::now();
		std::vector<uint8_t> byte_stream = BuildCongestionLabelPayload(nodeid, affectedDestinations);
		auto t1 = std::chrono::high_resolution_clock::now();
		auto encodeUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

		// 旧格式基准大小: nodeid(4) + vec_size(4) + N*ip(4)
		uint32_t originalBytes = 8 + static_cast<uint32_t>(affectedDestinations.size()) * 4;
		uint32_t compressedBytes = static_cast<uint32_t>(byte_stream.size());
		double compressionRatio = originalBytes == 0 ? 1.0 : static_cast<double>(compressedBytes) / static_cast<double>(originalBytes);
		uint8_t* ptr_to_byte_stream = byte_stream.data();

		Ptr<Packet> p = Create<Packet>(ptr_to_byte_stream,byte_stream.size());
		Ipv4Header ipv4h;  // Prepare IPv4 header
	    ipv4h.SetProtocol(0xFF);
	    ipv4h.SetSource(m_node->GetObject<Ipv4>()->GetAddress(m_ifIndex, 0).GetLocal());
	    ipv4h.SetDestination(Ipv4Address("255.255.255.255"));
	    ipv4h.SetPayloadSize(p->GetSize());
	    ipv4h.SetTtl(1);
	    ipv4h.SetIdentification(UniformVariable(0, 65536).GetValue());
	    p->AddHeader(ipv4h);
	    AddHeader(p, 0x800);
		m_congestionLabelPayloadBytes += byte_stream.size();
		m_congestionLabelTxBytes += p->GetSize();
		m_congestionLabelTxCount++;
	    // 0xFF 拥塞标签是可变长负载，不能按固定 L4 头解析。
	    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header);
	    p->PeekHeader(ch);
	    SwitchSend(0, p, ch);
		auto genTxEnd = std::chrono::high_resolution_clock::now();
		m_congestionLabelGenTxWallNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(genTxEnd - genTxStart).count());
        
    }
	
	// 记录某个 node 对应的受影响目的IP列表(覆盖式更新)。
	void QbbNetDevice::addAffectedDestinations(uint32_t nodeId, const std::vector<uint32_t>& destinations) {
    nodeIDToAffectedDestinations[nodeId] = destinations;
    }

	void QbbNetDevice::uint32_to_bytes(uint32_t value, uint8_t* buffer) {
    buffer[0] = (value >> 24) & 0xFF;
    buffer[1] = (value >> 16) & 0xFF;
    buffer[2] = (value >> 8) & 0xFF;
    buffer[3] = value & 0xFF;
    }

    std::map<uint32_t, std::vector<uint32_t>> QbbNetDevice::getRxspineinformationCopy() const {
        return Rxspineinformation;
    }

	const std::map<uint32_t, std::map<uint32_t, std::bitset<256>>>& QbbNetDevice::getRxspineinformationCompressed() const {
		return RxspineinformationCompressed;
	}

	// 在压缩位图表中判断 dip 是否被 nodeId 标记为阻塞目的地。
	bool QbbNetDevice::IsDipBlockedByNodeCompressed(uint32_t nodeId, uint32_t dip) const {
		auto nodeIt = RxspineinformationCompressed.find(nodeId);
		if (nodeIt == RxspineinformationCompressed.end()){
			return false;
		}
		uint32_t prefix = dip & 0xFFFFFF00u;
		uint8_t suffix = static_cast<uint8_t>(dip & 0xFFu);
		auto prefixIt = nodeIt->second.find(prefix);
		if (prefixIt == nodeIt->second.end()){
			return false;
		}
		return prefixIt->second.test(suffix);
	}

 
	// 发送 PFC 控制报文。
	// type==0: pause; type!=0: resume(时间置0)。L3 协议号 0xFE。
	void QbbNetDevice::SendPfc(uint32_t qIndex, uint32_t type){
		Ptr<Packet> p = Create<Packet>(0);
		PauseHeader pauseh((type == 0 ? m_pausetime : 0), m_queue->GetNBytes(qIndex), qIndex);
		p->AddHeader(pauseh);
		Ipv4Header ipv4h;  // Prepare IPv4 header
		ipv4h.SetProtocol(0xFE);
		ipv4h.SetSource(m_node->GetObject<Ipv4>()->GetAddress(m_ifIndex, 0).GetLocal());
		ipv4h.SetDestination(Ipv4Address("255.255.255.255"));
		ipv4h.SetPayloadSize(p->GetSize());
		ipv4h.SetTtl(1);
		ipv4h.SetIdentification(UniformVariable(0, 65536).GetValue());
		p->AddHeader(ipv4h);
		AddHeader(p, 0x800);
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		p->PeekHeader(ch);
		SwitchSend(0, p, ch);
	}

	uint64_t QbbNetDevice::GetCongestionLabelTxBytes() const {
		return m_congestionLabelTxBytes;
	}

	uint64_t QbbNetDevice::GetCongestionLabelPayloadBytes() const {
		return m_congestionLabelPayloadBytes;
	}

	uint64_t QbbNetDevice::GetCongestionLabelTxCount() const {
		return m_congestionLabelTxCount;
	}

	uint64_t QbbNetDevice::GetCongestionLabelGenTxWallNs() const {
		return m_congestionLabelGenTxWallNs;
	}

	uint64_t QbbNetDevice::GetCongestionLabelRxWallNs() const {
		return m_congestionLabelRxWallNs;
	}

	uint64_t QbbNetDevice::GetCongestionLabelRxCount() const {
		return m_congestionLabelRxCount;
	}

	bool
		QbbNetDevice::Attach(Ptr<QbbChannel> ch)
	{
		NS_LOG_FUNCTION(this << &ch);
		m_channel = ch;
		m_channel->Attach(this);
		NotifyLinkUp();
		return true;
	}

	bool
		QbbNetDevice::TransmitStart(Ptr<Packet> p)
	{
		NS_LOG_FUNCTION(this << p);
		NS_LOG_LOGIC("UID is " << p->GetUid() << ")");
		//
		// This function is called to start the process of transmitting a packet.
		// We need to tell the channel that we've started wiggling the wire and
		// schedule an event that will be executed when the transmission is complete.
		//
		m_txMachineState = READY;
		NS_ASSERT_MSG(m_txMachineState == READY, "Must be READY to transmit");
		m_txMachineState = BUSY;
		m_currentPkt = p;
		m_phyTxBeginTrace(m_currentPkt);
		Time txTime = Seconds(m_bps.CalculateTxTime(p->GetSize()));
		Time txCompleteTime = txTime + m_tInterframeGap;
		NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.GetSeconds() << "sec");
		Simulator::Schedule(txCompleteTime, &QbbNetDevice::TransmitComplete, this);

		bool result = m_channel->TransmitStart(p, this, txTime);
		if (result == false)
		{
			m_phyTxDropTrace(p);
		}
		return result;
	}

	Ptr<Channel>
		QbbNetDevice::GetChannel(void) const
	{
		return m_channel;
	}

   bool QbbNetDevice::IsQbb(void) const{
	   return true;
   }

   void QbbNetDevice::NewQp(Ptr<RdmaQueuePair> qp){
	   qp->m_nextAvail = Simulator::Now();
	   DequeueAndTransmit();
   }
   void QbbNetDevice::ReassignedQp(Ptr<RdmaQueuePair> qp){
	   DequeueAndTransmit();
   }
   void QbbNetDevice::TriggerTransmit(void){
	   DequeueAndTransmit();
   }

	void QbbNetDevice::SetQueue(Ptr<BEgressQueue> q){
		NS_LOG_FUNCTION(this << q);
		m_queue = q;
	}

	Ptr<BEgressQueue> QbbNetDevice::GetQueue(){
		return m_queue;
	}

	Ptr<RdmaEgressQueue> QbbNetDevice::GetRdmaQueue(){
		return m_rdmaEQ;
	}

	void QbbNetDevice::RdmaEnqueueHighPrioQ(Ptr<Packet> p){
		m_traceEnqueue(p, 0);
		m_rdmaEQ->EnqueueHighPrioQ(p);
	}

	void QbbNetDevice::TakeDown(){
		// TODO: delete packets in the queue, set link down
		if (m_node->GetNodeType() == 0){
			// clean the high prio queue
			m_rdmaEQ->CleanHighPrio(m_traceDrop);
			// notify driver/RdmaHw that this link is down
			m_rdmaLinkDownCb(this);
		}else { // switch
			// clean the queue
			for (uint32_t i = 0; i < qCnt; i++)
				m_paused[i] = false;
			while (1){
				Ptr<Packet> p = m_queue->DequeueRR(m_paused);
				if (p == 0)
					 break;
				m_traceDrop(p, m_queue->GetLastQueue());
			}
			// TODO: Notify switch that this link is down
		}
		m_linkUp = false;
	}

	void QbbNetDevice::UpdateNextAvail(Time t){
		if (!m_nextSend.IsExpired() && t < m_nextSend.GetTs()){
			Simulator::Cancel(m_nextSend);
			Time delta = t < Simulator::Now() ? Time(0) : t - Simulator::Now();
			m_nextSend = Simulator::Schedule(delta, &QbbNetDevice::DequeueAndTransmit, this);
		}
	}
} // namespace ns3
