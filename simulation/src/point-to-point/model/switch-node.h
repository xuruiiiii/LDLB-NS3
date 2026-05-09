#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <unordered_map>
#include <ns3/node.h>
#include "qbb-net-device.h"
#include "switch-mmu.h"
#include "pint.h"
#include <algorithm>  
#include <random>  
#include <set>
#include "ns3/node-container.h"

namespace ns3 {

class Packet;

class SwitchNode : public Node{
	static const uint32_t pCnt = 257;	// Number of ports used
	static const uint32_t qCnt = 8;	// Number of queues/priorities used
	uint32_t m_ecmpSeed;
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)
	
	std::map<uint32_t, std::vector<uint32_t>> nodeIDToAffectedDestinations;//交换机编号及其不可达目的ip
	
	std::map<uint32_t, std::vector<uint32_t>>Rxspineinformationinswitch;
	 std::vector<int> lastCongestedIntfIdxs;
	 std::vector<int> newCongestedIntfIdxs;
	 std::vector<int> pendingCongestedIntfIdxs;
	 uint32_t pendingStableRounds;
	 uint64_t lastCongestionBroadcastNs;
	
	 
	 std::unordered_map<uint32_t, Ptr<ns3::Node>> m_ltpnmap;//交换机编号和地址表
	
	 std::map<Ptr<ns3::Node>, std::map<Ptr<ns3::Node>,uint32_t>> m_topograph;//相邻节点链路关系
	

	// monitor of PFC
	uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx
	
	uint64_t m_txBytes[pCnt]; // counter of tx bytes
	uint64_t m_dataPlaneTxBytes[pCnt];
	uint64_t m_congestionDetectWallNs;
	uint64_t m_congestionDetectCount;

	uint32_t m_lastPktSize[pCnt];
	uint64_t m_lastPktTs[pCnt]; // ns
	double m_u[pCnt];

protected:
	bool m_ecnEnabled;
	//
	bool m_ecmpEnabled;
	//
	uint32_t m_ccMode;
	uint64_t m_maxRtt;

	uint32_t m_ackHighPrio; // set high priority for ACK/NACK
	bool m_enableBroadcastDampening;
	uint32_t m_broadcastStableRoundsRequired;
	uint64_t m_minBroadcastIntervalNs;
	std::unordered_map<uint64_t, int> m_flowPinnedOutDev;

private:
	int GetOutDev(Ptr<const Packet>, CustomHeader &ch, uint32_t qIndex);
	void SendToDev(Ptr<Packet>p, CustomHeader &ch);
	static uint32_t EcmpHash(const uint8_t* key, size_t len, uint32_t seed);
	void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
	void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
	static std::vector<int> congestedlabel;
	
public:
	Ptr<SwitchMmu> m_mmu;
    void ceshi();
	void AddTopoTable(Ptr<Node> snode, Ptr<Node> nnode,uint32_t intf_idx);
	void Addlabelmap(std::unordered_map<uint32_t, Ptr<Node>> labelToPtrnodeMap);
	static TypeId GetTypeId (void);
	std::vector<uint32_t> findDIPsByIntfIdx(uint32_t intf_idx);
	void addAffectedDestinations(uint32_t nodeId, const std::vector<uint32_t>& destinations);
	uint64_t GetDataPlaneTxBytes() const;
	uint64_t GetCongestionDetectWallNs() const;
	uint64_t GetCongestionDetectCount() const;
	SwitchNode();
	void SetEcmpSeed(uint32_t seed);
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void ClearTable();
	bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
	void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);

	// for approximate calc in PINT
	int logres_shift(int b, int l);
	int log2apprx(int x, int b, int m, int l); // given x of at most b bits, use most significant m bits of x, calc the result in l bits
	
	
	std::vector<int> swlabel;
	
	std::vector<int> labels;
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
