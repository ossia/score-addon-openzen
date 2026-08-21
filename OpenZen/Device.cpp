#include "Device.hpp"

#include "Protocol.hpp"
#include "SpecificSettings.hpp"

#include <Device/Protocol/DeviceSettings.hpp>

#include <ossia/network/generic/generic_device.hpp>
#include <ossia/network/rate_limiting_protocol.hpp>

#include <QDebug>

#include <wobjectimpl.h>

W_OBJECT_IMPL(OpenZen::DeviceImplementation)

namespace OpenZen
{

DeviceImplementation::DeviceImplementation(
    const Device::DeviceSettings& settings, const ossia::net::network_context_ptr& ctx)
    : OwningDeviceInterface{settings}
    , m_ctx{ctx}
{
  m_capas.canRefreshTree = true;
  m_capas.canAddNode = false;
  m_capas.canRemoveNode = false;
  m_capas.canRenameNode = false;
  m_capas.canSetProperties = false;
  m_capas.canSerialize = false;
}

DeviceImplementation::~DeviceImplementation() = default;

bool DeviceImplementation::reconnect()
{
  disconnect();

  try
  {
    const auto& set = m_settings.deviceSpecificSettings.value<SpecificSettings>();

    std::unique_ptr<ossia::net::protocol_base> proto
        = std::make_unique<ossia::openzen::protocol>(m_ctx, set);

    if(set.rate)
    {
      proto = std::make_unique<ossia::net::rate_limiting_protocol>(
          std::chrono::milliseconds{*set.rate}, std::move(proto));
    }

    m_dev = std::make_unique<ossia::net::generic_device>(
        std::move(proto), settings().name.toStdString());

    deviceChanged(nullptr, m_dev.get());
    enableCallbacks();
  }
  catch(const std::exception& e)
  {
    qDebug() << "OpenZen device error:" << e.what();
  }
  catch(...)
  {
    qDebug() << "OpenZen device error";
  }

  // The device is usable as soon as the tree exists, whether or not the sensor
  // happens to be plugged in: that is what lets a score keep its cables while
  // the hardware comes and goes.
  return bool(m_dev);
}

void DeviceImplementation::disconnect()
{
  OwningDeviceInterface::disconnect();
}
}
