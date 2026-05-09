#include "ns3/ipv4.h"
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
#include "ns3/pause-header.h"
#include "ns3/flow-id-tag.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "switch-node.h"
#include "switch-mmu.h"
#include "qbb-net-device.h"
#include "ppp-header.h"
#include "ns3/int-header.h"
#include <cmath>
#include <algorithm>  
#include <random>  
#include <limits>
#include <chrono>
#include "ns3/node-container.h"

namespace ns3 {

static std::vector<int> NormalizeIntfVector(std::vector<int> v)
{
	std::sort(v.begin(), v.end());
	v.erase(std::unique(v.begin(), v.end()), v.end());
	return v;
}

TypeId SwitchNode::GetTypeId (void)//������createobject<switchnode>���ҵ���Ӧ��ʵ��������������ָ��
{
  static TypeId tid = TypeId ("ns3::SwitchNode")
    .SetParent<Node> ()
    .AddConstructor<SwitchNode> ()
	.AddAttribute("EcnEnabled",
			"Enable ECN marking.",
			BooleanValue(false),
			MakeBooleanAccessor(&SwitchNode::m_ecnEnabled),
			MakeBooleanChecker())
	.AddAttribute("CcMode",
			"CC mode.",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_ccMode),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("AckHighPrio",
			"Set high priority for ACK/NACK or not",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_ackHighPrio),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("MaxRtt",
			"Max Rtt of the network",
			UintegerValue(9000),
			MakeUintegerAccessor(&SwitchNode::m_maxRtt),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("EnableBroadcastDampening",
			"Enable dampening for congestion broadcast updates.",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_enableBroadcastDampening),
			MakeBooleanChecker())
	.AddAttribute("BroadcastStableRounds",
			"Required stable rounds before broadcasting a congestion update.",
			UintegerValue(2),
			MakeUintegerAccessor(&SwitchNode::m_broadcastStableRoundsRequired),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("MinBroadcastIntervalNs",
			"Minimum interval between two congestion broadcasts in nanoseconds.",
			UintegerValue(50000),
			MakeUintegerAccessor(&SwitchNode::m_minBroadcastIntervalNs),
			MakeUintegerChecker<uint64_t>())
	//
	.AddAttribute("FlowECMP",
			"Set ECMP Hash or not",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_ecmpEnabled),
			MakeBooleanChecker())


  ;
  return tid;
}

SwitchNode::SwitchNode(){
	//
	// if (m_ecmpEnabled)
	// 	m_ecmpSeed = m_id;
	
	m_ecmpSeed = m_id;
	m_node_type = 1;
	m_mmu = CreateObject<SwitchMmu>();
	for (uint32_t i = 0; i < pCnt; i++)
		for (uint32_t j = 0; j < pCnt; j++)
			for (uint32_t k = 0; k < qCnt; k++)
				m_bytes[i][j][k] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_txBytes[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_dataPlaneTxBytes[i] = 0;
	m_congestionDetectWallNs = 0;
	m_congestionDetectCount = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_lastPktSize[i] = m_lastPktTs[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_u[i] = 0;
	pendingStableRounds = 0;
	lastCongestionBroadcastNs = 0;
	m_enableBroadcastDampening = true;
	m_broadcastStableRoundsRequired = 2;
	m_minBroadcastIntervalNs = 50000;
	
}


int SwitchNode::GetOutDev(Ptr<const Packet> p, CustomHeader &ch, uint32_t qIndex){
	// look up entries
	auto entry = m_rtTable.find(ch.dip);
	

	// no matching entry
	if (entry == m_rtTable.end())
		return -1;

	// entry found
	auto &nexthops = entry->second;
	if (nexthops.empty())
		return -1;

	// pick one next hop based on hash
	union {
		uint8_t u8[4+4+2+2];
		uint32_t u32[3];
	} buf;
	buf.u32[0] = ch.sip;
	buf.u32[1] = ch.dip;
	if (ch.l3Prot == 0x6)
		buf.u32[2] = ch.tcp.sport | ((uint32_t)ch.tcp.dport << 16);
	else if (ch.l3Prot == 0x11)
		buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
	else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD)
		buf.u32[2] = ch.ack.sport | ((uint32_t)ch.ack.dport << 16);
	else
		buf.u32[2] = 0;

	uint64_t flowKey = (static_cast<uint64_t>(buf.u32[0]) << 32) ^ static_cast<uint64_t>(buf.u32[1]);
	flowKey ^= (static_cast<uint64_t>(buf.u32[2]) << 1) ^ static_cast<uint64_t>(ch.l3Prot);

	// 已固定路径的流优先沿用原出口，避免拥塞状态变化引起反复重路由。
	auto pinnedIt = m_flowPinnedOutDev.find(flowKey);
	if (pinnedIt != m_flowPinnedOutDev.end()) {
		int pinnedOutDev = pinnedIt->second;
		if (pinnedOutDev >= 0 && static_cast<uint32_t>(pinnedOutDev) < GetNDevices() &&
			std::find(nexthops.begin(), nexthops.end(), pinnedOutDev) != nexthops.end()) {
			Ptr<QbbNetDevice> pinnedDev = DynamicCast<QbbNetDevice>(m_devices[pinnedOutDev]);
			if (pinnedDev != 0 && pinnedDev->IsLinkUp()) {
				return pinnedOutDev;
			}
		}
		m_flowPinnedOutDev.erase(pinnedIt);
	}

	if (!m_ecmpEnabled) {
		uint32_t idx = EcmpHash(buf.u8, 12, m_ecmpSeed) % nexthops.size();
		return nexthops[idx];
	}

	auto normalizeCandidates = [](std::vector<int> v) {
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		return v;
	};

	// 汇总当前已经被判定为拥塞的接口：MMU 上报 + 脊交换机广播回来的受影响接口。
	std::set<uint32_t> blockedNodeIds;
	for (uint32_t i = 1; i < GetNDevices(); i++){
	   Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[i]);
	   if (device == 0){
		   continue;
	   }
	   const auto& compressedInfo = device->getRxspineinformationCompressed();
	   for (const auto& pair : compressedInfo) {
		   if (device->IsDipBlockedByNodeCompressed(pair.first, buf.u32[1])){
			   blockedNodeIds.insert(pair.first);
		   }
	   }
	}
	
    


	std::vector<int> CongestedNextnodeIntfIdxs;
	for (uint32_t blockedNodeId : blockedNodeIds) {
			auto nIt = m_ltpnmap.find(blockedNodeId);
			auto sIt = m_ltpnmap.find(m_mmu->node_id);
			if (nIt == m_ltpnmap.end() || sIt == m_ltpnmap.end()){
				continue;
			}
			Ptr<Node> nnode = nIt->second;//拥塞的下一节点的内存地址
            Ptr<Node> snode = sIt->second;//当前节点的内存地址
			auto topoSrcIt = m_topograph.find(snode);
			if (topoSrcIt != m_topograph.end()){
				auto topoDstIt = topoSrcIt->second.find(nnode);
				if (topoDstIt != topoSrcIt->second.end()){
					int spineinterface = topoDstIt->second;//找到达下一个拥塞节点的链路编号
					CongestedNextnodeIntfIdxs.push_back(spineinterface);
				}
			}
	}

	
	
	
	std::vector<int> congestedIntfs = normalizeCandidates(m_mmu->GetCongestedLabels());
	congestedIntfs.insert(congestedIntfs.end(), CongestedNextnodeIntfIdxs.begin(), CongestedNextnodeIntfIdxs.end());
	congestedIntfs = normalizeCandidates(congestedIntfs);

	std::vector<int> filteredArray;
	std::copy_if(nexthops.begin(), nexthops.end(), std::back_inserter(filteredArray),
		[&congestedIntfs](int num) {
			return std::find(congestedIntfs.begin(), congestedIntfs.end(), num) == congestedIntfs.end();
		});

	const std::vector<int>& candidateArray = filteredArray.empty() ? nexthops : filteredArray;

	uint32_t minQueueBytes = std::numeric_limits<uint32_t>::max();
	std::vector<int> minQueueCandidates;
	for (int outDev : candidateArray) {
		if (outDev < 0 || static_cast<uint32_t>(outDev) >= GetNDevices()) {
			continue;
		}
		Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[outDev]);
		if (device == 0 || !device->IsLinkUp()) {
			continue;
		}

		uint32_t queueBytes = m_mmu->GetEgressQueueBytes(outDev, qIndex);
		Ptr<BEgressQueue> queue = device->GetQueue();
		if (queue != 0) {
			queueBytes = std::max(queueBytes, queue->GetNBytesTotal());
		}

		if (queueBytes < minQueueBytes) {
			minQueueBytes = queueBytes;
			minQueueCandidates.clear();
			minQueueCandidates.push_back(outDev);
		} else if (queueBytes == minQueueBytes) {
			minQueueCandidates.push_back(outDev);
		}
	}

	if (minQueueCandidates.empty()) {
		for (int outDev : nexthops) {
			if (outDev >= 0 && static_cast<uint32_t>(outDev) < GetNDevices()) {
				Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[outDev]);
				if (device != 0 && device->IsLinkUp()) {
					return outDev;
				}
			}
		}
		return -1;
	}

	uint32_t tieIdx = EcmpHash(buf.u8, 12, m_ecmpSeed ^ 0x7f4a7c15u) % minQueueCandidates.size();
	int selectedOutDev = minQueueCandidates[tieIdx];
	m_flowPinnedOutDev[flowKey] = selectedOutDev;
	return selectedOutDev;

}

void SwitchNode::CheckAndSendPfc(uint32_t inDev, uint32_t qIndex){
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldPause(inDev, qIndex)){
		device->SendPfc(qIndex, 0);
		m_mmu->SetPause(inDev, qIndex);
	}
}
void SwitchNode::CheckAndSendResume(uint32_t inDev, uint32_t qIndex){
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldResume(inDev, qIndex)){
		device->SendPfc(qIndex, 1);
		m_mmu->SetResume(inDev, qIndex);
	}
}

void SwitchNode::SendToDev(Ptr<Packet>p, CustomHeader &ch){
	// determine the qIndex first so GetOutDev can evaluate queue-aware WCMP weights.
	uint32_t qIndex;
	if (ch.l3Prot == 0xFF || ch.l3Prot == 0xFE || (m_ackHighPrio && (ch.l3Prot == 0xFD || ch.l3Prot == 0xFC))){  //QCN or PFC or NACK, go highest priority
		qIndex = 0;
	}else{
		qIndex = (ch.l3Prot == 0x06 ? 1 : ch.udp.pg); // if TCP, put to queue 1
	}

	int idx = GetOutDev(p, ch, qIndex);
	if (idx >= 0){
		NS_ASSERT_MSG(m_devices[idx]->IsLinkUp(), "The routing table look up should return link that is up");

		// admission control
			
			uint32_t nodeid=m_mmu->node_id;
			auto selfIt = m_ltpnmap.find(nodeid);
			bool canBroadcast = false;
			Ptr<Node> cnode = 0;
			if (selfIt != m_ltpnmap.end()){
				cnode = selfIt->second;
				auto topoIt = m_topograph.find(cnode);
				canBroadcast = (topoIt != m_topograph.end() && !topoIt->second.empty());
			}
			if (canBroadcast) {
		       newCongestedIntfIdxs = NormalizeIntfVector(m_mmu->GetCongestedLabels());//更新机制

			   if (newCongestedIntfIdxs != pendingCongestedIntfIdxs){
				   pendingCongestedIntfIdxs = newCongestedIntfIdxs;
				   pendingStableRounds = 1;
			   } else {
				   pendingStableRounds++;
			   }

			   uint64_t nowNs = Simulator::Now().GetNanoSeconds();
			   bool stableEnough = (!m_enableBroadcastDampening) || (pendingStableRounds >= m_broadcastStableRoundsRequired);
			   bool changedFromLastBroadcast = pendingCongestedIntfIdxs != lastCongestedIntfIdxs;
			   bool intervalSatisfied = (!m_enableBroadcastDampening) || (lastCongestionBroadcastNs == 0) || (nowNs >= lastCongestionBroadcastNs + m_minBroadcastIntervalNs);

		      if (stableEnough && changedFromLastBroadcast && intervalSatisfied){
				   lastCongestedIntfIdxs = pendingCongestedIntfIdxs;
				   lastCongestionBroadcastNs = nowNs;
			        //std::vector<int> myVector = {3};//测试假设拥塞点，spine拥塞控制阶段可能需要一套更新机制，如果拥塞解除，得发送解除数据包,（目前拥塞链路数组有无数据都会）
			        const std::vector<int> congestedIntfIdxs = newCongestedIntfIdxs;
			   
			        // 用于存储所有受影响的目的IP地址(临时)
                    std::vector<uint32_t> affectedDestinations;
         
                    // 遍历所有拥塞的接口索引
                    for (int intfIdx : congestedIntfIdxs) {
                    // 根据拥塞接口查到dips放到destinationForIntf,再插入总表
                        std::vector<uint32_t> destinationForIntf = findDIPsByIntfIdx(intfIdx); // 注意这里的函数名已经更改
 
                        // 将结果添加到总列表中
                        affectedDestinations.insert(affectedDestinations.end(), destinationForIntf.begin(), destinationForIntf.end());
                        }
			
						auto topoIt = m_topograph.find(cnode);
						if (topoIt != m_topograph.end()){
							for (const auto &kv : topoIt->second){
								uint32_t outIf = kv.second;
								if (outIf >= GetNDevices()){
									continue;
								}
								Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[outIf]);
								if (device == 0){
									continue;
								}
								device->Sendspineinformation(nodeid,affectedDestinations);
							}
						}
			    }
			}
		
		
		
		
		
		
		FlowIdTag t;
		p->PeekPacketTag(t);
		uint32_t inDev = t.GetFlowId();
		if (qIndex != 0){ //not highest priority
			if (1){			// Admission control
				m_mmu->UpdateIngressAdmission(inDev, qIndex, p->GetSize());
				m_mmu->UpdateEgressAdmission(idx, qIndex, p->GetSize());
			}else{
				return; // Drop
			}
			CheckAndSendPfc(inDev, qIndex);
		}
		m_bytes[inDev][idx][qIndex] += p->GetSize();
		m_dataPlaneTxBytes[idx] += p->GetSize();
		m_devices[idx]->SwitchSend(qIndex, p, ch);
	}else
		return; // Drop
    
}

uint32_t SwitchNode::EcmpHash(const uint8_t* key, size_t len, uint32_t seed) {
  uint32_t h = seed;
  if (len > 3) {
    const uint32_t* key_x4 = (const uint32_t*) key;
    size_t i = len >> 2;
    do {
      uint32_t k = *key_x4++;
      k *= 0xcc9e2d51;
      k = (k << 15) | (k >> 17);
      k *= 0x1b873593;
      h ^= k;
      h = (h << 13) | (h >> 19);
      h += (h << 2) + 0xe6546b64;
    } while (--i);
    key = (const uint8_t*) key_x4;
  }
  if (len & 3) {
    size_t i = len & 3;
    uint32_t k = 0;
    key = &key[i - 1];
    do {
      k <<= 8;
      k |= *key--;
    } while (--i);
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    h ^= k;
  }
  h ^= len;
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

void SwitchNode::SetEcmpSeed(uint32_t seed){
	m_ecmpSeed = seed;
}

void SwitchNode::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx){
	uint32_t dip = dstAddr.Get();
	m_rtTable[dip].push_back(intf_idx);

}
//************************************ *//my change















void SwitchNode::Addlabelmap( std::unordered_map<uint32_t, Ptr<Node>> labelToPtrnodeMap){
	//这里是交换机的编号和内存地址关系
	m_ltpnmap = labelToPtrnodeMap;
    

}
void SwitchNode::AddTopoTable(Ptr<Node> snode, Ptr<Node> nnode, uint32_t intf_idx){
	//相邻交换机之间内存地址到链路编号
    m_topograph[snode][nnode] = intf_idx;
	

}

uint64_t SwitchNode::GetDataPlaneTxBytes() const {
	uint64_t total = 0;
	for (uint32_t i = 0; i < pCnt; i++){
		total += m_dataPlaneTxBytes[i];
	}
	return total;
}

uint64_t SwitchNode::GetCongestionDetectWallNs() const {
	return m_congestionDetectWallNs;
}

uint64_t SwitchNode::GetCongestionDetectCount() const {
	return m_congestionDetectCount;
}

std::vector<uint32_t> SwitchNode::findDIPsByIntfIdx(uint32_t intf_idx){//根据拥塞接口反推不可达的dstIP,仅限再脊节点单链路前提
	std::vector<uint32_t> dips;
 
    // 遍历路由表
    for (const auto& pair : m_rtTable) {
        // 检查接口索引是否在值列表中
        for (uint32_t idx : pair.second) {
            if (idx == intf_idx) {
                // 如果找到，将目的IP地址添加到结果列表中
                dips.push_back(pair.first);
                break; // 不需要继续检查当前条目的其他接口索引
            }
        }
	}
	return dips;
}






//************************************************ *//my change

void SwitchNode::ClearTable(){
	m_rtTable.clear();
}

// This function can only be called in switch mode
bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch){
	SendToDev(packet, ch);
	return true;
}

void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p){
	FlowIdTag t;
	p->PeekPacketTag(t);
	if (qIndex != 0){
		uint32_t inDev = t.GetFlowId();
		m_mmu->RemoveFromIngressAdmission(inDev, qIndex, p->GetSize());
		m_mmu->RemoveFromEgressAdmission(ifIndex, qIndex, p->GetSize());
		m_bytes[inDev][ifIndex][qIndex] -= p->GetSize();
		if (m_ecnEnabled){
			bool egressCongested = 0;
			auto detectStart = std::chrono::high_resolution_clock::now();
			m_mmu->SetSwitchLabel(ifIndex, qIndex);
			auto detectEnd = std::chrono::high_resolution_clock::now();
			m_congestionDetectWallNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(detectEnd - detectStart).count());
			m_congestionDetectCount++;


			if (egressCongested){
				
				PppHeader ppp;
				Ipv4Header h;
				p->RemoveHeader(ppp);
				p->RemoveHeader(h);
				h.SetEcn((Ipv4Header::EcnType)0x03);
				p->AddHeader(h);
				p->AddHeader(ppp);
			}
		}
		//CheckAndSendPfc(inDev, qIndex);
		CheckAndSendResume(inDev, qIndex);
	}
	if (1){
		uint8_t* buf = p->GetBuffer();
		if (buf[PppHeader::GetStaticSize() + 9] == 0x11){ // udp packet
			IntHeader *ih = (IntHeader*)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6]; // ppp, ip, udp, SeqTs, INT
			Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
			if (m_ccMode == 3){ // HPCC
				ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
			}else if (m_ccMode == 10){ // HPCC-PINT
				uint64_t t = Simulator::Now().GetTimeStep();
				uint64_t dt = t - m_lastPktTs[ifIndex];
				if (dt > m_maxRtt)
					dt = m_maxRtt;
				uint64_t B = dev->GetDataRate().GetBitRate() / 8; //Bps
				uint64_t qlen = dev->GetQueue()->GetNBytesTotal();
				double newU;

				/**************************
				 * approximate calc
				 *************************/
				int b = 20, m = 16, l = 20; // see log2apprx's paremeters
				int sft = logres_shift(b,l);
				double fct = 1<<sft; // (multiplication factor corresponding to sft)
				double log_T = log2(m_maxRtt)*fct; // log2(T)*fct
				double log_B = log2(B)*fct; // log2(B)*fct
				double log_1e9 = log2(1e9)*fct; // log2(1e9)*fct
				double qterm = 0;
				double byteTerm = 0;
				double uTerm = 0;
				if ((qlen >> 8) > 0){
					int log_dt = log2apprx(dt, b, m, l); // ~log2(dt)*fct
					int log_qlen = log2apprx(qlen >> 8, b, m, l); // ~log2(qlen / 256)*fct
					qterm = pow(2, (
								log_dt + log_qlen + log_1e9 - log_B - 2*log_T
								)/fct
							) * 256;
					// 2^((log2(dt)*fct+log2(qlen/256)*fct+log2(1e9)*fct-log2(B)*fct-2*log2(T)*fct)/fct)*256 ~= dt*qlen*1e9/(B*T^2)
				}
				if (m_lastPktSize[ifIndex] > 0){
					int byte = m_lastPktSize[ifIndex];
					int log_byte = log2apprx(byte, b, m, l);
					byteTerm = pow(2, (
								log_byte + log_1e9 - log_B - log_T
								)/fct
							);
					// 2^((log2(byte)*fct+log2(1e9)*fct-log2(B)*fct-log2(T)*fct)/fct) ~= byte*1e9 / (B*T)
				}
				if (m_maxRtt > dt && m_u[ifIndex] > 0){
					int log_T_dt = log2apprx(m_maxRtt - dt, b, m, l); // ~log2(T-dt)*fct
					int log_u = log2apprx(int(round(m_u[ifIndex] * 8192)), b, m, l); // ~log2(u*512)*fct
					uTerm = pow(2, (
								log_T_dt + log_u - log_T
								)/fct
							) / 8192;
					// 2^((log2(T-dt)*fct+log2(u*512)*fct-log2(T)*fct)/fct)/512 = (T-dt)*u/T
				}
				newU = qterm+byteTerm+uTerm;

				#if 0
				/**************************
				 * accurate calc
				 *************************/
				double weight_ewma = double(dt) / m_maxRtt;
				double u;
				if (m_lastPktSize[ifIndex] == 0)
					u = 0;
				else{
					double txRate = m_lastPktSize[ifIndex] / double(dt); // B/ns
					u = (qlen / m_maxRtt + txRate) * 1e9 / B;
				}
				newU = m_u[ifIndex] * (1 - weight_ewma) + u * weight_ewma;
				#endif

				/************************
				 * update PINT header
				 ***********************/
				uint16_t power = Pint::encode_u(newU);
				if (power > ih->GetPower())
					ih->SetPower(power);

				m_u[ifIndex] = newU;
			}
		}
	}
	m_txBytes[ifIndex] += p->GetSize();
	m_lastPktSize[ifIndex] = p->GetSize();
	m_lastPktTs[ifIndex] = Simulator::Now().GetTimeStep();
}

int SwitchNode::logres_shift(int b, int l){
	static int data[] = {0,0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5};
	return l - data[b];
}

int SwitchNode::log2apprx(int x, int b, int m, int l){
	int x0 = x;
	int msb = int(log2(x)) + 1;
	if (msb > m){
		x = (x >> (msb - m) << (msb - m));
		#if 0
		x += + (1 << (msb - m - 1));
		#else
		int mask = (1 << (msb-m)) - 1;
		if ((x0 & mask) > (rand() & mask))
			x += 1<<(msb-m);
		#endif
	}
	return int(log2(x) * (1<<logres_shift(b, l)));
}

} /* namespace ns3 */
