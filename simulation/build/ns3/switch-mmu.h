#ifndef SWITCH_MMU_H
#define SWITCH_MMU_H

#include <unordered_map>
#include <ns3/node.h>
#include <set>
#include <vector>  
#include <iostream>


namespace ns3 {

class Packet;

class SwitchMmu: public Object{
public:
	static const uint32_t pCnt = 257;	// Number of ports used
	static const uint32_t qCnt = 8;	// Number of queues/priorities used

	static TypeId GetTypeId (void);

	SwitchMmu(void);

	bool CheckIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
	bool CheckEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
	void UpdateIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
	void UpdateEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
	void RemoveFromIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
	void RemoveFromEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);

	bool CheckShouldPause(uint32_t port, uint32_t qIndex);
	bool CheckShouldResume(uint32_t port, uint32_t qIndex);
	void SetPause(uint32_t port, uint32_t qIndex);
	void SetResume(uint32_t port, uint32_t qIndex);
	//void GetPauseClasses(uint32_t port, uint32_t qIndex);
	//bool GetResumeClasses(uint32_t port, uint32_t qIndex);

	uint32_t GetPfcThreshold(uint32_t port);
	uint32_t GetSharedUsed(uint32_t port, uint32_t qIndex);

	bool ShouldSendCN(uint32_t ifindex, uint32_t qIndex);
    void SetSwitchLabel(uint32_t ifindex, uint32_t qIndex);
	uint32_t GetEgressQueueBytes(uint32_t port, uint32_t qIndex) const;
	
	void ConfigEcn(uint32_t port, uint32_t _kmin, uint32_t _kmax, double _pmax);
	void ConfigHdrm(uint32_t port, uint32_t size);
	void ConfigNPort(uint32_t n_port);
	void ConfigBufferSize(uint32_t size);

	// config
	uint32_t node_id;
	uint32_t buffer_size;
	uint32_t pfc_a_shift[pCnt];
	uint32_t reserve;
	uint32_t headroom[pCnt];
	uint32_t resume_offset;
	uint32_t kmin[pCnt], kmax[pCnt];
	double pmax[pCnt];
	uint32_t total_hdrm;
	uint32_t total_rsrv;
	bool enable_stability_guard;

	// runtime
	uint32_t shared_used_bytes;
	uint32_t hdrm_bytes[pCnt][qCnt];
	uint32_t ingress_bytes[pCnt][qCnt];
	uint32_t paused[pCnt][qCnt];
	uint32_t egress_bytes[pCnt][qCnt];
	double egress_ewma[pCnt][qCnt];
	uint8_t enter_counter[pCnt][qCnt];
	uint8_t exit_counter[pCnt][qCnt];

	// stability safeguards for congestion state update
	double ewma_alpha;
	uint8_t enter_required_samples;
	uint8_t exit_required_samples;

	
	const std::vector<int>& GetCongestedLabels() const;

private:
	std::unordered_map<int, bool> congestedlabel;
	std::vector<int> congested_if_indices;
};

} /* namespace ns3 */

#endif /* SWITCH_MMU_H */

