//
// Created by rakesh on 13/08/18.
//
#include <cmath>
#include <map>
#include <numeric>
#include <chrono>

#include <boost/atomic/atomic.hpp>
#include <opencv2/flann/miniflann.hpp>

#include <icp_slam/icp_slam.h>
#include <icp_slam/utils.h>
#include <icp_slam/config.h>

#define TIME_DIFF(tic, toc) ((std::chrono::duration<double, std::milli>((toc) - (tic))).count())

using namespace std;

namespace icp_slam
{

ICPSlam::ICPSlam(tfScalar max_keyframes_distance, tfScalar max_keyframes_angle, double max_keyframes_time)
  : max_keyframes_distance_(max_keyframes_distance),
    max_keyframes_angle_(max_keyframes_angle),
    max_keyframes_time_(max_keyframes_time),
    last_kf_laser_scan_(new sensor_msgs::LaserScan()),
    is_tracker_running_(false),
    max_icp_itr(100),
    max_icp_dist(0.01)
{
  last_kf_tf_odom_laser_.stamp_ = ros::Time(0);
}

bool ICPSlam::track(const sensor_msgs::LaserScanConstPtr &laser_scan,
                    const tf::StampedTransform &current_frame_tf_odom_laser,
                    tf::StampedTransform &tf_map_laser)
{
  if (is_tracker_running_)
  {
    ROS_WARN_THROTTLE(1.0, "Couldn't track frame, tracker already running");
    return false;
  }
  is_tracker_running_ = true;
  // Initialize the map for the first time
  if (last_kf_laser_scan_->ranges.size() == 0) 
  {
    // initialize the last laser pose wrt odom
    last_kf_tf_odom_laser_.frame_id_ = current_frame_tf_odom_laser.frame_id_;
    last_kf_tf_odom_laser_.child_frame_id_ = current_frame_tf_odom_laser.child_frame_id_;
    last_kf_tf_odom_laser_.setOrigin(current_frame_tf_odom_laser.getOrigin());
    last_kf_tf_odom_laser_.setRotation(current_frame_tf_odom_laser.getRotation());
    
    // define map
    last_kf_tf_map_laser_.frame_id_ = "map";
    last_kf_tf_map_laser_.child_frame_id_ = current_frame_tf_odom_laser.frame_id_;
    last_kf_tf_map_laser_.stamp_ = ros::Time::now();
    last_kf_tf_map_laser_.setOrigin(current_frame_tf_odom_laser.getOrigin());
    last_kf_tf_map_laser_.setRotation(current_frame_tf_odom_laser.getRotation());

    // copy the laser scan
    *last_kf_laser_scan_ = *laser_scan;
    // ROS_INFO("current_scan row:%d cols:%d", last_scan_trimmed.rows, last_scan_trimmed.cols);
  }
  // Find the conversion from map
  bool is_key_frame = isCreateKeyframe(current_frame_tf_odom_laser, last_kf_tf_odom_laser_);
  // std::cout << is_key_frame << std::endl;
  if (is_key_frame) 
  {
    // convert the laser scans to matrix
    cv::Mat current_scan_matrix = utils::laserScanToPointMat(laser_scan);
    cv::Mat last_scan_matrix = utils::laserScanToPointMat(last_kf_laser_scan_);

    // Find estimate of the transform between current and last pose.
    tf::Transform icp_transform_est = last_kf_tf_odom_laser_.inverse() * current_frame_tf_odom_laser;
    
    // Use ICP to currect the estimate
    tf::Transform icp_transform = icpRegistration(last_scan_matrix, current_scan_matrix, icp_transform_est);
    last_scan_hat = utils::transformPointMat(icp_transform, current_scan_matrix);
    vizClosestPoints(last_scan_hat, current_scan_matrix, icp_transform);

    // update the robot's pose wrt to the map.
    tf_map_laser.stamp_ = ros::Time::now();
    tf_map_laser.frame_id_ = last_kf_tf_map_laser_.frame_id_;
    tf_map_laser.child_frame_id_ = last_kf_tf_map_laser_.child_frame_id_;
    tf_map_laser.setData(last_kf_tf_map_laser_ * icp_transform);
    last_kf_tf_map_laser_ = tf_map_laser;

    // update the robot's last pose wrt to the odom.
    last_kf_tf_odom_laser_.stamp_ = ros::Time::now();
    last_kf_tf_odom_laser_.setOrigin(current_frame_tf_odom_laser.getOrigin());
    last_kf_tf_odom_laser_.setRotation(current_frame_tf_odom_laser.getRotation());

    // copy the laser scan
    *last_kf_laser_scan_ = *laser_scan;

  } else 
  {
    // obtain laser pose in map based on odometry update
    tf::Transform icp_transform_est = last_kf_tf_odom_laser_.inverse() * current_frame_tf_odom_laser;
    tf_map_laser.stamp_ = ros::Time::now();
    tf_map_laser.frame_id_ = last_kf_tf_map_laser_.frame_id_;
    tf_map_laser.child_frame_id_ = last_kf_tf_map_laser_.child_frame_id_;
    tf_map_laser.setData(last_kf_tf_map_laser_ * icp_transform_est);
  }
  is_tracker_running_ = false;

  return is_key_frame;
  // TODO: find the pose of laser in map frame
  // if a new keyframe is created, run ICP
  // if not a keyframe, obtain the laser pose in map frame based on odometry update
}

bool ICPSlam::isCreateKeyframe(const tf::StampedTransform &current_frame_tf, const tf::StampedTransform &last_kf_tf) const
{
  // assert(current_frame_tf.frame_id_ == last_kf_tf.frame_id_);
  // assert(current_frame_tf.child_frame_id_ == last_kf_tf.child_frame_id_);

  // compute distance between frames
  tf::Vector3 current_orig_ = current_frame_tf.getOrigin();
  tf::Vector3 last_orig_ = last_kf_tf.getOrigin();
  // cout << current_orig_.x() << ", " << current_orig_.y() << endl;
  // cout << last_orig_.x() << ", " << last_orig_.x() << endl;

  tfScalar keyframes_dist_ = sqrt(pow(current_orig_.x() - last_orig_.x(), 2) + pow(current_orig_.y() - last_orig_.y(), 2));

  // compute the time diff between frames
  tfScalar keyframes_time = current_frame_tf.stamp_.toSec() - last_kf_tf.stamp_.toSec();
  // cout << current_frame_tf.stamp_.toSec() << ", " << last_kf_tf.stamp_.toSec() << endl;

  // compute the angle difference between frames
  tfScalar keyframes_angle_ = abs(current_frame_tf.getRotation().getAngle() - last_kf_tf.getRotation().getAngle());
  // std::cout << keyframes_dist_ << ", " << max_keyframes_distance_ << std::endl;
  // std::cout << keyframes_time << ", " << max_keyframes_time_ << std::endl;
  // std::cout << keyframes_angle_ << ", " << max_keyframes_angle_ << std::endl;   
  return ((keyframes_dist_ > max_keyframes_distance_) || 
          (keyframes_time > max_keyframes_time_) ||
          (keyframes_angle_ > max_keyframes_angle_));
  // TODO: check whether you want to create keyframe (based on max_keyframes_distance_, max_keyframes_angle_, max_keyframes_time_)
}

tf::Transform ICPSlam::icpIteration(cv::Mat &point_mat1,
                                    cv::Mat &point_mat2) 
{
  // Find the means and normalize
  // std::cout << point_mat1.rows << std::endl;
  // std::cout << point_mat2.rows << std::endl;
  cv::Mat mu_x;
  cv::reduce(point_mat1, mu_x, 0, CV_REDUCE_SUM, CV_32F);
  mu_x /= point_mat1.rows;
  
  cv::Mat mu_p;
  cv::reduce(point_mat2, mu_p, 0, CV_REDUCE_SUM, CV_32F);
  mu_p /= point_mat2.rows;

  cv::Mat point_mat1_prime;
  cv::Mat point_mat2_prime;
  for (int i=0; i<point_mat1.rows; ++i)
  {
    point_mat1_prime.push_back(point_mat1.row(i) - mu_x);
    point_mat2_prime.push_back(point_mat2.row(i) - mu_p);
  }

  cv::Mat mu_p_transpose;
  cv::transpose(mu_p, mu_p_transpose);

  cv::Mat mu_x_transpose;
  cv::transpose(mu_x, mu_x_transpose);

  // compute the SVD.
  cv::Mat point_mat1_prime_tr; 
  cv::transpose(point_mat1_prime, point_mat1_prime_tr);
  cv::Mat w = point_mat1_prime_tr * point_mat2_prime;
  cv::SVD svd(w);

  // compute the rotation and translation
  cv::Mat R = svd.u * svd.vt;
  cv::Mat t = mu_x_transpose - R * mu_p_transpose;

  // create the transform form the rotation and translation
  tf::Transform icp_transform;
  icp_transform.setOrigin(tf::Vector3(t.at<float>(0), t.at<float>(1), 0.0));
  double theta;
  double cos_theta = R.at<float>(0,0);
  double sin_theta = R.at<float>(1,0);
  if (cos_theta == 0) cos_theta += 1e-6;
  if (sin_theta >= 0) 
  {
    if (cos_theta > 0) 
    {
      theta = atan(sin_theta/cos_theta);   
    } 
    else 
    {
      theta = M_PI - atan(sin_theta/cos_theta);
    }
  } 
  else 
  {
    if (cos_theta > 0)
    {
      theta = atan(sin_theta/cos_theta);
    }
    else
    {
      theta = atan(sin_theta/cos_theta) - M_PI; 
    }
  }

  icp_transform.setRotation(tf::createQuaternionFromYaw(theta));

  return icp_transform;
}

// void ICPSlam::computeError(cv::Mat last_scan_matrix,
//                            cv::Mat current_scan_matrix,
//                            const tf::Transform &itr_transform,
//                            std::vector<float> &errors)
// {
//   cv::Mat current_scan_matrix_hat = utils::transformPointMat(itr_transform, current_scan_matrix);
//   // cout << "current scan matrix hat: " << current_scan_matrix_hat << endl;
//   // cout << "matrix size: " << current_scan_matrix_hat.rows << endl;
//   cv::Mat diff = current_scan_matrix_hat - last_scan_matrix;
//   cv::Mat diff_squared;
//   cv::multiply(diff, diff, diff_squared);
//   // cout << "diff_squared: " << diff_squared << endl;
//   cv::reduce(diff_squared, errors, 1, CV_REDUCE_SUM, CV_32F);
// }

// void ICPSlam::retainClosest(cv::Mat &last_scan_matrix,
//                             cv::Mat &current_scan_matrix,
//                             const tf::Transform &itr_transform)
// {
//   cv::Mat current_scan_matrix_est = utils::transformPointMat(itr_transform, current_scan_matrix);
//   std::vector<int> closest_indices;
//   std::vector<float> closest_distances_2;
//   closestPoints(last_scan_matrix, current_scan_matrix_est, closest_indices,
//                 closest_distances_2);

//   float mean_val=0;
//   float std_val=0;
//   utils::meanAndStdDev(closest_distances_2, mean_val, std_val);
//   cv::Mat last_scan_temp;
//   cv::Mat current_scan_temp;
//   for (int i=0; i<last_scan_matrix.rows; ++i)
//   {
//     bool I1 = closest_distances_2[i] >= (mean_val - 3 * std_val);
//     bool I2 = closest_distances_2[i] <= (mean_val + 3 * std_val);
//     if (I1 && I2 && (closest_indices[i] >= 0))
//     {
//       last_scan_temp.push_back(last_scan_matrix.row(i));
//       current_scan_temp.push_back(current_scan_matrix.row(closest_indices[i]));
//     }
//   }
//   last_scan_matrix = last_scan_temp;
//   current_scan_matrix = current_scan_temp;
// }

tf::Transform ICPSlam::icpRegistration(cv::Mat &last_scan_matrix,
                                       cv::Mat &current_scan_matrix,
                                       const tf::Transform &T_2_1)
{
  cv::Mat last_scan_est = utils::transformPointMat(T_2_1, current_scan_matrix);
  std::vector<int> closest_indices = {0};
  std::vector<float> closest_distances_2 = {0.0};
  closestPoints(last_scan_matrix, last_scan_est, closest_indices,
                closest_distances_2);
  float mean_val=0;
  float std_val=0;
  utils::meanAndStdDev(closest_distances_2, mean_val, std_val);

  cv::Mat last_scan_temp = cv::Mat();
  cv::Mat current_scan_temp = cv::Mat();
  for (int j=0; j<last_scan_matrix.rows; ++j)
  {
    bool I1 = closest_distances_2[j] >= (mean_val - 3 * std_val);
    bool I2 = closest_distances_2[j] <= (mean_val + 3 * std_val);
    bool I3 = closest_distances_2[j] < 0.001;
    if ((last_scan_temp.rows == 0) && ((last_scan_matrix.rows - j) < 5))
    {
      I1 = true;
      I2 = true;
    }
    if ((I1 && I2 && (closest_indices[j] >= 0)) || I3)
    {
      last_scan_temp.push_back(last_scan_matrix.row(j));
      current_scan_temp.push_back(current_scan_matrix.row(closest_indices[j]));
    }
  }

  tf::Transform prev_transform = icpIteration(last_scan_temp, current_scan_temp);
  tf::Transform icp_transform = prev_transform;

  last_scan_est = utils::transformPointMat(prev_transform, current_scan_temp);
  cv::Mat diff = last_scan_est - last_scan_temp;
  cv::Mat diff_squared = cv::Mat();
  cv::multiply(diff, diff, diff_squared);
  double prev_error = cv::sum(diff_squared)[0] / diff_squared.rows;

  for (int i=0; i<100; ++i)
  {
    last_scan_est = utils::transformPointMat(prev_transform, current_scan_matrix);
    closest_indices = {0};
    closest_distances_2 = {0.0};
    closestPoints(last_scan_matrix, last_scan_est, closest_indices,
                  closest_distances_2);

    mean_val = 0;
    std_val = 0;
    last_scan_temp = cv::Mat();
    current_scan_temp = cv::Mat();;
    utils::meanAndStdDev(closest_distances_2, mean_val, std_val);
    for (int j=0; j<last_scan_matrix.rows; ++j)
    {
      bool I1 = closest_distances_2[j] > (mean_val - 3 * std_val);
      bool I2 = closest_distances_2[j] < (mean_val + 3 * std_val);
      bool I3 = closest_distances_2[j] < 0.001;
      if ((last_scan_temp.rows == 0) && ((last_scan_matrix.rows - j) < 5))
      {
        I1 = true;
        I2 = true;
      }
      if ((I1 && I2 && (closest_indices[j] >= 0)) || I3)
      {
        last_scan_temp.push_back(last_scan_matrix.row(j));
        current_scan_temp.push_back(current_scan_matrix.row(closest_indices[j]));
      }
    }

    icp_transform = icpIteration(last_scan_temp, current_scan_temp);

    // compute the error
    last_scan_est = utils::transformPointMat(icp_transform, current_scan_temp);
    diff = last_scan_est - last_scan_temp;
    diff_squared = cv::Mat();
    cv::multiply(diff, diff, diff_squared);
    double curr_error = cv::sum(diff_squared)[0] / diff_squared.rows;
    if ((prev_error < 0.001) && (curr_error < 0.001))
    {
      cout << "best iteraton was: " << i << endl;
      return icp_transform;
    } //else
    // {
    //   // outlier rejection
    //   // apply the transformation and compute the error.
    //   cv::Mat current_scan_matrix_temp_hat = utils::transformPointMat(icp_transform, current_scan_matrix_temp);
    //   cv::Mat diff = current_scan_matrix_temp_hat - last_scan_matrix_temp;
    //   cv::multiply(diff.clone(), diff.clone(), diff);
    //   std::vector<float> errors;
    //   cv::reduce(diff, errors, 1, CV_REDUCE_SUM, CV_32F);

    //   // only take the top %99 points wrt error
    //   std::vector<float> sorted_errors = errors;
    //   std::sort(sorted_errors.begin(), sorted_errors.end());
    //   int max_error_idx = 0.99 * errors.size();
    //   cout << max_error_idx << endl;
    //   double max_error = sorted_errors[max_error_idx];
    //   // cout << last_scan_matrix << endl;
    //   // cout << current_scan_matrix << endl;
    //   last_scan_matrix = cv::Mat();
    //   current_scan_matrix = cv::Mat();
    //   cout << max_error << " max error" << endl; 
    //   // avg_err = 0;
    //   // counts = 0;
    //   for (int j=0; j<errors.size(); ++j)
    //   {
    //     if (errors[j] < max_error)
    //     {
    //       cout << "happened" << endl;
    //       // avg_err += errors[j];
    //       // counts++;
    //       last_scan_matrix.push_back(last_scan_matrix_temp.row(j));
    //       current_scan_matrix.push_back(current_scan_matrix_temp.row(j));
    //     }
    //   }
      // last_scan_matrix_temp = last_scan_matrix_temp.clone();
      // current_scan_matrix = current_scan_matrix_temp.clone();
    // }
    prev_transform = icp_transform;
    // vizClosestPoints(last_scan_trimmed, current_scan_trimmed, icp_transform_);
  }
  return icp_transform;
}

void ICPSlam::closestPoints(cv::Mat &point_mat1,
                            cv::Mat &point_mat2,
                            std::vector<int> &closest_indices,
                            std::vector<float> &closest_distances_2)
{
  // uses FLANN for K-NN e.g. http://www.morethantechnical.com/2010/06/06/iterative-closest-point-icp-with-opencv-w-code/
  closest_indices = std::vector<int>(point_mat1.rows, -1);
  closest_distances_2 = std::vector<float>(point_mat1.rows, -1);


  cv::Mat multi_channeled_mat1;
  cv::Mat multi_channeled_mat2;

  point_mat1.convertTo(multi_channeled_mat1, CV_32FC2);
  point_mat2.convertTo(multi_channeled_mat2, CV_32FC2);

  cv::flann::Index flann_index(multi_channeled_mat2, cv::flann::KDTreeIndexParams(2));  // using 2 randomized kdtrees

  cv::Mat mat_indices(point_mat1.rows, 1, CV_32S);
  cv::Mat mat_dists(point_mat1.rows, 1, CV_32F);
  flann_index.knnSearch(multi_channeled_mat1, mat_indices, mat_dists, 1, cv::flann::SearchParams(64) );

  int* indices_ptr = mat_indices.ptr<int>(0);
  //float* dists_ptr = mat_dists.ptr<float>(0);
  for (int i=0;i<mat_indices.rows;++i) {
    closest_indices[i] = indices_ptr[i];
  }

  mat_dists.copyTo(cv::Mat(closest_distances_2));

  // ---------------------------- naive version ---------------------------- //
  // max allowed distance between corresponding points
//  const float max_distance = 0.5;
//
//  for (size_t i = 0, len_i = (size_t)point_mat1.rows; i < len_i; i++)
//  {
//    int closest_point_idx = -1;
//    float closest_distance_2 = std::pow(max_distance, 2.0f);
//
//    for (size_t j = 0, len_j = (size_t)point_mat2.rows; j < len_j; j++)
//    {
//      auto distance2 =
//        std::pow(point_mat2.at<float>(j, 0) - point_mat1.at<float>(i, 0), 2.0f)
//        + std::pow(point_mat2.at<float>(j, 1) - point_mat1.at<float>(i, 1), 2.0f);
//
//      if (distance2 < closest_distance_2)
//      {
//        closest_distance_2 = distance2;
//        closest_point_idx = (int)j;
//      }
//    }
//
//    if (closest_point_idx >= 0)
//    {
//      closest_indices[i] = closest_point_idx;
//      closest_distances_2[i] = closest_distance_2;
//    }
//  }
}

void ICPSlam::vizClosestPoints(cv::Mat &point_mat1,
                               cv::Mat &point_mat2,
                               const tf::Transform &T_2_1)
{
  assert(point_mat1.size == point_mat2.size);

  const float resolution = 0.005;

  float *float_array = (float*)(point_mat1.data);
  float size_m = std::accumulate(
    float_array, float_array + point_mat1.total(), std::numeric_limits<float>::min(),
    [](float max, float current)
    {
      return current > max ? current : max;
    }
  );
  // add some slack
  size_m += 0.5;

  int size_pix = (int)(size_m / resolution);

  cv::Mat img(
    size_pix,
    size_pix,
    CV_8UC3,
    cv::Scalar(0, 0, 0)
  );

  auto meters_to_pix = [&size_pix, resolution](float meters) {
    int pix = (int)(meters / resolution + size_pix / 2.0f);
    pix = std::max(0, pix);
    pix = std::min(size_pix - 1, pix);
    return pix;
  };

  cv::Mat transformed_point_mat2 = utils::transformPointMat(T_2_1.inverse(), point_mat2);

  for (size_t i = 0, len_i = (size_t)point_mat1.rows; i < len_i; i++)
  {
    float x1 = point_mat1.at<float>(i, 0);
    float y1 = point_mat1.at<float>(i, 1);
    float x2 = transformed_point_mat2.at<float>(i, 0);
    float y2 = transformed_point_mat2.at<float>(i, 1);

    auto pix_x1 = meters_to_pix(x1);
    auto pix_y1 = meters_to_pix(y1);
    auto pix_x2 = meters_to_pix(x2);
    auto pix_y2 = meters_to_pix(y2);

    cv::Point point1(pix_x1, pix_y1);
    cv::Point point2(pix_x2, pix_y2);

    cv::circle(img, point1, 5, cv::Scalar(0, 0, 255), -1);
    cv::circle(img, point2, 5, cv::Scalar(255, 0, 0), -1);

    cv::line(img, point1, point2, cv::Scalar(0, 255, 0), 2);
  }

  cv::Mat tmp;
  cv::flip(img, tmp, 0);
  cv::imwrite("/tmp/icp_laser.png", img);
}

} // namespace icp_slam

