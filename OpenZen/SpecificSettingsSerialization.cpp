#include "SpecificSettings.hpp"

#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>

template <>
void DataStreamReader::read(const OpenZen::OutputSettings& n)
{
  m_stream << n.accel << n.gyro << n.mag << n.quaternion << n.euler
           << n.angularVelocity << n.linearAccel << n.pressure << n.altitude
           << n.temperature << n.heave;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(OpenZen::OutputSettings& n)
{
  m_stream >> n.accel >> n.gyro >> n.mag >> n.quaternion >> n.euler
      >> n.angularVelocity >> n.linearAccel >> n.pressure >> n.altitude
      >> n.temperature >> n.heave;
  checkDelimiter();
}

template <>
void JSONReader::read(const OpenZen::OutputSettings& n)
{
  stream.StartObject();
  obj["Quaternion"] = n.quaternion;
  obj["Euler"] = n.euler;
  obj["Accel"] = n.accel;
  obj["Gyro"] = n.gyro;
  obj["Mag"] = n.mag;
  obj["AngularVelocity"] = n.angularVelocity;
  obj["LinearAccel"] = n.linearAccel;
  obj["Pressure"] = n.pressure;
  obj["Altitude"] = n.altitude;
  obj["Temperature"] = n.temperature;
  obj["Heave"] = n.heave;
  stream.EndObject();
}

template <>
void JSONWriter::write(OpenZen::OutputSettings& n)
{
  const auto get = [this](const std::string& k, bool& target) {
    if(auto v = obj.tryGet(k))
      target = v->toBool();
  };
  get("Quaternion", n.quaternion);
  get("Euler", n.euler);
  get("Accel", n.accel);
  get("Gyro", n.gyro);
  get("Mag", n.mag);
  get("AngularVelocity", n.angularVelocity);
  get("LinearAccel", n.linearAccel);
  get("Pressure", n.pressure);
  get("Altitude", n.altitude);
  get("Temperature", n.temperature);
  get("Heave", n.heave);
}

template <>
void DataStreamReader::read(const OpenZen::SpecificSettings& n)
{
  m_stream << n.ioType << n.serialNumber << n.identifier << n.baudRate
           << n.matchBySerial << n.samplingRate << n.filterMode << n.degrees << n.outputs
           << n.autoReconnect << n.watchdogMs << n.rate;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(OpenZen::SpecificSettings& n)
{
  m_stream >> n.ioType >> n.serialNumber >> n.identifier >> n.baudRate >> n.matchBySerial
      >> n.samplingRate >> n.filterMode >> n.degrees >> n.outputs >> n.autoReconnect
      >> n.watchdogMs >> n.rate;
  checkDelimiter();
}

template <>
void JSONReader::read(const OpenZen::SpecificSettings& n)
{
  obj["IoType"] = n.ioType;
  obj["SerialNumber"] = n.serialNumber;
  obj["Identifier"] = n.identifier;
  obj["BaudRate"] = n.baudRate;
  obj["MatchBySerial"] = n.matchBySerial;
  obj["SamplingRate"] = n.samplingRate;
  obj["FilterMode"] = n.filterMode;
  obj["Degrees"] = n.degrees;
  obj["Outputs"] = n.outputs;
  obj["AutoReconnect"] = n.autoReconnect;
  obj["WatchdogMs"] = n.watchdogMs;
  if(n.rate)
    obj["Rate"] = *n.rate;
}

template <>
void JSONWriter::write(OpenZen::SpecificSettings& n)
{
  n.ioType <<= obj["IoType"];
  n.serialNumber <<= obj["SerialNumber"];
  n.identifier <<= obj["Identifier"];
  n.baudRate <<= obj["BaudRate"];
  if(auto v = obj.tryGet("MatchBySerial"))
    n.matchBySerial = v->toBool();
  n.samplingRate <<= obj["SamplingRate"];
  n.filterMode <<= obj["FilterMode"];
  if(auto v = obj.tryGet("Degrees"))
    n.degrees = v->toBool();
  if(auto v = obj.tryGet("Outputs"))
    n.outputs <<= *v;
  if(auto v = obj.tryGet("AutoReconnect"))
    n.autoReconnect = v->toBool();
  n.watchdogMs <<= obj["WatchdogMs"];
  if(auto v = obj.tryGet("Rate"))
    n.rate = v->toInt();
}
