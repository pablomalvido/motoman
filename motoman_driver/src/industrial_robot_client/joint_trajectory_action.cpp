/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2014, Fraunhofer IPA
 * Author: Thiago de Freitas
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *  notice, this list of conditions and the following disclaimer in the
 *  documentation and/or other materials provided with the distribution.
 *  * Neither the name of the Fraunhofer IPA, nor the names
 *  of its contributors may be used to endorse or promote products derived
 *  from this software without specific prior written permission.
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

#include <motoman_driver/industrial_robot_client/joint_trajectory_action.h>
#include "motoman_driver/industrial_robot_client/motoman_utils.h"
#include <industrial_robot_client/utils.h>
#include <industrial_utils/param_utils.h>
#include <industrial_utils/utils.h>
#include <map>
#include <string>
#include <vector>

using industrial_robot_client::motoman_utils::getJointGroups;

namespace industrial_robot_client
{
namespace joint_trajectory_action
{

const double JointTrajectoryAction::WATCHD0G_PERIOD_ = 1.0;
const double JointTrajectoryAction::DEFAULT_GOAL_THRESHOLD_ = 0.01;

JointTrajectoryAction::JointTrajectoryAction() :
  action_server_(node_, "joint_trajectory_action",
                 boost::bind(&JointTrajectoryAction::goalCB, this, _1),
                 boost::bind(&JointTrajectoryAction::cancelCB, this, _1), false)
{
  ros::NodeHandle pn("~");

  pn.param("constraints/goal_threshold", goal_threshold_, DEFAULT_GOAL_THRESHOLD_);

  std::map<int, RobotGroup> robot_groups;
  getJointGroups("topic_list", robot_groups);

  for (size_t i = 0; i < robot_groups.size(); i++)
  {
    std::string joint_path_action_name = robot_groups[i].get_ns() + "/" + robot_groups[i].get_name();
    std::vector<std::string> rg_joint_names = robot_groups[i].get_joint_names();
    int group_number_int = robot_groups[i].get_group_id();

    all_joint_names_.insert(all_joint_names_.end(), rg_joint_names.begin(), rg_joint_names.end());

    actionServer_ = new actionlib::ActionServer<control_msgs::FollowJointTrajectoryAction>(
      node_, joint_path_action_name + "/joint_trajectory_action" , false);
    actionServer_->registerGoalCallback(
      boost::bind(&JointTrajectoryAction::goalCB,
                  this, _1, group_number_int));
    actionServer_->registerCancelCallback(
      boost::bind(&JointTrajectoryAction::cancelCB,
                  this, _1, group_number_int));

    pub_trajectory_command_ = node_.advertise<motoman_msgs::DynamicJointTrajectory>(
                                joint_path_action_name + "/joint_path_command", 1);
    sub_trajectory_state_  = this->node_.subscribe<control_msgs::FollowJointTrajectoryFeedback>(
                               joint_path_action_name + "/feedback_states", 1,
                               boost::bind(&JointTrajectoryAction::controllerStateCB,
                                           this, _1, group_number_int));
    sub_robot_status_ = node_.subscribe(
                          "robot_status", 1, &JointTrajectoryAction::robotStatusCB, this);

    pub_trajectories_[group_number_int] = pub_trajectory_command_;
    sub_trajectories_[group_number_int] = (sub_trajectory_state_);
    sub_status_[group_number_int] = (sub_robot_status_);

    this->act_servers_[group_number_int] = actionServer_;

    this->act_servers_[group_number_int]->start();

    this->watchdog_timer_map_[group_number_int] = node_.createTimer(
          ros::Duration(WATCHD0G_PERIOD_), boost::bind(
            &JointTrajectoryAction::watchdog, this, _1, group_number_int));
    // Multigroup trajectories sub-group active map initialization
    multi_group_active_goals_map_[group_number_int] = false;
  }

  pub_trajectory_command_ = node_.advertise<motoman_msgs::DynamicJointTrajectory>(
                              "joint_path_command", 1);
  // Set watchdog timer for entire robot state.
  this->watchdog_timer_ = node_.createTimer(
          ros::Duration(WATCHD0G_PERIOD_), boost::bind(
            &JointTrajectoryAction::watchdog, this, _1));

  this->robot_groups_ = robot_groups;

  action_server_.start();
}

JointTrajectoryAction::~JointTrajectoryAction()
{
}

void JointTrajectoryAction::robotStatusCB(
  const industrial_msgs::RobotStatusConstPtr &msg)
{
  last_robot_status_ = msg;  // caching robot status for later use.
}

void JointTrajectoryAction::watchdog(const ros::TimerEvent &e)
{
  // Some debug logging
  if (!last_trajectory_state_)
  {
    ROS_DEBUG("Waiting for subscription to joint trajectory state");
  }
  if (!trajectory_state_recvd_)
  {
    //ROS_DEBUG("Trajectory state not received since last watchdog");
    ROS_DEBUG("Trajectory state of the entire robot not received since last watchdog");
    for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
    {
      if (!trajectory_state_recvd_map_[group_number])
      {
        ROS_DEBUG("Group [%s] state not received since last watchdog", 
          robot_groups_[group_number].get_name().c_str());
      }
    }
  }

  // Aborts the active goal if the controller does not appear to be active.
  if (has_active_goal_)
  {
    if (!trajectory_state_recvd_)
    {
      // last_trajectory_state_ is null if the subscriber never makes a connection
      if (!last_trajectory_state_)
      {
        ROS_WARN("Aborting goal because we have never heard a controller state message.");
      }
      else
      {
        ROS_WARN_STREAM(
          "Aborting goal because we haven't heard from the controller in " << WATCHD0G_PERIOD_ << " seconds");
      }
      abortGoal();
    }
  }

  // Reset the trajectory state received flag
  trajectory_state_recvd_ = false;
  for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
    trajectory_state_recvd_map_[group_number] = false;
}

void JointTrajectoryAction::watchdog(const ros::TimerEvent &e, int group_number)
{
  // Some debug logging
  if (!last_trajectory_state_map_[group_number])
  {
    ROS_DEBUG("Waiting for subscription to joint trajectory state");
  }
  if (!trajectory_state_recvd_map_[group_number])
  {
    //ROS_DEBUG("Trajectory state not received since last watchdog");
    ROS_DEBUG("Trajectory state of group [%s] not received since last watchdog", 
      robot_groups_[group_number].get_name().c_str());
  }

  // Aborts the active goal if the controller does not appear to be active.
  if (has_active_goal_map_[group_number])
  {
    if (!trajectory_state_recvd_map_[group_number])
    {
      // last_trajectory_state_ is null if the subscriber never makes a connection
      if (!last_trajectory_state_map_[group_number])
      {
        //ROS_WARN("Aborting goal because we have never heard a controller state message.");
        ROS_WARN("Aborting goal because we have never heard a controller state message for group [%s].",
          robot_groups_[group_number].get_name().c_str());
      }
      else
      {
        ROS_WARN_STREAM(
          "Aborting goal because we haven't heard from the controller in " << WATCHD0G_PERIOD_ << " seconds");
      }
      abortGoal(group_number);
    }
  }
}

void JointTrajectoryAction::goalCB(JointTractoryActionServer::GoalHandle gh)
{
  ROS_DEBUG("Multi-group trajectory execution request received");

  if (!gh.getGoal()->trajectory.points.empty()) 
  {
    std::map<int, size_t> group_joints_start_idx;
    ros::Duration last_time_from_start(0.0);

    if (has_active_goal_)  // Cancels the currently active goal.
    {
      ROS_WARN("Received new goal, canceling current goal");
      abortGoal();
    }
    // Detect which robot groups are active in the current motion trajectory
    for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
    {
      size_t ros_idx = std::find(
                        gh.getGoal()->trajectory.joint_names.begin(),
                        gh.getGoal()->trajectory.joint_names.end(),
                        robot_groups_[group_number].get_joint_names()[0])
                      - gh.getGoal()->trajectory.joint_names.begin();

      bool is_found = ros_idx < gh.getGoal()->trajectory.joint_names.size();

      if (is_found)
      {
        multi_group_active_goals_map_[group_number] = true;
        group_joints_start_idx.insert(std::pair<int, size_t>(group_number, ros_idx));
        ROS_DEBUG("Group [%s] is active in trajectory plan", robot_groups_[group_number].get_name().c_str());
      }

      else
      {
        multi_group_active_goals_map_[group_number] = false;
        ROS_DEBUG("Group [%s] not present in trajectory plan, using its last received joint positions as goal", 
                                                                robot_groups_[group_number].get_name().c_str());
      }
    }
    // Check if active robot groups are already in goal position
    if (withinGoalConstraints(last_trajectory_state_, gh.getGoal()->trajectory))
    {
      ROS_INFO("Already within goal constraints, setting goal succeeded");
      gh.setAccepted();
      gh.setSucceeded();
      has_active_goal_ = false;
      // Set robot groups active goals flags to false
      for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
        multi_group_active_goals_map_[group_number] = false;
    }
    else  // Sends the trajectory along to the controller 
    {
      gh.setAccepted();
      active_goal_ = gh;
      has_active_goal_ = true;
      motoman_msgs::DynamicJointTrajectory dyn_traj;

      ROS_DEBUG("Publishing trajectory");

      int group_number;
      current_traj_ = active_goal_.getGoal()->trajectory;

      for (int i = 0; i < gh.getGoal()->trajectory.points.size(); i++)
      {
        motoman_msgs::DynamicJointPoint dpoint;

        for (int rbt_idx = 0; rbt_idx < robot_groups_.size(); rbt_idx++)
        {
          group_number = rbt_idx;
          motoman_msgs::DynamicJointsGroup dyn_group;

          int num_joints = robot_groups_[group_number].get_joint_names().size();

          if (multi_group_active_goals_map_[group_number])  // If group is active on current goal
          { 
            if (gh.getGoal()->trajectory.points[i].positions.empty())
            {
              std::vector<double> positions(num_joints, 0.0);
              dyn_group.positions = positions;
            }
            else
              dyn_group.positions.insert(
                dyn_group.positions.begin(),
                gh.getGoal()->trajectory.points[i].positions.begin() + group_joints_start_idx[rbt_idx],
                gh.getGoal()->trajectory.points[i].positions.begin() + group_joints_start_idx[rbt_idx]
                + robot_groups_[rbt_idx].get_joint_names().size());

            if (gh.getGoal()->trajectory.points[i].velocities.empty())
            {
              std::vector<double> velocities(num_joints, 0.0);
              dyn_group.velocities = velocities;
            }
            else
              dyn_group.velocities.insert(
                dyn_group.velocities.begin(),
                gh.getGoal()->trajectory.points[i].velocities.begin()
                + group_joints_start_idx[rbt_idx], gh.getGoal()->trajectory.points[i].velocities.begin()
                + group_joints_start_idx[rbt_idx] + robot_groups_[rbt_idx].get_joint_names().size());

            if (gh.getGoal()->trajectory.points[i].accelerations.empty())
            {
              std::vector<double> accelerations(num_joints, 0.0);
              dyn_group.accelerations = accelerations;
            }
            else
              dyn_group.accelerations.insert(
                dyn_group.accelerations.begin(),
                gh.getGoal()->trajectory.points[i].accelerations.begin()
                + group_joints_start_idx[rbt_idx], gh.getGoal()->trajectory.points[i].accelerations.begin()
                + group_joints_start_idx[rbt_idx] + robot_groups_[rbt_idx].get_joint_names().size());
            if (gh.getGoal()->trajectory.points[i].effort.empty())
            {
              std::vector<double> effort(num_joints, 0.0);
              dyn_group.effort = effort;
            }
            else
              dyn_group.effort.insert(
                dyn_group.effort.begin(),
                gh.getGoal()->trajectory.points[i].effort.begin()
                + group_joints_start_idx[rbt_idx], gh.getGoal()->trajectory.points[i].effort.begin()
                + group_joints_start_idx[rbt_idx] + robot_groups_[rbt_idx].get_joint_names().size());
            dyn_group.time_from_start = gh.getGoal()->trajectory.points[i].time_from_start;
            dyn_group.group_number = group_number;
            dyn_group.num_joints = dyn_group.positions.size();
          }

          // Generating message for groups that were not present in the trajectory message
          // Assume that joints from these groups will mantain its current position.
          // Velocity, acceleration and effort are zero out. 
          else
          {
            std::vector<double> positions = last_trajectory_state_map_[group_number]->actual.positions;
            std::vector<double> velocities(num_joints, 0.0);
            std::vector<double> accelerations(num_joints, 0.0);
            std::vector<double> effort(num_joints, 0.0);

            dyn_group.positions = positions;
            dyn_group.velocities = velocities;
            dyn_group.accelerations = accelerations;
            dyn_group.effort = effort;

            dyn_group.time_from_start = gh.getGoal()->trajectory.points[i].time_from_start;
            dyn_group.group_number = group_number;
            dyn_group.num_joints = num_joints;
          }
          dpoint.groups.push_back(dyn_group);
        }
        dpoint.num_groups = dpoint.groups.size();
        dyn_traj.points.push_back(dpoint);
      }
      dyn_traj.header = gh.getGoal()->trajectory.header;
      dyn_traj.header.stamp = ros::Time::now();
      // Publishing the joint names for the 4 groups
      dyn_traj.joint_names = all_joint_names_;

      this->pub_trajectory_command_.publish(dyn_traj);
    }
  }
  else
  {
    ROS_ERROR("Multi-group joint trajectory action failed on empty trajectory");
    control_msgs::FollowJointTrajectoryResult rslt;
    rslt.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_GOAL;
    gh.setRejected(rslt, "Empty trajectory");
  }

  // Adding some informational log messages to indicate unsupported goal constraints
  if (!gh.getGoal()->goal_tolerance.empty())
  {
    ROS_WARN_STREAM(
      "Ignoring goal tolerance in action, using paramater tolerance of " << goal_threshold_ << " instead");
  }
  if (!gh.getGoal()->path_tolerance.empty())
  {
    ROS_WARN_STREAM("Ignoring goal path tolerance, option not supported by ROS-Industrial drivers");
  }
}

void JointTrajectoryAction::cancelCB(JointTractoryActionServer::GoalHandle gh)
{
  ROS_DEBUG("Received action cancel request");
  if (active_goal_ == gh)
  {
    // Stops the controller.
    motoman_msgs::DynamicJointTrajectory empty;
    empty.joint_names = all_joint_names_;
    this->pub_trajectory_command_.publish(empty);

    // Marks the current goal as canceled.
    active_goal_.setCanceled();
    has_active_goal_ = false;
    // Set robot groups active goals flags to false
    for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
      multi_group_active_goals_map_[group_number] = false;
  }
  else
  {
    ROS_WARN("Active goal and goal cancel do not match, ignoring cancel request");
  }
}

void JointTrajectoryAction::goalCB(JointTractoryActionServer::GoalHandle gh, int group_number)
{
  if (!gh.getGoal()->trajectory.points.empty())
  {
    if (industrial_utils::isSimilar(
          this->robot_groups_[group_number].get_joint_names(),
          gh.getGoal()->trajectory.joint_names))
    {
      // Cancels the currently active goal.
      if (has_active_goal_map_[group_number])
      {
        ROS_WARN("Received new goal, canceling current goal");
        abortGoal(group_number);
      }
      // Sends the trajectory along to the controller
      if (withinGoalConstraints(last_trajectory_state_map_[group_number],
                                gh.getGoal()->trajectory, group_number))
      {
        ROS_INFO_STREAM("Already within goal constraints, setting goal succeeded");
        gh.setAccepted();
        gh.setSucceeded();
        has_active_goal_map_[group_number] = false;
      }
      else
      {
        gh.setAccepted();
        active_goal_map_[group_number] = gh;
        has_active_goal_map_[group_number]  = true;

        ROS_INFO("Publishing trajectory");

        current_traj_map_[group_number] = active_goal_map_[group_number].getGoal()->trajectory;

        int num_joints = robot_groups_[group_number].get_joint_names().size();

        motoman_msgs::DynamicJointTrajectory dyn_traj;

        for (size_t i = 0; i < current_traj_map_[group_number].points.size(); ++i)
        {
          motoman_msgs::DynamicJointsGroup dyn_group;
          motoman_msgs::DynamicJointPoint dyn_point;

          if (gh.getGoal()->trajectory.points[i].positions.empty())
          {
            std::vector<double> positions(num_joints, 0.0);
            dyn_group.positions = positions;
          }
          else
            dyn_group.positions = gh.getGoal()->trajectory.points[i].positions;

          if (gh.getGoal()->trajectory.points[i].velocities.empty())
          {
            std::vector<double> velocities(num_joints, 0.0);
            dyn_group.velocities = velocities;
          }
          else
            dyn_group.velocities = gh.getGoal()->trajectory.points[i].velocities;
          if (gh.getGoal()->trajectory.points[i].accelerations.empty())
          {
            std::vector<double> accelerations(num_joints, 0.0);
            dyn_group.accelerations = accelerations;
          }
          else
            dyn_group.accelerations = gh.getGoal()->trajectory.points[i].accelerations;
          if (gh.getGoal()->trajectory.points[i].effort.empty())
          {
            std::vector<double> effort(num_joints, 0.0);
            dyn_group.effort = effort;
          }
          else
            dyn_group.effort = gh.getGoal()->trajectory.points[i].effort;
          dyn_group.time_from_start = gh.getGoal()->trajectory.points[i].time_from_start;
          dyn_group.group_number = group_number;
          dyn_group.num_joints = robot_groups_[group_number].get_joint_names().size();
          dyn_point.groups.push_back(dyn_group);

          dyn_point.num_groups = 1;
          dyn_traj.points.push_back(dyn_point);
        }
        dyn_traj.header = gh.getGoal()->trajectory.header;
        dyn_traj.joint_names = gh.getGoal()->trajectory.joint_names;
        this->pub_trajectories_[group_number].publish(dyn_traj);
      }
    }
    else
    {
      ROS_ERROR("Joint trajectory action failing on invalid joints");
      control_msgs::FollowJointTrajectoryResult rslt;
      rslt.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_JOINTS;
      gh.setRejected(rslt, "Joint names do not match");
    }
  }
  else
  {
    ROS_ERROR("Joint trajectory action failed on empty trajectory");
    control_msgs::FollowJointTrajectoryResult rslt;
    rslt.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_GOAL;
    gh.setRejected(rslt, "Empty trajectory");
  }

  // Adding some informational log messages to indicate unsupported goal constraints
  if (gh.getGoal()->goal_time_tolerance.toSec() > 0.0)
  {
    ROS_WARN_STREAM("Ignoring goal time tolerance in action goal, may be supported in the future");
  }
  if (!gh.getGoal()->goal_tolerance.empty())
  {
    ROS_WARN_STREAM(
      "Ignoring goal tolerance in action, using paramater tolerance of " << goal_threshold_ << " instead");
  }
  if (!gh.getGoal()->path_tolerance.empty())
  {
    ROS_WARN_STREAM("Ignoring goal path tolerance, option not supported by ROS-Industrial drivers");
  }
}

void JointTrajectoryAction::cancelCB(
  JointTractoryActionServer::GoalHandle gh, int group_number)
{
  ROS_DEBUG("Received action cancel request");
  if (active_goal_map_[group_number] == gh)
  {
    // Stops the controller.
    motoman_msgs::DynamicJointTrajectory empty;
    empty.joint_names = robot_groups_[group_number].get_joint_names();
    this->pub_trajectories_[group_number].publish(empty);

    // Marks the current goal as canceled.
    active_goal_map_[group_number].setCanceled();
    has_active_goal_map_[group_number] = false;
  }
  else
  {
    ROS_WARN("Active goal and goal cancel do not match, ignoring cancel request");
  }
}

void JointTrajectoryAction::controllerStateCB(
  const control_msgs::FollowJointTrajectoryFeedbackConstPtr &msg, int robot_id)
{
  ROS_DEBUG("Checking controller state feedback");
  last_trajectory_state_map_[robot_id] = msg;
  trajectory_state_recvd_map_[robot_id] = true;

  // If a multi-group goal is active, process feedback as multi-group feedback.
  if (has_active_goal_)   
  {
    controllerStateCB(msg);
    return;
  }
  else if (!has_active_goal_map_[robot_id]) 
  {
    ROS_DEBUG("No active goal for group [%s], ignoring feedback", robot_groups_[robot_id].get_name().c_str());
    return;
  }

  if (current_traj_map_[robot_id].points.empty())
  {
    ROS_DEBUG("Current trajectory is empty, ignoring feedback");
    return;
  }

  if (!industrial_utils::isSimilar(robot_groups_[robot_id].get_joint_names(), msg->joint_names))
  {
    ROS_ERROR("Joint names from the controller don't match our joint names.");
    return;
  }

  // Checking for goal constraints
  // Checks that we have ended inside the goal constraints and has motion stopped

  ROS_DEBUG("Checking goal constraints");
  if (withinGoalConstraints(last_trajectory_state_map_[robot_id], current_traj_map_[robot_id], robot_id))
  {
    if (last_robot_status_)
    {
      // Additional check for motion stoppage since the controller goal may still
      // be moving.  The current robot driver calls a motion stop if it receives
      // a new trajectory while it is still moving.  If the driver is not publishing
      // the motion state (i.e. old driver), this will still work, but it warns you.
      if (last_robot_status_->in_motion.val == industrial_msgs::TriState::FALSE)
      {
        ROS_INFO("Inside goal constraints, stopped moving, return success for action");
        active_goal_map_[robot_id].setSucceeded();
        has_active_goal_map_[robot_id] = false;
      }
      else if (last_robot_status_->in_motion.val == industrial_msgs::TriState::UNKNOWN)
      {
        ROS_INFO("Inside goal constraints, return success for action");
        ROS_WARN("Robot status in motion unknown, the robot driver node and controller code should be updated");
        active_goal_map_[robot_id].setSucceeded();
        has_active_goal_map_[robot_id] = false;
      }
      else
      {
        ROS_DEBUG("Within goal constraints but robot is still moving");
      }
    }
    else
    {
      ROS_INFO("Inside goal constraints, return success for action");
      ROS_WARN("Robot status is not being published the robot driver node and controller code should be updated");
      active_goal_map_[robot_id].setSucceeded();
      has_active_goal_map_[robot_id] = false;
    }
  }
}

void JointTrajectoryAction::controllerStateCB(
  const control_msgs::FollowJointTrajectoryFeedbackConstPtr &msg)
{
  ROS_DEBUG("Checking controller state feedback");
  last_trajectory_state_ = msg;
  trajectory_state_recvd_ = true;

  // Full robot state is marked as received if all robot groups states have been received.
  for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
    trajectory_state_recvd_ = trajectory_state_recvd_ && trajectory_state_recvd_map_[group_number];

  if (!trajectory_state_recvd_)
  {
    ROS_DEBUG("Waiting for all robot groups feedback states before processing multi-group feedback");
    return;
  }

  if (!has_active_goal_)
  {
    ROS_DEBUG("No active multi-group goal, ignoring feedback");
    return;
  }
  if (current_traj_.points.empty())
  {
    ROS_DEBUG("Current trajectory is empty, ignoring feedback");
    return;
  }

  // Checking for goal constraints
  // Checks that we have ended inside the goal constraints and has motion stopped

  ROS_DEBUG("Checking goal constraints");
  if (withinGoalConstraints(last_trajectory_state_, current_traj_))
  {
    if (last_robot_status_)
    {
      // Additional check for motion stoppage since the controller goal may still
      // be moving.  The current robot driver calls a motion stop if it receives
      // a new trajectory while it is still moving.  If the driver is not publishing
      // the motion state (i.e. old driver), this will still work, but it warns you.
      if (last_robot_status_->in_motion.val == industrial_msgs::TriState::FALSE)
      {
        ROS_INFO("Inside goal constraints, stopped moving, return success for action");
        active_goal_.setSucceeded();
        has_active_goal_ = false;
        // Set robot groups active goals flags to false
        for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
          multi_group_active_goals_map_[group_number] = false;
      }
      else if (last_robot_status_->in_motion.val == industrial_msgs::TriState::UNKNOWN)
      {
        ROS_INFO("Inside goal constraints, return success for action");
        ROS_WARN("Robot status in motion unknown, the robot driver node and controller code should be updated");
        active_goal_.setSucceeded();
        has_active_goal_ = false;
        // Set robot groups active goals flags to false
        for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
          multi_group_active_goals_map_[group_number] = false;
      }
      else
      {
        ROS_DEBUG("Within goal constraints but robot is still moving");
      }
    }
    else
    {
      ROS_INFO("Inside goal constraints, return success for action");
      ROS_WARN("Robot status is not being published the robot driver node and controller code should be updated");
      active_goal_.setSucceeded();
      has_active_goal_ = false;
      // Set robot groups active goals flags to false
      for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
          multi_group_active_goals_map_[group_number] = false;
    }
  }
}

void JointTrajectoryAction::abortGoal()
{
  // Stops the controller.
  motoman_msgs::DynamicJointTrajectory empty;
  pub_trajectory_command_.publish(empty);

  // Marks the current goal as aborted.
  active_goal_.setAborted();
  has_active_goal_ = false;
  // Set robot groups active goals flags to false
  for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
    multi_group_active_goals_map_[group_number] = false;
}

void JointTrajectoryAction::abortGoal(int robot_id)
{
  // Stops the controller.
  motoman_msgs::DynamicJointTrajectory empty;
  pub_trajectories_[robot_id].publish(empty);

  // Marks the current goal as aborted.
  active_goal_map_[robot_id].setAborted();
  has_active_goal_map_[robot_id] = false;
}

bool JointTrajectoryAction::withinGoalConstraints(
  const control_msgs::FollowJointTrajectoryFeedbackConstPtr &msg,
  const trajectory_msgs::JointTrajectory & traj)
{
  bool rtn = false;
  if (traj.points.empty())
  {
    ROS_WARN("Empty joint trajectory passed to check goal constraints, return false");
    rtn = false;
  }
  else
  {
    // Asume true, and proof wrong by checking each robot group that is active in current goal
    std::map<int, bool> groups_at_goal_state_map;
    int last_point = traj.points.size() - 1;

    for (int group_number = 0; group_number < robot_groups_.size(); group_number++)
    { 

      // Check if robot group is active in current multi-group trajectory goal.
      if (multi_group_active_goals_map_[group_number])
      {
        ROS_DEBUG("Checking if group [%s] has reached its goal", robot_groups_[group_number].get_name().c_str());
        // Asume group is not at goal position
        groups_at_goal_state_map[group_number] = false;

        size_t group_joints_start_idx = std::find(
                        traj.joint_names.begin(),
                        traj.joint_names.end(),
                        robot_groups_[group_number].get_joint_names()[0])
                      - traj.joint_names.begin();
        // Get an ordered map of the robot group last feedback state.
        std::map<std::string, double> robot_group_last_state_map;
        industrial_robot_client::utils::toMap(robot_groups_[group_number].get_joint_names(), 
                                              last_trajectory_state_map_[group_number]->actual.positions, 
                                              robot_group_last_state_map);
        // Get an ordered map of the robot group goal state.
        std::vector<double> group_goal_positions( 
                traj.points[last_point].positions.begin() + group_joints_start_idx,
                traj.points[last_point].positions.begin() + group_joints_start_idx
                  + robot_groups_[group_number].get_joint_names().size());
        std::map<std::string, double> group_traj_last_point_map;    
        industrial_robot_client::utils::toMap(robot_groups_[group_number].get_joint_names(), 
                                              group_goal_positions, 
                                              group_traj_last_point_map);
        // Check if group is already at goal position 
        if ( !industrial_robot_client::utils::isWithinRange(robot_groups_[group_number].get_joint_names(),
                                                          group_traj_last_point_map,
                                                          robot_group_last_state_map, 
                                                          goal_threshold_) )
        {
          // Current Robot Group is not in goal position yet
          groups_at_goal_state_map[group_number] = false;
          return false;        // Stop checking other sub-groups 
        }
        else  
          groups_at_goal_state_map[group_number] = true;
      }
      else  // Group is not active -> should not be checked.
      {
        continue;
      }
    }
    if (groups_at_goal_state_map.empty()) 
    {
      ROS_ERROR("Multi-group goal has not a single active group");
      return false;
    }
    // If this point is reached, all active groups are at goal position
    return true;
  }
  return rtn;
}

bool JointTrajectoryAction::withinGoalConstraints(
  const control_msgs::FollowJointTrajectoryFeedbackConstPtr &msg,
  const trajectory_msgs::JointTrajectory & traj, int robot_id)
{
  bool rtn = false;
  if (traj.points.empty())
  {
    ROS_WARN("Empty joint trajectory passed to check goal constraints, return false");
    rtn = false;
  }
  else
  {
    int last_point = traj.points.size() - 1;

    int group_number = robot_id;

    if (industrial_robot_client::utils::isWithinRange(
          robot_groups_[group_number].get_joint_names(),
          last_trajectory_state_map_[group_number]->actual.positions, traj.joint_names,
          traj.points[last_point].positions, goal_threshold_))
    {
      rtn = true;
    }
    else
    {
      rtn = false;
    }
  }
  return rtn;
}

}  // namespace joint_trajectory_action
}  // namespace industrial_robot_client


