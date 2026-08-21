#include "ProtocolFactory.hpp"

#include "Device.hpp"
#include "Manager.hpp"
#include "ProtocolSettingsWidget.hpp"
#include "SpecificSettings.hpp"

#include <Device/Protocol/DeviceSettings.hpp>
#include <Explorer/DocumentPlugin/DeviceDocumentPlugin.hpp>

#include <ossia/detail/algorithms.hpp>

#include <QObject>
#include <QTimer>

namespace OpenZen
{
namespace
{
Device::DeviceSettings settings_for(const ossia::openzen::sensor_desc& d)
{
  Device::DeviceSettings s;
  s.protocol = ProtocolFactory::static_concreteKey();

  // The serial number is the stable identity; fall back to the port when the
  // IO system cannot report one before connecting.
  s.name = QString::fromStdString(!d.serial.empty() ? d.serial : d.name);
  s.name.replace('/', '_');
  s.name.replace('.', '_');
  s.name.replace('\\', '_');

  SpecificSettings specif;
  specif.ioType = QString::fromStdString(d.io_type);
  specif.serialNumber = QString::fromStdString(d.serial);
  specif.identifier = QString::fromStdString(d.identifier);
  specif.baudRate = int(d.baud_rate);
  s.deviceSpecificSettings = QVariant::fromValue(specif);
  return s;
}

QString label_for(const ossia::openzen::sensor_desc& d)
{
  QString label = QString::fromStdString(d.name.empty() ? d.identifier : d.name);
  if(!d.serial.empty() && d.serial != d.name)
    label += QStringLiteral(" (%1)").arg(QString::fromStdString(d.serial));
  return label;
}

/**
 * Lists the IMUs OpenZen can see, and keeps that list current.
 *
 * Scanning is refcounted in the manager: it is only running while an
 * enumerator like this one exists, or while some device is looking for a
 * sensor that went away. A Bluetooth inquiry is expensive enough that we do
 * not want it running in the background of every session.
 */
class Enumerator : public Device::DeviceEnumerator
{
public:
  Enumerator()
  {
    ossia::openzen::manager::instance().set_scanning(true);

    m_timer.setInterval(1000);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] { rescan(); });
    m_timer.start();
  }

  ~Enumerator() override { ossia::openzen::manager::instance().set_scanning(false); }

  void enumerate(std::function<void(const QString&, const Device::DeviceSettings&)> f)
      const override
  {
    for(const auto& d : m_current)
      f(label_for(d), settings_for(d));
  }

private:
  static bool same(
      const ossia::openzen::sensor_desc& a, const ossia::openzen::sensor_desc& b) noexcept
  {
    return a.io_type == b.io_type && a.identifier == b.identifier
           && a.serial == b.serial;
  }

  void rescan()
  {
    auto next = ossia::openzen::manager::instance().sensors();
    if(next.empty() && m_current.empty())
      return;

    for(const auto& d : m_current)
      if(!ossia::any_of(next, [&](const auto& n) { return same(d, n); }))
        deviceRemoved(label_for(d));

    bool added = false;
    for(const auto& d : next)
    {
      if(!ossia::any_of(m_current, [&](const auto& c) { return same(d, c); }))
      {
        deviceAdded(label_for(d), settings_for(d));
        added = true;
      }
    }

    m_current = std::move(next);
    if(added)
      sort();
  }

  QTimer m_timer;
  ossia::openzen::sensor_list m_current;
};
}

QString ProtocolFactory::prettyName() const noexcept
{
  return QObject::tr("OpenZen IMU");
}

QString ProtocolFactory::category() const noexcept
{
  return StandardCategories::tracking;
}

Device::DeviceEnumerators
ProtocolFactory::getEnumerators(const score::DocumentContext& ctx) const
{
  return {{"Sensors", new Enumerator}};
}

Device::DeviceInterface* ProtocolFactory::makeDevice(
    const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin& plugin,
    const score::DocumentContext& ctx)
{
  return new DeviceImplementation{settings, plugin.networkContext()};
}

const Device::DeviceSettings& ProtocolFactory::defaultSettings() const noexcept
{
  static const Device::DeviceSettings settings = [] {
    Device::DeviceSettings s;
    s.protocol = static_concreteKey();
    s.name = "IMU";
    s.deviceSpecificSettings = QVariant::fromValue(SpecificSettings{});
    return s;
  }();
  return settings;
}

Device::ProtocolSettingsWidget* ProtocolFactory::makeSettingsWidget()
{
  return new ProtocolSettingsWidget;
}

QVariant
ProtocolFactory::makeProtocolSpecificSettings(const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<SpecificSettings>(visitor);
}

void ProtocolFactory::serializeProtocolSpecificSettings(
    const QVariant& data, const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<SpecificSettings>(data, visitor);
}

bool ProtocolFactory::checkCompatibility(
    const Device::DeviceSettings& a, const Device::DeviceSettings& b) const noexcept
{
  if(a.protocol != b.protocol)
    return true;

  // Two devices must not open the same serial port. Compare on whichever
  // identity each one is actually matching by.
  const auto& as = a.deviceSpecificSettings.value<SpecificSettings>();
  const auto& bs = b.deviceSpecificSettings.value<SpecificSettings>();

  const auto key = [](const SpecificSettings& s) {
    return s.ioType + '|'
           + (s.matchBySerial && !s.serialNumber.isEmpty() ? s.serialNumber
                                                           : s.identifier);
  };
  return key(as) != key(bs);
}
}
