/*
    Copyright (c) 2010, Daniel Hewlett, Antons Rebguns
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
        * Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.
        * Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in the
        documentation and/or other materials provided with the distribution.
        * Neither the name of the <organization> nor the
        names of its contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY Antons Rebguns <email> ''AS IS'' AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL Antons Rebguns <email> BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <algorithm>
#include <assert.h>

#include <erratic_gazebo_plugins/diffdrive_plugin.h>

#include <gazebo.h>
#include <common/Exception.hh>
#include <math/Quaternion.hh>
#include <math/Pose.hh>
#include <physics/Joint.hh>
#include <physics/Physics.hh>
#include <physics/PhysicsEngine.hh>
#include <physics/PhysicsTypes.hh>
#include <physics/World.hh>
#include <sdf/interface/SDF.hh>
#include <sdf/interface/Param.hh>

#include <ros/ros.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <geometry_msgs/Twist.h>
#include <nav_msgs/Odometry.h>
#include <robot_kf/WheelOdometry.h>
#include <boost/bind.hpp>

static double const min_variance = 1e-6;

namespace gazebo
{

enum
{
  RIGHT,
  LEFT,
};

// Constructor
DiffDrivePlugin::DiffDrivePlugin(void)
  : last_true_yaw_(0)
  , last_odom_yaw_(0)
  , last_true_pos_(0, 0, 0)
  , last_odom_pos_(0, 0, 0)
{
}

// Destructor
DiffDrivePlugin::~DiffDrivePlugin()
{
  delete rosnode_;
  delete transform_broadcaster_;
}

// Load the controller
void DiffDrivePlugin::Load(physics::ModelPtr _parent, sdf::ElementPtr _sdf)
{
  this->parent = _parent;
  this->world = _parent->GetWorld();

  gzdbg << "plugin parent sensor name: " << parent->GetName() << "\n";

  if (!this->parent) { gzthrow("Differential_Position2d controller requires a Model as its parent"); }

  this->robotNamespace = "";
  if (_sdf->HasElement("robotNamespace"))
  {
    this->robotNamespace = _sdf->GetElement("robotNamespace")->GetValueString() + "/";
  }

  if (!_sdf->HasElement("leftJoint"))
  {
    ROS_WARN("Differential Drive plugin missing <leftJoint>, defaults to left_joint");
    this->leftJointName = "left_joint";
  }
  else
  {
    this->leftJointName = _sdf->GetElement("leftJoint")->GetValueString();
  }

  if (!_sdf->HasElement("rightJoint"))
  {
    ROS_WARN("Differential Drive plugin missing <rightJoint>, defaults to right_joint");
    this->rightJointName = "right_joint";
  }
  else
  {
    this->rightJointName = _sdf->GetElement("rightJoint")->GetValueString();
  }

  if (!_sdf->HasElement("wheelSeparation"))
  {
    ROS_WARN("Differential Drive plugin missing <wheelSeparation>, defaults to 0.34");
    this->wheelSeparation = 0.34;
  }
  else
  {
    this->wheelSeparation = _sdf->GetElement("wheelSeparation")->GetValueDouble();
  }

  if (!_sdf->HasElement("wheelDiameter"))
  {
    ROS_WARN("Differential Drive plugin missing <wheelDiameter>, defaults to 0.15");
    this->wheelDiameter = 0.15;
  }
  else
  {
    this->wheelDiameter = _sdf->GetElement("wheelDiameter")->GetValueDouble();
  }

  if (!_sdf->HasElement("torque"))
  {
    ROS_WARN("Differential Drive plugin missing <torque>, defaults to 5.0");
    this->torque = 5.0;
  }
  else {
    this->torque = _sdf->GetElement("torque")->GetValueDouble();
  }

  if (!_sdf->HasElement("twistTopicName"))
  {
    ROS_WARN("Differential Drive plugin missing <twistTopicName>, defaults to cmd_vel");
    this->twistTopicName = "cmd_vel";
  }
  else
  {
    this->twistTopicName = _sdf->GetElement("twistTopicName")->GetValueString();
  }

  if (!_sdf->HasElement("odomTopicName"))
  {
    ROS_WARN("Differential Drive plugin missing <odomTopicName>, defaults to odom");
    this->odomTopicName = "odom";
  }
  else
  {
    this->odomTopicName = _sdf->GetElement("odomTopicName")->GetValueString();
  }

  if (!_sdf->HasElement("wheelTopicName"))
  {
    ROS_WARN("Differential Drive plugin missing <wheelTopicName>, defaults to wheel_odom");
    this->wheelOdomTopicName = "wheel_odom";
  }
  else
  {
    this->wheelOdomTopicName = _sdf->GetElement("wheelTopicName")->GetValueString();
  }

  if (!_sdf->HasElement("baseFrame"))
  {
    ROS_WARN("Differential Drive plugin missing <baseFrame>, defaults to base_footprint");
    this->tf_base_frame_ = "base_footprint";
  }
  else
  {
    this->tf_base_frame_ = _sdf->GetElement("baseFrame")->GetValueString(); 
  }

  if (!_sdf->HasElement("odomFrame"))
  {
    ROS_WARN("Differential Drive plugin missing <odomFrame>, defaults to odom");
    this->tf_odom_frame_ = "odom";
  }
  else
  {
    this->tf_odom_frame_ = _sdf->GetElement("odomFrame")->GetValueString(); 
  }

  if (!_sdf->HasElement("alpha"))
  {
    ROS_WARN("Differential Drive plugin missing <alpha>, defaults to 0.0");
    this->alpha = 0.0;
  }
  else
  {
    this->alpha = _sdf->GetElement("alpha")->GetValueDouble();
  }

  if (!_sdf->HasElement("updateRate"))
  {
    ROS_WARN("Differential Drive plugin missing <updateRate>, defaults to 50.0 ms");
    rate_ = 50.0;
  }
  else
  {
    rate_ = _sdf->GetElement("updateRate")->GetValueDouble();
  }

  wheelSpeed[RIGHT] = 0;
  wheelSpeed[LEFT] = 0;

  x_ = 0;
  rot_ = 0;
  alive_ = true;

  joints[LEFT] = this->parent->GetJoint(leftJointName);
  joints[RIGHT] = this->parent->GetJoint(rightJointName);

  if (!joints[LEFT])  { gzthrow("The controller couldn't get left hinge joint"); }
  if (!joints[RIGHT]) { gzthrow("The controller couldn't get right hinge joint"); }

  // Initialize the ROS node and subscribe to cmd_vel
  int argc = 0;
  char** argv = NULL;
  ros::init(argc, argv, "diff_drive_plugin", ros::init_options::NoSigintHandler | ros::init_options::AnonymousName);
  rosnode_ = new ros::NodeHandle(this->robotNamespace);

  ROS_INFO("starting diffdrive plugin in ns: %s", this->robotNamespace.c_str());

  tf_prefix_ = tf::getPrefixParam(*rosnode_);
  transform_broadcaster_ = new tf::TransformBroadcaster();

  // ROS: Subscribe to the velocity command topic (usually "cmd_vel")
  ros::SubscribeOptions so =
      ros::SubscribeOptions::create<geometry_msgs::Twist>(twistTopicName, 1,
                                                          boost::bind(&DiffDrivePlugin::cmdVelCallback, this, _1),
                                                          ros::VoidPtr(), &queue_);
  sub_ = rosnode_->subscribe(so);
  pub_odom_  = rosnode_->advertise<nav_msgs::Odometry>(odomTopicName, 1);
  pub_wheel_ = rosnode_->advertise<robot_kf::WheelOdometry>(wheelOdomTopicName, 10);

  // Initialize the controller
  // Reset odometric pose
  odomPose[0] = 0.0;
  odomPose[1] = 0.0;
  odomPose[2] = 0.0;

  odomVel[0] = 0.0;
  odomVel[1] = 0.0;
  odomVel[2] = 0.0;

  // start custom queue for diff drive
  this->callback_queue_thread_ = boost::thread(boost::bind(&DiffDrivePlugin::QueueThread, this));

  // listen to the update event (broadcast every simulation iteration)
  this->updateConnection = event::Events::ConnectWorldUpdateStart(boost::bind(&DiffDrivePlugin::UpdateChild, this));
}

// Update the controller
void DiffDrivePlugin::UpdateChild()
{
  // TODO: Step should be in a parameter of this function
  double wd, ws;
  double d1, d2;
  double dr, da;
  double stepTime = this->world->GetPhysicsEngine()->GetStepTime();

  GetPositionCmd();

  wd = wheelDiameter;
  ws = wheelSeparation;

  // Distance travelled by front wheels
  d1 = stepTime * wd / 2 * joints[LEFT]->GetVelocity(0);
  d2 = stepTime * wd / 2 * joints[RIGHT]->GetVelocity(0);

  dr = (d1 + d2) / 2;
  da = (d1 - d2) / ws;

  // Compute odometric pose
  odomPose[0] += dr * cos(odomPose[2]);
  odomPose[1] += dr * sin(odomPose[2]);
  odomPose[2] += da;

  // Compute odometric instantaneous velocity
  odomVel[0] = dr / stepTime;
  odomVel[1] = 0.0;
  odomVel[2] = da / stepTime;

  joints[LEFT]->SetVelocity(0, wheelSpeed[LEFT] / (wheelDiameter / 2.0));
  joints[RIGHT]->SetVelocity(0, wheelSpeed[RIGHT] / (wheelDiameter / 2.0));

  joints[LEFT]->SetMaxForce(0, torque);
  joints[RIGHT]->SetMaxForce(0, torque);

  write_position_data();
  publish_odometry();
}

// Finalize the controller
void DiffDrivePlugin::FiniChild()
{
  alive_ = false;
  queue_.clear();
  queue_.disable();
  rosnode_->shutdown();
  callback_queue_thread_.join();
}

void DiffDrivePlugin::GetPositionCmd()
{
  lock.lock();

  double vr, va;

  vr = x_; //myIface->data->cmdVelocity.pos.x;
  va = rot_; //myIface->data->cmdVelocity.yaw;

  //std::cout << "X: [" << x_ << "] ROT: [" << rot_ << "]" << std::endl;

  wheelSpeed[LEFT] = vr + va * wheelSeparation / 2.0;
  wheelSpeed[RIGHT] = vr - va * wheelSeparation / 2.0;

  lock.unlock();
}

void DiffDrivePlugin::cmdVelCallback(const geometry_msgs::Twist::ConstPtr& cmd_msg)
{
  lock.lock();

  x_ = cmd_msg->linear.x;
  rot_ = cmd_msg->angular.z;

  lock.unlock();
}

void DiffDrivePlugin::QueueThread()
{
  static const double timeout = 0.01;

  while (alive_ && rosnode_->ok())
  {
    queue_.callAvailable(ros::WallDuration(timeout));
  }
}

DiffDrivePlugin::OdometryUpdate DiffDrivePlugin::generateError(
  btVector3 const &curr_true_pos, double const curr_true_yaw)
{
  OdometryUpdate odom;

  // Convert the changes into polar coordinates.
  btVector3 const delta_true_pos = curr_true_pos - last_true_pos_;
  btVector3 const u(cos(last_true_yaw_), sin(last_true_yaw_), 0.0);
  double const delta_linear = delta_true_pos.dot(u);
  double const delta_yaw    = angles::normalize_angle(curr_true_yaw - last_true_yaw_);

  // Convert from polar coordinates to encoder ticks.
  double const v_left  = delta_linear - 0.5 * wheelSeparation * delta_yaw;
  double const v_right = delta_linear + 0.5 * wheelSeparation * delta_yaw;

  // Add noise to the encoder ticks.
  double const sigma_left  = std::max(fabs(alpha * v_left),  min_variance);
  double const sigma_right = std::max(fabs(alpha * v_right), min_variance);
  normal_dist const dist_left(v_left, sigma_left);
  normal_dist const dist_right(v_right, sigma_right);
  normal_gen gen_noisy_left(rng_, dist_left);
  normal_gen gen_noisy_right(rng_, dist_right);
  double const noisy_v_left  = gen_noisy_left();
  double const noisy_v_right = gen_noisy_right();

  // Convert back from encoder ticks to polar coordinates.
  double const noisy_delta_linear = 0.5 * (noisy_v_left + noisy_v_right);
  double const noisy_delta_yaw    = (noisy_v_right - noisy_v_left) / wheelSeparation;

  // Convert back from polar coordinates to cartaesian coordinates.
  odom.curr_odom_pos[0] = last_odom_pos_[0] + noisy_delta_linear * cos(last_odom_yaw_);
  odom.curr_odom_pos[1] = last_odom_pos_[1] + noisy_delta_linear * sin(last_odom_yaw_);
  odom.curr_odom_pos[2] = last_odom_pos_[2];
  odom.curr_odom_yaw = angles::normalize_angle(last_odom_yaw_ + noisy_delta_yaw);
  odom.v_left  = noisy_v_left;
  odom.v_right = noisy_v_right;
  return odom;
}

void DiffDrivePlugin::publish_odometry()
{
  typedef boost::normal_distribution<> normal_distribution;
  typedef boost::variate_generator<boost::mt19937 &, boost::normal_distribution<> > normal_generator;

  // Throttle the update rate to the user-defined period.
  ros::Time const curr_time = ros::Time::now();
  double const delta_time = (curr_time - last_time_).toSec();
  if (delta_time < 0.001 * rate_) return;
  
  std::string const odom_frame = tf::resolve(tf_prefix_, tf_odom_frame_);
  std::string const base_footprint_frame = tf::resolve(tf_prefix_, tf_base_frame_);

  // Get the actual pose from Gazebo.
  math::Pose const pose = parent->GetState().GetPose();
  btVector3 const curr_true_pos(pose.pos.x, pose.pos.y, pose.pos.z);
  btQuaternion const curr_true_qt(pose.rot.x, pose.rot.y, pose.rot.z, pose.rot.w);
  double const curr_true_yaw = tf::getYaw(curr_true_qt);

  // Add encoder noise.
  OdometryUpdate const update = generateError(curr_true_pos, curr_true_yaw);

  // Publish the Odometry message.
  nav_msgs::Odometry odom;
  odom.header.stamp = curr_time;
  odom.header.frame_id = odom_frame;
  odom.child_frame_id = base_footprint_frame;
  odom.pose.pose.position.x = update.curr_odom_pos[0];
  odom.pose.pose.position.y = update.curr_odom_pos[1];
  odom.pose.pose.orientation = tf::createQuaternionMsgFromYaw(update.curr_odom_yaw);

  // FIXME: This velocity should be corrupted by the same noise as the
  // position estimate, since both would be estimated by the same sensor.
  math::Vector3 const v_linear = parent->GetWorldLinearVel();
  math::Vector3 const v_angular = parent->GetWorldAngularVel();
  odom.twist.twist.linear.x = v_linear.x;
  odom.twist.twist.linear.y = v_linear.y;
  odom.twist.twist.angular.z = v_angular.z;
  pub_odom_.publish(odom);

  // FIXME: Hack.
  double const beta = 1;

  // Publish the WheelOdometry message.
  double const stddev_left  = std::max(fabs(alpha * update.v_left), min_variance);
  double const stddev_right = std::max(fabs(alpha * update.v_right), min_variance);
  robot_kf::WheelOdometry wheel_odom;
  wheel_odom.header.stamp = curr_time;
  wheel_odom.header.frame_id = base_footprint_frame;
  wheel_odom.timestep = curr_time - last_time_;
  wheel_odom.separation = wheelSeparation;
  wheel_odom.left.movement = update.v_left;
  wheel_odom.left.variance = pow(beta * stddev_left, 2);
  wheel_odom.right.movement = update.v_right;
  wheel_odom.right.variance = pow(beta * stddev_right, 2);
  pub_wheel_.publish(wheel_odom);

  // Broadcast the corresponding TF transform from /odom to /base_footprint.
  tf::Quaternion const curr_odom_qt = tf::createQuaternionFromYaw(update.curr_odom_yaw);
  tf::Transform const base_footprint_to_odom(curr_odom_qt, update.curr_odom_pos);
  transform_broadcaster_->sendTransform(
    tf::StampedTransform(
      base_footprint_to_odom, curr_time, odom_frame, base_footprint_frame));

  last_time_ = curr_time;
  last_true_pos_ = curr_true_pos;
  last_true_yaw_ = curr_true_yaw;
  last_odom_pos_ = update.curr_odom_pos;
  last_odom_yaw_ = update.curr_odom_yaw;
}

// Update the data in the interface
void DiffDrivePlugin::write_position_data()
{
  // // TODO: Data timestamp
  // pos_iface_->data->head.time = Simulator::Instance()->GetSimTime().Double();

  // pose.pos.x = odomPose[0];
  // pose.pos.y = odomPose[1];
  // pose.rot.GetYaw() = NORMALIZE(odomPose[2]);

  // pos_iface_->data->velocity.pos.x = odomVel[0];
  // pos_iface_->data->velocity.yaw = odomVel[2];

  math::Pose orig_pose = this->parent->GetWorldPose();

  math::Pose new_pose = orig_pose;
  new_pose.pos.x = odomPose[0];
  new_pose.pos.y = odomPose[1];
  new_pose.rot.SetFromEuler(math::Vector3(0,0,odomPose[2]));

  this->parent->SetWorldPose( new_pose );
}

GZ_REGISTER_MODEL_PLUGIN(DiffDrivePlugin)
}

/* vim: set ts=2 sts=2 sw=2: */
