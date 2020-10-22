#ifndef CORE_INTERNAL_MOCK_SERVICE_CONTROLLER_H_
#define CORE_INTERNAL_MOCK_SERVICE_CONTROLLER_H_

#include "core/internal/service_controller.h"
#include "gmock/gmock.h"

namespace location {
namespace nearby {
namespace connections {

/* Mock implementation for ServiceController:
 * All methods execute asynchronously (in a private executor thread).
 * To synchronise, two approaches may be used:
 * 1. For methods that have result callback, we use it to unblock main thread.
 * 2. For methods that do not have callbacks, we provide a mock implementation
 *    that unblocks main thread.
 */
class MockServiceController : public ServiceController {
 public:
  MOCK_METHOD(Status, StartAdvertising,
              (ClientProxy * client, const std::string& service_id,
               const ConnectionOptions& options,
               const ConnectionRequestInfo& info),
              (override));

  MOCK_METHOD(void, StopAdvertising, (ClientProxy * client), (override));

  MOCK_METHOD(Status, StartDiscovery,
              (ClientProxy * client, const std::string& service_id,
               const ConnectionOptions& options,
               const DiscoveryListener& listener),
              (override));

  MOCK_METHOD(void, StopDiscovery, (ClientProxy * client), (override));

  MOCK_METHOD(void, InjectEndpoint,
              (ClientProxy * client, const std::string& service_id,
               const OutOfBandConnectionMetadata& metadata),
              (override));

  MOCK_METHOD(Status, RequestConnection,
              (ClientProxy * client, const std::string& endpoint_id,
               const ConnectionRequestInfo& info,
               const ConnectionOptions& options),
              (override));

  MOCK_METHOD(Status, AcceptConnection,
              (ClientProxy * client, const std::string& endpoint_id,
               const PayloadListener& listener),
              (override));

  MOCK_METHOD(Status, RejectConnection,
              (ClientProxy * client, const std::string& endpoint_id),
              (override));

  MOCK_METHOD(void, InitiateBandwidthUpgrade,
              (ClientProxy * client, const std::string& endpoint_id),
              (override));

  MOCK_METHOD(void, SendPayload,
              (ClientProxy * client,
               const std::vector<std::string>& endpoint_ids, Payload payload),
              (override));

  MOCK_METHOD(Status, CancelPayload,
              (ClientProxy * client, std::int64_t payload_id), (override));

  MOCK_METHOD(void, DisconnectFromEndpoint,
              (ClientProxy * client, const std::string& endpoint_id),
              (override));
};

}  // namespace connections
}  // namespace nearby
}  // namespace location

#endif  // CORE_INTERNAL_MOCK_SERVICE_CONTROLLER_H_