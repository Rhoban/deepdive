/*
  This ROS node creates an instance of the libdeepdive driver, which it uses
  to pull data from all available trackers, as well as lighthouse/tracker info.
*/

// Libsurvive interface
extern "C" {
  #include <deepdive/deepdive.h>
}

// ROS includes
#include <ros/ros.h>

// Standard messages
#include <sensor_msgs/Imu.h>
#include <geometry_msgs/Vector3.h>

// Non-standard messages
#include <deepdive_ros/Event.h>
#include <deepdive_ros/Light.h>
#include <deepdive_ros/Pulse.h>
#include <deepdive_ros/Motor.h>
#include <deepdive_ros/Sensor.h>
#include <deepdive_ros/Lighthouses.h>
#include <deepdive_ros/Trackers.h>

// Services to get tracker/lighthouse/system config
#include <deepdive_ros/GetTracker.h>
#include <deepdive_ros/GetLighthouse.h>

// C++ includes
#include <cstdint>
#include <cmath>
#include <map>
#include <string>
#include <limits>

// Various constants used by the Vive system
static constexpr double GRAVITY         = 9.80665;
static constexpr double GYRO_SCALE      = 32.768;
static constexpr double ACC_SCALE       = 4096.0;
static constexpr double SWEEP_DURATION  = 400000.0;
static constexpr double SWEEP_CENTER    = 200000.0;
static constexpr double TICKS_PER_SEC   = 48e6;

// Data structures for storing lighthouses and trackers
std::map<std::string, deepdive_ros::Lighthouse> lighthouses_;
std::map<std::string, deepdive_ros::Tracker> trackers_;
 
// Create data publishers
ros::Publisher pub_lighthouses_;
ros::Publisher pub_trackers_;
ros::Publisher pub_light_;
ros::Publisher pub_event_;
ros::Publisher pub_imu_;

// Convert time an return overflow count
ros::Time TimeConvert(std::string const& serial, uint32_t timecode) {
  // How many seconds in each overflow cycle of the counter
  static uint32_t ticks_per_period = std::numeric_limits<uint32_t>::max();
  // If we can't find the tracker we initialize its cycle counter
  if (trackers_.find(serial) == trackers_.end()) {
    trackers_[serial].lastcount = 0;
    trackers_[serial].overflows = 1;
  }
  // In most cases we will be in the same cycle as the overflow. However,
  // these two lines advance and pull back the overflow to take care of the
  // fact that there is a bad time-ordering of packets
  if (timecode > trackers_[serial].lastcount &&
      timecode - trackers_[serial].lastcount > ticks_per_period / 2)
      trackers_[serial].overflows--;
  if (trackers_[serial].lastcount > timecode &&
      trackers_[serial].lastcount - timecode > ticks_per_period / 2)
      trackers_[serial].overflows++;
  trackers_[serial].lastcount = timecode;
  // Calculate the time based on the number of overflows and the timecode
  uint64_t tick = static_cast<uint64_t>(trackers_[serial].overflows)
                * static_cast<uint64_t>(ticks_per_period)
                + static_cast<uint64_t>(timecode);
  int32_t secs = static_cast<int32_t>(tick / 48000000ULL);
  int32_t nsec = static_cast<int32_t>((tick % 48000000ULL) * 1000 / 48);
  // Add the tick store to the time code and convert to seconds
  return ros::Time(secs, nsec);
}

// Callback to display light info
void LightCallback(struct Tracker * tracker,
  struct Lighthouse * lighthouse, uint8_t axis, uint32_t synctime,
  uint16_t num_sensors, uint16_t *sensors, uint32_t *sweeptimes,
  uint32_t *angles, uint16_t *lengths) {
  static deepdive_ros::Light msg;
  msg.header.frame_id = tracker->serial;
  msg.header.stamp = TimeConvert(tracker->serial, synctime);
  msg.lighthouse = lighthouse->serial;
  msg.axis = axis;
  msg.pulses.resize(num_sensors);
  for (uint16_t i = 0; i < num_sensors; i++) {
    msg.pulses[i].sensor = sensors[i];
    msg.pulses[i].angle = (M_PI / SWEEP_DURATION)
      * (static_cast<double>(angles[i]) - SWEEP_CENTER);
    msg.pulses[i].duration =
      static_cast<double>(lengths[i]) / TICKS_PER_SEC;
  }
  // Publish the data
  pub_light_.publish(msg);
}

// Called back when new IMU data is available
void ImuCallback(struct Tracker * tracker, uint32_t timecode,
  int16_t acc[3], int16_t gyr[3], int16_t mag[3]) {
  // Package up the IMU data
  static sensor_msgs::Imu msg;
  msg.header.frame_id = tracker->serial;
  msg.header.stamp = TimeConvert(tracker->serial, timecode);
  msg.linear_acceleration.x =
    static_cast<double>(acc[0]) * GRAVITY / ACC_SCALE;
  msg.linear_acceleration.y =
    static_cast<double>(acc[1]) * GRAVITY / ACC_SCALE;
  msg.linear_acceleration.z =
    static_cast<double>(acc[2]) * GRAVITY / ACC_SCALE;
  msg.angular_velocity.x =
    static_cast<double>(gyr[0]) * (1./GYRO_SCALE) * (M_PI/180.);
  msg.angular_velocity.y =
    static_cast<double>(gyr[1]) * (1./GYRO_SCALE) * (M_PI/180.);
  msg.angular_velocity.z =
    static_cast<double>(gyr[2]) * (1./GYRO_SCALE) * (M_PI/180.);
  // Publish the data
  pub_imu_.publish(msg);
}

// Called when a button is pressed
void ButtonCallback(struct Tracker * tracker, uint32_t timecode,
  uint8_t mask) {
  // Package up the IMU data
  static deepdive_ros::Event msg;
  msg.header.frame_id = tracker->serial;
  msg.header.stamp = TimeConvert(tracker->serial, timecode);
  msg.mask = mask;
  // Publish the data
  pub_event_.publish(msg);
}

// Configuration call from the vive_tool
void TrackerCallback(struct Tracker * t) {
  if (!t) return;
  deepdive_ros::Tracker & tracker = trackers_[t->serial];
  tracker.serial = t->serial;
  tracker.sensors.resize(t->cal.num_channels);
  for (size_t i = 0; i < t->cal.num_channels; i++) {
    tracker.sensors[i].position.x = t->cal.positions[i][0];
    tracker.sensors[i].position.y = t->cal.positions[i][1];
    tracker.sensors[i].position.z = t->cal.positions[i][2];
    tracker.sensors[i].normal.x = t->cal.normals[i][0];
    tracker.sensors[i].normal.y = t->cal.normals[i][1];
    tracker.sensors[i].normal.z = t->cal.normals[i][2];
  }
  // Set the IMU sensor calibration data
  tracker.acc_bias.x = t->cal.acc_bias[0];
  tracker.acc_bias.y = t->cal.acc_bias[1];
  tracker.acc_bias.z = t->cal.acc_bias[2];
  tracker.acc_scale.x = t->cal.acc_scale[0];
  tracker.acc_scale.y = t->cal.acc_scale[1];
  tracker.acc_scale.z = t->cal.acc_scale[2];
  tracker.gyr_bias.x = t->cal.gyr_bias[0];
  tracker.gyr_bias.y = t->cal.gyr_bias[1];
  tracker.gyr_bias.z = t->cal.gyr_bias[2];
  tracker.gyr_scale.x = t->cal.gyr_scale[0];
  tracker.gyr_scale.y = t->cal.gyr_scale[1];
  tracker.gyr_scale.z = t->cal.gyr_scale[2];
  // Set the default IMU transform
  tracker.imu_transform.rotation.x = t->cal.imu_transform[0];
  tracker.imu_transform.rotation.y = t->cal.imu_transform[1];
  tracker.imu_transform.rotation.z = t->cal.imu_transform[2];
  tracker.imu_transform.rotation.w = t->cal.imu_transform[3];
  tracker.imu_transform.translation.x = t->cal.imu_transform[4];
  tracker.imu_transform.translation.y = t->cal.imu_transform[5];
  tracker.imu_transform.translation.z = t->cal.imu_transform[6];
  // Send all trackers at once
  deepdive_ros::Trackers msg;
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "world";
  std::map<std::string, deepdive_ros::Tracker>::iterator it;
  for (it = trackers_.begin(); it != trackers_.end(); it++)
    msg.trackers.push_back(it->second);
  pub_trackers_.publish(msg);
}

// Configuration call from the vive_tool
void LighthouseCallback(struct Lighthouse *l) {
  if (!l) return;
  deepdive_ros::Lighthouse & lighthouse = lighthouses_[l->serial];
  lighthouse.serial = l->serial;
  lighthouse.motors.resize(2);
  for (size_t i = 0; i < MAX_NUM_MOTORS; i++) {
    lighthouse.motors[i].axis = i;
    lighthouse.motors[i].phase = l->motors[i].phase;
    lighthouse.motors[i].tilt = l->motors[i].tilt;
    lighthouse.motors[i].gibphase = l->motors[i].gibphase;
    lighthouse.motors[i].gibmag = l->motors[i].gibmag;
    lighthouse.motors[i].curve = l->motors[i].curve;
  }
  lighthouse.acceleration.x = l->accel[0];
  lighthouse.acceleration.y = l->accel[1];
  lighthouse.acceleration.z = l->accel[2];
  // Send all lighthouses at once
  deepdive_ros::Lighthouses msg;
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "world";
  std::map<std::string, deepdive_ros::Lighthouse>::iterator it;
  for (it = lighthouses_.begin(); it != lighthouses_.end(); it++)
    msg.lighthouses.push_back(it->second);
  pub_lighthouses_.publish(msg);
}

// Main entry point of application
int main(int argc, char **argv) {
  // Initialize ROS
  ros::init(argc, argv, "deepdive_bridge");
  ros::NodeHandle nh;

  // Latched publishers
  pub_lighthouses_ =
    nh.advertise<deepdive_ros::Lighthouses>("lighthouses", 10, true);
  pub_trackers_ =
    nh.advertise<deepdive_ros::Trackers>("trackers", 10, true);

  // Non-latched
  pub_light_ = nh.advertise<deepdive_ros::Light>("light", 10);
  pub_event_ = nh.advertise<deepdive_ros::Event>("event", 10);
  pub_imu_ = nh.advertise<sensor_msgs::Imu>("imu", 10);

  // Try to initialize vive
  struct Driver *driver = deepdive_init();
  if (!driver) {
    ROS_ERROR("Deepdive initialization failed");
    return 1;
  }

  // Install the callbacks
  deepdive_install_lig_fn(driver, LightCallback);
  deepdive_install_imu_fn(driver, ImuCallback);
  deepdive_install_but_fn(driver, ButtonCallback);
  deepdive_install_lighthouse_fn(driver, LighthouseCallback);
  deepdive_install_tracker_fn(driver, TrackerCallback);

  // Set active to true on initialization
  while (ros::ok()) {
    // Poll the ros driver for activity
    deepdive_poll(driver);
    // Flush the ROS messaging queue
    ros::spinOnce();
  }

  // Close the vive context
  deepdive_close(driver);

  // Success!
  return 0;
}