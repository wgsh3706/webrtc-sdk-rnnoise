/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <memory>
#include <utility>
#include <vector>

#include "absl/strings/str_replace.h"
#include "api/audio/audio_mixer.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/media_types.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "p2p/base/port_allocator.h"
#include "pc/peer_connection_wrapper.h"
#include "pc/session_description.h"
#include "pc/test/fake_audio_capture_module.h"
#include "pc/test/mock_peer_connection_observers.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/metrics.h"
#include "test/gtest.h"

// This file contains unit tests that relate to the behavior of the
// SdpOfferAnswer module.
// Tests are writen as integration tests with PeerConnection, since the
// behaviors are still linked so closely that it is hard to test them in
// isolation.

namespace webrtc {

using RTCConfiguration = PeerConnectionInterface::RTCConfiguration;

namespace {

std::unique_ptr<rtc::Thread> CreateAndStartThread() {
  auto thread = rtc::Thread::Create();
  thread->Start();
  return thread;
}

}  // namespace

class SdpOfferAnswerTest : public ::testing::Test {
 public:
  SdpOfferAnswerTest()
      // Note: We use a PeerConnectionFactory with a distinct
      // signaling thread, so that thread handling can be tested.
      : signaling_thread_(CreateAndStartThread()),
        pc_factory_(CreatePeerConnectionFactory(
            nullptr,
            nullptr,
            signaling_thread_.get(),
            FakeAudioCaptureModule::Create(),
            CreateBuiltinAudioEncoderFactory(),
            CreateBuiltinAudioDecoderFactory(),
            std::make_unique<
                VideoEncoderFactoryTemplate<LibvpxVp8EncoderTemplateAdapter,
                                            LibvpxVp9EncoderTemplateAdapter,
                                            OpenH264EncoderTemplateAdapter,
                                            LibaomAv1EncoderTemplateAdapter>>(),
            std::make_unique<
                VideoDecoderFactoryTemplate<LibvpxVp8DecoderTemplateAdapter,
                                            LibvpxVp9DecoderTemplateAdapter,
                                            OpenH264DecoderTemplateAdapter,
                                            Dav1dDecoderTemplateAdapter>>(),
            nullptr /* audio_mixer */,
            nullptr /* audio_processing */)) {
    webrtc::metrics::Reset();
  }

  std::unique_ptr<PeerConnectionWrapper> CreatePeerConnection() {
    RTCConfiguration config;
    config.sdp_semantics = SdpSemantics::kUnifiedPlan;
    return CreatePeerConnection(config);
  }

  std::unique_ptr<PeerConnectionWrapper> CreatePeerConnection(
      const RTCConfiguration& config) {
    auto observer = std::make_unique<MockPeerConnectionObserver>();
    auto result = pc_factory_->CreatePeerConnectionOrError(
        config, PeerConnectionDependencies(observer.get()));
    EXPECT_TRUE(result.ok());
    observer->SetPeerConnectionInterface(result.value().get());
    return std::make_unique<PeerConnectionWrapper>(
        pc_factory_, result.MoveValue(), std::move(observer));
  }

 protected:
  std::unique_ptr<rtc::Thread> signaling_thread_;
  rtc::scoped_refptr<PeerConnectionFactoryInterface> pc_factory_;

 private:
  rtc::AutoThread main_thread_;
};

TEST_F(SdpOfferAnswerTest, OnTrackReturnsProxiedObject) {
  auto caller = CreatePeerConnection();
  auto callee = CreatePeerConnection();

  auto audio_transceiver = caller->AddTransceiver(cricket::MEDIA_TYPE_AUDIO);

  ASSERT_TRUE(caller->ExchangeOfferAnswerWith(callee.get()));
  // Verify that caller->observer->OnTrack() has been called with a
  // proxied transceiver object.
  ASSERT_EQ(callee->observer()->on_track_transceivers_.size(), 1u);
  auto transceiver = callee->observer()->on_track_transceivers_[0];
  // Since the signaling thread is not the current thread,
  // this will DCHECK if the transceiver is not proxied.
  transceiver->stopped();
}

TEST_F(SdpOfferAnswerTest, BundleRejectsCodecCollisionsAudioVideo) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:1\r\n"
      "a=rtpmap:111 H264/90000\r\n"
      "a=fmtp:111 "
      "level-asymmetry-allowed=1;packetization-mode=0;profile-level-id="
      "42e01f\r\n";

  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  ASSERT_NE(desc, nullptr);
  RTCError error;
  pc->SetRemoteDescription(std::move(desc), &error);
  EXPECT_TRUE(error.ok());
  EXPECT_METRIC_EQ(
      1, webrtc::metrics::NumEvents(
             "WebRTC.PeerConnection.ValidBundledPayloadTypes", false));
}

TEST_F(SdpOfferAnswerTest, BundleRejectsCodecCollisionsVideoFmtp) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:111 H264/90000\r\n"
      "a=fmtp:111 "
      "level-asymmetry-allowed=1;packetization-mode=0;profile-level-id="
      "42e01f\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:1\r\n"
      "a=rtpmap:111 H264/90000\r\n"
      "a=fmtp:111 "
      "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id="
      "42e01f\r\n";

  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  ASSERT_NE(desc, nullptr);
  RTCError error;
  pc->SetRemoteDescription(std::move(desc), &error);
  EXPECT_TRUE(error.ok());
  EXPECT_METRIC_EQ(
      1, webrtc::metrics::NumEvents(
             "WebRTC.PeerConnection.ValidBundledPayloadTypes", false));
}

TEST_F(SdpOfferAnswerTest, BundleCodecCollisionInDifferentBundlesAllowed) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0\r\n"
      "a=group:BUNDLE 1\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:111 H264/90000\r\n"
      "a=fmtp:111 "
      "level-asymmetry-allowed=1;packetization-mode=0;profile-level-id="
      "42e01f\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:1\r\n"
      "a=rtpmap:111 H264/90000\r\n"
      "a=fmtp:111 "
      "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id="
      "42e01f\r\n";

  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  ASSERT_NE(desc, nullptr);
  RTCError error;
  pc->SetRemoteDescription(std::move(desc), &error);
  EXPECT_TRUE(error.ok());
  EXPECT_METRIC_EQ(
      0, webrtc::metrics::NumEvents(
             "WebRTC.PeerConnection.ValidBundledPayloadTypes", false));
}

TEST_F(SdpOfferAnswerTest, BundleMeasuresHeaderExtensionIdCollision) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=extmap:3 "
      "http://www.ietf.org/id/"
      "draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 112\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:1\r\n"
      "a=rtpmap:112 VP8/90000\r\n"
      "a=extmap:3 "
      "http://www.ietf.org/id/"
      "draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n";
  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  ASSERT_NE(desc, nullptr);
  RTCError error;
  pc->SetRemoteDescription(std::move(desc), &error);
  EXPECT_TRUE(error.ok());
  EXPECT_METRIC_EQ(1,
                   webrtc::metrics::NumEvents(
                       "WebRTC.PeerConnection.ValidBundledExtensionIds", true));
}

// extmap:3 is used with two different URIs which is not allowed.
TEST_F(SdpOfferAnswerTest, BundleRejectsHeaderExtensionIdCollision) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=extmap:3 "
      "http://www.ietf.org/id/"
      "draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 112\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:1\r\n"
      "a=rtpmap:112 VP8/90000\r\n"
      "a=extmap:3 urn:3gpp:video-orientation\r\n";
  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  ASSERT_NE(desc, nullptr);
  RTCError error;
  pc->SetRemoteDescription(std::move(desc), &error);
  EXPECT_FALSE(error.ok());
  EXPECT_EQ(error.type(), RTCErrorType::INVALID_PARAMETER);
  EXPECT_METRIC_EQ(
      1, webrtc::metrics::NumEvents(
             "WebRTC.PeerConnection.ValidBundledExtensionIds", false));
}

// transport-wide cc is negotiated with two different ids 3 and 4.
// This is not a good idea but tolerable.
TEST_F(SdpOfferAnswerTest, BundleAcceptsDifferentIdsForSameExtension) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=extmap:3 "
      "http://www.ietf.org/id/"
      "draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 112\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:1\r\n"
      "a=rtpmap:112 VP8/90000\r\n"
      "a=extmap:4 "
      "http://www.ietf.org/id/"
      "draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n";
  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  ASSERT_NE(desc, nullptr);
  RTCError error;
  pc->SetRemoteDescription(std::move(desc), &error);
  EXPECT_TRUE(error.ok());
  EXPECT_METRIC_EQ(1,
                   webrtc::metrics::NumEvents(
                       "WebRTC.PeerConnection.ValidBundledExtensionIds", true));
}

TEST_F(SdpOfferAnswerTest, LargeMidsAreRejected) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:111 VP8/90000\r\n"
      "a=mid:01234567890123456\r\n";
  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  ASSERT_NE(desc, nullptr);
  RTCError error;
  pc->SetRemoteDescription(std::move(desc), &error);
  EXPECT_FALSE(error.ok());
  EXPECT_EQ(error.type(), RTCErrorType::INVALID_PARAMETER);
}

TEST_F(SdpOfferAnswerTest, RollbackPreservesAddTrackMid) {
  std::string sdp =
      "v=0\r\n"
      "o=- 4131505339648218884 3 IN IP4 **-----**\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=ice-lite\r\n"
      "a=msid-semantic: WMS 100030878598094:4Qs1PjbLM32RK5u3\r\n"
      "a=ice-ufrag:zGWFZ+fVXDeN6UoI/136\r\n"
      "a=ice-pwd:9AUNgUqRNI5LSIrC1qFD2iTR\r\n"
      "a=fingerprint:sha-256 "
      "AD:52:52:E0:B1:37:34:21:0E:15:8E:B7:56:56:7B:B4:39:0E:6D:1C:F5:84:A7:EE:"
      "B5:27:3E:30:B1:7D:69:42\r\n"
      "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
      "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "m=audio 40005 UDP/TLS/RTP/SAVPF 111\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=fmtp:111 "
      "maxaveragebitrate=20000;maxplaybackrate=16000;minptime=10;usedtx=1;"
      "useinbandfec=1;stereo=0\r\n"
      "a=rtcp-fb:111 nack\r\n"
      "a=setup:passive\r\n"
      "a=mid:0\r\n"
      "a=msid:- 75156ebd-e705-4da1-920e-2dac39794dfd\r\n"
      "a=ptime:60\r\n"
      "a=recvonly\r\n"
      "a=rtcp-mux\r\n"
      "m=audio 40005 UDP/TLS/RTP/SAVPF 111\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=fmtp:111 "
      "maxaveragebitrate=20000;maxplaybackrate=16000;minptime=10;usedtx=1;"
      "useinbandfec=1;stereo=0\r\n"
      "a=rtcp-fb:111 nack\r\n"
      "a=setup:passive\r\n"
      "a=mid:1\r\n"
      "a=msid:100030878598094:4Qs1PjbLM32RK5u3 9695447562408476674\r\n"
      "a=ptime:60\r\n"
      "a=sendonly\r\n"
      "a=ssrc:2565730539 cname:100030878598094:4Qs1PjbLM32RK5u3\r\n"
      "a=rtcp-mux\r\n";
  auto pc = CreatePeerConnection();
  auto audio_track = pc->AddAudioTrack("audio_track", {});
  auto first_transceiver = pc->pc()->GetTransceivers()[0];
  EXPECT_FALSE(first_transceiver->mid().has_value());
  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  ASSERT_NE(desc, nullptr);
  RTCError error;
  ASSERT_TRUE(pc->SetRemoteDescription(std::move(desc)));
  pc->CreateAnswerAndSetAsLocal();
  auto saved_mid = first_transceiver->mid();
  EXPECT_TRUE(saved_mid.has_value());
  auto offer_before_rollback = pc->CreateOfferAndSetAsLocal();
  EXPECT_EQ(saved_mid, first_transceiver->mid());
  auto rollback = pc->CreateRollback();
  ASSERT_NE(rollback, nullptr);
  ASSERT_TRUE(pc->SetLocalDescription(std::move(rollback)));
  EXPECT_EQ(saved_mid, first_transceiver->mid());
  auto offer2 = pc->CreateOfferAndSetAsLocal();
  ASSERT_NE(offer2, nullptr);
  EXPECT_EQ(saved_mid, first_transceiver->mid());
}

#ifdef WEBRTC_HAVE_SCTP

TEST_F(SdpOfferAnswerTest, RejectedDataChannelsDoNotGetReoffered) {
  auto pc = CreatePeerConnection();
  EXPECT_TRUE(pc->pc()->CreateDataChannelOrError("dc", nullptr).ok());
  EXPECT_TRUE(pc->CreateOfferAndSetAsLocal());
  auto mid = pc->pc()->local_description()->description()->contents()[0].mid();

  // An answer that rejects the datachannel content.
  std::string sdp =
      "v=0\r\n"
      "o=- 4131505339648218884 3 IN IP4 **-----**\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=ice-ufrag:zGWFZ+fVXDeN6UoI/136\r\n"
      "a=ice-pwd:9AUNgUqRNI5LSIrC1qFD2iTR\r\n"
      "a=fingerprint:sha-256 "
      "AD:52:52:E0:B1:37:34:21:0E:15:8E:B7:56:56:7B:B4:39:0E:6D:1C:F5:84:A7:EE:"
      "B5:27:3E:30:B1:7D:69:42\r\n"
      "a=setup:passive\r\n"
      "m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=sctp-port:5000\r\n"
      "a=max-message-size:262144\r\n"
      "a=mid:" +
      mid + "\r\n";
  auto answer = CreateSessionDescription(SdpType::kAnswer, sdp);
  ASSERT_TRUE(pc->SetRemoteDescription(std::move(answer)));
  // The subsequent offer should not recycle the m-line since the existing data
  // channel is closed.
  auto offer = pc->CreateOffer();
  const auto& offer_contents = offer->description()->contents();
  ASSERT_EQ(offer_contents.size(), 1u);
  EXPECT_EQ(offer_contents[0].mid(), mid);
  EXPECT_EQ(offer_contents[0].rejected, true);
}

TEST_F(SdpOfferAnswerTest, RejectedDataChannelsDoGetReofferedWhenActive) {
  auto pc = CreatePeerConnection();
  EXPECT_TRUE(pc->pc()->CreateDataChannelOrError("dc", nullptr).ok());
  EXPECT_TRUE(pc->CreateOfferAndSetAsLocal());
  auto mid = pc->pc()->local_description()->description()->contents()[0].mid();

  // An answer that rejects the datachannel content.
  std::string sdp =
      "v=0\r\n"
      "o=- 4131505339648218884 3 IN IP4 **-----**\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=ice-ufrag:zGWFZ+fVXDeN6UoI/136\r\n"
      "a=ice-pwd:9AUNgUqRNI5LSIrC1qFD2iTR\r\n"
      "a=fingerprint:sha-256 "
      "AD:52:52:E0:B1:37:34:21:0E:15:8E:B7:56:56:7B:B4:39:0E:6D:1C:F5:84:A7:EE:"
      "B5:27:3E:30:B1:7D:69:42\r\n"
      "a=setup:passive\r\n"
      "m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=sctp-port:5000\r\n"
      "a=max-message-size:262144\r\n"
      "a=mid:" +
      mid + "\r\n";
  auto answer = CreateSessionDescription(SdpType::kAnswer, sdp);
  ASSERT_TRUE(pc->SetRemoteDescription(std::move(answer)));

  // The subsequent offer should recycle the m-line when there is a new data
  // channel.
  EXPECT_TRUE(pc->pc()->CreateDataChannelOrError("dc2", nullptr).ok());
  EXPECT_TRUE(pc->pc()->ShouldFireNegotiationNeededEvent(
      pc->observer()->latest_negotiation_needed_event()));

  auto offer = pc->CreateOffer();
  const auto& offer_contents = offer->description()->contents();
  ASSERT_EQ(offer_contents.size(), 1u);
  EXPECT_EQ(offer_contents[0].mid(), mid);
  EXPECT_EQ(offer_contents[0].rejected, false);
}

#endif  // WEBRTC_HAVE_SCTP

<<<<<<< HEAD
TEST_F(SdpOfferAnswerTest, SimulcastAnswerWithNoRidsIsRejected) {
  auto pc = CreatePeerConnection();

  RtpTransceiverInit init;
  RtpEncodingParameters rid1;
  rid1.rid = "1";
  init.send_encodings.push_back(rid1);
  RtpEncodingParameters rid2;
  rid2.rid = "2";
  init.send_encodings.push_back(rid2);

  auto transceiver = pc->AddTransceiver(cricket::MEDIA_TYPE_VIDEO, init);
  EXPECT_TRUE(pc->CreateOfferAndSetAsLocal());
  auto mid = pc->pc()->local_description()->description()->contents()[0].mid();

  // A SDP answer with simulcast but without mid/rid extensions.
  std::string sdp =
      "v=0\r\n"
      "o=- 4131505339648218884 3 IN IP4 **-----**\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=ice-ufrag:zGWFZ+fVXDeN6UoI/136\r\n"
      "a=ice-pwd:9AUNgUqRNI5LSIrC1qFD2iTR\r\n"
      "a=fingerprint:sha-256 "
      "AD:52:52:E0:B1:37:34:21:0E:15:8E:B7:56:56:7B:B4:39:0E:6D:1C:F5:84:A7:EE:"
      "B5:27:3E:30:B1:7D:69:42\r\n"
      "a=setup:passive\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp:9 IN IP4 0.0.0.0\r\n"
      "a=mid:" +
      mid +
      "\r\n"
      "a=recvonly\r\n"
      "a=rtcp-mux\r\n"
      "a=rtcp-rsize\r\n"
      "a=rtpmap:96 VP8/90000\r\n"
      "a=rid:1 recv\r\n"
      "a=rid:2 recv\r\n"
      "a=simulcast:recv 1;2\r\n";
  std::string extensions =
      "a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
      "a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id\r\n";
  auto answer = CreateSessionDescription(SdpType::kAnswer, sdp);
  EXPECT_FALSE(pc->SetRemoteDescription(std::move(answer)));

  auto answer_with_extensions =
      CreateSessionDescription(SdpType::kAnswer, sdp + extensions);
  EXPECT_TRUE(pc->SetRemoteDescription(std::move(answer_with_extensions)));
}

TEST_F(SdpOfferAnswerTest, ExpectAllSsrcsSpecifiedInSsrcGroupFid) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 "
      "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id="
      "42e01f\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=ssrc-group:FID 1 2\r\n"
      "a=ssrc:1 cname:test\r\n";
  auto offer = CreateSessionDescription(SdpType::kOffer, sdp);
  EXPECT_FALSE(pc->SetRemoteDescription(std::move(offer)));
}

TEST_F(SdpOfferAnswerTest, ExpectAllSsrcsSpecifiedInSsrcGroupFecFr) {
  auto pc = CreatePeerConnection();
  std::string sdp =
      "v=0\r\n"
      "o=- 0 3 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0\r\n"
      "a=fingerprint:sha-1 "
      "4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB\r\n"
      "a=setup:actpass\r\n"
      "a=ice-ufrag:ETEn\r\n"
      "a=ice-pwd:OtSK0WpNtpUjkY4+86js7Z/l\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96 98\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtcp-mux\r\n"
      "a=sendonly\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 "
      "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id="
      "42e01f\r\n"
      "a=rtpmap:98 flexfec-03/90000\r\n"
      "a=fmtp:98 repair-window=10000000\r\n"
      "a=ssrc-group:FEC-FR 1 2\r\n"
      "a=ssrc:1 cname:test\r\n";
  auto offer = CreateSessionDescription(SdpType::kOffer, sdp);
  EXPECT_FALSE(pc->SetRemoteDescription(std::move(offer)));
}

TEST_F(SdpOfferAnswerTest, DuplicateSsrcsDisallowedInLocalDescription) {
  auto pc = CreatePeerConnection();
  pc->AddAudioTrack("audio_track", {});
  pc->AddVideoTrack("video_track", {});
  auto offer = pc->CreateOffer();
  auto& offer_contents = offer->description()->contents();
  ASSERT_EQ(offer_contents.size(), 2u);
  uint32_t second_ssrc = offer_contents[1].media_description()->first_ssrc();

  offer->description()
      ->contents()[0]
      .media_description()
      ->mutable_streams()[0]
      .ssrcs[0] = second_ssrc;
  EXPECT_FALSE(pc->SetLocalDescription(std::move(offer)));
}

=======
>>>>>>> kairtc-m114
}  // namespace webrtc
