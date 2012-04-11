/*
 * Copyright (c) 2012, The Johns Hopkins University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of The Johns Hopkins University. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <iostream>

#include <Eigen/Dense>

#include <ros/ros.h>

#include <kdl/jntarray.hpp>
#include <kdl/jntarrayvel.hpp>
#include <kdl/tree.hpp>
#include <kdl/chain.hpp>
#include <kdl/chaindynparam.hpp>
#include <rtt/RTT.hpp>
#include <rtt/Port.hpp>

#include <sensor_msgs/JointState.h>

#include <bard_common/util.h>
#include <bard_simulation/wam_stub.h>

using namespace bard_common;
using namespace bard_simulation;

WAMStub::WAMStub(string const& name) :
  WAMInterface(name)
  //RTT Properties
  ,coriolis_enabled_(true)
  ,gravity_enabled_(true)
  ,gravity_(3,0.0)
  ,damping_(7,0.0)
{
  // Initialize all the RTT properties/ports/services
  this->init_rtt_interface();

  // Initialize simulator-specific interface
  this->addProperty("coriolis_enabled",gravity_enabled_)
    .doc("True if coriolis forces are applied to the simulated arm.");
  this->addProperty("gravity_enabled",gravity_enabled_)
    .doc("True if gravity is applied to the simulated arm.");
  this->addProperty("gravity",gravity_)
    .doc("Gravity vector in the root_link frame.");
  this->addProperty("damping",damping_)
    .doc("Damping coefficients for each joint.");

  ROS_INFO_STREAM("WAM stub component \""<<name<<"\" constructed !");
}

bool WAMStub::configureHook()
{
  // Initialize the arm kinematics from the robot description
  if(!this->init_kinematics()) {
    return false;
  }

  // Initialize dynamics
  kdl_chain_dynamics_.reset(
      new KDL::ChainDynParam(
        kdl_chain_,
        KDL::Vector(gravity_[0],gravity_[1],gravity_[2])));

  // Initialize arrays and ports
  damping_.resize(n_dof_,0.0);

  return true;
}

bool WAMStub::startHook()
{
  // Check the data ports
  if ( !torques_in_port_.connected() ) {
    ROS_WARN_STREAM("No connection to \"torques_in\" for WAM stub!");
  }
  if ( !positions_out_port_.connected() ) {
    ROS_WARN_STREAM("No connection to \"positions_out\" for WAM stub!");
  }

  if(gravity_enabled_ && (fabs(gravity_[0] + gravity_[1] + gravity_[2]) < 1E-6)) {
    ROS_WARN("Gravity is enabled, but set to zero!");
  }

  // This is a stub, so we can always set the position
  this->calibrate_position(initial_positions_);

  ROS_INFO_STREAM("WAM stub started!");
  return true;
}

void WAMStub::updateHook()
{
  // Only send joint torques if new data is coming in
  if( torques_in_port_.read( torques_ ) == RTT::NewData ) {
    // Apply torque limits
    for(unsigned int i=0; i<n_dof_; i++) {
      if(fabs(torques_(i)) > torque_limits_[i]) {
        // Truncate this joint torque
        torques_(i) = (torques_(i)>0.0)?(torque_limits_[i]):(-torque_limits_[i]);
        // TODO: Raise warning flag
      }
    }
  }

  // Initialize dynamic force vectors
  KDL::JntArray coriolis_torques_(n_dof_);
  KDL::JntArray gravity_torques_(n_dof_);
  KDL::SetToZero(coriolis_torques_);
  KDL::SetToZero(gravity_torques_);

  // Compute coriolis torques
  if(coriolis_enabled_) {
    kdl_chain_dynamics_->JntToCoriolis(positions_.q, positions_.qdot, coriolis_torques_);
  }
  // Compute gravity torques
  if(gravity_enabled_) {
    kdl_chain_dynamics_->JntToGravity(positions_.q, gravity_torques_);
  }

  // Compute inertia matrix
  KDL::JntSpaceInertiaMatrix joint_inertia_(n_dof_);
  kdl_chain_dynamics_->JntToMass(positions_.q, joint_inertia_);
    
  // Get the actual loop period
  loop_period_ = RTT::os::TimeService::Instance()->secondsSince(last_loop_time_);
  last_loop_time_ = RTT::os::TimeService::Instance()->getTicks();
  
  // Update joint positions
  double &dT = loop_period_;
  Eigen::VectorXd &q = positions_.q.data;
  Eigen::VectorXd &qdot = positions_.qdot.data;
  Eigen::VectorXd &tau = torques_.data;
  Eigen::MatrixXd &M = joint_inertia_.data;
  Eigen::MatrixXd const &c = Eigen::Map<Eigen::VectorXd>(&damping_[0],n_dof_);

  // Check sizes of matrices
  ROS_DEBUG_STREAM("q:\n"<<q);
  ROS_DEBUG_STREAM("qdot:\n"<<qdot);
  ROS_DEBUG_STREAM("tau:\n"<<tau);
  ROS_DEBUG_STREAM("M:\n"<<M);
  ROS_DEBUG_STREAM("c:\n"<<c);

  // Integrate!
  positions_.q.data = q + dT*(qdot);
  positions_.qdot.data = qdot + dT*(M.inverse()*tau - c.cwiseProduct(qdot));

  // Send joint positions
  positions_out_port_.write( positions_ );

  // Publish joint state
  this->publish_throttled_joint_state();
}

void WAMStub::stopHook()
{
}

void WAMStub::cleanupHook()
{
}

void WAMStub::calibrate_position(std::vector<double> &actual_positions)
{
  for(unsigned int i=0; i<n_dof_; i++) {
    positions_.q(i) = actual_positions[i];
    positions_.qdot(i) = 0.0;
  }
}

void WAMStub::set_velocity_warn(unsigned int thresh)
{
}
void WAMStub::set_velocity_fault(unsigned int thresh)
{
}
void WAMStub::set_torque_warn(unsigned int thresh)
{
}
void WAMStub::set_torque_fault(unsigned int thresh)
{
}