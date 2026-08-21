#include "ProtocolSettingsWidget.hpp"

#include "ProtocolFactory.hpp"

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

#include <wobjectimpl.h>

W_OBJECT_IMPL(OpenZen::ProtocolSettingsWidget)

namespace OpenZen
{

ProtocolSettingsWidget::ProtocolSettingsWidget(QWidget* parent)
    : Device::ProtocolSettingsWidget{parent}
{
  m_deviceNameEdit = new State::AddressFragmentLineEdit{this};
  m_deviceNameEdit->setText("IMU");

  m_serial = new QLineEdit{this};
  m_serial->setPlaceholderText(tr("hardware serial number"));

  m_identifier = new QLineEdit{this};
  m_identifier->setPlaceholderText(tr("/dev/ttyUSB0, COM3, 00:11:22:33:44:55"));

  m_ioType = new QComboBox{this};
  m_ioType->setEditable(true);
  m_ioType->addItems(
      {"LinuxDevice", "WindowsDevice", "MacDevice", "Bluetooth", "SiUsb", "Ftdi"});

  m_baudRate = new QComboBox{this};
  m_baudRate->setEditable(true);
  m_baudRate->addItem(tr("Auto"), 0);
  for(int b : {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600})
    m_baudRate->addItem(QString::number(b), b);

  m_matchBySerial = new QCheckBox{tr("Match by serial number"), this};
  m_matchBySerial->setChecked(true);
  m_matchBySerial->setToolTip(
      tr("Find the sensor by its serial number rather than by the port it was last\n"
         "seen on, so that it is still found after being plugged in elsewhere.\n"
         "Where the driver cannot report a serial number before connecting - plain\n"
         "COM ports on Windows - the remembered port is tried first and the serial\n"
         "number is verified over the wire."));

  m_samplingRate = new QSpinBox{this};
  m_samplingRate->setRange(0, 1000);
  m_samplingRate->setSuffix(tr(" Hz"));
  m_samplingRate->setSpecialValueText(tr("Leave as-is"));
  m_samplingRate->setValue(200);

  m_filterMode = new QSpinBox{this};
  m_filterMode->setRange(-1, 10);
  m_filterMode->setSpecialValueText(tr("Leave as-is"));
  m_filterMode->setValue(-1);

  m_degrees = new QCheckBox{tr("Angles in degrees"), this};
  m_degrees->setChecked(true);

  m_autoReconnect = new QCheckBox{tr("Reconnect automatically"), this};
  m_autoReconnect->setChecked(true);

  m_watchdog = new QSpinBox{this};
  m_watchdog->setRange(50, 10000);
  m_watchdog->setSuffix(tr(" ms"));
  m_watchdog->setValue(500);
  m_watchdog->setToolTip(
      tr("How long the sensor may stay silent before the link is considered lost.\n"
         "OpenZen does not report unplugged devices, so this is what detects them."));

  // 0 means "as fast as the sensor produces"; anything else wraps the
  // protocol in a rate_limiting_protocol.
  m_rate = new QSpinBox{this};
  m_rate->setRange(0, 5000);
  m_rate->setSuffix(tr(" ms"));
  m_rate->setSpecialValueText(tr("Unlimited"));
  m_rate->setValue(0);
  m_rate->setToolTip(
      tr("Minimum delay between updates sent into the score.\n"
         "Useful when the sensor streams faster than the patch needs."));

  m_autoOutputs = new QCheckBox{tr("Detect measurements from the sensor"), this};
  m_autoOutputs->setChecked(true);
  m_autoOutputs->setToolTip(
      tr("Read back what the sensor actually measures and create a node for each,\n"
         "so nothing has to be picked by hand. Turn this off only to trade\n"
         "measurements against bandwidth."));

  auto* outputs = new QGroupBox{tr("Measurements"), this};
  m_outputsBox = outputs;
  outputs->setToolTip(
      tr("Only the measurements enabled here are produced by the sensor at all.\n"
         "Each one costs room in the frame it puts on the wire, so at a given baud\n"
         "rate this is what decides the sample rate that can be reached."));
  connect(m_autoOutputs, &QCheckBox::toggled, outputs, &QWidget::setDisabled);
  outputs->setDisabled(true);

  auto* outputsLayout = new QGridLayout{outputs};
  int row = 0, col = 0;
  const auto addOutput = [&](QCheckBox*& box, const QString& text, bool on) {
    box = new QCheckBox{text, outputs};
    box->setChecked(on);
    outputsLayout->addWidget(box, row, col);
    if(++col == 3)
    {
      col = 0;
      ++row;
    }
  };
  addOutput(m_outputs.accel, tr("Acceleration"), true);
  addOutput(m_outputs.gyro, tr("Gyroscope"), true);
  addOutput(m_outputs.mag, tr("Magnetometer"), false);
  addOutput(m_outputs.quaternion, tr("Quaternion"), true);
  addOutput(m_outputs.euler, tr("Euler angles"), true);
  addOutput(m_outputs.angularVelocity, tr("Angular velocity"), false);
  addOutput(m_outputs.linearAccel, tr("Linear acceleration"), false);
  addOutput(m_outputs.pressure, tr("Pressure"), false);
  addOutput(m_outputs.altitude, tr("Altitude"), false);
  addOutput(m_outputs.temperature, tr("Temperature"), false);
  addOutput(m_outputs.heave, tr("Heave"), false);

  auto* layout = new QFormLayout;
  layout->addRow(tr("Name"), m_deviceNameEdit);
  layout->addRow(tr("Serial number"), m_serial);
  layout->addRow(tr("Port"), m_identifier);
  layout->addRow(tr("IO system"), m_ioType);
  layout->addRow(tr("Baud rate"), m_baudRate);
  layout->addRow(QString{}, m_matchBySerial);
  layout->addRow(tr("Sampling rate"), m_samplingRate);
  layout->addRow(tr("Filter mode"), m_filterMode);
  layout->addRow(QString{}, m_degrees);
  layout->addRow(QString{}, m_autoOutputs);
  layout->addRow(outputs);
  layout->addRow(QString{}, m_autoReconnect);
  layout->addRow(tr("Link timeout"), m_watchdog);
  layout->addRow(tr("Update rate"), m_rate);

  setLayout(layout);
}

ProtocolSettingsWidget::~ProtocolSettingsWidget() = default;

Device::DeviceSettings ProtocolSettingsWidget::getSettings() const
{
  Device::DeviceSettings s;
  s.name = m_deviceNameEdit->text();
  s.protocol = ProtocolFactory::static_concreteKey();

  SpecificSettings specif;
  specif.ioType = m_ioType->currentText();
  specif.serialNumber = m_serial->text();
  specif.identifier = m_identifier->text();
  specif.baudRate = m_baudRate->currentText().toInt();
  specif.matchBySerial = m_matchBySerial->isChecked();
  specif.samplingRate = m_samplingRate->value();
  specif.filterMode = m_filterMode->value();
  specif.degrees = m_degrees->isChecked();
  specif.autoOutputs = m_autoOutputs->isChecked();
  specif.autoReconnect = m_autoReconnect->isChecked();
  specif.watchdogMs = m_watchdog->value();
  if(const int r = m_rate->value(); r > 0)
    specif.rate = r;

  specif.outputs.accel = m_outputs.accel->isChecked();
  specif.outputs.gyro = m_outputs.gyro->isChecked();
  specif.outputs.mag = m_outputs.mag->isChecked();
  specif.outputs.quaternion = m_outputs.quaternion->isChecked();
  specif.outputs.euler = m_outputs.euler->isChecked();
  specif.outputs.angularVelocity = m_outputs.angularVelocity->isChecked();
  specif.outputs.linearAccel = m_outputs.linearAccel->isChecked();
  specif.outputs.pressure = m_outputs.pressure->isChecked();
  specif.outputs.altitude = m_outputs.altitude->isChecked();
  specif.outputs.temperature = m_outputs.temperature->isChecked();
  specif.outputs.heave = m_outputs.heave->isChecked();

  s.deviceSpecificSettings = QVariant::fromValue(specif);
  return s;
}

void ProtocolSettingsWidget::setSettings(const Device::DeviceSettings& settings)
{
  m_deviceNameEdit->setText(settings.name);

  const auto& specif = settings.deviceSpecificSettings.value<SpecificSettings>();
  m_ioType->setCurrentText(specif.ioType);
  m_serial->setText(specif.serialNumber);
  m_identifier->setText(specif.identifier);
  m_baudRate->setCurrentText(
      specif.baudRate > 0 ? QString::number(specif.baudRate) : tr("Auto"));
  m_matchBySerial->setChecked(specif.matchBySerial);
  m_samplingRate->setValue(specif.samplingRate);
  m_filterMode->setValue(specif.filterMode);
  m_degrees->setChecked(specif.degrees);
  m_autoOutputs->setChecked(specif.autoOutputs);
  m_outputsBox->setDisabled(specif.autoOutputs);
  m_autoReconnect->setChecked(specif.autoReconnect);
  m_watchdog->setValue(specif.watchdogMs);
  m_rate->setValue(specif.rate ? *specif.rate : 0);

  m_outputs.accel->setChecked(specif.outputs.accel);
  m_outputs.gyro->setChecked(specif.outputs.gyro);
  m_outputs.mag->setChecked(specif.outputs.mag);
  m_outputs.quaternion->setChecked(specif.outputs.quaternion);
  m_outputs.euler->setChecked(specif.outputs.euler);
  m_outputs.angularVelocity->setChecked(specif.outputs.angularVelocity);
  m_outputs.linearAccel->setChecked(specif.outputs.linearAccel);
  m_outputs.pressure->setChecked(specif.outputs.pressure);
  m_outputs.altitude->setChecked(specif.outputs.altitude);
  m_outputs.temperature->setChecked(specif.outputs.temperature);
  m_outputs.heave->setChecked(specif.outputs.heave);
}
}
