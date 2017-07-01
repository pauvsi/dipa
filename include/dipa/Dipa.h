/*
 * Dipa.h
 *
 *  Created on: Jul 1, 2017
 *      Author: kevin
 */

#ifndef DIPA_INCLUDE_DIPA_DIPA_H_
#define DIPA_INCLUDE_DIPA_DIPA_H_

#include <ros/ros.h>

#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp> // OpenCV window I/O
#include <opencv2/imgproc.hpp> // OpenCV image transformations
#include <opencv2/imgproc/types_c.h>
#include <opencv2/imgcodecs/imgcodecs_c.h>
#include <opencv2/highgui/highgui_c.h>

#include "opencv2/reg/mapaffine.hpp"
#include "opencv2/reg/mapshift.hpp"
#include "opencv2/reg/mapprojec.hpp"
#include "opencv2/reg/mappergradshift.hpp"
#include "opencv2/reg/mappergradeuclid.hpp"
#include "opencv2/reg/mappergradsimilar.hpp"
#include "opencv2/reg/mappergradaffine.hpp"
#include "opencv2/reg/mappergradproj.hpp"
#include "opencv2/reg/mapperpyramid.hpp"

#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/message_filter.h>
#include <tf2_ros/transform_listener.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <tf/tf.h>
#include <tf/tfMessage.h>

#include <image_transport/image_transport.h>
#include "std_msgs/String.h"
#include "sensor_msgs/Image.h"
#include <sstream>
#include <cv_bridge/cv_bridge.h>

#include <dipa/GridRenderer.h>

class Dipa {
public:

	struct Match{
		cv::Point3d obj;
		cv::Point2d obj_px;
		cv::Point2d measurement;

		double getPixelNorm()
		{
			double dx = obj_px.x - measurement.x;
			double dy = obj_px.y - measurement.y;
			return sqrt(dx*dx+dy*dy);
		}
	};

	struct Matches{
		std::vector<Match> matches;


	};

	Dipa();
	virtual ~Dipa();
};

#endif /* DIPA_INCLUDE_DIPA_DIPA_H_ */