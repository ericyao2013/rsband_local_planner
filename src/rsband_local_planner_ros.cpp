/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2016, George Kouros.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the the copyright holder nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author:  George Kouros
*********************************************************************/

#include "rsband_local_planner/rsband_local_planner_ros.h"

#include <base_local_planner/goal_functions.h>
#include <pluginlib/class_list_macros.h>

PLUGINLIB_DECLARE_CLASS(rsband_local_planner, RSBandPlannerROS,
  rsband_local_planner::RSBandPlannerROS, nav_core::BaseLocalPlanner);

namespace rsband_local_planner
{

  RSBandPlannerROS::RSBandPlannerROS() : initialized_(false)
  {
  }


  RSBandPlannerROS::~RSBandPlannerROS()
  {
  }


  void RSBandPlannerROS::initialize(std::string name,
    tf::TransformListener* tfListener, costmap_2d::Costmap2DROS* costmapROS)
  {
    if (initialized_)
    {
      ROS_WARN("Planner already initialized. Should not be called more than "
        "once");
      return;
    }

    // store tflistener and costmapROS
    tfListener_ = tfListener;
    costmapROS_ = costmapROS;

    ros::NodeHandle pnh("~/" + name);

    // initialize plan publishers
    globalPlanPub_ = pnh.advertise<nav_msgs::Path>("global_plan", 1);
    localPlanPub_ = pnh.advertise<nav_msgs::Path>("local_plan", 1);
    ebandPlanPub_ = pnh.advertise<nav_msgs::Path>("eband_plan", 1);
    rsbandPlanPub_ = pnh.advertise<nav_msgs::Path>("reeds_sheep_plan", 1);

    // create new eband planner
    ebandPlanner_ = boost::shared_ptr<eband_local_planner::EBandPlanner>(
      new eband_local_planner::EBandPlanner(name, costmapROS_));

    // create new reeds shepp band planner
    rsbandPlanner_ = boost::shared_ptr<ReedsSheppPlanner>(
      new ReedsSheppPlanner(name, costmapROS_, tfListener_));
    eband2RSStrategy_ = 0;

    // create new path tracking controller
    ptc_ = boost::shared_ptr<CarLikeFuzzyPTC>(new CarLikeFuzzyPTC(name));

    // create and initialize dynamic reconfigure
    drs_.reset(new drs(pnh));
    drs::CallbackType cb =
      boost::bind(&RSBandPlannerROS::reconfigureCallback, this, _1, _2);
    drs_->setCallback(cb);

    // set initilized
    initialized_ = true;

    ROS_DEBUG("Local Planner Plugin Initialized!");
  }

  void RSBandPlannerROS::reconfigureCallback(RSBandPlannerConfig& config,
    uint32_t level)
  {
    xyGoalTolerance_ = config.xy_goal_tolerance;
    yawGoalTolerance_ = config.yaw_goal_tolerance;

    eband2RSStrategy_ = config.eband_to_rs_strategy;

    if (rsbandPlanner_)
      rsbandPlanner_->reconfigure(config);
    else
      ROS_ERROR("Reconfigure CB called before reeds shepp planner "
        "initialization");

    if (ptc_)
      ptc_->reconfigure(config);
    else
      ROS_ERROR("Reconfigure CB called before path tracking controller "
        "initialization!");
  }


  bool RSBandPlannerROS::setPlan(
    const std::vector<geometry_msgs::PoseStamped>& globalPlan)
  {
    if (!initialized_)
    {
      ROS_ERROR("Planner must be initialized before setPlan is called!");
      return false;
    }

    globalPlan_ = globalPlan;

    std::vector<int> planStartEndCounters(2, globalPlan_.size());

    if (!eband_local_planner::transformGlobalPlan(*tfListener_, globalPlan_,
        *costmapROS_, costmapROS_->getGlobalFrameID(), transformedPlan_,
        planStartEndCounters))
    {
      ROS_WARN("Could not transform global plan to the local frame");
      return false;
    }

    if (transformedPlan_.empty())
    {
      ROS_WARN("Transformed Plan is empty.");
      return false;
    }


    if(!ebandPlanner_->setPlan(transformedPlan_))
    {
      costmapROS_->resetLayers();
      if(!ebandPlanner_->setPlan(transformedPlan_))
      {
        ROS_ERROR("Setting plan to Elastic Band failed!");
        return false;
      }
    }

    planStartEndCounters_ = planStartEndCounters;

    if (!ebandPlanner_->optimizeBand())
      ROS_WARN("Optimization of eband failed!");


    return true;
  }


  bool RSBandPlannerROS::computeVelocityCommands(geometry_msgs::Twist& cmd)
  {
    if (!initialized_)
    {
      ROS_ERROR("Planner must be initialized before computeVelocityCommands "
        "is called!");
      return false;
    }

    std::vector<geometry_msgs::PoseStamped> ebandPlan, rsbandPlan, localPlan;

    if (isGoalReached())
    {
      cmd.linear.x = 0.0;
      cmd.linear.y = 0.0;
      cmd.angular.z = 0.0;
    }
    else
    {
      if (!updateEBand())
      {
        ROS_ERROR("Failed to update eband!");
        return false;
      }

      if (!ebandPlanner_->getPlan(ebandPlan)
        || ebandPlan.empty())
      {
        ROS_ERROR("Failed to get eband planner plan!");
        return false;
      }

      // interpolate orientations of eband plan
      interpolateOrientations(ebandPlan);

      // publish eband plan
      base_local_planner::publishPlan(ebandPlan, ebandPlanPub_);

      // use reeds shepp planner to connect eband waypoints using RS paths
      // select between the available eband to reeds shepp conversion strategies
      int failIdx;
      switch (eband2RSStrategy_)
      {
        case 0:  // start to end planning strategy
          failIdx = ebandPlan.size() * rsbandPlanner_->planPath(
              ebandPlan.front(), ebandPlan.back(), rsbandPlan);
          break;
        case 1:  // point to point planning strategy until failure
          failIdx = rsbandPlanner_->planPathUntilFailure(ebandPlan, rsbandPlan);
          break;
        case 2:  // point to point planning strategy that skips failures
          failIdx = rsbandPlanner_->planPathSkipFailures(ebandPlan, rsbandPlan);
          break;
        case 3:  // receding end planning strategy
          // plan path between start and end of eband and if it fails, decrement
          // end of eband and try again, until a solution or start is reached
          failIdx = rsbandPlanner_->planRecedingPath(ebandPlan, rsbandPlan);
          break;
        default:  // invalid strategy
          ROS_ERROR("Invalid eband_to_rs_strategy!");
          exit(EXIT_FAILURE);
      }
      if (!failIdx)
      {
        ROS_ERROR("Failed to get rsband plan");
        return false;
      }

      localPlan = rsbandPlan;

      // merge rsbandPlan with the left out waypoints of eband
      for (unsigned int i = failIdx+1; i < ebandPlan.size(); i++)
      {
        geometry_msgs::PoseStamped pose;
        tfListener_->transformPose(rsbandPlan.front().header.frame_id,
          ebandPlan.front().header.stamp, ebandPlan[i], ebandPlan[i].header.frame_id,
          pose);
        localPlan.push_back(pose);
      }

      // publish global, local and rsband plans
      base_local_planner::publishPlan(globalPlan_, globalPlanPub_);
      base_local_planner::publishPlan(localPlan, localPlanPub_);
      base_local_planner::publishPlan(rsbandPlan, rsbandPlanPub_);

      // compute velocity command
      if (!ptc_->computeVelocityCommands(localPlan, cmd))
      {
        ROS_ERROR("Path tracking controller failed to produce command");
        return false;
      }
    }

    return true;
  }


  bool RSBandPlannerROS::isGoalReached()
  {
    if (!initialized_)
    {
      ROS_ERROR("Planner must be initialized before isGoalReached is called!");
      return false;
    }

    tf::Stamped<tf::Pose> robotPose;
    if (!costmapROS_->getRobotPose(robotPose))
    {
      ROS_ERROR("Could not get robot pose!");
      return false;
    }

    geometry_msgs::PoseStamped goal = globalPlan_.back();

    double dist = base_local_planner::getGoalPositionDistance(
      robotPose, goal.pose.position.x, goal.pose.position.y);
    double yawDiff = base_local_planner::getGoalOrientationAngleDifference(
      robotPose, tf::getYaw(goal.pose.orientation));

    if (dist < xyGoalTolerance_ && fabs(yawDiff) < yawGoalTolerance_)
    {
      ROS_INFO("Goal Reached!");
      return true;
    }

    return false;
  }


  bool RSBandPlannerROS::updateEBand()
  {
    if (!initialized_)
    {
      ROS_WARN("Planner must be initialized before updateEBand is called!");
      return false;
    }

    // get robot pose
    tf::Stamped<tf::Pose> robotPose;
    if (!costmapROS_->getRobotPose(robotPose))
    {
      ROS_ERROR("Could not get robot pose!");
      return false;
    }
    // transform robot pose tf to msg
    geometry_msgs::PoseStamped robotPoseMsg;
    tf::poseStampedTFToMsg(robotPose, robotPoseMsg);

    std::vector<geometry_msgs::PoseStamped> tmpPlan(1, robotPoseMsg);

    // add robot pose to front of eband
    if (!ebandPlanner_->addFrames(tmpPlan, eband_local_planner::add_front))
    {
      ROS_WARN("Could not connect current robot pose to existing eband!");
      return false;
    }

    // add new frames that have entered the local costmap window and remove the
    // ones that have left it

    std::vector<int> planStartEndCounters = planStartEndCounters_;
    if (!eband_local_planner::transformGlobalPlan(*tfListener_, globalPlan_,
        *costmapROS_, costmapROS_->getGlobalFrameID(), transformedPlan_,
        planStartEndCounters))
    {
      ROS_WARN("Failed to transform the global plan to the local frame!");
      return false;
    }

    if (transformedPlan_.empty())
    {
      ROS_WARN("Transformed plan is empty!");
      return false;
    }

    std::vector<geometry_msgs::PoseStamped> planToAppend;

    if (planStartEndCounters_[1] > planStartEndCounters[1])
    {
      if (planStartEndCounters_[1] > planStartEndCounters[0])
      {
        planToAppend = transformedPlan_;
      }
      else
      {
        int numOfDiscardedFrames =
          planStartEndCounters[0] - planStartEndCounters_[1];
        planToAppend.assign(transformedPlan_.begin() + numOfDiscardedFrames + 1,
          transformedPlan_.end());
      }

      if (ebandPlanner_->addFrames(planToAppend, eband_local_planner::add_back))
        planStartEndCounters_ = planStartEndCounters;
      else
      {
        ROS_WARN("Failed to add frames to existing band");
        return false;
      }
    }

    if (!ebandPlanner_->optimizeBand())
    {
      ROS_WARN("Failed to optimize eband!");
      return false;
    }

    return true;
  }

  void RSBandPlannerROS::interpolateOrientations(
    std::vector<geometry_msgs::PoseStamped>& plan)
  {
    for (unsigned int i = 1; i < plan.size() - 1; i++)
    {
      double dx, dy, yaw;
      dx = plan[i+1].pose.position.x - plan[i].pose.position.x;
      dy = plan[i+1].pose.position.y - plan[i].pose.position.y;
      yaw = atan2(dy, dx);
      plan[i].pose.orientation = tf::createQuaternionMsgFromYaw(yaw);
      plan[i].header.stamp = plan.front().header.stamp;
    }
    plan.back().header.stamp = plan.front().header.stamp;

    if ((!plan.back().pose.orientation.z && !plan.back().pose.orientation.w)
        || tf::getYaw(plan.back().pose.orientation) == 0.0)
      plan.back().pose.orientation = plan[plan.size()-2].pose.orientation;
  }

}  // namespace rsband_local_planner
