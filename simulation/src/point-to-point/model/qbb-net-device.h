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
* Author: Yibo Zhu <yibzh@microsoft.com>
*/
#ifndef QBB_NET_DEVICE_H
#define QBB_NET_DEVICE_H

#include "ns3/point-to-point-net-device.h"
#include "ns3/qbb-channel.h"
//#include "ns3/fivetuple.h"
#include "ns3/event-id.h"
#include "ns3/broadcom-egress-queue.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/rdma-queue-pair.h"
#include <vector>
#include<map>
#include <bitset>
#include <cstdint>
#include <ns3/rdma.h>
#include "switch-node.h"
#include "switch-mmu.h"

namespace ns3 {

class RdmaEgressQueue : public Object{
public:
	static const uint32_t qCnt = 8;
	static uint32_t ack_q_idx;
	int m_qlast;
	uint32_t m_rrlast;
	Ptr<DropTailQueue> m_ackQ; // highest priority queue
	Ptr<RdmaQueuePairGroup> m_qpGrp; // queue pairs

	// callback for get next packet
	typedef Callback<Ptr<Packet>, Ptr<RdmaQueuePair> > RdmaGetNxtPkt;
	RdmaGetNxtPkt m_rdmaGetNxtPkt;

	static TypeId GetTypeId (void);
	RdmaEgressQueue();
	Ptr<Packet> DequeueQindex(int qIndex);
	int GetNextQindex(bool paused[]);
	int GetLastQueue();
	uint32_t GetNBytes(uint32_t qIndex);
	uint32_t GetFlowCount(void);
	Ptr<RdmaQueuePair> GetQp(uint32_t i);
	void RecoverQueue(uint32_t i);
	void EnqueueHighPrioQ(Ptr<Packet> p);
	void CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb);

	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaEnqueue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaDequeue;
};

/**
 * \class QbbNetDevice
 * \brief A Device for a IEEE 802.1Qbb Network Link.
 */
class QbbNetDevice : public PointToPointNetDevice 
{
public:
  static const uint32_t qCnt = 8;	// Number of queues/priorities used

  static TypeId GetTypeId (void);

  QbbNetDevice ();
  virtual ~QbbNetDevice ();

  /**
   * Receive a packet from a connected PointToPointChannel.
   *
   * This is to intercept the same call from the PointToPointNetDevice
   * so that the pause messages are honoured without letting
   * PointToPointNetDevice::Receive(p) know
   *
   * @see PointToPointNetDevice
   * @param p Ptr to the received packet.
   */
  virtual void Receive (Ptr<Packet> p);

  /**
   * Send a packet to the channel by putting it to the queue
   * of the corresponding priority class
   *
   * @param packet Ptr to the packet to send
   * @param dest Unused
   * @param protocolNumber Protocol used in packet
   */
  virtual bool Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber);
  virtual bool SwitchSend (uint32_t qIndex, Ptr<Packet> packet, CustomHeader &ch);

  /**
   * Get the size of Tx buffer available in the device
   *
   * @return buffer available in bytes
   */
  //virtual uint32_t GetTxAvailable(unsigned) const;

  /**
   * TracedCallback hooks
   */
  void ConnectWithoutContext(const CallbackBase& callback);
  void DisconnectWithoutContext(const CallbackBase& callback);

  bool Attach (Ptr<QbbChannel> ch);

   virtual Ptr<Channel> GetChannel (void) const;

   void SetQueue (Ptr<BEgressQueue> q);
   Ptr<BEgressQueue> GetQueue ();
   virtual bool IsQbb(void) const;
   void NewQp(Ptr<RdmaQueuePair> qp);
   void ReassignedQp(Ptr<RdmaQueuePair> qp);
   void TriggerTransmit(void);

	void SendPfc(uint32_t qIndex, uint32_t type); // type: 0 = pause, 1 = resume
  void Sendspineinformation(uint32_t nodeid,std::vector<uint32_t> affectedDestinations);

	TracedCallback<Ptr<const Packet>, uint32_t> m_traceEnqueue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceDequeue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceDrop;
	TracedCallback<uint32_t> m_tracePfc; // 0: resume, 1: pause
protected:

	//Ptr<Node> m_node;

  bool TransmitStart (Ptr<Packet> p);
  
  virtual void DoDispose(void);

  /// Reset the channel into READY state and try transmit again
  virtual void TransmitComplete(void);

  /// Look for an available packet and send it using TransmitStart(p)
  virtual void DequeueAndTransmit(void);

  /// Resume a paused queue and call DequeueAndTransmit()
  virtual void Resume(unsigned qIndex);

  /**
   * The queues for each priority class.
   * @see class Queue
   * @see class InfiniteQueue
   */
  Ptr<BEgressQueue> m_queue;

  Ptr<QbbChannel> m_channel;
  
  //pfc
  bool m_qbbEnabled;	//< PFC behaviour enabled
  bool m_qcnEnabled;
  bool m_dynamicth;
  bool m_enableCongestionLabelCompression;
  uint32_t m_pausetime;	//< Time for each Pause
  bool m_paused[qCnt];	//< Whether a queue paused

  //qcn

  /* RP parameters */
  EventId  m_nextSend;		//< The next send event
  /* State variable for rate-limited queues */

  //qcn

  struct ECNAccount{
	  Ipv4Address source;
	  uint32_t qIndex;
	  uint32_t port;
	  uint8_t ecnbits;
	  uint16_t qfb;
	  uint16_t total;
  };

  std::vector<ECNAccount> *m_ecn_source;

public:
 
	
  std::map<uint32_t, std::vector<uint32_t>>Rxspineinformation;
  std::map<uint32_t, std::map<uint32_t, std::bitset<256>>> RxspineinformationCompressed;
  Ptr<RdmaEgressQueue> m_rdmaEQ;
	void RdmaEnqueueHighPrioQ(Ptr<Packet> p);
  std::vector<uint32_t> affectedDestinations;//受影响的ip地址
  std::map<uint32_t, std::vector<uint32_t>> nodeIDToAffectedDestinations;//交换机编号及其不可达目的ip
  std::map<uint32_t, std::vector<uint32_t>> getRxspineinformationCopy() const;//获取交换机编号和ip地址映射关系
  const std::map<uint32_t, std::map<uint32_t, std::bitset<256>>>& getRxspineinformationCompressed() const;
  bool IsDipBlockedByNodeCompressed(uint32_t nodeId, uint32_t dip) const;
  void addAffectedDestinations(uint32_t nodeId, const std::vector<uint32_t>& destinations);
  void uint32_to_bytes(uint32_t value, uint8_t* bytes);
  std::vector<uint8_t> BuildCongestionLabelPayload(uint32_t nodeid, const std::vector<uint32_t>& affectedDestinations) const;
  bool DecodeCongestionLabelPayload(const std::vector<uint8_t>& payload);
  static uint32_t bytes_to_uint32(const uint8_t* buffer);
  static uint16_t bytes_to_uint16(const uint8_t* buffer);
  uint64_t GetCongestionLabelTxBytes() const;
  uint64_t GetCongestionLabelPayloadBytes() const;
  uint64_t GetCongestionLabelTxCount() const;
  uint64_t GetCongestionLabelGenTxWallNs() const;
  uint64_t GetCongestionLabelRxWallNs() const;
  uint64_t GetCongestionLabelRxCount() const;
  

	// callback for processing packet in RDMA
	typedef Callback<int, Ptr<Packet>, CustomHeader&> RdmaReceiveCb;
	RdmaReceiveCb m_rdmaReceiveCb;
	// callback for link down
	typedef Callback<void, Ptr<QbbNetDevice> > RdmaLinkDownCb;
	RdmaLinkDownCb m_rdmaLinkDownCb;
	// callback for sent a packet
	typedef Callback<void, Ptr<RdmaQueuePair>, Ptr<Packet>, Time> RdmaPktSent;
	RdmaPktSent m_rdmaPktSent;

	Ptr<RdmaEgressQueue> GetRdmaQueue();
	void TakeDown(); // take down this device
	void UpdateNextAvail(Time t);

	TracedCallback<Ptr<const Packet>, Ptr<RdmaQueuePair> > m_traceQpDequeue; // the trace for printing dequeue

  uint64_t m_congestionLabelTxBytes;
  uint64_t m_congestionLabelPayloadBytes;
  uint64_t m_congestionLabelTxCount;
  uint64_t m_congestionLabelGenTxWallNs;
  uint64_t m_congestionLabelRxWallNs;
  uint64_t m_congestionLabelRxCount;
};

} // namespace ns3

#endif // QBB_NET_DEVICE_H
