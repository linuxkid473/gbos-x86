/*
 * Minimal stand-in for the xsdc-generated sensor_hal_configuration_V1_0 parser.
 *
 * The sub-HAL only uses the parser to read an optional sensor_hal_configuration.xml
 * from /odm/etc/sensors or /vendor/etc/sensors. Google's desktop images ship none, so
 * the generated parser (which needs xsdc, libxml2 and ICU to build) always returned
 * "no file" on the devices this HAL targets. This header keeps the same types and
 * getters the HAL calls, and read() reports that no configuration exists, which is the
 * behaviour the HAL already handles (each sensor falls back to its built-in profile).
 *
 * If a device ever needs an XML placement file, build with the generated parser
 * instead (build_compact.py uses INPUTS/xsdc-output when it exists).
 */
#ifndef GBOS_SENSOR_HAL_CONFIGURATION_STUB_H
#define GBOS_SENSOR_HAL_CONFIGURATION_STUB_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sensor::hal::configuration::V1_0 {

class Axis {
  public:
    int64_t getMap() const { return map_; }
    bool getNegate() const { return negate_; }

  private:
    int64_t map_ = 0;
    bool negate_ = false;
};

class Orientation {
  public:
    bool getRotate() const { return rotate_; }
    const Axis* getFirstX() const { return x_.empty() ? nullptr : &x_[0]; }
    const Axis* getFirstY() const { return y_.empty() ? nullptr : &y_[0]; }
    const Axis* getFirstZ() const { return z_.empty() ? nullptr : &z_[0]; }

  private:
    bool rotate_ = false;
    std::vector<Axis> x_, y_, z_;
};

class Location {
  public:
    double getX() const { return x_; }
    double getY() const { return y_; }
    double getZ() const { return z_; }

  private:
    double x_ = 0, y_ = 0, z_ = 0;
};

class Configuration {
  public:
    const std::vector<Orientation>& getOrientation() const { return orientation_; }
    const std::vector<Location>& getLocation() const { return location_; }

  private:
    std::vector<Orientation> orientation_;
    std::vector<Location> location_;
};

class Sensor {
  public:
    const std::string& getName() const { return name_; }
    int64_t getType() const { return type_; }
    const std::vector<Configuration>& getConfiguration() const { return configuration_; }

  private:
    std::string name_;
    int64_t type_ = 0;
    std::vector<Configuration> configuration_;
};

class Sensors {
  public:
    const std::vector<Sensor>& getSensor() const { return sensor_; }

  private:
    std::vector<Sensor> sensor_;
};

class Module {
  public:
    const std::string& getHalName() const { return halName_; }
    const Sensors* getFirstSensors() const { return sensors_.empty() ? nullptr : &sensors_[0]; }

  private:
    std::string halName_;
    std::vector<Sensors> sensors_;
};

class Modules {
  public:
    const std::vector<Module>& get_module() const { return module_; }

  private:
    std::vector<Module> module_;
};

class SensorHalConfiguration {
  public:
    const Modules* getFirstModules() const { return modules_.empty() ? nullptr : &modules_[0]; }

  private:
    std::vector<Modules> modules_;
};

/* No XML placement file is parsed by this stand-in: report "not found". */
inline std::optional<SensorHalConfiguration> read(const char*) {
    return std::nullopt;
}

}  // namespace sensor::hal::configuration::V1_0

#endif
