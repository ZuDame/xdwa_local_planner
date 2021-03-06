//
// Created by shivesh on 7/12/18.
//

#include "xdwa_local_planner/xdwa_local_planner.h"

namespace xdwa_local_planner {
XDWALocalPlanner::XDWALocalPlanner() :
    Node("costmap_ros"),
    control_freq_(1.0),
    global_frame_("map"),
    base_frame_("base_link"),
    xy_goal_tolerance_(1),
    yaw_goal_tolerance_(1),
    compute_twist_stop_(false),
    transform_tolerance_(1.0),
    odom_topic_("/odom"),
    vel_init_(false),
    goal_topic_("/move_base_simple/goal"),
    depth_(1),
    num_best_traj_(10),
    num_steps_(50),
    sim_time_(3),
    cmd_vel_topic_("/cmd_vel"),
    costmap_topic_("/map"),
    plugin_loader_("xdwa_local_planner", "xdwa_local_planner::TrajectoryScoreFunction") {

  buffer_ = std::make_shared<tf2_ros::Buffer>(get_clock());
  tfl_ = std::make_shared<tf2_ros::TransformListener>(*buffer_);
  pose_ = std::make_shared<geometry_msgs::msg::PoseStamped>();
  goal_ = std::make_shared<geometry_msgs::msg::PoseStamped>();
  tg_ = std::make_shared<TrajectoryGenerator>();
  ts_ = std::make_shared<TrajectoryScorer>();

  odom_sub_ = this->create_subscription<nav_msgs::msg::Odometry>
      (odom_topic_, std::bind(&XDWALocalPlanner::velocityCallback, this, std::placeholders::_1));

  goal_sub_ = this->create_subscription<geometry_msgs::msg::PoseStamped>(
      goal_topic_,
      [this](const geometry_msgs::msg::PoseStamped::SharedPtr goal) {
        *goal_ = *goal;
        compute_twist_stop_ = true;
        if (compute_twist_thread_.joinable())
          compute_twist_thread_.join();
        compute_twist_stop_ = false;
        compute_twist_thread_ = std::thread(&XDWALocalPlanner::computeTwist, this);
      });

  cmd_vel_pub_ = this->create_publisher<geometry_msgs::msg::Twist>(cmd_vel_topic_);

  traj_pub_ = this->create_publisher<nav_msgs::msg::Path>("trajectories");

  footprint_.push_back({1, 1});
  footprint_.push_back({1, -1});
  footprint_.push_back({-1, -1});
  footprint_.push_back({-1, 1});

  plugins_list_.emplace_back("xdwa_local_planner::GoalDistScoreFunction");
  plugins_list_.emplace_back("xdwa_local_planner::CostmapScoreFunction");
  for (const std::string &type : plugins_list_) {
    pluginLoader(type);
  }
}

XDWALocalPlanner::~XDWALocalPlanner() {}

void XDWALocalPlanner::pluginLoader(std::string type) {
  RCLCPP_INFO(this->get_logger(), "Loading class %s", type.c_str());
  try {
    std::shared_ptr<TrajectoryScoreFunction> plugin = plugin_loader_.createSharedInstance(type);
    ts_->loadPlugin(plugin);
    plugin->initialize((rclcpp::Node::SharedPtr) this, buffer_, goal_, pose_, getCostmapTopic(), getRobotFootprint());
  }
  catch (pluginlib::LibraryLoadException &e) {
    RCLCPP_ERROR(this->get_logger(), "Class %s does not exist", type.c_str());
  }
  catch (...) {
    RCLCPP_ERROR(this->get_logger(), "Could not load class %s", type.c_str());
  }
}

void XDWALocalPlanner::computeTwist() {
  rclcpp::Rate rate(1.0);
  while (!getLocalGoal()) {
    rate.sleep();
  }
  rclcpp::Rate control_rate(control_freq_);
  while (!goalReached()) {
    if (compute_twist_stop_)
      return;
    pose_->pose = odom_->pose.pose;
    pose_->header = odom_->header;
    if (!getRobotPose()) {
      RCLCPP_INFO(this->get_logger(), "Could not get robot pose");
      control_rate.sleep();
      continue;
    }
    rclcpp::Time start = get_clock()->now();
    std::shared_ptr<Trajectory> best_traj;
    if (computeBestTrajectory(best_traj)) {
      cmd_vel_.linear.set__x(best_traj->vel_x_[0]);
      cmd_vel_.linear.set__y(best_traj->vel_y_[0]);
      cmd_vel_.angular.set__z(best_traj->vel_theta_[0]);
      cmd_vel_pub_->publish(cmd_vel_);
    } else {
      RCLCPP_INFO(this->get_logger(), "XDWA Local Planner failed to produce a valid path.");
    }

    control_rate.sleep();
    rclcpp::Time finish = get_clock()->now();
    double time_taken = (finish.nanoseconds() - start.nanoseconds()) / 1e9;
    if (time_taken > 1.0 / control_freq_) {
      RCLCPP_WARN(this->get_logger(),
                  "Control loop failed. Desired frequency is %fHz. The loop actually took %f seconds",
                  control_freq_, time_taken);
    }
  }
  cmd_vel_.linear.set__x(0);
  cmd_vel_.linear.set__y(0);
  cmd_vel_.angular.set__z(0);
  cmd_vel_pub_->publish(cmd_vel_);
  RCLCPP_INFO(this->get_logger(), "Goal Reached");
}

bool XDWALocalPlanner::getRobotPose() {
  pose_->header.stamp = get_clock()->now();
  rclcpp::Time start = get_clock()->now();
  try {
    geometry_msgs::msg::TransformStamped
        tfp = buffer_->lookupTransform(global_frame_, pose_->header.frame_id, tf2::TimePointZero);
    tf2::doTransform(*pose_, *pose_, tfp);
  }
  catch (tf2::TransformException &ex) {
    RCLCPP_WARN(this->get_logger(), "%s", ex.what());
    return false;
  }
  rclcpp::Time finish = get_clock()->now();
  if ((finish.seconds() - start.seconds()) > transform_tolerance_) {
    RCLCPP_WARN(this->get_logger(),
                "XDWA Local Planner %s to %s transform timed out. Current time: %d, global_pose stamp %d, tolerance %d",
                global_frame_.c_str(),
                base_frame_.c_str(),
                finish.seconds(),
                pose_->header.stamp,
                transform_tolerance_);
  }
  return true;
}

bool XDWALocalPlanner::getLocalGoal() {
  rclcpp::Time start = get_clock()->now();
  try {
    geometry_msgs::msg::TransformStamped
        tfp = buffer_->lookupTransform(global_frame_, goal_->header.frame_id, tf2::TimePointZero);
    tf2::doTransform(*goal_, *goal_, tfp);
  }
  catch (tf2::TransformException &ex) {
    RCLCPP_WARN(this->get_logger(), "%s", ex.what());
    return false;
  }
  rclcpp::Time finish = get_clock()->now();
  if ((finish.seconds() - start.seconds()) > transform_tolerance_) {
    RCLCPP_WARN(this->get_logger(),
                "XDWA Local Planner %s to odom transform timed out. Current time: %d, global_pose stamp %d, tolerance %d",
                goal_->header.frame_id.c_str(),
                finish.seconds(),
                goal_->header.stamp,
                transform_tolerance_);
  }
  return true;
}

bool XDWALocalPlanner::goalReached() {
  return hypot(goal_->pose.position.x - pose_->pose.position.x, goal_->pose.position.y - pose_->pose.position.y)
      < xy_goal_tolerance_;
}

bool XDWALocalPlanner::computeBestTrajectory(std::shared_ptr<Trajectory> &best_traj) {
  tg_->generateSamples(odom_->twist.twist.linear.x, odom_->twist.twist.linear.y, odom_->twist.twist.angular.z);
  std::vector<std::shared_ptr<Trajectory>> trajectories;
  for (auto &vsample : tg_->vsamples_) {
    std::shared_ptr<Trajectory> tj = std::make_shared<Trajectory>();
    tj->cost_ = 0;
    tj->num_points_ = 0;
    tj->num_points_scored_ = 0;
    if (tg_->generateTrajectory(vsample,
                                pose_->pose.position.x,
                                pose_->pose.position.y,
                                tf2::getYaw(pose_->pose.orientation),
                                sim_time_,
                                num_steps_,
                                tj)) {
      ts_->getTrajectoryScore(tj);
      if (tj->cost_ >= 0) {
        tj->num_points_scored_ = tj->num_points_;
        trajectories.push_back(tj);
      }
    }
  }

  if (trajectories.empty())
    return false;

  trajectories = getBestTrajectories(trajectories);

  for (int i = 1; i < depth_; ++i) {
    std::vector<std::shared_ptr<Trajectory>> traj;
    for (auto &tj : trajectories) {
      tg_->generateSamples(tj->vel_x_.back(), tj->vel_y_.back(), tj->vel_theta_.back());
      for (auto &vsample: tg_->vsamples_) {
        auto tj_new = std::make_shared<Trajectory>(tj);
        if (tg_->generateTrajectory(vsample, tj_new->x_.back(), tj_new->y_.back(), tj_new->theta_.back(),
                                    sim_time_, num_steps_, tj_new)) {
          ts_->getTrajectoryScore(tj_new);
          if (tj_new->cost_ >= 0) {
            tj_new->num_points_scored_ = tj_new->num_points_;
            traj.push_back(tj_new);
          }
        }
      }
    }
    trajectories = getBestTrajectories(traj);
    if (trajectories.empty())
      return false;
  }

  nav_msgs::msg::Path msg_;
  msg_.header.stamp = get_clock()->now();
  msg_.header.frame_id = global_frame_;
  for (auto &tj:trajectories) {
    for (int i = 0; i < tj->num_points_; i++) {
      geometry_msgs::msg::PoseStamped msg;
      msg.pose.position.set__x(tj->x_[i]);
      msg.pose.position.set__y(tj->y_[i]);
      msg_.poses.emplace_back(msg);
    }
  }
  traj_pub_->publish(msg_);
  best_traj = trajectories[0];
  for (auto &traj: trajectories) {
    if (best_traj->cost_ > traj->cost_)
      best_traj = traj;
  }
  return true;
}

std::vector<std::shared_ptr<Trajectory>> XDWALocalPlanner::getBestTrajectories(std::vector<std::shared_ptr<Trajectory>> trajectories) {
  std::vector<std::shared_ptr<Trajectory>> best_traj;
  int num_best_traj = std::min(num_best_traj_, (int) trajectories.size());
  double max_cost = trajectories[0]->cost_;
  int index = 0;
  for (int i = 0; i < num_best_traj; ++i) {
    best_traj.push_back(trajectories[i]);
    if (trajectories[i]->cost_ > max_cost) {
      max_cost = trajectories[i]->cost_;
      index = i;
    }
  }

  for (int i = num_best_traj; i < trajectories.size(); ++i) {
    if (trajectories[i]->cost_ < max_cost) {
      best_traj[index] = trajectories[i];
      max_cost = best_traj[0]->cost_;
      index = 0;
      for (int j = 1; j < num_best_traj; ++j) {
        if (best_traj[j]->cost_ > max_cost) {
          max_cost = best_traj[j]->cost_;
          index = j;
        }
      }
    }
  }
  return best_traj;
}

void XDWALocalPlanner::velocityCallback(const nav_msgs::msg::Odometry::SharedPtr msg) {
  odom_ = msg;
  vel_init_ = true;
}
}