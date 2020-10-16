// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "core/internal/mediums/webrtc/connection_flow.h"

#include <iterator>
#include <memory>

#include "core/internal/mediums/webrtc/session_description_wrapper.h"
#include "platform/public/logging.h"
#include "platform/public/mutex_lock.h"
#include "platform/public/webrtc.h"
#include "absl/memory/memory.h"
#include "absl/time/time.h"
#include "webrtc/api/data_channel_interface.h"
#include "webrtc/api/jsep.h"

namespace location {
namespace nearby {
namespace connections {
namespace mediums {

constexpr absl::Duration ConnectionFlow::kTimeout;

namespace {
// This is the same as the nearby data channel name.
const char kDataChannelName[] = "dataChannel";

class CreateSessionDescriptionObserverImpl
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateSessionDescriptionObserverImpl(
      Future<SessionDescriptionWrapper>* settable_future)
      : settable_future_(settable_future) {}
  ~CreateSessionDescriptionObserverImpl() override = default;

  // webrtc::CreateSessionDescriptionObserver
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    settable_future_->Set(SessionDescriptionWrapper{desc});
  }

  void OnFailure(webrtc::RTCError error) override {
    NEARBY_LOG(ERROR, "Error when creating session description: %s",
               error.message());
    settable_future_->SetException({Exception::kFailed});
  }

 private:
  std::unique_ptr<Future<SessionDescriptionWrapper>> settable_future_;
};

class SetSessionDescriptionObserverImpl
    : public webrtc::SetSessionDescriptionObserver {
 public:
  explicit SetSessionDescriptionObserverImpl(Future<bool>* settable_future)
      : settable_future_(settable_future) {}

  void OnSuccess() override { settable_future_->Set(true); }

  void OnFailure(webrtc::RTCError error) override {
    NEARBY_LOG(ERROR, "Error when setting session description: %s",
               error.message());
    settable_future_->SetException({Exception::kFailed});
  }

 private:
  std::unique_ptr<Future<bool>> settable_future_;
};

using PeerConnectionState =
    webrtc::PeerConnectionInterface::PeerConnectionState;

}  // namespace

std::unique_ptr<ConnectionFlow> ConnectionFlow::Create(
    LocalIceCandidateListener local_ice_candidate_listener,
    DataChannelListener data_channel_listener, WebRtcMedium& webrtc_medium) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::Create c1");
  auto connection_flow = absl::WrapUnique(
      new ConnectionFlow(std::move(local_ice_candidate_listener),
                         std::move(data_channel_listener)));
  if (connection_flow->InitPeerConnection(webrtc_medium)) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::Create c2");
    return connection_flow;
  }

  NEARBY_LOG(INFO, "GGG in ConnectionFlow::Create c3");
  return nullptr;
}

ConnectionFlow::ConnectionFlow(
    LocalIceCandidateListener local_ice_candidate_listener,
    DataChannelListener data_channel_listener)
    : data_channel_listener_(std::move(data_channel_listener)),
      peer_connection_observer_(this, std::move(local_ice_candidate_listener)) {
}

ConnectionFlow::~ConnectionFlow() { Close(); }

SessionDescriptionWrapper ConnectionFlow::CreateOffer() {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateOffer c1");
  MutexLock lock(&mutex_);

  if (!TransitionState(State::kInitialized, State::kCreatingOffer)) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateOffer c2");
    return SessionDescriptionWrapper();
  }

  webrtc::DataChannelInit data_channel_init;
  data_channel_init.reliable = true;
  rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel =
      peer_connection_->CreateDataChannel(kDataChannelName, &data_channel_init);
  data_channel->RegisterObserver(CreateDataChannelObserver(data_channel));

  auto success_future = new Future<SessionDescriptionWrapper>();
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer =
      new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>(
          success_future);
  peer_connection_->CreateOffer(observer, options);

  ExceptionOr<SessionDescriptionWrapper> result = success_future->Get(kTimeout);
  if (result.ok() &&
      TransitionState(State::kCreatingOffer, State::kWaitingForAnswer)) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateOffer c3");
    return std::move(result.result());
  }

  NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateOffer c4");
  return SessionDescriptionWrapper();
}

SessionDescriptionWrapper ConnectionFlow::CreateAnswer() {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateAnswer c1");
  MutexLock lock(&mutex_);

  if (!TransitionState(State::kReceivedOffer, State::kCreatingAnswer)) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateAnswer c2");
    return SessionDescriptionWrapper();
  }

  auto success_future = new Future<SessionDescriptionWrapper>();
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer =
      new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>(
          success_future);
  peer_connection_->CreateAnswer(observer, options);

  ExceptionOr<SessionDescriptionWrapper> result = success_future->Get(kTimeout);
  if (result.ok() &&
      TransitionState(State::kCreatingAnswer, State::kWaitingToConnect)) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateAnswer c3");
    return std::move(result.result());
  }

  NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateAnswer c4");
  return SessionDescriptionWrapper();
}

bool ConnectionFlow::SetLocalSessionDescription(SessionDescriptionWrapper sdp) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::SetLocalSessionDescription c1");
  MutexLock lock(&mutex_);

  if (!sdp.IsValid()) return false;

  auto success_future = new Future<bool>();
  rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer =
      new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>(
          success_future);

  NEARBY_LOG(INFO, "GGG in ConnectionFlow::SetLocalSessionDescription c2");
  peer_connection_->SetLocalDescription(observer, sdp.Release());

  ExceptionOr<bool> result = success_future->Get(kTimeout);
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::SetLocalSessionDescription c3");
  return result.ok() && result.result();
}

bool ConnectionFlow::SetRemoteSessionDescription(
    SessionDescriptionWrapper sdp) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::SetRemoteSessionDescription c1");
  if (!sdp.IsValid())
  {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::SetRemoteSessionDescription c2");
    return false;
  }

  auto success_future = new Future<bool>();
  rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer =
      new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>(
          success_future);

  peer_connection_->SetRemoteDescription(observer, sdp.Release());

  ExceptionOr<bool> result = success_future->Get(kTimeout);
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::SetRemoteSessionDescription c3");
  return result.ok() && result.result();
}

bool ConnectionFlow::OnOfferReceived(SessionDescriptionWrapper offer) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnOfferReceived c1");
  MutexLock lock(&mutex_);

  if (!TransitionState(State::kInitialized, State::kReceivedOffer)) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnOfferReceived c2");
    return false;
  }
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnOfferReceived c3");
  return SetRemoteSessionDescription(std::move(offer));
}

bool ConnectionFlow::OnAnswerReceived(SessionDescriptionWrapper answer) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnAnswerReceived c1");
  MutexLock lock(&mutex_);

  if (!TransitionState(State::kWaitingForAnswer, State::kWaitingToConnect)) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnAnswerReceived c2");
    return false;
  }
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnAnswerReceived c3");
  return SetRemoteSessionDescription(std::move(answer));
}

bool ConnectionFlow::OnRemoteIceCandidatesReceived(
    std::vector<std::unique_ptr<webrtc::IceCandidateInterface>>
        ice_candidates) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnRemoteIceCandidatesReceived c1");
  MutexLock lock(&mutex_);

  if (state_ == State::kEnded) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnRemoteIceCandidatesReceived c2");
    NEARBY_LOG(WARNING,
               "You cannot add ice candidates to a disconnected session.");
    return false;
  }

  if (state_ != State::kWaitingToConnect && state_ != State::kConnected) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnRemoteIceCandidatesReceived c3");
    cached_remote_ice_candidates_.insert(
        cached_remote_ice_candidates_.end(),
        std::make_move_iterator(ice_candidates.begin()),
        std::make_move_iterator(ice_candidates.end()));
    return true;
  }

  for (auto&& ice_candidate : ice_candidates) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnRemoteIceCandidatesReceived c4");
    if (!peer_connection_->AddIceCandidate(ice_candidate.get())) {
      NEARBY_LOG(WARNING, "Unable to add remote ice candidate.");
    }
  }
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnRemoteIceCandidatesReceived c5");
  return true;
}

Future<rtc::scoped_refptr<webrtc::DataChannelInterface>>
ConnectionFlow::GetDataChannel() {
  return data_channel_future_;
}

bool ConnectionFlow::Close() {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::Close c1");
  MutexLock lock(&mutex_);
  return CloseLocked();
}

bool ConnectionFlow::InitPeerConnection(WebRtcMedium& webrtc_medium) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::InitPeerConnection c1");
  Future<bool> success_future;
  // CreatePeerConnection callback may be invoked after ConnectionFlow lifetime
  // has ended, in case of a timeout. Future is captured by value, and is safe
  // to access, but it is not safe to access ConnectionFlow member variables
  // unless the Future::Set() returns true.
  webrtc_medium.CreatePeerConnection(
      &peer_connection_observer_,
      [this, success_future](rtc::scoped_refptr<webrtc::PeerConnectionInterface>
                                 peer_connection) mutable {
        if (!peer_connection) {
          NEARBY_LOG(INFO, "GGG in ConnectionFlow::InitPeerConnection c2");
          success_future.Set(false);
          return;
        }

        // If this fails, means we have already assigned something to
        // success_future; it is either:
        // 1) this is the 2nd call of this callback (and this is a bug), or
        // 2) Get(timeout) has set the future value as exception already.
        if (success_future.IsSet()) 
        {
          NEARBY_LOG(INFO, "GGG in ConnectionFlow::InitPeerConnection c3");
          return;
        }
        peer_connection_ = peer_connection;
        success_future.Set(true);
      });

  NEARBY_LOG(INFO, "GGG in ConnectionFlow::InitPeerConnection c300");
  ExceptionOr<bool> result = success_future.Get(kTimeout);
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::InitPeerConnection c4");
  return result.ok() && result.result();
}

void ConnectionFlow::OnSignalingStable() {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::OnSignalingStable c1");
  MutexLock lock(&mutex_);

  if (state_ != State::kWaitingToConnect && state_ != State::kConnected) return;

  for (auto&& ice_candidate : cached_remote_ice_candidates_) {
    if (!peer_connection_->AddIceCandidate(ice_candidate.get())) {
      NEARBY_LOG(WARNING, "Unable to add remote ice candidate.");
    }
  }
  cached_remote_ice_candidates_.clear();
}

void ConnectionFlow::ProcessOnPeerConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::ProcessOnPeerConnectionChange c1");
  if (new_state == PeerConnectionState::kClosed ||
      new_state == PeerConnectionState::kFailed ||
      new_state == PeerConnectionState::kDisconnected) {
    MutexLock lock(&mutex_);
    CloseAndNotifyLocked();
  }
}

void ConnectionFlow::ProcessDataChannelConnected() {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::ProcessDataChannelConnected c1");
  MutexLock lock(&mutex_);
  NEARBY_LOG(INFO, "Data channel state changed to connected.");
  if (!TransitionState(State::kWaitingToConnect, State::kConnected))
    CloseAndNotifyLocked();
}

webrtc::DataChannelObserver* ConnectionFlow::CreateDataChannelObserver(
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::CreateDataChannelObserver c1");
  if (!data_channel_observer_) {
    auto state_change_callback = [this,
                                  data_channel{std::move(data_channel)}]() {
      if (data_channel->state() ==
          webrtc::DataChannelInterface::DataState::kOpen) {
        data_channel_future_.Set(std::move(data_channel));
        OffloadFromSignalingThread([this]() { ProcessDataChannelConnected(); });
      } else if (data_channel->state() ==
                 webrtc::DataChannelInterface::DataState::kClosed) {
        data_channel->UnregisterObserver();
        OffloadFromSignalingThread([this]() {
          MutexLock lock(&mutex_);
          CloseAndNotifyLocked();
        });
      }
    };
    data_channel_observer_ = absl::make_unique<DataChannelObserverImpl>(
        &data_channel_listener_, std::move(state_change_callback));
  }

  return reinterpret_cast<webrtc::DataChannelObserver*>(
      data_channel_observer_.get());
}

bool ConnectionFlow::TransitionState(State current_state, State new_state) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::TransitionState c1");
  if (current_state != state_) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::TransitionState c2");
    NEARBY_LOG(
        WARNING,
        "Invalid state transition to %d: current state is %d but expected %d.",
        new_state, state_, current_state);
    return false;
  }
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::TransitionState c3");
  state_ = new_state;
  return true;
}

void ConnectionFlow::CloseAndNotifyLocked() {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::CloseAndNotifyLocked c1");
  if (CloseLocked()) {
    data_channel_listener_.data_channel_closed_cb();
  }
}

bool ConnectionFlow::CloseLocked() {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::CloseLocked c1");
  if (state_ == State::kEnded) {
    NEARBY_LOG(INFO, "GGG in ConnectionFlow::CloseLocked c2");
    return false;
  }
  state_ = State::kEnded;

  data_channel_future_.SetException({Exception::kInterrupted});
  if (peer_connection_) peer_connection_->Close();

  data_channel_observer_.reset();
  NEARBY_LOG(INFO, "Closed WebRTC connection.");
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::CloseLocked c3");
  return true;
}

void ConnectionFlow::OffloadFromSignalingThread(Runnable runnable) {
  NEARBY_LOG(INFO, "GGG in ConnectionFlow::OffloadFromSignalingThread c1");
  single_threaded_signaling_offloader_.Execute(std::move(runnable));
}

}  // namespace mediums
}  // namespace connections
}  // namespace nearby
}  // namespace location
