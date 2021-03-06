/*
 * FeatureTracker.h
 *
 *  Created on: Jul 8, 2017
 *      Author: kevin
 */

#ifndef DIPA_INCLUDE_DIPA_PLANAR_ODOMETRY_FEATURETRACKER_H_
#define DIPA_INCLUDE_DIPA_PLANAR_ODOMETRY_FEATURETRACKER_H_

#include <ros/ros.h>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include "opencv2/core/core.hpp"
#include "opencv2/features2d/features2d.hpp"
#include "opencv2/xfeatures2d.hpp"
#include "opencv2/video.hpp"
#include <vector>
#include <string>

#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/message_filter.h>
#include <tf2_ros/transform_listener.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <tf/tf.h>
#include <tf/tfMessage.h>

#include <dipa/DipaParams.h>

class FeatureTracker {
public:

	struct Feature{
		cv::Point2f px;
		tf::Vector3 obj;

		bool computeObjectPosition(tf::Transform w2c, cv::Mat_<float> K)
		{
			tf::Vector3 pixel = tf::Vector3((px.x - K(2)) / K(0), (px.y - K(5)) / K(4), 1.0);

			tf::Vector3 dir = w2c * pixel - w2c.getOrigin();

			double dt = (-w2c.getOrigin().z() / dir.z());

			if(dt <= 0)
			{
				ROS_DEBUG("removing NEW feature from planar odom because it is not on the xy plane");
				return false;
			}
			else
			{
				obj = w2c.getOrigin() + dir * dt;
				return true;
			}
		}
	};

	struct VOState{
		std::vector<Feature> features;

		ros::Time time_at_last_realignment;

		double ppe;

		cv::Mat currentImg; // the image that the features are currently in

		tf::Transform currentPose; // w2c transform

		std::vector<cv::Point2d> getPixelsInOrder(){
			std::vector<cv::Point2d> pixels;

			for(auto e : features)
			{
				pixels.push_back(cv::Point2d(e.px.x, e.px.y));
			}

			return pixels;
		}

		std::vector<cv::Point2f> getPixels2fInOrder(){
			std::vector<cv::Point2f> pixels;

			for(auto e : features)
			{
				pixels.push_back(e.px);
			}

			return pixels;
		}

		std::vector<cv::Point3d> getObjectsInOrder(){
			std::vector<cv::Point3d> obj;

			for(auto e : features)
			{
				obj.push_back(cv::Point3d(e.obj.x(), e.obj.y(), e.obj.z()));
			}

			return obj;
		}

		double getTimeSinceLastRealignment(ros::Time t){
			if(this->time_at_last_realignment == ros::Time(0))
			{
				ROS_DEBUG("setting the first alignment time for dataset");
				this->time_at_last_realignment = t;
			}
			return (t - this->time_at_last_realignment).toSec();
		}

		/*
		 * uses the current pose and current pixels to determine the 3d position of the objects
		 * assumes that all pixels lie on a plane
		 */
		void updateObjectPositions(cv::Mat_<float> K)
		{
			for(int i = 0; i < features.size(); i++)
			{
				tf::Vector3 pixel = tf::Vector3((features.at(i).px.x - K(2)) / K(0), (features.at(i).px.y - K(5)) / K(4), 1.0);

				tf::Vector3 dir = currentPose * pixel - currentPose.getOrigin();

				double dt = (-currentPose.getOrigin().z() / dir.z());

				if(dt <= 0)
				{
					ROS_DEBUG("removing feature from planar odom because it is not on the xy plane");
					//remove this feature it is not on the plane
					features.erase(features.begin() + i);
					i--;
				}
				else
				{
					features.at(i).obj = currentPose.getOrigin() + dir * dt;
				}
			}

			if(features.size() <= 4)
			{
				ROS_WARN("planar odometry has too few features!");
			}
		}
	};

	cv::Mat_<float> K;
	VOState state;

	FeatureTracker();
	virtual ~FeatureTracker();

	void updateFeatures(cv::Mat img);

	bool computePose(double& perPixelError);

	void updatePose(tf::Transform w2c, ros::Time t);

	void replenishFeatures(cv::Mat img);

	void tf2rvecAndtvec(tf::Transform tf, cv::Mat& tvec, cv::Mat& rvec);

	tf::Transform rvecAndtvec2tf(cv::Mat tvec, cv::Mat rvec);

	cv::Mat draw(cv::Mat in)
	{
		for(auto e : this->state.features)
		{
			cv::drawMarker(in, e.px, cv::Scalar(255, 0, 0));
		}
		return in;
	}
};

#endif /* DIPA_INCLUDE_DIPA_PLANAR_ODOMETRY_FEATURETRACKER_H_ */
