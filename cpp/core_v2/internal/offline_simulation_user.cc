#include "core_v2/internal/offline_simulation_user.h"

#include "core_v2/listeners.h"
#include "platform_v2/public/count_down_latch.h"
#include "platform_v2/public/system_clock.h"
#include "absl/functional/bind_front.h"

namespace location {
namespace nearby {
namespace connections {

void OfflineSimulationUser::OnConnectionInitiated(
    const std::string& endpoint_id, const ConnectionResponseInfo& info,
    bool is_outgoing) {
  if (is_outgoing) {
    NEARBY_LOG(INFO, "RequestConnection: initiated_cb called");
  } else {
    NEARBY_LOG(INFO, "StartAdvertising: initiated_cb called");
    discovered_ = DiscoveredInfo{
        .endpoint_id = endpoint_id,
        .endpoint_name = name_,
        .service_id = service_id_,
    };
  }
  if (initiated_latch_) initiated_latch_->CountDown();
}

void OfflineSimulationUser::OnConnectionAccepted(
    const std::string& endpoint_id) {
  if (accept_latch_) accept_latch_->CountDown();
}

void OfflineSimulationUser::OnConnectionRejected(const std::string& endpoint_id,
                                                 Status status) {
  if (reject_latch_) reject_latch_->CountDown();
}

void OfflineSimulationUser::OnEndpointDisconnect(
    const std::string& endpoint_id) {
  NEARBY_LOGS(INFO) << "OnEndpointDisconnect: self=" << this
                    << "; id=" << endpoint_id;
  if (disconnect_latch_) disconnect_latch_->CountDown();
}

void OfflineSimulationUser::OnEndpointFound(const std::string& endpoint_id,
                                            const std::string& endpoint_name,
                                            const std::string& service_id) {
  NEARBY_LOG(INFO, "Device discovered: id=%s", endpoint_id.c_str());
  discovered_ = DiscoveredInfo{
      .endpoint_id = endpoint_id,
      .endpoint_name = endpoint_name,
      .service_id = service_id,
  };
  if (found_latch_) found_latch_->CountDown();
}

void OfflineSimulationUser::OnEndpointLost(const std::string& endpoint_id) {
  if (lost_latch_) lost_latch_->CountDown();
}

void OfflineSimulationUser::OnPayload(const std::string& endpoint_id,
                                      Payload payload) {
  payload_ = std::move(payload);
  if (payload_latch_) payload_latch_->CountDown();
}

void OfflineSimulationUser::OnPayloadProgress(const std::string& endpoint_id,
                                              const PayloadProgressInfo& info) {
  MutexLock lock(&progress_mutex_);
  progress_info_ = info;
  if (future_ && predicate_ && predicate_(info)) future_->Set(true);
}

bool OfflineSimulationUser::WaitForProgress(
    std::function<bool(const PayloadProgressInfo&)> predicate,
    absl::Duration timeout) {
  Future<bool> future;
  {
    MutexLock lock(&progress_mutex_);
    if (predicate(progress_info_)) return true;
    future_ = &future;
    predicate_ = std::move(predicate);
  }
  auto response = future.Get(timeout);
  {
    MutexLock lock(&progress_mutex_);
    future_ = nullptr;
    predicate_ = nullptr;
  }
  return response.ok() && response.result();
}

Status OfflineSimulationUser::StartAdvertising(const std::string& service_id,
                                               CountDownLatch* latch) {
  initiated_latch_ = latch;
  service_id_ = service_id;
  ConnectionListener listener = {
      .initiated_cb =
          std::bind(&OfflineSimulationUser::OnConnectionInitiated, this,
                    std::placeholders::_1, std::placeholders::_2, false),
      .accepted_cb =
          absl::bind_front(&OfflineSimulationUser::OnConnectionAccepted, this),
      .rejected_cb =
          absl::bind_front(&OfflineSimulationUser::OnConnectionRejected, this),
      .disconnected_cb =
          absl::bind_front(&OfflineSimulationUser::OnEndpointDisconnect, this),
  };
  return ctrl_.StartAdvertising(&client_, service_id_, options_,
                                {
                                    .name = name_,
                                    .listener = std::move(listener),
                                });
}

void OfflineSimulationUser::StopAdvertising() {
  ctrl_.StopAdvertising(&client_);
}

Status OfflineSimulationUser::StartDiscovery(const std::string& service_id,
                                             CountDownLatch* latch) {
  found_latch_ = latch;
  DiscoveryListener listener = {
      .endpoint_found_cb =
          absl::bind_front(&OfflineSimulationUser::OnEndpointFound, this),
      .endpoint_lost_cb =
          absl::bind_front(&OfflineSimulationUser::OnEndpointLost, this),
  };
  return ctrl_.StartDiscovery(&client_, service_id, options_,
                              std::move(listener));
}

void OfflineSimulationUser::StopDiscovery() { ctrl_.StopDiscovery(&client_); }

Status OfflineSimulationUser::RequestConnection(CountDownLatch* latch) {
  initiated_latch_ = latch;
  ConnectionListener listener = {
      .initiated_cb =
          std::bind(&OfflineSimulationUser::OnConnectionInitiated, this,
                    std::placeholders::_1, std::placeholders::_2, true),
      .accepted_cb =
          absl::bind_front(&OfflineSimulationUser::OnConnectionAccepted, this),
      .rejected_cb =
          absl::bind_front(&OfflineSimulationUser::OnConnectionRejected, this),
      .disconnected_cb =
          absl::bind_front(&OfflineSimulationUser::OnEndpointDisconnect, this),
  };
  return ctrl_.RequestConnection(&client_, discovered_.endpoint_id,
                                 {
                                     .name = discovered_.endpoint_name,
                                     .listener = std::move(listener),
                                 });
}

Status OfflineSimulationUser::AcceptConnection(CountDownLatch* latch) {
  accept_latch_ = latch;
  PayloadListener listener = {
      .payload_cb = absl::bind_front(&OfflineSimulationUser::OnPayload, this),
      .payload_progress_cb =
          absl::bind_front(&OfflineSimulationUser::OnPayloadProgress, this),
  };
  return ctrl_.AcceptConnection(&client_, discovered_.endpoint_id,
                                std::move(listener));
}

Status OfflineSimulationUser::RejectConnection(CountDownLatch* latch) {
  reject_latch_ = latch;
  return ctrl_.RejectConnection(&client_, discovered_.endpoint_id);
}

void OfflineSimulationUser::Disconnect() {
  NEARBY_LOGS(INFO) << "Disconnecting from id=" << discovered_.endpoint_id;
  ctrl_.DisconnectFromEndpoint(&client_, discovered_.endpoint_id);
}

}  // namespace connections
}  // namespace nearby
}  // namespace location