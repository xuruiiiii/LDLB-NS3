#include <iostream>
#include <fstream>
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/object-vector.h"
#include "ns3/uinteger.h"
#include "ns3/log.h"
#include "ns3/assert.h"
#include "ns3/global-value.h"
#include "ns3/boolean.h"
#include "ns3/simulator.h"
#include "ns3/random-variable.h"
#include "switch-mmu.h"
#include <unordered_map> 
#include <set>
#include <vector> 

NS_LOG_COMPONENT_DEFINE("SwitchMmu");
namespace ns3 {
	TypeId SwitchMmu::GetTypeId(void){
		static TypeId tid = TypeId("ns3::SwitchMmu")
			.SetParent<Object>()
			.AddConstructor<SwitchMmu>();

		return tid;
	}

	SwitchMmu::SwitchMmu(void){
		buffer_size = 12 * 1024 * 1024;
		reserve = 4 * 1024;
		resume_offset = 3 * 1024;
		ewma_alpha = 0.25;
		enter_required_samples = 2;
		exit_required_samples = 3;
		enable_stability_guard = true;
		

		// headroom
		shared_used_bytes = 0;
		memset(hdrm_bytes, 0, sizeof(hdrm_bytes));
		memset(ingress_bytes, 0, sizeof(ingress_bytes));
		memset(paused, 0, sizeof(paused));
		memset(egress_bytes, 0, sizeof(egress_bytes));
		memset(egress_ewma, 0, sizeof(egress_ewma));
		memset(enter_counter, 0, sizeof(enter_counter));
		memset(exit_counter, 0, sizeof(exit_counter));
	}
	


		



	bool SwitchMmu::CheckIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		if (psize + hdrm_bytes[port][qIndex] > headroom[port] && psize + GetSharedUsed(port, qIndex) > GetPfcThreshold(port)){
			printf("%lu %u Drop: queue:%u,%u: Headroom full\n", Simulator::Now().GetTimeStep(), node_id, port, qIndex);
			for (uint32_t i = 1; i < 64; i++)
				printf("(%u,%u)", hdrm_bytes[i][3], ingress_bytes[i][3]);
			printf("\n");
			return false;
		}
		return true;
	}
	bool SwitchMmu::CheckEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		return true;
	}
	void SwitchMmu::UpdateIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		uint32_t new_bytes = ingress_bytes[port][qIndex] + psize;
		if (new_bytes <= reserve){
			ingress_bytes[port][qIndex] += psize;
		}else {
			uint32_t thresh = GetPfcThreshold(port);
			if (new_bytes - reserve > thresh){
				hdrm_bytes[port][qIndex] += psize;
			}else {
				ingress_bytes[port][qIndex] += psize;
				shared_used_bytes += std::min(psize, new_bytes - reserve);
			}
		}
	}
	void SwitchMmu::UpdateEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		egress_bytes[port][qIndex] += psize;
	}
	void SwitchMmu::RemoveFromIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		uint32_t from_hdrm = std::min(hdrm_bytes[port][qIndex], psize);
		uint32_t from_shared = std::min(psize - from_hdrm, ingress_bytes[port][qIndex] > reserve ? ingress_bytes[port][qIndex] - reserve : 0);
		hdrm_bytes[port][qIndex] -= from_hdrm;
		ingress_bytes[port][qIndex] -= psize - from_hdrm;
		shared_used_bytes -= from_shared;
	}
	void SwitchMmu::RemoveFromEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		egress_bytes[port][qIndex] -= psize;
	}
	bool SwitchMmu::CheckShouldPause(uint32_t port, uint32_t qIndex){
		return !paused[port][qIndex] && (hdrm_bytes[port][qIndex] > 0 || GetSharedUsed(port, qIndex) >= GetPfcThreshold(port));
	}
	bool SwitchMmu::CheckShouldResume(uint32_t port, uint32_t qIndex){
		if (!paused[port][qIndex])
			return false;
		uint32_t shared_used = GetSharedUsed(port, qIndex);
		return hdrm_bytes[port][qIndex] == 0 && (shared_used == 0 || shared_used + resume_offset <= GetPfcThreshold(port));
	}
	void SwitchMmu::SetPause(uint32_t port, uint32_t qIndex){
		paused[port][qIndex] = true;
	}
	void SwitchMmu::SetResume(uint32_t port, uint32_t qIndex){
		paused[port][qIndex] = false;
	}

	uint32_t SwitchMmu::GetPfcThreshold(uint32_t port){
		return (buffer_size - total_hdrm - total_rsrv - shared_used_bytes) >> pfc_a_shift[port];
	}
	uint32_t SwitchMmu::GetSharedUsed(uint32_t port, uint32_t qIndex){
		uint32_t used = ingress_bytes[port][qIndex];
		return used > reserve ? used - reserve : 0;
	}
	bool SwitchMmu::ShouldSendCN(uint32_t ifindex, uint32_t qIndex) {
		if (qIndex == 0)
			return false;
		if (egress_bytes[ifindex][qIndex] > kmax[ifindex])
			return true;
		if (egress_bytes[ifindex][qIndex] > kmin[ifindex]) {
			double p = pmax[ifindex] * double(egress_bytes[ifindex][qIndex] - kmin[ifindex]) / (kmax[ifindex] - kmin[ifindex]);
			if (UniformVariable(0, 1).GetValue() < p)
				return true;
		}
		return false;
	}

	uint32_t SwitchMmu::GetEgressQueueBytes(uint32_t port, uint32_t qIndex) const {
		if (port >= pCnt || qIndex >= qCnt) {
			return 0;
		}
		return egress_bytes[port][qIndex];
	}

	const std::vector<int>& SwitchMmu::GetCongestedLabels() const {
		return congested_if_indices;
	}

	void SwitchMmu::SetSwitchLabel(uint32_t ifindex, uint32_t qIndex) {
		if (!enable_stability_guard) {
			if (egress_bytes[ifindex][qIndex] > kmax[ifindex]) {
				congestedlabel[ifindex] = true;
			}
			else if (egress_bytes[ifindex][qIndex] <= kmin[ifindex]) {
				congestedlabel.erase(ifindex);
			}
			else if (egress_bytes[ifindex][qIndex] > kmin[ifindex]) {
				double p = pmax[ifindex] * double(egress_bytes[ifindex][qIndex] - kmin[ifindex]) / (kmax[ifindex] - kmin[ifindex]);
				if (UniformVariable(0, 1).GetValue() < p) {
					congestedlabel[ifindex] = true;
				}
			}

			bool nowCongestedRaw = congestedlabel.find(ifindex) != congestedlabel.end();
			auto itRaw = std::find(congested_if_indices.begin(), congested_if_indices.end(), ifindex);
			if (nowCongestedRaw) {
				if (itRaw == congested_if_indices.end()) {
					congested_if_indices.push_back(ifindex);
				}
			}
			else {
				if (itRaw != congested_if_indices.end()) {
					congested_if_indices.erase(itRaw);
				}
			}
			return;
		}
		// 对瞬时队列占用进行 EWMA 平滑，避免阈值边界抖动导致状态频繁翻转。
		double sample = static_cast<double>(egress_bytes[ifindex][qIndex]);
		egress_ewma[ifindex][qIndex] = (1.0 - ewma_alpha) * egress_ewma[ifindex][qIndex] + ewma_alpha * sample;

		bool isCongested = congestedlabel.find(ifindex) != congestedlabel.end();
		double q = egress_ewma[ifindex][qIndex];

		if (!isCongested) {
			if (q > kmax[ifindex]) {
				// 超过高阈值：累计进入计数，达到门限后再置位。
				enter_counter[ifindex][qIndex]++;
				exit_counter[ifindex][qIndex] = 0;
				if (enter_counter[ifindex][qIndex] >= enter_required_samples) {
					congestedlabel[ifindex] = true;
					enter_counter[ifindex][qIndex] = 0;
				}
			}
			else if (q > kmin[ifindex]) {
				// 在 kmin~kmax 区间内概率触发，但仍要求连续样本确认。
				double p = pmax[ifindex] * (q - kmin[ifindex]) / (kmax[ifindex] - kmin[ifindex]);
				if (UniformVariable(0, 1).GetValue() < p) {
					enter_counter[ifindex][qIndex]++;
					if (enter_counter[ifindex][qIndex] >= enter_required_samples) {
						congestedlabel[ifindex] = true;
						enter_counter[ifindex][qIndex] = 0;
					}
				}
				else {
					enter_counter[ifindex][qIndex] = 0;
				}
				exit_counter[ifindex][qIndex] = 0;
			}
			else {
				// 低于低阈值，复位进入计数。
				enter_counter[ifindex][qIndex] = 0;
				exit_counter[ifindex][qIndex] = 0;
			}
		}
		else {
			if (q <= kmin[ifindex]) {
				// 低于低阈值：累计退出计数，达到门限后再清除。
				exit_counter[ifindex][qIndex]++;
				enter_counter[ifindex][qIndex] = 0;
				if (exit_counter[ifindex][qIndex] >= exit_required_samples) {
					congestedlabel.erase(ifindex);
					exit_counter[ifindex][qIndex] = 0;
				}
			}
			else {
				// 仍处于拥塞或回差区，保持状态。
				exit_counter[ifindex][qIndex] = 0;
			}
		}
		// 根据 congestedlabel 的结果同步更新拥塞接口列表。
		bool nowCongested = congestedlabel.find(ifindex) != congestedlabel.end();
		auto it = std::find(congested_if_indices.begin(), congested_if_indices.end(), ifindex);
		if (nowCongested) {
			if (it == congested_if_indices.end()) {
				// 如果当前接口被标记为拥塞，但列表中还没有，则加入列表。
				congested_if_indices.push_back(ifindex);
			}
			// 如果已经存在，就不需要重复添加。
		}
		else {
			if (it != congested_if_indices.end()) {
				// 如果当前接口不再拥塞，但列表中仍存在，则移除。
				congested_if_indices.erase(it);
			}
			// 如果列表中本来就没有，则无需处理。
		}

	}
	


	void SwitchMmu::ConfigEcn(uint32_t port, uint32_t _kmin, uint32_t _kmax, double _pmax){
		kmin[port] = _kmin * 1000;
		kmax[port] = _kmax * 1000;
		pmax[port] = _pmax;
	}
	void SwitchMmu::ConfigHdrm(uint32_t port, uint32_t size){
		headroom[port] = size;
	}
	void SwitchMmu::ConfigNPort(uint32_t n_port){
		total_hdrm = 0;
		total_rsrv = 0;
		for (uint32_t i = 1; i <= n_port; i++){
			total_hdrm += headroom[i];
			total_rsrv += reserve;
		}
	}
	void SwitchMmu::ConfigBufferSize(uint32_t size){
		buffer_size = size;
	}
}
