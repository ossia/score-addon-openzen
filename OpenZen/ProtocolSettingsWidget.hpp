#pragma once
#include <Device/Protocol/DeviceSettings.hpp>
#include <Device/Protocol/ProtocolSettingsWidget.hpp>

#include <OpenZen/SpecificSettings.hpp>

#include <verdigris>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace OpenZen
{
class ProtocolSettingsWidget final : public Device::ProtocolSettingsWidget
{
  W_OBJECT(ProtocolSettingsWidget)

public:
  ProtocolSettingsWidget(QWidget* parent = nullptr);
  ~ProtocolSettingsWidget() override;

  Device::DeviceSettings getSettings() const override;
  void setSettings(const Device::DeviceSettings& settings) override;

private:
  QLineEdit* m_deviceNameEdit{};
  QLineEdit* m_serial{};
  QLineEdit* m_identifier{};
  QComboBox* m_ioType{};
  QComboBox* m_baudRate{};
  QCheckBox* m_matchBySerial{};

  QSpinBox* m_samplingRate{};
  QSpinBox* m_filterMode{};
  QCheckBox* m_degrees{};

  QCheckBox* m_autoReconnect{};
  QSpinBox* m_watchdog{};
  QSpinBox* m_rate{};

  struct
  {
    QCheckBox* accel{};
    QCheckBox* gyro{};
    QCheckBox* mag{};
    QCheckBox* quaternion{};
    QCheckBox* euler{};
    QCheckBox* angularVelocity{};
    QCheckBox* linearAccel{};
    QCheckBox* pressure{};
    QCheckBox* altitude{};
    QCheckBox* temperature{};
    QCheckBox* heave{};
  } m_outputs;
};
}
