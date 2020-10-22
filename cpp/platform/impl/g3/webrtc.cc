#include "platform/impl/g3/webrtc.h"

#include <memory>

#include "platform/base/medium_environment.h"
#include "webrtc/api/task_queue/default_task_queue_factory.h"

namespace location {
namespace nearby {
namespace g3 {

WebRtcSignalingMessenger::WebRtcSignalingMessenger(
    absl::string_view self_id, const connections::LocationHint& location_hint)
    : self_id_(self_id), location_hint_(location_hint) {}

bool WebRtcSignalingMessenger::SendMessage(absl::string_view peer_id,
                                           const ByteArray& message) {
  auto& env = MediumEnvironment::Instance();
  env.SendWebRtcSignalingMessage(peer_id, message);
  return true;
}

bool WebRtcSignalingMessenger::StartReceivingMessages(
    OnSignalingMessageCallback listener) {
  auto& env = MediumEnvironment::Instance();
  env.RegisterWebRtcSignalingMessenger(self_id_, listener);
  return true;
}

void WebRtcSignalingMessenger::StopReceivingMessages() {
  auto& env = MediumEnvironment::Instance();
  env.UnregisterWebRtcSignalingMessenger(self_id_);
}

void WebRtcMedium::CreatePeerConnection(
    webrtc::PeerConnectionObserver* observer, PeerConnectionCallback callback) {
  auto& env = MediumEnvironment::Instance();
  if (!env.GetUseValidPeerConnection()) {
    callback(nullptr);
    return;
  }

  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
  webrtc::PeerConnectionDependencies dependencies(observer);

  signaling_thread_ = rtc::Thread::Create();
  signaling_thread_->SetName("signaling_thread", nullptr);
  RTC_CHECK(signaling_thread_->Start()) << "Failed to start thread";

  webrtc::PeerConnectionFactoryDependencies factory_dependencies;
  factory_dependencies.task_queue_factory =
      webrtc::CreateDefaultTaskQueueFactory();
  factory_dependencies.signaling_thread = signaling_thread_.get();

  callback(webrtc::CreateModularPeerConnectionFactory(
               std::move(factory_dependencies))
               ->CreatePeerConnection(rtc_config, std::move(dependencies)));
}

std::unique_ptr<api::WebRtcSignalingMessenger>
WebRtcMedium::GetSignalingMessenger(
    absl::string_view self_id, const connections::LocationHint& location_hint) {
  return std::make_unique<WebRtcSignalingMessenger>(self_id, location_hint);
}

}  // namespace g3
}  // namespace nearby
}  // namespace location