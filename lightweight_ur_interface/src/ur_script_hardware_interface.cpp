#include <stdlib.h>
#include <stdio.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <functional>
#include <ros/ros.h>
#include <std_srvs/SetBool.h>
#include <sensor_msgs/JointState.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TwistStamped.h>
#include <geometry_msgs/WrenchStamped.h>
#include <lightweight_ur_interface/ur_robot_config.hpp>
#include <lightweight_ur_interface/ur_minimal_realtime_driver.hpp>
#include <lightweight_ur_interface/VelocityCommand.h>
#include <tri_driver_common/print.hpp>
#include <tri_driver_common/math.hpp>
#include <tri_driver_common/conversions.hpp>
#include <tri_driver_common/serialization.hpp>

namespace lightweight_ur_interface
{
using tri_driver_common::conversions::EigenIsometry3dToGeometryPoseStamped;
using tri_driver_common::conversions::EigenVector3dToGeometryVector3;
using tri_driver_common::math::RotateVectorReverse;
using tri_driver_common::math::RotateVector;
using tri_driver_common::utility::ClampValueAndWarn;
using tri_driver_common::serialization::SerializeNetworkMemcpyable;
using tri_driver_common::utility::SetsEqual;
using tri_driver_common::utility::GetKeys;

class URScriptHardwareInterface
{
protected:

  class ControlScriptCommand
  {
  public:

    enum CONTROL_MODE : int32_t { MODE_IDLE=0,
                                  MODE_TEACH=1,
                                  MODE_SPEEDJ=2,
                                  MODE_SPEEDL=3,
                                  MODE_WRENCH=4 };
    enum FORCE_MODE : int32_t { MODE_RIGID=0,
                                MODE_FORCE=1,
                                MODE_DONT_CHANGE=2,
                                MODE_KEEP_LIMITS=3 };

  protected:

    std::vector<double> speed_;
    std::vector<double> wrench_;
    std::vector<double> force_mode_limits_;
    std::vector<int32_t> force_mode_selection_vector_;
    CONTROL_MODE control_mode_;
    FORCE_MODE force_mode_;
    bool running_;

    template<typename T, typename Allocator>
    static uint64_t SerializeKnownSizeVector(
        const std::vector<T, Allocator>& vec_to_serialize,
        std::vector<uint8_t>& buffer,
        const std::function<uint64_t
        (const T&, std::vector<uint8_t>&)>& item_serializer)
    {
      const uint64_t start_buffer_size = buffer.size();
      // Serialize the contained items
      for (size_t idx = 0; idx < vec_to_serialize.size(); idx++)
      {
        const T& current = vec_to_serialize[idx];
        item_serializer(current, buffer);
      }
      // Figure out how many bytes were written
      const uint64_t end_buffer_size = buffer.size();
      const uint64_t bytes_written = end_buffer_size - start_buffer_size;
      return bytes_written;
    }

    static uint64_t SerializeKnownSizeInt32Vector(
        const std::vector<int32_t>& vec_to_serialize,
        std::vector<uint8_t>& buffer)
    {
      return SerializeKnownSizeVector<int32_t>(
            vec_to_serialize, buffer, SerializeNetworkMemcpyable<int32_t>);
    }

  public:

    ControlScriptCommand()
    {
      speed_.resize(6, 0.0);
      wrench_.resize(6, 0.0);
      force_mode_limits_.resize(6, 0.0);
      force_mode_selection_vector_.resize(6, 0);
      control_mode_ = MODE_IDLE;
      force_mode_ = MODE_RIGID;
      running_ = true;
    }

    static int64_t Serialize(const ControlScriptCommand& command,
                             const double float_conversion_ratio,
                             std::vector<uint8_t>& buffer)
    {
      return command.SerializeSelf(float_conversion_ratio, buffer);
    }

    static std::vector<int32_t> ConvertToInt32Vector(
        const std::vector<double>& vector, const double float_conversion_ratio)
    {
      std::vector<int32_t> int32_vector(vector.size());
      for (size_t idx = 0; idx < vector.size(); idx++)
      {
        int32_vector[idx] = (int32_t)(vector[idx] * float_conversion_ratio);
      }
      return int32_vector;
    }

    int64_t SerializeSelf(const double float_conversion_ratio,
                          std::vector<uint8_t>& buffer) const
    {
      const uint64_t start_buffer_size = buffer.size();
      // Serialize the contained items
      SerializeKnownSizeInt32Vector(
            ConvertToInt32Vector(speed_, float_conversion_ratio), buffer);
      SerializeKnownSizeInt32Vector(
            ConvertToInt32Vector(wrench_, float_conversion_ratio), buffer);
      SerializeKnownSizeInt32Vector(
            ConvertToInt32Vector(force_mode_limits_,
                                 float_conversion_ratio), buffer);
      SerializeKnownSizeInt32Vector(force_mode_selection_vector_, buffer);
      SerializeNetworkMemcpyable<int32_t>((int32_t)control_mode_, buffer);
      SerializeNetworkMemcpyable<int32_t>((int32_t)force_mode_, buffer);
      if (running_)
      {
        SerializeNetworkMemcpyable<int32_t>(1, buffer);
      }
      else
      {
        SerializeNetworkMemcpyable<int32_t>(0, buffer);
      }
      // Figure out how many bytes were written
      const uint64_t end_buffer_size = buffer.size();
      const uint64_t bytes_written = end_buffer_size - start_buffer_size;
      return bytes_written;
    }

    static ControlScriptCommand MakeSpeedJCommand(
        const std::vector<double>& joint_velocities)
    {
      ControlScriptCommand command;
      if (joint_velocities.size() != 6)
      {
        throw std::runtime_error("joint_velocities.size() != 6");
      }
      command.speed_ = joint_velocities;
      command.control_mode_ = MODE_SPEEDJ;
      command.force_mode_ = MODE_DONT_CHANGE;
      return command;
    }

    static ControlScriptCommand MakeSpeedLCommand(
        const std::vector<double>& ee_velocities)
    {
      ControlScriptCommand command;
      if (ee_velocities.size() != 6)
      {
        throw std::runtime_error("ee_velocities.size() != 6");
      }
      command.speed_ = ee_velocities;
      command.control_mode_ = MODE_SPEEDL;
      command.force_mode_ = MODE_DONT_CHANGE;
      return command;
    }

    static ControlScriptCommand MakeTeachModeCommand()
    {
      ControlScriptCommand command;
      command.control_mode_ = MODE_TEACH;
      command.force_mode_ = MODE_RIGID;
      return command;
    }

    static ControlScriptCommand MakeExitTeachModeCommand()
    {
      ControlScriptCommand command;
      command.control_mode_ = MODE_IDLE;
      command.force_mode_ = MODE_RIGID;
      return command;
    }

    static ControlScriptCommand MakeForceModeCommand(
        const std::vector<double>& wrench,
        const std::vector<double>& force_mode_limits,
        const std::vector<int32_t>& force_mode_selection_vector)
    {
      ControlScriptCommand command;
      if (wrench.size() != 6)
      {
        throw std::runtime_error("wrench.size() != 6");
      }
      command.wrench_ = wrench;
      if (force_mode_limits.size() != 6)
      {
        throw std::runtime_error("force_mode_limits.size() != 6");
      }
      command.force_mode_limits_ = force_mode_limits;
      if (force_mode_selection_vector.size() != 6)
      {
        throw std::runtime_error("force_mode_selection_vector.size() != 6");
      }
      command.force_mode_selection_vector_ = force_mode_selection_vector;
      command.control_mode_ = MODE_WRENCH;
      command.force_mode_ = MODE_FORCE;
      return command;
    }

    static ControlScriptCommand MakeWrenchModeCommand(
        const std::vector<double>& ee_velocities,
        const std::vector<double>& wrench)
    {
      ControlScriptCommand command;
      if (ee_velocities.size() != 6)
      {
        throw std::runtime_error("ee_velocities.size() != 6");
      }
      if (wrench.size() != 6)
      {
        throw std::runtime_error("wrench.size() != 6");
      }
      command.speed_ = ee_velocities;
      command.wrench_ = wrench;
      command.control_mode_ = MODE_WRENCH;
      command.force_mode_ = MODE_KEEP_LIMITS;
      return command;
    }

    static ControlScriptCommand MakeIdleModeCommand()
    {
      ControlScriptCommand command;
      command.control_mode_ = MODE_IDLE;
      command.force_mode_ = MODE_RIGID;
      return command;
    }

    static ControlScriptCommand MakeExitForceModeCommand()
    {
      ControlScriptCommand command;
      command.control_mode_ = MODE_IDLE;
      command.force_mode_ = MODE_RIGID;
      return command;
    }

    static ControlScriptCommand MakeExitProgramCommand()
    {
      ControlScriptCommand command;
      command.running_ = false;
      return command;
    }
  };

  static constexpr double FLOAT_CONVERSION_RATIO = 1000000.0;
  static constexpr double STOP_DECELERATION = 1.0;
  static constexpr double SPEED_ACCELERATION = 3.2;
  static constexpr double SPEED_COMMAND_WAIT = 0.008;

  std::string base_frame_;
  std::string ee_frame_;
  std::vector<std::string> joint_names_;
  std::map<std::string, JointLimits> joint_limits_;
  double max_acceleration_limit_;

  std::string our_ip_address_;
  int32_t control_port_;
  std::vector<ControlScriptCommand> control_script_command_queue_;

  ros::NodeHandle nh_;
  ros::Publisher joint_state_pub_;
  ros::Publisher ee_pose_pub_;
  ros::Publisher ee_world_twist_pub_;
  ros::Publisher ee_body_twist_pub_;
  ros::Publisher ee_wrench_pub_;
  ros::Subscriber velocity_command_sub_;
  ros::Subscriber twist_command_sub_;
  ros::ServiceServer switch_teach_mode_server_;

  std::atomic<bool> in_teach_mode_;
  std::unique_ptr<URRealtimeInterface> robot_ptr_;
  std::mutex latest_state_mutex_;
  bool valid_latest_state_;
  sensor_msgs::JointState latest_joint_state_;
  Eigen::Isometry3d latest_tcp_pose_;

public:

  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  URScriptHardwareInterface(
      const ros::NodeHandle& nh,
      const std::string& velocity_command_topic,
      const std::string& twist_command_topic,
      const std::string& joint_state_topic,
      const std::string& ee_pose_topic,
      const std::string& ee_world_twist_topic,
      const std::string& ee_body_twist_topic,
      const std::string& ee_wrench_topic,
      const std::string& base_frame,
      const std::string& ee_frame,
      const std::string& teach_mode_service,
      const std::vector<std::string>& ordered_joint_names,
      const std::map<std::string, JointLimits>& joint_limits,
      const std::string& robot_host,
      const std::string& our_ip_address,
      const int32_t control_port)
    : nh_(nh)
  {
    our_ip_address_ = our_ip_address;
    control_port_ = control_port;
    in_teach_mode_.store(false);
    valid_latest_state_ = false;
    // Make sure our ordered joint names match our joint limits
    joint_names_ = ordered_joint_names;
    if (joint_names_.size() != 6)
    {
      throw std::invalid_argument("There must be exactly 6 joints");
    }
    if (SetsEqual(joint_names_, GetKeys(joint_limits)) == false)
    {
      throw std::invalid_argument(
            "Ordered joint names do not match names provided with limits");
    }
    base_frame_ = base_frame;
    ee_frame_ = ee_frame;
    joint_limits_ = joint_limits;
    // Compute the maximum joint acceleration
    max_acceleration_limit_ = 0.0;
    for (auto itr = joint_limits_.begin(); itr != joint_limits_.end(); ++itr)
    {
      const JointLimits& joint_limit = itr->second;
      const double acceleration_limit = joint_limit.MaxAcceleration();
      max_acceleration_limit_
          = std::max(max_acceleration_limit_, acceleration_limit);
    }
    ROS_INFO_NAMED(ros::this_node::getName(),
                   "Set max_acceleration_limit to %f",
                   max_acceleration_limit_);
    joint_state_pub_
        = nh_.advertise<sensor_msgs::JointState>(joint_state_topic, 1, false);
    ee_pose_pub_
        = nh_.advertise<geometry_msgs::PoseStamped>(ee_pose_topic, 1, false);
    ee_world_twist_pub_
        = nh_.advertise<geometry_msgs::TwistStamped>(ee_world_twist_topic,
                                                     1,
                                                     false);
    ee_body_twist_pub_
        = nh_.advertise<geometry_msgs::TwistStamped>(ee_body_twist_topic,
                                                     1,
                                                     false);
    ee_wrench_pub_
        = nh_.advertise<geometry_msgs::WrenchStamped>(ee_wrench_topic,
                                                      1,
                                                      false);
    velocity_command_sub_
        = nh_.subscribe(velocity_command_topic,
                        1,
                        &URScriptHardwareInterface::VelocityCommandCallback,
                        this);
    twist_command_sub_
        = nh_.subscribe(twist_command_topic,
                        1,
                        &URScriptHardwareInterface::TwistCommandCallback,
                        this);
    switch_teach_mode_server_
        = nh_.advertiseService(teach_mode_service,
                               &URScriptHardwareInterface::SwitchTeachModeCB,
                               this);
    // Build robot interface
    const std::function<void(const URRealtimeState&)> callback_fn
        = [&] (const URRealtimeState& latest_state)
    {
      return PublishState(joint_names_, base_frame_, ee_frame_, latest_state);
    };
    std::function<void(const std::string&)> logging_fn
        = [] (const std::string& message)
    {
      ROS_INFO_NAMED(ros::this_node::getName(), "%s", message.c_str());
    };
    robot_ptr_ = std::unique_ptr<URRealtimeInterface>(
                   new URRealtimeInterface(robot_host,
                                           callback_fn,
                                           logging_fn));
  }

  void Run(const double control_rate)
  {
    // Start robot interface
    robot_ptr_->StartRecv();
    ROS_INFO_NAMED(ros::this_node::getName(),
                   "Started robot realtime interface");
    // Start control program interface
    const std::pair<int32_t, int32_t> control_program_socket_fds
        = StartControlProgram(our_ip_address_,
                              control_port_,
                              FLOAT_CONVERSION_RATIO,
                              STOP_DECELERATION,
                              SPEED_ACCELERATION,
                              SPEED_COMMAND_WAIT);
    int32_t control_program_incoming_sock_fd = control_program_socket_fds.first;
    int32_t control_program_sock_fd = control_program_socket_fds.second;
    ROS_INFO_NAMED(ros::this_node::getName(),
                   "Started robot control interface with fds %d, %d",
                   control_program_incoming_sock_fd,
                   control_program_sock_fd);
    // Start ROS spinloop
    ros::Rate looprate(control_rate);
    while (nh_.ok())
    {
      ros::spinOnce();
      for (size_t idx = 0; idx < control_script_command_queue_.size(); idx++)
      {
        const ControlScriptCommand& current_command
            = control_script_command_queue_[idx];
        std::vector<uint8_t> buffer;
        current_command.SerializeSelf(FLOAT_CONVERSION_RATIO, buffer);
        const ssize_t bytes_written = write(control_program_sock_fd,
                                            buffer.data(),
                                            buffer.size());
        if (bytes_written != (ssize_t)buffer.size())
        {
          perror(NULL);
          ROS_ERROR_NAMED(ros::this_node::getName(),
                          "Failed to send command with %zu bytes, "
                          "sent %zd instead", buffer.size(), bytes_written);
          ROS_INFO_NAMED(ros::this_node::getName(),
                         "Trying to restart/reconnect to the "
                         "robot control script...");
          close(control_program_sock_fd);
          close(control_program_incoming_sock_fd);
          exit(1);
          const std::pair<int32_t, int32_t> control_program_socket_fds
              = StartControlProgram(our_ip_address_,
                                    control_port_,
                                    FLOAT_CONVERSION_RATIO,
                                    STOP_DECELERATION,
                                    SPEED_ACCELERATION,
                                    SPEED_COMMAND_WAIT);
          control_program_incoming_sock_fd = control_program_socket_fds.first;
          control_program_sock_fd = control_program_socket_fds.second;
          ROS_INFO_NAMED(ros::this_node::getName(),
                         "Restarted/reconnected robot control interface with"
                         " fds %d, %d",
                         control_program_incoming_sock_fd,
                         control_program_sock_fd);
        }
      }
      control_script_command_queue_.clear();
      looprate.sleep();
    }
    const ControlScriptCommand exit_command
        = ControlScriptCommand::MakeExitProgramCommand();
    std::vector<uint8_t> buffer;
    exit_command.SerializeSelf(FLOAT_CONVERSION_RATIO, buffer);
    const ssize_t bytes_written = write(control_program_sock_fd,
                                        buffer.data(),
                                        buffer.size());
    if (bytes_written != (ssize_t)buffer.size())
    {
      perror(NULL);
      ROS_ERROR_NAMED(ros::this_node::getName(),
                      "Failed to send command with %zu bytes, sent %zd instead",
                      buffer.size(),
                      bytes_written);
    }
    close(control_program_sock_fd);
    close(control_program_incoming_sock_fd);
    robot_ptr_->StopRecv();
  }

  std::pair<int32_t, int32_t> StartControlProgram(
      const std::string& our_ip_address,
      const int32_t control_port,
      const double float_conversion_ratio,
      const double stop_deceleration,
      const double speed_acceleration,
      const double speed_command_wait)
  {
    std::string control_script_string = "def ur_driver_program():\n";
    control_script_string += "  PC_IP_ADDRESS = \"" + our_ip_address + "\"\n";
    control_script_string += "  PC_CONTROL_PORT = " + std::to_string(control_port) + "\n";
    control_script_string += "  FLOAT_CONVERSION = " + std::to_string(float_conversion_ratio) + "\n";
    control_script_string += "  STOP_DECELERATION = " + std::to_string(stop_deceleration) + "\n";
    control_script_string += "  SPEED_ACCELERATION = " + std::to_string(speed_acceleration) + "\n";
    control_script_string += "  SPEED_COMMAND_WAIT = " + std::to_string(speed_command_wait) + "\n";
    control_script_string += "  MODE_IDLE = 0\n";
    control_script_string += "  MODE_TEACH = 1\n";
    control_script_string += "  MODE_SPEEDJ = 2\n";
    control_script_string += "  MODE_SPEEDL = 3\n";
    control_script_string += "  MODE_WRENCH = 4\n";
    control_script_string += "  MODE_RIGID = 0\n";
    control_script_string += "  MODE_FORCE = 1\n";
    control_script_string += "  MODE_DONT_CHANGE = 2\n";
    control_script_string += "  MODE_KEEP_LIMITS = 3\n";
    control_script_string += "  command_speed = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]\n";
    control_script_string += "  command_wrench = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]\n";
    control_script_string += "  command_force_mode_limits = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]\n";
    control_script_string += "  command_force_mode_selection_vector = [0, 0, 0, 0, 0, 0]\n";
    control_script_string += "  command_control_mode = MODE_IDLE\n";
    control_script_string += "  command_force_mode = MODE_RIGID\n";
    control_script_string += "  def update_command(new_speed, new_wrench, new_fml, new_fmsv, new_control_mode, new_force_mode):\n";
    control_script_string += "    enter_critical\n";
    control_script_string += "    command_speed = new_speed\n";
    control_script_string += "    command_wrench = new_wrench\n";
    control_script_string += "    command_force_mode_limits = new_fml\n";
    control_script_string += "    command_force_mode_selection_vector = new_fmsv\n";
    control_script_string += "    command_control_mode = new_control_mode\n";
    control_script_string += "    command_force_mode = new_force_mode\n";
    control_script_string += "    exit_critical\n";
    control_script_string += "  end\n";
    control_script_string += "  thread command_thread():\n";
    control_script_string += "    active_force_mode_limits = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]\n";
    control_script_string += "    active_force_mode_selection_vector = [0, 0, 0, 0, 0, 0]\n";
    control_script_string += "    active_control_mode = MODE_IDLE\n";
    control_script_string += "    active_force_mode = MODE_RIGID\n";
    control_script_string += "    while True:\n";
    control_script_string += "      enter_critical\n";
    control_script_string += "      cmd_speed = command_speed\n";
    control_script_string += "      cmd_wrench = command_wrench\n";
    control_script_string += "      cmd_force_mode_limits = command_force_mode_limits\n";
    control_script_string += "      cmd_force_mode_selection_vector = command_force_mode_selection_vector\n";
    control_script_string += "      cmd_control_mode = command_control_mode\n";
    control_script_string += "      cmd_force_mode = command_force_mode\n";
    control_script_string += "      exit_critical\n";
    control_script_string += "      if (active_control_mode == MODE_TEACH) and (cmd_control_mode != MODE_TEACH):\n";
    control_script_string += "        active_control_mode = MODE_IDLE\n";
    control_script_string += "        end_teach_mode()\n";
    control_script_string += "      elif (active_control_mode != MODE_TEACH) and (cmd_control_mode == MODE_TEACH):\n";
    control_script_string += "        active_control_mode = MODE_TEACH\n";
    control_script_string += "        stopj(STOP_DECELERATION)\n";
    control_script_string += "        teach_mode()\n";
    control_script_string += "      elif (active_control_mode != MODE_IDLE) and (cmd_control_mode == MODE_IDLE):\n";
    control_script_string += "        active_control_mode = MODE_IDLE\n";
    control_script_string += "        stopj(STOP_DECELERATION)\n";
    control_script_string += "      end\n";
    control_script_string += "      if (active_force_mode != MODE_FORCE) and (cmd_force_mode == MODE_FORCE):\n";
    control_script_string += "        active_force_mode = MODE_FORCE\n";
    control_script_string += "        active_force_mode_limits = cmd_force_mode_limits\n";
    control_script_string += "        active_force_mode_selection_vector = cmd_force_mode_selection_vector\n";
    control_script_string += "        textmsg(\"MF:\", cmd_force_mode_selection_vector)\n";
    control_script_string += "        force_mode(get_actual_tcp_pose(), cmd_force_mode_selection_vector, cmd_wrench, 2, cmd_force_mode_limits)\n";
    control_script_string += "      elif (active_force_mode == MODE_FORCE) and (cmd_force_mode == MODE_KEEP_LIMITS):\n";
    //control_script_string += "        end_force_mode()\n";
    control_script_string += "        textmsg(\"MKL:\", active_force_mode_selection_vector)\n";
    control_script_string += "        force_mode(get_actual_tcp_pose(), active_force_mode_selection_vector, cmd_wrench, 2, active_force_mode_limits)\n";
    control_script_string += "      elif (active_force_mode != MODE_RIGID) and (cmd_force_mode == MODE_RIGID):\n";
    control_script_string += "        active_force_mode = MODE_RIGID\n";
    control_script_string += "        end_force_mode()\n";
    control_script_string += "      end\n";
    control_script_string += "      if cmd_control_mode == MODE_SPEEDJ:\n";
    control_script_string += "        active_control_mode = MODE_SPEEDJ\n";
    control_script_string += "        speedj(cmd_speed, SPEED_ACCELERATION, SPEED_COMMAND_WAIT)\n";
    control_script_string += "      elif cmd_control_mode == MODE_SPEEDL:\n";
    control_script_string += "        active_control_mode = MODE_SPEEDL\n";
    control_script_string += "        speedl(cmd_speed, SPEED_ACCELERATION, SPEED_COMMAND_WAIT)\n";
    //control_script_string += "      elif cmd_control_mode == MODE_WRENCH:\n";
    //control_script_string += "        active_control_mode = MODE_WRENCH\n";
    //control_script_string += "        speedl(cmd_speed, SPEED_ACCELERATION, SPEED_COMMAND_WAIT)\n";
    control_script_string += "      else:\n";
    control_script_string += "        sync()\n";
    control_script_string += "      end\n";
    control_script_string += "    end\n";
    control_script_string += "  end\n";
    control_script_string += "  socket_open(PC_IP_ADDRESS, PC_CONTROL_PORT)\n";
    control_script_string += "  thread_command = run command_thread()\n";
    control_script_string += "  recv_running = 1\n";
    control_script_string += "  while recv_running == 1:\n";
    control_script_string += "    params = socket_read_binary_integer(6 + 6 + 6 + 6 + 3)\n";
    control_script_string += "    valid_data = params[0]\n";
    control_script_string += "    if valid_data > 0:\n";
    control_script_string += "      recv_speed = [params[1] / FLOAT_CONVERSION, params[2] / FLOAT_CONVERSION, params[3] / FLOAT_CONVERSION, params[4] / FLOAT_CONVERSION, params[5] / FLOAT_CONVERSION, params[6] / FLOAT_CONVERSION]\n";
    control_script_string += "      recv_wrench = [params[7] / FLOAT_CONVERSION, params[8] / FLOAT_CONVERSION, params[9] / FLOAT_CONVERSION, params[10] / FLOAT_CONVERSION, params[11] / FLOAT_CONVERSION, params[12] / FLOAT_CONVERSION]\n";
    control_script_string += "      recv_force_mode_limits = [params[13] / FLOAT_CONVERSION, params[14] / FLOAT_CONVERSION, params[15] / FLOAT_CONVERSION, params[16] / FLOAT_CONVERSION, params[17] / FLOAT_CONVERSION, params[18] / FLOAT_CONVERSION]\n";
    control_script_string += "      recv_force_mode_selection_vector = [params[19], params[20], params[21], params[22], params[23], params[24]]\n";
    control_script_string += "      recv_control_mode = params[25]\n";
    control_script_string += "      recv_force_mode = params[26]\n";
    control_script_string += "      recv_running = params[27]\n";
    control_script_string += "      if recv_running == 1:\n";
    control_script_string += "        update_command(recv_speed, recv_wrench, recv_force_mode_limits, recv_force_mode_selection_vector, recv_control_mode, recv_force_mode)\n";
    control_script_string += "      else:\n";
    control_script_string += "        update_command([0.0, 0.0, 0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], [0, 0, 0, 0, 0, 0], MODE_IDLE, MODE_RIGID)\n";
    control_script_string += "      end\n";
    control_script_string += "    else:\n";
    control_script_string += "      update_command([0.0, 0.0, 0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], [0, 0, 0, 0, 0, 0], MODE_IDLE, MODE_DONT_CHANGE)\n";
    control_script_string += "    end\n";
    control_script_string += "  end\n";
    control_script_string += "  sleep(0.1)\n";
    control_script_string += "  socket_close()\n";
    control_script_string += "  kill thread_command\n";
    control_script_string += "end\n";
    const bool success = robot_ptr_->SendURScriptCommand(control_script_string);
    if (success)
    {
      ROS_INFO_NAMED(ros::this_node::getName(), "Uploaded control program");
    }
    else
    {
      throw std::runtime_error("Failed to upload control program");
    }
    // Setup communications with the control program
    int incoming_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (incoming_sock_fd < 0)
    {
      throw std::runtime_error(
            "ERROR opening socket for control program communication");
    }
    else
    {
      ROS_INFO_NAMED(ros::this_node::getName(),
                     "Opened socket for control program communication");
    }
    struct sockaddr_in serv_addr;
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(control_port);
    int flag = 1;
    setsockopt(incoming_sock_fd,
               IPPROTO_TCP, TCP_NODELAY,
               (char *) &flag,
               sizeof(int));
    setsockopt(incoming_sock_fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &flag,
               sizeof(int));
    if (bind(incoming_sock_fd,
             (struct sockaddr *)
             &serv_addr,
             sizeof(serv_addr)) < 0)
    {
      throw std::runtime_error(
            "ERROR on binding socket for control program communication");
    }
    else
    {
      ROS_INFO_NAMED(ros::this_node::getName(),
                     "Bound socket for control program communication");
    }
    listen(incoming_sock_fd, 5);
    struct sockaddr_in cli_addr;
    socklen_t clilen;
    clilen = sizeof(cli_addr);
    int new_sock_fd = accept(incoming_sock_fd,
                             (struct sockaddr *)
                             &cli_addr,
                             &clilen);
    if (new_sock_fd < 0)
    {
      throw std::runtime_error(
            "ERROR on accepting control program communication");
    }
    else
    {
      ROS_INFO_NAMED(ros::this_node::getName(),
                     "Accepted connection for control program communication");
    }
    return std::make_pair(incoming_sock_fd, new_sock_fd);
  }

  void PublishState(const std::vector<std::string>& joint_names,
            const std::string& base_frame_name,
            const std::string& ee_frame_name,
            const URRealtimeState& robot_state)
  {
    // Get the current time
    const ros::Time state_time = ros::Time::now();
    // Joint State
    sensor_msgs::JointState joint_state_msg;
    joint_state_msg.header.stamp = state_time;
    joint_state_msg.name = joint_names;
    joint_state_msg.position = robot_state.ActualPosition();
    joint_state_msg.velocity = robot_state.ActualVelocity();
    joint_state_msg.effort = robot_state.TargetTorque();
    latest_state_mutex_.lock();
    latest_joint_state_ = joint_state_msg;
    latest_tcp_pose_ = robot_state.ActualTcpPose();
    valid_latest_state_ = true;
    latest_state_mutex_.unlock();
    // EE transform
    geometry_msgs::PoseStamped ee_transform_msg
        = EigenIsometry3dToGeometryPoseStamped(robot_state.ActualTcpPose(),
                                               base_frame_);
    ee_transform_msg.header.stamp = state_time;
    // EE twist
    const Eigen::Matrix<double, 6, 1>& ee_world_twist
        = robot_state.ActualTcpTwist();
    geometry_msgs::TwistStamped ee_world_twist_msg;
    ee_world_twist_msg.header.stamp = state_time;
    ee_world_twist_msg.header.frame_id = base_frame_name;
    ee_world_twist_msg.twist.linear
        = EigenVector3dToGeometryVector3(ee_world_twist.block<3, 1>(0, 0));
    ee_world_twist_msg.twist.angular
        = EigenVector3dToGeometryVector3(ee_world_twist.block<3, 1>(3, 0));
    const Eigen::Quaterniond ee_rotation(
          robot_state.ActualTcpPose().rotation());
    const Eigen::Vector3d body_frame_linear_velocity
        = RotateVectorReverse(ee_rotation, ee_world_twist.block<3, 1>(0, 0));
    const Eigen::Vector3d body_frame_angular_velocity
        = RotateVectorReverse(ee_rotation, ee_world_twist.block<3, 1>(3, 0));
    geometry_msgs::TwistStamped ee_body_twist_msg;
    ee_body_twist_msg.header.stamp = state_time;
    ee_body_twist_msg.header.frame_id = ee_frame_name;
    ee_body_twist_msg.twist.linear
        = EigenVector3dToGeometryVector3(body_frame_linear_velocity);
    ee_body_twist_msg.twist.angular
        = EigenVector3dToGeometryVector3(body_frame_angular_velocity);
    // EE wrench
    const Eigen::Matrix<double, 6, 1>& ee_wrench
        = robot_state.ActualTcpWrench();
    geometry_msgs::WrenchStamped ee_wrench_msg;
    ee_wrench_msg.header.stamp = state_time;
    ee_wrench_msg.header.frame_id = ee_frame_name;
    ee_wrench_msg.wrench.force
        = EigenVector3dToGeometryVector3(ee_wrench.block<3, 1>(0, 0));
    ee_wrench_msg.wrench.torque
        = EigenVector3dToGeometryVector3(ee_wrench.block<3, 1>(3, 0));
    // Publish
    joint_state_pub_.publish(joint_state_msg);
    ee_pose_pub_.publish(ee_transform_msg);
    ee_world_twist_pub_.publish(ee_world_twist_msg);
    ee_body_twist_pub_.publish(ee_body_twist_msg);
    ee_wrench_pub_.publish(ee_wrench_msg);
  }

  bool SwitchTeachModeCB(std_srvs::SetBool::Request& req,
                         std_srvs::SetBool::Response& res)
  {
    if (req.data)
    {
      if (in_teach_mode_.load() == false)
      {
        control_script_command_queue_.push_back(
              ControlScriptCommand::MakeTeachModeCommand());
        in_teach_mode_.store(true);
        res.success = true;
        res.message = "Entered teach mode";
      }
      else
      {
        res.success = true;
        res.message = "Already in teach mode, ignoring request to enter";
      }
    }
    else
    {
      if (in_teach_mode_.load() == true)
      {
        control_script_command_queue_.push_back(
              ControlScriptCommand::MakeExitTeachModeCommand());
        in_teach_mode_.store(false);
        res.success = true;
        res.message = "Exited teach mode";
      }
      else
      {
        res.success = true;
        res.message = "Not in teach mode, ignoring request to exit";
      }
    }
    return true;
  }

  void VelocityCommandCallback(
      lightweight_ur_interface::VelocityCommand config_target)
  {
    if (config_target.name.size() == config_target.velocity.size())
    {
      // Push the command into a map
      std::map<std::string, double> command_map;
      for (size_t idx = 0; idx < config_target.name.size(); idx++)
      {
        const std::string& name = config_target.name[idx];
        const double command = config_target.velocity[idx];
        command_map[name] = command;
      }
      // Extract the joint commands in order
      std::vector<double> target_velocity(joint_names_.size(), 0.0);
      bool command_valid = true;
      for (size_t idx = 0; idx < joint_names_.size(); idx++)
      {
        // Get the name of the joint
        const std::string& joint_name = joint_names_[idx];
        // Get the commanded value
        const auto found_itr = command_map.find(joint_name);
        if (found_itr != command_map.end())
        {
          const double velocity = found_itr->second;
          // Get the limits for the joint
          const auto limits_found_itr = joint_limits_.find(joint_name);
          // If we have limits saved, limit the joint command
          if (limits_found_itr != joint_limits_.end())
          {
            const double velocity_limit
                = limits_found_itr->second.MaxVelocity();
            const double limited_velocity
                = ClampValueAndWarn(velocity, -velocity_limit, velocity_limit);
            target_velocity[idx] = limited_velocity;
          }
          // If we don't have limits saved, then we don't need to limit
          else
          {
            target_velocity[idx] = velocity;
          }
        }
        else
        {
          ROS_WARN_NAMED(ros::this_node::getName(),
                         "Invalid VelocityCommand: joint %s missing",
                         joint_name.c_str());
          command_valid = false;
        }
      }
      if (command_valid)
      {
        if (in_teach_mode_.load() == false)
        {
          control_script_command_queue_.push_back(
                ControlScriptCommand::MakeSpeedJCommand(target_velocity));
        }
        else
        {
          ROS_WARN_NAMED(ros::this_node::getName(),
                         "Ignoring VelocityCommand since robot is in teach mode");
        }
      }
    }
    else
    {
      ROS_WARN_NAMED(ros::this_node::getName(),
                     "Invalid VelocityCommand: %zu names, %zu velocities",
                     config_target.name.size(),
                     config_target.velocity.size());
    }
  }

  void TwistCommandCallback(geometry_msgs::TwistStamped twist_command)
  {
    bool valid_twist = true;
    if (std::isinf(twist_command.twist.linear.x)
        || std::isnan(twist_command.twist.linear.x))
    {
      ROS_WARN_NAMED(ros::this_node::getName(),
                     "Invalid Twist command, linear.x is NAN or INF");
      valid_twist = false;
    }
    if (std::isinf(twist_command.twist.linear.y)
        || std::isnan(twist_command.twist.linear.y))
    {
      ROS_WARN_NAMED(ros::this_node::getName(),
                     "Invalid Twist command, linear.y is NAN or INF");
      valid_twist = false;
    }
    if (std::isinf(twist_command.twist.linear.z)
        || std::isnan(twist_command.twist.linear.z))
    {
      ROS_WARN_NAMED(ros::this_node::getName(),
                     "Invalid Twist command, linear.z is NAN or INF");
      valid_twist = false;
    }
    if (std::isinf(twist_command.twist.angular.x)
        || std::isnan(twist_command.twist.angular.x))
    {
      ROS_WARN_NAMED(ros::this_node::getName(),
                     "Invalid Twist command, angular.x is NAN or INF");
      valid_twist = false;
    }
    if (std::isinf(twist_command.twist.angular.y)
        || std::isnan(twist_command.twist.angular.y))
    {
      ROS_WARN_NAMED(ros::this_node::getName(),
                     "Invalid Twist command, angular.y is NAN or INF");
      valid_twist = false;
    }
    if (std::isinf(twist_command.twist.angular.z)
        || std::isnan(twist_command.twist.angular.z))
    {
      ROS_WARN_NAMED(ros::this_node::getName(),
                     "Invalid Twist command, angular.z is NAN or INF");
      valid_twist = false;
    }
    if (valid_twist)
    {
      const std::vector<double> raw_twist = {twist_command.twist.linear.x,
                                             twist_command.twist.linear.y,
                                             twist_command.twist.linear.z,
                                             twist_command.twist.angular.x,
                                             twist_command.twist.angular.y,
                                             twist_command.twist.angular.z};
      if (in_teach_mode_.load() == false)
      {
        if (twist_command.header.frame_id == ee_frame_)
        {
          latest_state_mutex_.lock();
          const Eigen::Isometry3d latest_tcp_pose = latest_tcp_pose_;
          const bool valid_latest_state = valid_latest_state_;
          latest_state_mutex_.unlock();
          if (valid_latest_state)
          {
            const Eigen::Quaterniond latest_tcp_rotation(
                  latest_tcp_pose.rotation());
            const Eigen::Vector3d ee_frame_linear_velocity(raw_twist[0],
                                                           raw_twist[1],
                                                           raw_twist[2]);
            const Eigen::Vector3d ee_frame_angular_velocity(raw_twist[3],
                                                            raw_twist[4],
                                                            raw_twist[5]);
            const Eigen::Vector3d base_frame_linear_velocity
                = RotateVector(latest_tcp_rotation, ee_frame_linear_velocity);
            const Eigen::Vector3d base_frame_angular_velocity
                = RotateVector(latest_tcp_rotation, ee_frame_angular_velocity);
            const std::vector<double> base_frame_twist
                = {base_frame_linear_velocity.x(),
                   base_frame_linear_velocity.y(),
                   base_frame_linear_velocity.z(),
                   base_frame_angular_velocity.x(),
                   base_frame_angular_velocity.y(),
                   base_frame_angular_velocity.z()};
            control_script_command_queue_.push_back(
                  ControlScriptCommand::MakeSpeedLCommand(base_frame_twist));
          }
          else
          {
            ROS_WARN_NAMED(ros::this_node::getName(),
                           "Ignoring ee-frame Twist as latest state invalid");
          }
        }
        else if (twist_command.header.frame_id == base_frame_)
        {
          control_script_command_queue_.push_back(
                ControlScriptCommand::MakeSpeedLCommand(raw_twist));
        }
        else
        {
          ROS_WARN_NAMED(ros::this_node::getName(),
                         "Invalid Twist frame: got [%s] needs [%s] or [%s]",
                         twist_command.header.frame_id.c_str(),
                         base_frame_.c_str(),
                         ee_frame_.c_str());
        }
      }
      else
      {
        ROS_WARN_NAMED(ros::this_node::getName(),
                       "Ignoring Twist since robot is in teach mode");
      }
    }
  }
};
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "ur_script_hardware_interface");
  ROS_INFO_NAMED(ros::this_node::getName(),
                 "Starting ur_script_hardware_interface...");
  ros::NodeHandle nh;
  ros::NodeHandle nhp("~");
  const std::string DEFAULT_JOINT_STATE_TOPIC = "/ur10/joint_states";
  const std::string DEFAULT_VELOCITY_COMMAND_TOPIC
      = "/ur10/joint_command_velocity";
  const std::string DEFAULT_TWIST_COMMAND_TOPIC = "/ur10/ee_twist_command";
  const std::string DEFAULT_EE_POSE_TOPIC = "/ur10/ee_pose";
  const std::string DEFAULT_EE_WORLD_TWIST_TOPIC = "/ur10/ee_world_twist";
  const std::string DEFAULT_EE_BODY_TWIST_TOPIC = "/ur10/ee_body_twist";
  const std::string DEFAULT_EE_WRENCH_TOPIC = "/ur10/ee_wrench";
  const std::string DEFAULT_BASE_FRAME = "base";
  const std::string DEFAULT_EE_FRAME = "ur10_ee_frame";
  const std::string DEFAULT_TEACH_MODE_SERVICE = "/ur10/switch_teach_mode";
  const std::string DEFAULT_ROBOT_HOSTNAME = "172.31.1.200";
  const std::string DEFAULT_OUR_IP_ADDRESS = "172.31.1.100";
  const int32_t DEFAULT_CONTROL_PORT = 50007;
  const double DEFAULT_VELOCITY_LIMIT_SCALING = 0.5;
  const double DEFAULT_ACCELERATION_LIMIT_SCALING = 0.5;
  const std::string joint_state_topic
      = nhp.param(std::string("joint_state_topic"), DEFAULT_JOINT_STATE_TOPIC);
  const std::string velocity_command_topic
      = nhp.param(std::string("velocity_command_topic"),
                  DEFAULT_VELOCITY_COMMAND_TOPIC);
  const std::string twist_command_topic
      = nhp.param(std::string("twist_command_topic"),
                  DEFAULT_TWIST_COMMAND_TOPIC);
  const std::string ee_pose_topic
      = nhp.param(std::string("ee_pose_topic"), DEFAULT_EE_POSE_TOPIC);
  const std::string ee_world_twist_topic
      = nhp.param(std::string("ee_world_twist_topic"),
                  DEFAULT_EE_WORLD_TWIST_TOPIC);
  const std::string ee_body_twist_topic
      = nhp.param(std::string("ee_body_twist_topic"),
                  DEFAULT_EE_BODY_TWIST_TOPIC);
  const std::string ee_wrench_topic
      = nhp.param(std::string("ee_wrench_topic"), DEFAULT_EE_WRENCH_TOPIC);
  const std::string base_frame
      = nhp.param(std::string("base_frame"), DEFAULT_BASE_FRAME);
  const std::string ee_frame
      = nhp.param(std::string("ee_wrench_frame"), DEFAULT_EE_FRAME);;
  const std::string teach_mode_service
      = nhp.param(std::string("teach_mode_service"),
                  DEFAULT_TEACH_MODE_SERVICE);
  const std::string robot_hostname
      = nhp.param(std::string("robot_hostname"), DEFAULT_ROBOT_HOSTNAME);
  const std::string our_ip_address
      = nhp.param(std::string("our_ip_address"), DEFAULT_OUR_IP_ADDRESS);
  const int32_t control_port
      = std::abs(nhp.param(std::string("control_port"), DEFAULT_CONTROL_PORT));
  const double velocity_limit_scaling
      = std::abs(nhp.param(std::string("velocity_limit_scaling"),
                           DEFAULT_VELOCITY_LIMIT_SCALING));
  const double acceleration_limit_scaling
      = std::abs(nhp.param(std::string("acceleration_limit_scaling"),
                           DEFAULT_ACCELERATION_LIMIT_SCALING));
  const double real_velocity_limit_scaling
      = tri_driver_common::utility::ClampValueAndWarn(
          velocity_limit_scaling, 0.0, 1.0);
  const double real_acceleration_limit_scaling
      = tri_driver_common::utility::ClampValueAndWarn(
          acceleration_limit_scaling, 0.0, 1.0);
  // Joint names in true order
  const std::vector<std::string> ordered_joint_names
      = lightweight_ur_interface::GetOrderedJointNames();
  // Joint limits
  const std::map<std::string, lightweight_ur_interface::JointLimits> limits
      = lightweight_ur_interface::GetLimits(real_velocity_limit_scaling,
                                            real_acceleration_limit_scaling);
  lightweight_ur_interface::URScriptHardwareInterface interface(
        nh,
        velocity_command_topic,
        twist_command_topic,
        joint_state_topic,
        ee_pose_topic,
        ee_world_twist_topic,
        ee_body_twist_topic,
        ee_wrench_topic,
        base_frame,
        ee_frame,
        teach_mode_service,
        ordered_joint_names,
        limits,
        robot_hostname,
        our_ip_address,
        control_port);
  ROS_INFO_NAMED(ros::this_node::getName(), "...startup complete");
  interface.Run(400.0);
  return 0;
}
