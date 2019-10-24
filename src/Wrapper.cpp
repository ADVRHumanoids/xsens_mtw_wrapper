// Internal dependencies
#include "xsens_mtw/Wrapper.h"

bool hiros::xsens_mtw::Wrapper::s_request_shutdown = false;

hiros::xsens_mtw::Wrapper::Wrapper()
  : m_number_of_connected_mtws(0)
  , m_xsens_mtw_configured(false)
  , m_nh("~")
  , m_node_namespace(m_nh.getNamespace())
{
  struct sigaction sig_act;
  memset(&sig_act, 0, sizeof(sig_act));
  sig_act.sa_handler = hiros::xsens_mtw::Wrapper::sighandler;
  sigaction(SIGINT, &sig_act, nullptr);
}

void hiros::xsens_mtw::Wrapper::start()
{
  ROS_INFO_STREAM("Xsens Mtw Wrapper... Starting");

  do {
    if (!m_xsens_mtw_configured) {
      if (!configure()) {
        ros::shutdown();
      }
    }
    else {
      ROS_WARN_STREAM("Xsens Mtw Wrapper... Trying to reduce the update rate");

      stopXsensMtw();

      unsigned long up_rate_index = static_cast<unsigned long>(m_supported_update_rates.find(m_update_rate));

      if (up_rate_index == m_supported_update_rates.size() - 1) {
        ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to go to measurement mode");
        ros::shutdown();
      }

      m_mtw_params.desired_update_rate = m_supported_update_rates.at(++up_rate_index);

      configureXsensMtw();
    }

    if (!waitMtwConnection()) {
      ros::shutdown();
    }

    if (!getMtwsDeviceIstances()) {
      ros::shutdown();
    }

    attachCallbackHandlers();
  } while (!startMeasurement());

  setSampleTimeEpsilon();
  initializeTimeMaps();
  setupRos();

  if (m_mtw_params.reset_initial_orientation) {
    resetInitialOrientation();
  }
}

void hiros::xsens_mtw::Wrapper::run()
{
  ROS_INFO_STREAM(BASH_MSG_GREEN << "Xsens Mtw Wrapper... RUNNING" << BASH_MSG_RESET);

  while (ros::ok() && !s_request_shutdown) {
    for (size_t i = 0; i < m_number_of_connected_mtws; ++i) {
      if (m_mtw_callbacks.at(i)->dataAvailable()) {
        m_packet = m_mtw_callbacks.at(i)->getOldestPacket();
        computeSampleTime();

        if (m_wrapper_params.publish_imu) {
          m_imu_pub.at(i).publish(getImuMsg());
        }

        if (m_wrapper_params.publish_acceleration) {
          m_acceleration_pub.at(i).publish(getAccelerationMsg());
        }

        if (m_wrapper_params.publish_angular_velocity) {
          m_angular_velocity_pub.at(i).publish(getAngularVelocityMsg());
        }

        if (m_wrapper_params.publish_mag) {
          m_mag_pub.at(i).publish(getMagMsg());
        }

        if (m_wrapper_params.publish_euler) {
          m_euler_pub.at(i).publish(getEulerMsg());
        }

        if (m_wrapper_params.publish_quaternion) {
          m_quaternion_pub.at(i).publish(getQuaternionMsg());
        }

        if (m_wrapper_params.publish_free_acceleration) {
          m_free_acceleration_pub.at(i).publish(getFreeAccelerationMsg());
        }

        if (m_wrapper_params.publish_pressure) {
          m_pressure_pub.at(i).publish(getPressureMsg());
        }

        if (m_wrapper_params.publish_tf) {
          if (m_packet->containsOrientation()) {
            m_tf_broadcaster.sendTransform(getTf());
          }
        }

        m_mtw_callbacks.at(i)->deleteOldestPacket();
        ros::spinOnce();
      }
    }
  }

  stop();
}

bool hiros::xsens_mtw::Wrapper::configure()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Configuring");

  if (m_xsens_mtw_configured) {
    m_xsens_mtw_configured = false;
    stop();
  }

  configureWrapper();
  m_xsens_mtw_configured = configureXsensMtw();

  if (m_xsens_mtw_configured) {
    ROS_DEBUG_STREAM("Xsens Mtw Wrapper... CONFIGURED");
  }

  return m_xsens_mtw_configured;
}

void hiros::xsens_mtw::Wrapper::stop()
{
  ROS_INFO_STREAM("Xsens Mtw Wrapper... Stopping");

  stopXsensMtw();
  stopWrapper();

  ROS_INFO_STREAM(BASH_MSG_GREEN << "Xsens Mtw Wrapper... STOPPED" << BASH_MSG_RESET);

  ros::shutdown();
}

void hiros::xsens_mtw::Wrapper::configureWrapper()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Configuring Wrapper");

  m_nh.getParam("desired_update_rate", m_mtw_params.desired_update_rate);
  m_nh.getParam("desired_radio_channel", m_mtw_params.desired_radio_channel);

  m_nh.getParam("reset_initial_orientation", m_mtw_params.reset_initial_orientation);

  m_nh.getParam("enable_custom_labeling", m_wrapper_params.enable_custom_labeling);

  m_nh.getParam("publish_imu", m_wrapper_params.publish_imu);
  m_nh.getParam("publish_acceleration", m_wrapper_params.publish_acceleration);
  m_nh.getParam("publish_angular_velocity", m_wrapper_params.publish_angular_velocity);
  m_nh.getParam("publish_mag", m_wrapper_params.publish_mag);
  m_nh.getParam("publish_euler", m_wrapper_params.publish_euler);
  m_nh.getParam("publish_quaternion", m_wrapper_params.publish_quaternion);
  m_nh.getParam("publish_free_acceleration", m_wrapper_params.publish_free_acceleration);
  m_nh.getParam("publish_pressure", m_wrapper_params.publish_pressure);
  m_nh.getParam("publish_tf", m_wrapper_params.publish_tf);

  bool nothing_to_publish =
    (!m_wrapper_params.publish_imu && !m_wrapper_params.publish_acceleration
     && !m_wrapper_params.publish_angular_velocity && !m_wrapper_params.publish_mag && !m_wrapper_params.publish_euler
     && !m_wrapper_params.publish_quaternion && !m_wrapper_params.publish_free_acceleration
     && !m_wrapper_params.publish_pressure && !m_wrapper_params.publish_tf);

  if (nothing_to_publish) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Nothing to publish. Closing");
    ros::shutdown();
  }

  if (m_wrapper_params.enable_custom_labeling) {
    XmlRpc::XmlRpcValue xml_sensor_labels;
    if (m_nh.getParam("sensor_labels", xml_sensor_labels)) {

      for (int i = 0; i < xml_sensor_labels.size(); ++i) {
        m_ids_to_labels.emplace(utils::toXsDeviceId(xml_sensor_labels[i]["imu_id"]), xml_sensor_labels[i]["label"]);
        m_labels_to_ids.emplace(xml_sensor_labels[i]["label"], utils::toXsDeviceId(xml_sensor_labels[i]["imu_id"]));
      }
    }
  }
}

bool hiros::xsens_mtw::Wrapper::configureXsensMtw()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Configuring Xsens Mtw");

  bool success = constructControl();
  success = success && findWirelessMaster();
  success = success && openPort();
  success = success && getXsdeviceInstance();
  success = success && setConfigMode();
  attachCallbackHandler();
  success = success && getClosestUpdateRate();
  success = success && setUpdateRate();
  success = success && setRadioChannel();

  if (!success) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to configure Xsens Mtw");
  }

  return success;
}

void hiros::xsens_mtw::Wrapper::stopXsensMtw()
{
  if (!setConfigMode()) {
    ros::shutdown();
  }

  if (!disableRadio()) {
    ros::shutdown();
  }

  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Closing XsControl");
  m_control->close();

  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Deleting MTW callbacks");
  for (auto& mtw_callback : m_mtw_callbacks) {
    delete (mtw_callback);
  }
  m_mtw_callbacks.clear();

  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Clearing MTW devices");
  m_connected_devices.clear();
}

void hiros::xsens_mtw::Wrapper::stopWrapper()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Shutting down ROS publishers");

  if (m_wrapper_params.publish_imu) {
    for (auto& pub : m_imu_pub) {
      if (pub) {
        pub.shutdown();
      }
    }
  }

  if (m_wrapper_params.publish_acceleration) {
    for (auto& pub : m_acceleration_pub) {
      if (pub) {
        pub.shutdown();
      }
    }
  }

  if (m_wrapper_params.publish_angular_velocity) {
    for (auto& pub : m_angular_velocity_pub) {
      if (pub) {
        pub.shutdown();
      }
    }
  }

  if (m_wrapper_params.publish_mag) {
    for (auto& pub : m_mag_pub) {
      if (pub) {
        pub.shutdown();
      }
    }
  }

  if (m_wrapper_params.publish_euler) {
    for (auto& pub : m_euler_pub) {
      if (pub) {
        pub.shutdown();
      }
    }
  }

  if (m_wrapper_params.publish_quaternion) {
    for (auto& pub : m_quaternion_pub) {
      if (pub) {
        pub.shutdown();
      }
    }
  }

  if (m_wrapper_params.publish_free_acceleration) {
    for (auto& pub : m_free_acceleration_pub) {
      if (pub) {
        pub.shutdown();
      }
    }
  }

  if (m_wrapper_params.publish_pressure) {
    for (auto& pub : m_pressure_pub) {
      if (pub) {
        pub.shutdown();
      }
    }
  }

  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Shutting down ROS service server");
  m_reset_orientation_srv.shutdown();

  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Clearing label maps");
  m_ids_to_labels.clear();
  m_labels_to_ids.clear();
}

bool hiros::xsens_mtw::Wrapper::waitMtwConnection()
{
  ROS_INFO_STREAM("Xsens Mtw Wrapper... Waiting for MTW to wirelessly connect");

  XsTime::msleep(m_connection_timeout);

  m_number_of_connected_mtws = m_wireless_master_callback.getWirelessMTWs().size();
  ROS_INFO_STREAM("Xsens Mtw Wrapper... Number of connected MTWs: " << m_number_of_connected_mtws);

  if (m_number_of_connected_mtws == 0) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to connect to MTWs");
    return false;
  }

  return true;
}

bool hiros::xsens_mtw::Wrapper::getMtwsDeviceIstances()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Getting XsDevice instances for all MTWs");

  for (auto& xs_device_id : m_control->deviceIds()) {
    if (xs_device_id.isMtw()) {
      XsDevicePtr xs_device = m_control->device(xs_device_id);
      if (xs_device != nullptr) {
        m_connected_devices.emplace(xs_device_id, xs_device);
      }
      else {
        ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to create an MTW XsDevice instance");
        return false;
      }
    }
  }

  return true;
}

void hiros::xsens_mtw::Wrapper::attachCallbackHandlers()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Attaching callback handlers to MTWs");

  int mtw_index = 0;
  for (auto& device : m_connected_devices) {
    m_mtw_callbacks.push_back(new MtwCallback(mtw_index++, device.second));
    device.second->addCallbackHandler(m_mtw_callbacks.back());
  }
}

bool hiros::xsens_mtw::Wrapper::startMeasurement()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Starting measurement");

  if (!m_wireless_master_device->gotoMeasurement()) {
    ROS_WARN_STREAM("Xsens Mtw Wrapper... Failed to go to measurement mode");
    return false;
  }

  return true;
}

bool hiros::xsens_mtw::Wrapper::constructControl()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Constructing XsControl");

  m_control = XsControl::construct();

  if (m_control == nullptr) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to construct XsControl instance");
    return false;
  }

  return true;
}

bool hiros::xsens_mtw::Wrapper::findWirelessMaster()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Scanning ports");

  m_detected_devices = XsScanner::scanPorts();

  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Finding wireless master");

  m_wireless_master_port = m_detected_devices.begin();
  while (m_wireless_master_port != m_detected_devices.end() && !m_wireless_master_port->deviceId().isWirelessMaster()) {
    ++m_wireless_master_port;
  }

  if (m_wireless_master_port == m_detected_devices.end()) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... No wireless masters found");
    return false;
  }

  ROS_INFO_STREAM("Xsens Mtw Wrapper... Wireless master found @ " << *m_wireless_master_port);
  return true;
}

bool hiros::xsens_mtw::Wrapper::openPort()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Opening port");

  if (!m_control->openPort(m_wireless_master_port->portName().toStdString(), m_wireless_master_port->baudrate())) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to open port " << *m_wireless_master_port);
    return false;
  }

  return true;
}

bool hiros::xsens_mtw::Wrapper::getXsdeviceInstance()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Getting XsDevice instance for wireless master");

  m_wireless_master_device = m_control->device(m_wireless_master_port->deviceId());
  if (m_wireless_master_device == nullptr) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to construct XsDevice instance: " << *m_wireless_master_port);
    return false;
  }

  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... XsDevice instance created @ " << utils::toString(*m_wireless_master_device));
  return true;
}

bool hiros::xsens_mtw::Wrapper::setConfigMode()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Setting config mode");

  if (!m_wireless_master_device->gotoConfig()) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to go to config mode: " + utils::toString(*m_wireless_master_device));
    return false;
  }

  return true;
}

void hiros::xsens_mtw::Wrapper::attachCallbackHandler()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Attaching callback handler");

  m_wireless_master_device->addCallbackHandler(&m_wireless_master_callback);
}

bool hiros::xsens_mtw::Wrapper::getClosestUpdateRate()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Getting the list of the supported update rates");

  m_supported_update_rates = m_wireless_master_device->supportedUpdateRates();

  std::string info_str = "Xsens Mtw Wrapper... Supported update rates:";
  for (auto& up_rate : m_supported_update_rates) {
    info_str += " " + std::to_string(up_rate);
  }
  ROS_INFO_STREAM(info_str);

  if (m_supported_update_rates.empty()) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to get supported update rates");
    return false;
  }

  if (m_supported_update_rates.size() == 1) {
    return m_supported_update_rates.at(0);
  }

  int u_rate_dist = -1;
  int closest_update_rate = -1;

  for (auto& up_rate : m_supported_update_rates) {
    const int curr_dist = std::abs(up_rate - m_mtw_params.desired_update_rate);

    if ((u_rate_dist == -1) || (curr_dist < u_rate_dist)) {
      u_rate_dist = curr_dist;
      closest_update_rate = up_rate;
    }
  }

  m_update_rate = closest_update_rate;

  return true;
}

bool hiros::xsens_mtw::Wrapper::setUpdateRate()
{
  ROS_INFO_STREAM(BASH_MSG_GREEN << "Xsens Mtw Wrapper... Setting update rate to " << m_update_rate << " Hz"
                                 << BASH_MSG_RESET);

  if (!m_wireless_master_device->setUpdateRate(m_update_rate)) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to set update rate: " << utils::toString(*m_wireless_master_device));
    return false;
  }

  return true;
}

bool hiros::xsens_mtw::Wrapper::setRadioChannel()
{
  if (m_wireless_master_device->isRadioEnabled()) {
    ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Disabling previously enabled radio channel");
    if (!disableRadio()) {
      return false;
    }
  }

  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Setting radio channel to " << m_mtw_params.desired_radio_channel
                                                                    << " and enabling radio");
  if (!m_wireless_master_device->enableRadio(m_mtw_params.desired_radio_channel)) {
    ROS_FATAL_STREAM(
      "Xsens Mtw Wrapper... Failed to set radio channel: " << utils::toString(*m_wireless_master_device));
    return false;
  }

  return true;
}

bool hiros::xsens_mtw::Wrapper::disableRadio()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Disabling radio");

  if (!m_wireless_master_device->disableRadio()) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to disable radio: " << utils::toString(*m_wireless_master_device));
    return false;
  }

  return true;
}

void hiros::xsens_mtw::Wrapper::initializeTimeMaps()
{
  for (auto& device : m_connected_devices) {
    m_prev_packet_time_of_arrival.emplace(device.first, 0);
    m_prev_packet_sample_time.emplace(device.first, ros::Time(0));
  }
}

void hiros::xsens_mtw::Wrapper::setupRos()
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Setting up ROS");

  m_reset_orientation_srv = m_nh.advertiseService("reset_orientation", &Wrapper::resetOrientation, this);

  for (auto& device : m_connected_devices) {
    if (m_wrapper_params.publish_imu) {
      m_imu_pub.push_back(m_nh.advertise<sensor_msgs::Imu>(composeTopicPrefix(device.first) + "/imu/data", 1));
    }

    if (m_wrapper_params.publish_acceleration) {
      m_acceleration_pub.push_back(
        m_nh.advertise<geometry_msgs::Vector3Stamped>(composeTopicPrefix(device.first) + "/imu/acceleration", 1));
    }

    if (m_wrapper_params.publish_angular_velocity) {
      m_angular_velocity_pub.push_back(
        m_nh.advertise<geometry_msgs::Vector3Stamped>(composeTopicPrefix(device.first) + "/imu/angular_velocity", 1));
    }

    if (m_wrapper_params.publish_mag) {
      m_mag_pub.push_back(m_nh.advertise<sensor_msgs::MagneticField>(composeTopicPrefix(device.first) + "/imu/mag", 1));
    }

    if (m_wrapper_params.publish_euler) {
      m_euler_pub.push_back(
        m_nh.advertise<hiros_xsens_mtw_wrapper::Euler>(composeTopicPrefix(device.first) + "/imu/euler", 1));
    }

    if (m_wrapper_params.publish_quaternion) {
      m_quaternion_pub.push_back(
        m_nh.advertise<geometry_msgs::QuaternionStamped>(composeTopicPrefix(device.first) + "/filter/quaternion", 1));
    }

    if (m_wrapper_params.publish_free_acceleration) {
      m_free_acceleration_pub.push_back(m_nh.advertise<geometry_msgs::Vector3Stamped>(
        composeTopicPrefix(device.first) + "/filter/free_acceleration", 1));
    }

    if (m_wrapper_params.publish_pressure) {
      m_pressure_pub.push_back(
        m_nh.advertise<sensor_msgs::FluidPressure>(composeTopicPrefix(device.first) + "/pressure", 1));
    }
  }
}

bool hiros::xsens_mtw::Wrapper::resetInitialOrientation() const
{
  ROS_DEBUG_STREAM("Xsens Mtw Wrapper... Resetting initial orientation");

  bool success = true;

  for (auto& device : m_connected_devices) {
    success = device.second->resetOrientation(XRM_Heading) && success;
  }

  if (!success) {
    ROS_FATAL_STREAM("Xsens Mtw Wrapper... Failed to reset initial orientation");
  }

  return success;
}

std::string hiros::xsens_mtw::Wrapper::getDeviceLabel(const XsDeviceId& t_id) const
{
  return (m_ids_to_labels.find(t_id) != m_ids_to_labels.end()) ? m_ids_to_labels.at(t_id)
                                                               : t_id.toString().toStdString();
}

XsDeviceId hiros::xsens_mtw::Wrapper::getDeviceId(const std::string t_label) const
{
  return (m_labels_to_ids.find(t_label) != m_labels_to_ids.end()) ? m_labels_to_ids.at(t_label)
                                                                  : utils::toXsDeviceId(t_label);
}

std::string hiros::xsens_mtw::Wrapper::composeTopicPrefix(const XsDeviceId& t_id) const
{
  return "/" + m_node_namespace + "/" + getDeviceLabel(t_id);
}

void hiros::xsens_mtw::Wrapper::computeSampleTime()
{
  double delta_time_of_arrival =
    (m_packet->timeOfArrival() - m_prev_packet_time_of_arrival.at(m_packet->deviceId())).secTime();

  // same timeOfArrival
  if (delta_time_of_arrival < (0.5 / m_update_rate)) {
    m_sample_time = m_prev_packet_sample_time.at(m_packet->deviceId()) + ros::Duration(1.0 / m_update_rate);
  }
  // new timeOfArrival
  else {
    m_sample_time = (m_packet->timeOfArrival().secTime() > m_prev_packet_sample_time.at(m_packet->deviceId()).toSec())
                      ? ros::Time(m_packet->timeOfArrival().secTime())
                      : ros::Time(m_prev_packet_sample_time.at(m_packet->deviceId()).toSec() + m_sample_time_epsilon);
  }

  m_prev_packet_time_of_arrival.at(m_packet->deviceId()) = m_packet->timeOfArrival();
  m_prev_packet_sample_time.at(m_packet->deviceId()) = m_sample_time;
}

sensor_msgs::Imu hiros::xsens_mtw::Wrapper::getImuMsg() const
{
  sensor_msgs::Imu out_msg;
  out_msg.header.stamp = m_sample_time;
  out_msg.header.frame_id = getDeviceLabel(m_packet->deviceId());

  if (m_packet->containsOrientation()) {
    out_msg.orientation.x = m_packet->orientationQuaternion().x();
    out_msg.orientation.y = m_packet->orientationQuaternion().y();
    out_msg.orientation.z = m_packet->orientationQuaternion().z();
    out_msg.orientation.w = m_packet->orientationQuaternion().w();
    out_msg.orientation_covariance.front() = 0.0;
  }
  else {
    out_msg.orientation.x = std::numeric_limits<double>::quiet_NaN();
    out_msg.orientation.y = std::numeric_limits<double>::quiet_NaN();
    out_msg.orientation.z = std::numeric_limits<double>::quiet_NaN();
    out_msg.orientation.w = std::numeric_limits<double>::quiet_NaN();
    out_msg.orientation_covariance.front() = -1.0;
  }

  if (m_packet->containsCalibratedGyroscopeData()) {
    out_msg.angular_velocity.x = m_packet->calibratedGyroscopeData().at(0);
    out_msg.angular_velocity.y = m_packet->calibratedGyroscopeData().at(1);
    out_msg.angular_velocity.z = m_packet->calibratedGyroscopeData().at(2);
    out_msg.angular_velocity_covariance.front() = 0.0;
  }
  else {
    out_msg.angular_velocity.x = std::numeric_limits<double>::quiet_NaN();
    out_msg.angular_velocity.y = std::numeric_limits<double>::quiet_NaN();
    out_msg.angular_velocity.z = std::numeric_limits<double>::quiet_NaN();
    out_msg.angular_velocity_covariance.front() = -1.0;
  }

  if (m_packet->containsCalibratedAcceleration()) {
    out_msg.linear_acceleration.x = m_packet->calibratedAcceleration().at(0);
    out_msg.linear_acceleration.y = m_packet->calibratedAcceleration().at(1);
    out_msg.linear_acceleration.z = m_packet->calibratedAcceleration().at(2);
    out_msg.linear_acceleration_covariance.front() = 0.0;
  }
  else {
    out_msg.linear_acceleration.x = std::numeric_limits<double>::quiet_NaN();
    out_msg.linear_acceleration.y = std::numeric_limits<double>::quiet_NaN();
    out_msg.linear_acceleration.z = std::numeric_limits<double>::quiet_NaN();
    out_msg.linear_acceleration_covariance.front() = -1.0;
  }

  return out_msg;
}

geometry_msgs::Vector3Stamped hiros::xsens_mtw::Wrapper::getAccelerationMsg() const
{
  geometry_msgs::Vector3Stamped out_msg;
  out_msg.header.stamp = m_sample_time;
  out_msg.header.frame_id = getDeviceLabel(m_packet->deviceId());

  if (m_packet->containsCalibratedAcceleration()) {
    out_msg.vector.x = m_packet->calibratedAcceleration().at(0);
    out_msg.vector.y = m_packet->calibratedAcceleration().at(1);
    out_msg.vector.z = m_packet->calibratedAcceleration().at(2);
  }
  else {
    out_msg.vector.x = std::numeric_limits<double>::quiet_NaN();
    out_msg.vector.y = std::numeric_limits<double>::quiet_NaN();
    out_msg.vector.z = std::numeric_limits<double>::quiet_NaN();
  }

  return out_msg;
}

geometry_msgs::Vector3Stamped hiros::xsens_mtw::Wrapper::getAngularVelocityMsg() const
{
  geometry_msgs::Vector3Stamped out_msg;
  out_msg.header.stamp = m_sample_time;
  out_msg.header.frame_id = getDeviceLabel(m_packet->deviceId());

  if (m_packet->containsCalibratedGyroscopeData()) {
    out_msg.vector.x = m_packet->calibratedGyroscopeData().at(0);
    out_msg.vector.y = m_packet->calibratedGyroscopeData().at(1);
    out_msg.vector.z = m_packet->calibratedGyroscopeData().at(2);
  }
  else {
    out_msg.vector.x = std::numeric_limits<double>::quiet_NaN();
    out_msg.vector.y = std::numeric_limits<double>::quiet_NaN();
    out_msg.vector.z = std::numeric_limits<double>::quiet_NaN();
  }

  return out_msg;
}

sensor_msgs::MagneticField hiros::xsens_mtw::Wrapper::getMagMsg() const
{
  sensor_msgs::MagneticField out_msg;
  out_msg.header.stamp = m_sample_time;
  out_msg.header.frame_id = getDeviceLabel(m_packet->deviceId());

  if (m_packet->containsCalibratedMagneticField()) {
    out_msg.magnetic_field.x = m_packet->calibratedMagneticField().at(0);
    out_msg.magnetic_field.y = m_packet->calibratedMagneticField().at(1);
    out_msg.magnetic_field.z = m_packet->calibratedMagneticField().at(2);
    out_msg.magnetic_field_covariance.front() = 0.0;
  }
  else {
    out_msg.magnetic_field.x = std::numeric_limits<double>::quiet_NaN();
    out_msg.magnetic_field.y = std::numeric_limits<double>::quiet_NaN();
    out_msg.magnetic_field.z = std::numeric_limits<double>::quiet_NaN();
    out_msg.magnetic_field_covariance.front() = -1.0;
  }

  return out_msg;
}

hiros_xsens_mtw_wrapper::Euler hiros::xsens_mtw::Wrapper::getEulerMsg() const
{
  hiros_xsens_mtw_wrapper::Euler out_msg;
  out_msg.header.stamp = m_sample_time;
  out_msg.header.frame_id = getDeviceLabel(m_packet->deviceId());

  if (m_packet->containsOrientation()) {
    // roll = atan2(2 * (qw * qx + qy * qz), (1 - 2 * (pow(qx, 2) + pow(qy, 2))))
    out_msg.roll = m_packet->orientationEuler().roll();
    // pitch = asin(2 * (qw * qy - qz * qx))
    out_msg.pitch = m_packet->orientationEuler().pitch();
    // yaw = atan2(2 * (qw * qz + qx * qy), (1 - 2 * (pow(qy, 2) + pow(qz, 2))))
    out_msg.yaw = m_packet->orientationEuler().yaw();
  }
  else {
    out_msg.roll = std::numeric_limits<double>::quiet_NaN();
    out_msg.pitch = std::numeric_limits<double>::quiet_NaN();
    out_msg.yaw = std::numeric_limits<double>::quiet_NaN();
  }

  return out_msg;
}

geometry_msgs::QuaternionStamped hiros::xsens_mtw::Wrapper::getQuaternionMsg() const
{
  geometry_msgs::QuaternionStamped out_msg;
  out_msg.header.stamp = m_sample_time;
  out_msg.header.frame_id = getDeviceLabel(m_packet->deviceId());

  if (m_packet->containsOrientation()) {
    out_msg.quaternion.x = m_packet->orientationQuaternion().x();
    out_msg.quaternion.y = m_packet->orientationQuaternion().y();
    out_msg.quaternion.z = m_packet->orientationQuaternion().z();
    out_msg.quaternion.w = m_packet->orientationQuaternion().w();
  }
  else {
    out_msg.quaternion.x = std::numeric_limits<double>::quiet_NaN();
    out_msg.quaternion.y = std::numeric_limits<double>::quiet_NaN();
    out_msg.quaternion.z = std::numeric_limits<double>::quiet_NaN();
    out_msg.quaternion.w = std::numeric_limits<double>::quiet_NaN();
  }

  return out_msg;
}

geometry_msgs::Vector3Stamped hiros::xsens_mtw::Wrapper::getFreeAccelerationMsg() const
{
  geometry_msgs::Vector3Stamped out_msg;
  out_msg.header.stamp = m_sample_time;
  out_msg.header.frame_id = getDeviceLabel(m_packet->deviceId());

  if (m_packet->containsFreeAcceleration()) {
    out_msg.vector.x = m_packet->freeAcceleration().at(0);
    out_msg.vector.y = m_packet->freeAcceleration().at(1);
    out_msg.vector.z = m_packet->freeAcceleration().at(2);
  }
  else {
    out_msg.vector.x = std::numeric_limits<double>::quiet_NaN();
    out_msg.vector.y = std::numeric_limits<double>::quiet_NaN();
    out_msg.vector.z = std::numeric_limits<double>::quiet_NaN();
  }

  return out_msg;
}

sensor_msgs::FluidPressure hiros::xsens_mtw::Wrapper::getPressureMsg() const
{
  sensor_msgs::FluidPressure out_msg;
  out_msg.header.stamp = m_sample_time;
  out_msg.header.frame_id = getDeviceLabel(m_packet->deviceId());

  if (m_packet->containsPressure()) {
    out_msg.fluid_pressure = m_packet->pressure().m_pressure;
    out_msg.variance = 0.0;
  }
  else {
    out_msg.fluid_pressure = std::numeric_limits<double>::quiet_NaN();
    out_msg.variance = -1.0;
  }

  return out_msg;
}

geometry_msgs::TransformStamped hiros::xsens_mtw::Wrapper::getTf() const
{
  geometry_msgs::TransformStamped tf;
  tf.header.stamp = m_sample_time;
  tf.header.frame_id = "world";
  tf.child_frame_id = getDeviceLabel(m_packet->deviceId());

  tf.transform.translation.x = 0.0;
  tf.transform.translation.y = 0.0;
  tf.transform.translation.z = 0.0;
  tf.transform.rotation.x = m_packet->orientationQuaternion().x();
  tf.transform.rotation.y = m_packet->orientationQuaternion().y();
  tf.transform.rotation.z = m_packet->orientationQuaternion().z();
  tf.transform.rotation.w = m_packet->orientationQuaternion().w();

  return tf;
}

bool hiros::xsens_mtw::Wrapper::resetOrientation(hiros_xsens_mtw_wrapper::ResetOrientation::Request& t_req,
                                                 hiros_xsens_mtw_wrapper::ResetOrientation::Response& t_res)
{
  bool success = true;

  if (t_req.sensors.empty()) {
    ROS_INFO_STREAM(BASH_MSG_GREEN << "Xsens Mtw Wrapper... Resetting orientation of all connected sensors"
                                   << BASH_MSG_RESET);
    for (auto& device : m_connected_devices) {
      success = device.second->resetOrientation(XRM_Alignment) && success;
    }
  }
  else {
    for (auto& sensor_label : t_req.sensors) {
      if (m_connected_devices.find(getDeviceId(sensor_label)) != m_connected_devices.end()) {
        ROS_INFO_STREAM(BASH_MSG_GREEN << "Xsens Mtw Wrapper... Resetting orientation of '" << sensor_label << "'"
                                       << BASH_MSG_RESET);
        success = m_connected_devices.at(getDeviceId(sensor_label))->resetOrientation(XRM_Alignment) && success;
      }
      else {
        ROS_WARN_STREAM("Xsens Mtw Wrapper... Cannot find '" << sensor_label << "'");
        success = false;
      }
    }
  }

  return success;
}
