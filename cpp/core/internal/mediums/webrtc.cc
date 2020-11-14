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

#include "core/internal/mediums/webrtc.h"

#include <functional>
#include <memory>
#include <thread>

#include "core/internal/mediums/webrtc/session_description_wrapper.h"
#include "core/internal/mediums/webrtc/signaling_frames.h"
#include "platform/base/byte_array.h"
#include "platform/base/listeners.h"
#include "platform/public/cancelable_alarm.h"
#include "platform/public/future.h"
#include "platform/public/logging.h"
#include "platform/public/mutex_lock.h"
#include "location/nearby/mediums/proto/web_rtc_signaling_frames.pb.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "webrtc/api/jsep.h"

namespace location {
namespace nearby {
namespace connections {
namespace mediums {

namespace {

// The maximum amount of time to wait to connect to a data channel via WebRTC.
constexpr absl::Duration kDataChannelTimeout = absl::Milliseconds(5000);

// Delay between restarting signaling messenger to receive messages.
constexpr absl::Duration kRestartReceiveMessagesDuration = absl::Seconds(60);

}  // namespace

WebRtc::WebRtc() = default;

WebRtc::~WebRtc() {
  // This ensures that all pending callbacks are run before we reset the medium
  // and we are not accepting new runnables.
  restart_receive_messages_executor_.Shutdown();
  single_thread_executor_.Shutdown();

  Disconnect();
}

const std::string WebRtc::GetDefaultCountryCode() {
  return medium_.GetDefaultCountryCode();
}

bool WebRtc::IsAvailable() { return medium_.IsValid(); }

bool WebRtc::IsAcceptingConnections(const std::string& service_id) {
  MutexLock lock(&mutex_);
  // TODO(hais): refractor the implementation with maps.
  return role_ == Role::kOfferer;
}

bool WebRtc::StartAcceptingConnections(const PeerId& self_id,
                                       const std::string& service_id,
                                       const LocationHint& location_hint,
                                       AcceptedConnectionCallback callback) {
  std::stringstream ss;
  ss << std::this_thread::get_id();
  NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w1, tid: %s", ss.str().c_str());
  if (!IsAvailable()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w2");
    {
      MutexLock lock(&mutex_);
      LogAndDisconnect("WebRTC is not available for data transfer.");
    }
    return false;
  }

  if (IsAcceptingConnections(service_id)) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w3");
    NEARBY_LOG(WARNING, "Already accepting WebRTC connections.");
    return false;
  }

  {
    MutexLock lock(&mutex_);
    if (role_ != Role::kNone) {
      NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w4");
      NEARBY_LOG(WARNING,
                 "Cannot start accepting WebRTC connections, current role %d",
                 role_);
      return false;
    }

    if (!InitWebRtcFlow(Role::kOfferer, self_id, location_hint))
    {
      NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w5");
      return false;
    }

    restart_receive_messages_alarm_ = CancelableAlarm(
        "restart_receiving_messages_webrtc",
        std::bind(&WebRtc::RestartReceiveMessages, this, location_hint,
                  service_id),
        kRestartReceiveMessagesDuration, &restart_receive_messages_executor_);

    NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w6");
    SessionDescriptionWrapper offer = connection_flow_->CreateOffer();
    NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w7");
    pending_local_offer_ = webrtc_frames::EncodeOffer(self_id, offer.GetSdp());
    if (!SetLocalSessionDescription(std::move(offer))) {
      NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w8");
      return false;
    }

    NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w9");
    // There is no timeout set for the future returned since we do not know how
    // much time it will take for the two devices to discover each other before
    // the actual transport can begin.
    ListenForWebRtcSocketFuture(connection_flow_->GetDataChannel(),
                                std::move(callback));
    NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w10");
    NEARBY_LOG(ERROR, "Started listening for WebRtc connections as %s",
               self_id.GetId().c_str());
  }

  NEARBY_LOG(ERROR, "GGG in WebRtc::StartAcceptingConnections w11");
  return true;
}

WebRtcSocketWrapper WebRtc::Connect(const PeerId& peer_id,
                                    const LocationHint& location_hint) {
  NEARBY_LOG(INFO, "GGG in WebRtc::Connect w1: %s", peer_id.GetId().c_str());
  if (!IsAvailable()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::Connect w2");
    Disconnect();
    return WebRtcSocketWrapper();
  }

  NEARBY_LOG(ERROR, "GGG in WebRtc::Connect w3");
  {
    MutexLock lock(&mutex_);
    if (role_ != Role::kNone) {
      NEARBY_LOG(ERROR, "GGG in WebRtc::Connect w4");
      NEARBY_LOG(
          WARNING,
          "Cannot connect with WebRtc because we are already acting as %d",
          role_);
      return WebRtcSocketWrapper();
    }

    peer_id_ = peer_id;
    if (!InitWebRtcFlow(Role::kAnswerer, PeerId::FromRandom(), location_hint)) {
      NEARBY_LOG(INFO, "GGG in WebRtc::Connect w5");
      return WebRtcSocketWrapper();
    }
  }

  NEARBY_LOG(ERROR, "Attempting to make a WebRTC connection to %s.",
             peer_id.GetId().c_str());

  Future<WebRtcSocketWrapper> socket_future = ListenForWebRtcSocketFuture(
      connection_flow_->GetDataChannel(), AcceptedConnectionCallback());

  // The two devices have discovered each other, hence we have a timeout for
  // establishing the transport channel.
  // NOTE - We should not hold |mutex_| while waiting for the data channel since
  // it would block incoming signaling messages from being processed, resulting
  // in a timeout in creating the socket.
  ExceptionOr<WebRtcSocketWrapper> result =
      socket_future.Get(kDataChannelTimeout);
  NEARBY_LOG(ERROR, "GGG in WebRtc::Connect w6");
  if (result.ok())
  {
    NEARBY_LOG(ERROR, "GGG in WebRtc::Connect w7");
    return result.result();
  }

  NEARBY_LOG(ERROR, "GGG in WebRtc::Connect w8, e: %d ", result.exception());
  Disconnect();
  return WebRtcSocketWrapper();
}

bool WebRtc::SetLocalSessionDescription(SessionDescriptionWrapper sdp) {
  NEARBY_LOG(ERROR, "GGG in WebRtc::SetLocalSessionDescription w1");
  if (!connection_flow_->SetLocalSessionDescription(std::move(sdp))) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::SetLocalSessionDescription w2");
    LogAndDisconnect("Unable to set local session description");
    return false;
  }
  NEARBY_LOG(ERROR, "GGG in WebRtc::SetLocalSessionDescription w3");

  return true;
}

void WebRtc::StopAcceptingConnections(const std::string& service_id) {
  NEARBY_LOG(ERROR, "GGG in WebRtc::StopAcceptingConnections w1");
  if (!IsAcceptingConnections(service_id)) {
    NEARBY_LOG(INFO,
               "Skipped StopAcceptingConnections since we are not currently "
               "accepting WebRTC connections for %s",
               service_id.c_str());
    return;
  }

  {
    MutexLock lock(&mutex_);
    ShutdownSignaling();
  }
  NEARBY_LOG(ERROR, "Stopped accepting WebRTC connections");
}

Future<WebRtcSocketWrapper> WebRtc::ListenForWebRtcSocketFuture(
    Future<rtc::scoped_refptr<webrtc::DataChannelInterface>>
        data_channel_future,
    AcceptedConnectionCallback callback) {
  NEARBY_LOG(ERROR, "GGG in WebRtc::ListenForWebRtcSocketFuture w1");
  Future<WebRtcSocketWrapper> socket_future;
  auto data_channel_runnable = [this, socket_future, data_channel_future,
                                callback{std::move(callback)}]() mutable {
    // The overall timeout of creating the socket and data channel is controlled
    // by the caller of this function.
    ExceptionOr<rtc::scoped_refptr<webrtc::DataChannelInterface>> res =
        data_channel_future.Get();
    if (res.ok()) {
      WebRtcSocketWrapper wrapper = CreateWebRtcSocketWrapper(res.result());
      callback.accepted_cb(wrapper);
      {
        MutexLock lock(&mutex_);
        socket_ = wrapper;
      }
      socket_future.Set(wrapper);
    } else {
      NEARBY_LOG(WARNING, "WebRtc::InitWebRtcFlow.");
      socket_future.Set(WebRtcSocketWrapper());
    }
  };

  data_channel_future.AddListener(std::move(data_channel_runnable),
                                  &single_thread_executor_);

  return socket_future;
}

WebRtcSocketWrapper WebRtc::CreateWebRtcSocketWrapper(
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  if (data_channel == nullptr) {
    return WebRtcSocketWrapper();
  }

  auto socket = std::make_unique<WebRtcSocket>("WebRtcSocket", data_channel);
  socket->SetOnSocketClosedListener(
      {[this]() { OffloadFromSignalingThread([this]() { Disconnect(); }); }});
  return WebRtcSocketWrapper(std::move(socket));
}

bool WebRtc::InitWebRtcFlow(Role role, const PeerId& self_id,
                            const LocationHint& location_hint) {
  NEARBY_LOG(WARNING, "GGG WebRtc::InitWebRtcFlow l1");
  role_ = role;
  self_id_ = self_id;

  if (connection_flow_) {
    NEARBY_LOG(WARNING, "GGG in WebRtc::InitWebRtcFlow l2");
    LogAndShutdownSignaling(
        "Tried to initialize WebRTC without shutting down the previous "
        "connection");
    return false;
  }

  if (signaling_messenger_) {
    NEARBY_LOG(WARNING, "GGG in WebRtc::InitWebRtcFlow l3");
    LogAndShutdownSignaling(
        "Tried to initialize WebRTC without shutting down signaling messenger");
    return false;
  }

  NEARBY_LOG(WARNING, "GGG in WebRtc::InitWebRtcFlow l4");
  signaling_messenger_ =
      medium_.GetSignalingMessenger(self_id_.GetId(), location_hint);
  auto signaling_message_callback = [this](ByteArray message) {
    OffloadFromSignalingThread([this, message{std::move(message)}]() {
      NEARBY_LOG(WARNING, "GGG00 in WebRtc::InitWebRtcFlow l-400");
      ProcessSignalingMessage(message);
    });
  };

  if (!signaling_messenger_->IsValid() ||
      !signaling_messenger_->StartReceivingMessages(
          signaling_message_callback)) {
    NEARBY_LOG(WARNING, "GGG in WebRtc::InitWebRtcFlow l5");
    DisconnectLocked();
    return false;
  }

  if (role_ == Role::kAnswerer &&
      !signaling_messenger_->SendMessage(
          peer_id_.GetId(),
          webrtc_frames::EncodeReadyForSignalingPoke(self_id))) {
    NEARBY_LOG(WARNING, "GGG in WebRtc::InitWebRtcFlow l6");
    LogAndDisconnect(absl::StrCat("Could not send signaling poke to peer ",
                                  peer_id_.GetId()));
    return false;
  }

  connection_flow_ = ConnectionFlow::Create(GetLocalIceCandidateListener(),
                                            GetDataChannelListener(), medium_);
  if (!connection_flow_)
  {
      NEARBY_LOG(WARNING, "GGG in WebRtc::InitWebRtcFlow l6.5");
      return false;
  }

  NEARBY_LOG(WARNING, "GGG in WebRtc::InitWebRtcFlow l7");
  return true;
}

void WebRtc::OnLocalIceCandidate(
    const webrtc::IceCandidateInterface* local_ice_candidate) {
  NEARBY_LOG(ERROR, "GGG in WebRtc::OnLocalIceCandidate w1");
  ::location::nearby::mediums::IceCandidate ice_candidate =
      webrtc_frames::EncodeIceCandidate(*local_ice_candidate);

  NEARBY_LOG(ERROR, "GGG in WebRtc::OnLocalIceCandidate w2");
  OffloadFromSignalingThread([this, ice_candidate{std::move(ice_candidate)}]() {
    MutexLock lock(&mutex_);
    if (IsSignaling()) {
      NEARBY_LOG(ERROR, "GGG in WebRtc::OnLocalIceCandidate w3");
      signaling_messenger_->SendMessage(
          peer_id_.GetId(), webrtc_frames::EncodeIceCandidates(
                                self_id_, {std::move(ice_candidate)}));
    } else {
      NEARBY_LOG(ERROR, "GGG in WebRtc::OnLocalIceCandidate w4");
      pending_local_ice_candidates_.push_back(std::move(ice_candidate));
    }
  });
  NEARBY_LOG(ERROR, "GGG in WebRtc::OnLocalIceCandidate w5");
}

LocalIceCandidateListener WebRtc::GetLocalIceCandidateListener() {
  NEARBY_LOG(ERROR, "GGG in WebRtc::GetLocalIceCandidateListener w100");
  return {std::bind(&WebRtc::OnLocalIceCandidate, this, std::placeholders::_1)};
}

void WebRtc::OnDataChannelClosed() {
  NEARBY_LOG(ERROR, "GGG in WebRtc::OnDataChannelClosed w1");
  OffloadFromSignalingThread([this]() {
    MutexLock lock(&mutex_);
    LogAndDisconnect("WebRTC data channel closed");
  });
}

void WebRtc::OnDataChannelMessageReceived(const ByteArray& message) {
  NEARBY_LOG(ERROR, "GGG in WebRtc::OnDataChannelMessageReceived w1");
  OffloadFromSignalingThread([this, message]() {
    MutexLock lock(&mutex_);
    if (!socket_.IsValid()) {
      LogAndDisconnect("Received a data channel message without a socket");
      return;
    }

    socket_.NotifyDataChannelMsgReceived(message);
  });
}

void WebRtc::OnDataChannelBufferedAmountChanged() {
  OffloadFromSignalingThread([this]() {
    MutexLock lock(&mutex_);
    if (!socket_.IsValid()) {
      LogAndDisconnect("Data channel buffer changed without a socket");
      return;
    }

    socket_.NotifyDataChannelBufferedAmountChanged();
  });
}

DataChannelListener WebRtc::GetDataChannelListener() {
  return {
      .data_channel_closed_cb = std::bind(&WebRtc::OnDataChannelClosed, this),
      .data_channel_message_received_cb = std::bind(
          &WebRtc::OnDataChannelMessageReceived, this, std::placeholders::_1),
      .data_channel_buffered_amount_changed_cb =
          std::bind(&WebRtc::OnDataChannelBufferedAmountChanged, this),
  };
}

bool WebRtc::IsSignaling() {
  return (role_ != Role::kNone && self_id_.IsValid() && peer_id_.IsValid());
}

void WebRtc::ProcessSignalingMessage(const ByteArray& message) {
  NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w1");
  MutexLock lock(&mutex_);

  if (!connection_flow_) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w2");
    LogAndDisconnect("Received WebRTC frame before signaling was started");
    return;
  }

  location::nearby::mediums::WebRtcSignalingFrame frame;
  if (!frame.ParseFromString(std::string(message))) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w3");
    LogAndDisconnect("Failed to parse signaling message");
    return;
  }

  if (!frame.has_sender_id()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w4");
    LogAndDisconnect("Invalid WebRTC frame: Sender ID is missing");
    return;
  }

  if (frame.has_ready_for_signaling_poke() && !peer_id_.IsValid()) {
    peer_id_ = PeerId(frame.sender_id().id());
    NEARBY_LOG(ERROR, "Peer %s is ready for signaling",
               peer_id_.GetId().c_str());
  }

  if (!IsSignaling()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w5");
    NEARBY_LOG(ERROR,
               "Ignoring WebRTC frame: we are not currently listening for "
               "signaling messages");
    return;
  }

  if (frame.sender_id().id() != peer_id_.GetId()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w6");
    NEARBY_LOG(
        INFO, "Ignoring WebRTC frame: we are only listening for another peer.");
    return;
  }

  if (frame.has_ready_for_signaling_poke()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w6-1");
    SendOfferAndIceCandidatesToPeer();
  } else if (frame.has_offer()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w6-2");
    connection_flow_->OnOfferReceived(
        SessionDescriptionWrapper(webrtc_frames::DecodeOffer(frame).release()));
    SendAnswerToPeer();
  } else if (frame.has_answer()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w6-3");
    connection_flow_->OnAnswerReceived(SessionDescriptionWrapper(
        webrtc_frames::DecodeAnswer(frame).release()));
  } else if (frame.has_ice_candidates()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w6-4");
    if (!connection_flow_->OnRemoteIceCandidatesReceived(
            webrtc_frames::DecodeIceCandidates(frame))) {
      LogAndDisconnect("Could not add remote ice candidates.");
    }
  }
  NEARBY_LOG(ERROR, "GGG in WebRtc::ProcessSignalingMessage w7");
}

void WebRtc::SendOfferAndIceCandidatesToPeer() {
  NEARBY_LOG(ERROR, "GGG in WebRtc::SendOfferAndIceCandidatesToPeer w1");
  if (pending_local_offer_.Empty()) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::SendOfferAndIceCandidatesToPeer w2");
    LogAndDisconnect(
        "Unable to send pending offer to remote peer: local offer not set");
    return;
  }

  if (!signaling_messenger_->SendMessage(peer_id_.GetId(),
                                         pending_local_offer_)) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::SendOfferAndIceCandidatesToPeer w3");
    LogAndDisconnect("Failed to send local offer via signaling messenger");
    return;
  }
  pending_local_offer_ = ByteArray();

  if (!pending_local_ice_candidates_.empty()) {
    signaling_messenger_->SendMessage(
        peer_id_.GetId(),
        webrtc_frames::EncodeIceCandidates(
            self_id_, std::move(pending_local_ice_candidates_)));
  }
  NEARBY_LOG(ERROR, "GGG in WebRtc::SendOfferAndIceCandidatesToPeer w4");
}

void WebRtc::SendAnswerToPeer() {
  NEARBY_LOG(ERROR, "GGG in WebRtc::SendAnswerToPeer w1");
  SessionDescriptionWrapper answer = connection_flow_->CreateAnswer();
  ByteArray answer_message(
      webrtc_frames::EncodeAnswer(self_id_, answer.GetSdp()));

  if (!SetLocalSessionDescription(std::move(answer)))
  {
    NEARBY_LOG(ERROR, "GGG in WebRtc::SendAnswerToPeer w2");
    return;
  }

  if (!signaling_messenger_->SendMessage(peer_id_.GetId(), answer_message)) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::SendAnswerToPeer w3");
    LogAndDisconnect("Failed to send local answer via signaling messenger");
    return;
  }
}

void WebRtc::LogAndDisconnect(const std::string& error_message) {
  NEARBY_LOG(WARNING, "Disconnecting WebRTC : %s", error_message.c_str());
  DisconnectLocked();
}

void WebRtc::LogAndShutdownSignaling(const std::string& error_message) {
  NEARBY_LOG(WARNING, "Stopping WebRTC signaling : %s", error_message.c_str());
  ShutdownSignaling();
}

void WebRtc::ShutdownSignaling() {
  NEARBY_LOG(ERROR, "GGG in WebRtc::ShutdownSignaling w1");
  role_ = Role::kNone;
  self_id_ = PeerId();
  peer_id_ = PeerId();
  pending_local_offer_ = ByteArray();
  pending_local_ice_candidates_.clear();

  if (restart_receive_messages_alarm_.IsValid()) {
    restart_receive_messages_alarm_.Cancel();
    restart_receive_messages_alarm_ = CancelableAlarm();
  }

  if (signaling_messenger_) {
    signaling_messenger_->StopReceivingMessages();
    signaling_messenger_.reset();
  }

  if (!socket_.IsValid()) ShutdownIceCandidateCollection();
}

void WebRtc::Disconnect() {
  NEARBY_LOG(ERROR, "GGG in WebRtc::Disconnect w1");
  MutexLock lock(&mutex_);
  DisconnectLocked();
}

void WebRtc::DisconnectLocked() {
  NEARBY_LOG(ERROR, "GGG in WebRtc::DisconnectLocked w1");
  ShutdownSignaling();
  ShutdownWebRtcSocket();
  ShutdownIceCandidateCollection();
}

void WebRtc::ShutdownWebRtcSocket() {
  if (socket_.IsValid()) {
    socket_.Close();
    socket_ = WebRtcSocketWrapper();
  }
}

void WebRtc::ShutdownIceCandidateCollection() {
  NEARBY_LOG(ERROR, "GGG in WebRtc::ShutdownIceCandidateCollection w1");
  if (connection_flow_) {
    NEARBY_LOG(ERROR, "GGG in WebRtc::ShutdownIceCandidateCollection w2");
    connection_flow_->Close();
    connection_flow_.reset();
  }
}

void WebRtc::OffloadFromSignalingThread(Runnable runnable) {
  single_thread_executor_.Execute(std::move(runnable));
}

void WebRtc::RestartReceiveMessages(const LocationHint& location_hint,
                                    const std::string& service_id) {
  if (!IsAcceptingConnections(service_id)) {
    NEARBY_LOG(ERROR,
               "Skipping restart since we are not accepting connections.");
    return;
  }

  NEARBY_LOG(ERROR, "Restarting listening for receiving signaling messages.");
  {
    MutexLock lock(&mutex_);
    signaling_messenger_->StopReceivingMessages();

    signaling_messenger_ =
        medium_.GetSignalingMessenger(self_id_.GetId(), location_hint);

    auto signaling_message_callback = [this](ByteArray message) {
      OffloadFromSignalingThread([this, message{std::move(message)}]() {
        ProcessSignalingMessage(message);
      });
    };

    if (!signaling_messenger_->IsValid() ||
        !signaling_messenger_->StartReceivingMessages(
            signaling_message_callback)) {
      DisconnectLocked();
    }
  }
}

}  // namespace mediums
}  // namespace connections
}  // namespace nearby
}  // namespace location
