#pragma once
#include <Device/Protocol/DeviceInterface.hpp>

#include <ossia/network/context_functions.hpp>

#include <verdigris>

namespace Explorer
{
class DeviceDocumentPlugin;
}

namespace OpenZen
{
class DeviceImplementation final : public Device::OwningDeviceInterface
{
  W_OBJECT(DeviceImplementation)
public:
  DeviceImplementation(
      const Device::DeviceSettings& settings, const ossia::net::network_context_ptr& ctx);
  ~DeviceImplementation();

  bool reconnect() override;
  void disconnect() override;

private:
  ossia::net::network_context_ptr m_ctx;
};
}
