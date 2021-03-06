/*
 * Dipa.cpp
 *
 *  Created on: Jul 1, 2017
 *      Author: kevin
 */

#include <dipa/Dipa.h>

Dipa::Dipa(tf::Transform initial_world_to_base_transform, bool debug) {
	ros::NodeHandle nh;

	image_transport::ImageTransport it(nh);
	//TODO make a camera sub when I have a properly recorded dataset
	image_transport::CameraSubscriber bottom_cam_sub = it.subscribeCamera(BOTTOM_CAMERA_TOPIC, 2, &Dipa::bottomCamCb, this);


	//setup realignment sub
	this->pose_realignment_sub = nh.subscribe<geometry_msgs::PoseWithCovarianceStamped>(REALIGNMENT_TOPIC, 2, &Dipa::realignmentCb, this);
	this->time_at_last_realignment = ros::Time(0);

	this->odom_pub = nh.advertise<nav_msgs::Odometry>(ODOM_TOPIC, 1);

#if PUBLISH_INSIGHT
	this->insight_pub = nh.advertise<sensor_msgs::Image>(INSIGHT_TOPIC, 1);
#endif

	tf::StampedTransform b2c;

	ROS_INFO_STREAM("WAITING FOR TANSFORM FROM " << BASE_FRAME << " TO " << CAMERA_FRAME);
	if(tf_listener.waitForTransform(BASE_FRAME, CAMERA_FRAME, ros::Time(0), ros::Duration(10))){
		try {
			tf_listener.lookupTransform(BASE_FRAME, CAMERA_FRAME,
					ros::Time(0), b2c);
		} catch (tf::TransformException& e) {
			ROS_WARN_STREAM(e.what());
		}
	}
	else
	{
		ROS_FATAL("COULD NOT GET TRANSFORM");
		ros::shutdown();
		return;
	}

	TRACKING_LOST = false; //we have a good initial guess

	vo_initialized = false; // we must init vo before losing tracking set

	//set the initial guess to the passed in transform
	//this->state.updatePose(initial_world_to_base_transform, ros::Time::now()); // this will cause a problem with datasets
	this->state.updatePose(initial_world_to_base_transform, ros::Time(0));

	//initialize vo with the guess
	//TODO transform to the camera
	this->vo.updatePose(initial_world_to_base_transform * b2c, ros::Time(0));

	if(!debug)
	{
		ros::spin(); // go into the main loop;
	}

}

Dipa::~Dipa() {

}

void Dipa::realignmentCb(const geometry_msgs::PoseWithCovarianceStampedConstPtr& msg)
{
	//update the time
	this->time_at_last_realignment = msg->header.stamp;

	// if tracking is lost update the pose of vo and the state
	if(TRACKING_LOST)
	{
		ROS_INFO_STREAM("GOT POSE UPDATE TO REINITIALIZE TRACKING WITH!");

		//get the transform from the msg frame to CAMERA
		tf::StampedTransform b2c;
		try {
			tf_listener.lookupTransform(msg->header.frame_id, CAMERA_FRAME,
					ros::Time(0), b2c);
		} catch (tf::TransformException& e) {
			ROS_ERROR_STREAM(e.what());
			ROS_ERROR("THIS POSE WILL NOT BE USED TO REALIGN!");
			TRACKING_LOST = true;
			return; //
		}

		tf::Transform w2b = tf::Transform(tf::Quaternion(msg->pose.pose.orientation.x, msg->pose.pose.orientation.y, msg->pose.pose.orientation.z, msg->pose.pose.orientation.w),
				tf::Vector3(msg->pose.pose.position.x, msg->pose.pose.position.y, msg->pose.pose.position.z));

		this->vo.updatePose(w2b*b2c, msg->header.stamp); // update vo's pose estimate and its pixel depth's

		//manually replace the dipa state's current estimate
		this->state.manualPoseUpdate(w2b, msg->header.stamp);


		TRACKING_LOST = false; // regained tracking
	}
}

void Dipa::bottomCamCb(const sensor_msgs::ImageConstPtr& img, const sensor_msgs::CameraInfoConstPtr& cam)
//void Dipa::bottomCamCb(const sensor_msgs::ImageConstPtr& img)
{

	ROS_WARN_COND(TRACKING_LOST, "TRACKING LOST! waiting for pose update to reinitialize");

	tf::StampedTransform c2b;
	try {
		tf_listener.lookupTransform(CAMERA_FRAME, BASE_FRAME,
				ros::Time(0), c2b);
	} catch (tf::TransformException& e) {
		ROS_ERROR_STREAM(e.what());
		ROS_ERROR("THIS IMAGE WILL NOT BE TRACKED!");
		return; //
	}

	cv::Mat temp = cv_bridge::toCvShare(img, img->encoding)->image.clone();

	// scale the image parameters for the renderer
	this->image_size = cv::Size(temp.cols / INVERSE_IMAGE_SCALE, temp.rows / INVERSE_IMAGE_SCALE);
	this->image_K = (1.0 / INVERSE_IMAGE_SCALE) * (cv::Mat_<float>(3, 3) << cam->K.at(0), cam->K.at(1), cam->K.at(2), cam->K.at(3), cam->K.at(4), cam->K.at(5), cam->K.at(6), cam->K.at(7), cam->K.at(8));

	//set the vo K
	this->vo.K = this->image_K;
	//set the render K and size
	this->renderer.setIntrinsic(this->image_K);
	this->renderer.setSize(this->image_size);


	cv::Mat scaled_img;
	cv::resize(temp, scaled_img, cv::Size(temp.cols / INVERSE_IMAGE_SCALE, temp.rows / INVERSE_IMAGE_SCALE));

	//PLANAR ODOMETRY
	double vo_error = -1; //per pixel odometry error
	bool good_vo = false;
	if(this->vo.state.features.size() > 0)
	{
		ROS_DEBUG("start vo");
		//flow the features
		this->vo.updateFeatures(scaled_img);

		if(this->vo.state.features.size() >= MINIMUM_TRACKABLE_FEATURES)
		{
			//compute the new pose
			good_vo = this->vo.computePose(vo_error);
			if(!vo_initialized && good_vo){vo_initialized = true; ROS_INFO("VO INITIALIZED");}
		}
		else
		{
			ROS_WARN("the visual odometry algorithm doesnt have enough valid features to compute motion!");
		}

		ROS_DEBUG("end vo");
	}
	else
	{
		ROS_WARN("visual odometry has no valid features");
	}

	//get more features
	this->vo.replenishFeatures(scaled_img);

	// update the current pose estimate with this vo estimate if it is good
	if(good_vo)
	{
		ROS_DEBUG("had good vo estimate: updating the base pose");
		this->state.updatePose(this->vo.state.currentPose * c2b, img->header.stamp);

		//check if the ppe is too high
		if(this->vo.state.ppe > MAXIMUM_VO_PPE)
		{
			TRACKING_LOST = true;
			ROS_WARN_STREAM("LOST TRACKING: VO PPE too high: " << this->vo.state.ppe);
		}

	}
	else
	{
		if(vo_initialized) // only lose tracking if vo hasnt been init
		{
			ROS_WARN("LOST TRACKING: VO has FAILED!");
			TRACKING_LOST = true;
		}

	}



	//GRID ALIGNMENT
	this->detectFeatures(scaled_img);

	ROS_ASSERT(this->state.currentPoseSet());

	/*if(this->state.getCurrentBestPoseStamp() != ros::Time::now())
	{
		ROS_ASSERT(img->header.stamp > this->state.getCurrentBestPoseStamp());
	}*/


	bool icp_good = false;
	double icp_ppe = -1;

	if(this->detected_corners.size() > 0)
	{
		tf::Transform w2c_aligned = this->runICP(this->vo.state.currentPose, icp_ppe, icp_good);


		//IF HAD GOOD GRID ALIGNMENT UPDATE THE VO
		if(icp_good)
		{
			ROS_INFO_STREAM("GOOD GRID ALIGNMENT WITH ERROR: " << icp_ppe);
			ROS_ASSERT(icp_ppe != -1);

			this->vo.updatePose(w2c_aligned, img->header.stamp); // update vo's pose estimate and its pixel depth's

			//manually replace the dipa state's current estimate
			this->state.manualPoseUpdate(w2c_aligned * c2b, img->header.stamp);

			if(TRACKING_LOST)
			{
				//if we have passed all outlier checks we have regained tracking internally
				ROS_INFO("REGAINED TRACKING FROM A INTERNAL GRID ALIGNMENT. MAY BE WRONG.");
				TRACKING_LOST = false;

#if PUBLISH_INSIGHT
				ROS_DEBUG("pub insight start");
				this->publishInsight(scaled_img, icp_good);
				ROS_DEBUG("pub insight end");
#endif

				// return to prevent a false velocity/omega from being published
				return;
			}
		}
		else
		{
			ROS_INFO("GRID ALIGNMENT FAILED");
		}
	}
	else // no detected corners
	{
		ROS_ERROR("NO DETECTED CORNERS. DID NOT ATTEMPT TO ALIGN GRID!");
	}


#if PUBLISH_INSIGHT
	ROS_DEBUG("pub insight start");
	this->publishInsight(scaled_img, icp_good);
	ROS_DEBUG("pub insight end");
#endif

	// final outlier checks
	if(!this->fitsPositionalConstraints(this->state.getCurrentBestPose()))
	{
		TRACKING_LOST = true;
		ROS_WARN("TRACKING HAS BEEN LOST! the pose estimate is in an extreme position. will now attempt to reinitialize");
	}

	if(this->vo.state.getTimeSinceLastRealignment(img->header.stamp) > MAXIMUM_TIME_SINCE_REALIGNMENT)
	{
		TRACKING_LOST = true;
		ROS_WARN_STREAM("TRACKING HAS BEEN LOST! icp has not realigned the pose in " << this->vo.state.getTimeSinceLastRealignment(img->header.stamp) <<" seconds. will now attempt to reinitialize");
	}

	if(!TRACKING_LOST){this->publishOdometry();}


	//alert the user if there has not been a realignment pose published
	if(this->time_at_last_realignment == ros::Time(0)){ROS_WARN("REALIGNMENT POSE HAS NOT BEEN PUBLISHED YET");}

}

void Dipa::detectFeatures(cv::Mat scaled_img)
{
	ROS_DEBUG("detect start");
	cv::Mat scaled_img_blur;

	cv::GaussianBlur(scaled_img, scaled_img_blur, cv::Size(0, 0), CANNY_BLUR_SIGMA);

	//cv::Mat white_only;
	//cv::threshold(scaled_img, white_only, WHITE_THRESH, 255, CV_8UC1);

	/*cv::Mat harris = cv::Mat(scaled_img.size(), CV_32FC1);
	cv::cornerHarris(scaled_img, harris, HARRIS_SIZE, HARRIS_APERTURE, HARRIS_K, cv::BORDER_DEFAULT);

	cv::Mat harris_norm, harris_norm_scaled;

	cv::normalize( harris, harris_norm, 0, 255, cv::NORM_MINMAX, CV_32FC1, cv::Mat() );
	cv::convertScaleAbs( harris_norm, harris_norm_scaled );*/
	/*
#if USE_FAST_CORNERS
	cv::Mat fast_blur;
	cv::GaussianBlur(scaled_img, fast_blur, cv::Size(0, 0), FAST_BLUR_SIGMA);
	std::vector<cv::KeyPoint> fast_kp;
	cv::FAST(fast_blur, fast_kp, FAST_THRESHOLD, true, cv::FastFeatureDetector::TYPE_9_16);
#endif
	 */

	//detect hough lines
	cv::Mat canny;
	cv::Canny(scaled_img_blur, canny, CANNY_THRESH_1, CANNY_THRESH_2);

#if SUPER_DEBUG
	cv::imshow("raw canny", canny);
	cv::waitKey(30);
#endif

	std::vector<cv::Vec2f> lines;

	cv::HoughLines(canny, lines, 1, CV_PI/180, HOUGH_THRESH, 0, 0);

	if(lines.size() == 0)
	{
		ROS_ERROR("line detection failed to detect lines, please tune. SKIPPING FRAME AND CLEARING CORNERS!");
		this->detected_corners.clear(); // remove previous detected corners
		return;
	}
	ROS_DEBUG_STREAM("starting intersect alg: " << lines.size());
	std::vector<cv::Point2f> intersects =  this->findLineIntersections(lines, cv::Rect(0, 0, canny.cols, canny.rows));
	ROS_DEBUG("finish intersect alg");
	ROS_DEBUG("detect end");

#if SUPER_DEBUG

	cv::Mat out = scaled_img;
	cv::cvtColor(out,out,CV_GRAY2RGB);

	//draw lines
	for( size_t i = 0; i < lines.size(); i++ )
	{
		float rho = lines[i][0], theta = lines[i][1];
		cv::Point pt1, pt2;
		double a = cos(theta), b = sin(theta);
		double x0 = a*rho, y0 = b*rho;
		pt1.x = cvRound(x0 + 1000*(-b));
		pt1.y = cvRound(y0 + 1000*(a));
		pt2.x = cvRound(x0 - 1000*(-b));
		pt2.y = cvRound(y0 - 1000*(a));
		cv::line( out, pt1, pt2, cv::Scalar(255, 255, 0), 2, CV_AA);
	}
	/*
#if USE_FAST_CORNERS
	cv::drawKeypoints(out, fast_kp, out, cv::Scalar(0, 0, 255));
#endif
	 */
	cv::Mat final;
	cv::cvtColor(canny,canny,CV_GRAY2RGB);

	//draw intersects
	for(auto e : intersects){
		cv::drawMarker(canny, e, cv::Scalar(255, 0, 0));
	}

	cv::hconcat(out, canny, final);

	cv::imshow("kp", final);
	cv::waitKey(30);
#endif

	this->detected_corners = intersects; // set the corners

	/*
	//if the user wants to include fast corners
#if USE_FAST_CORNERS
	for(auto e : fast_kp)
	{
		this->detected_corners.push_back(e.pt);
	}
#endif
	 */

}

bool inBounds(cv::Point2f testPt, cv::Rect bounds)
{
	return (testPt.x >= bounds.x && testPt.y >= bounds.y && testPt.x <= bounds.width && testPt.y <= bounds.height);
}

std::vector<cv::Point2f> Dipa::findLineIntersections(std::vector<cv::Vec2f> lines, cv::Rect boundingBox)
{
	std::vector<cv::Point2f> pts;

	//float min_d_theta = MIN_D_THETA;

	for(int i = 0; i < lines.size() - 1; i++)
	{
		for(int j = i+1; j < lines.size(); j++)
		{
			float t1 = (lines[i][1]);
			float t2 = (lines[j][1]);

			//assert that the lines are not parallel


			float r1 = lines[i][0];
			float r2 = lines[j][0];

			float ct1=cos(t1);     //matrix element a
			float st1=sin(t1);     //b
			float ct2=cos(t2);     //c
			float st2=sin(t2);     //d
			float d=ct1*st2-st1*ct2;        //determinative (rearranged matrix for inverse)

			//check if the vectors are parallel
			if(fabs(d) < PARALLEL_THRESH)
			{
				continue;
			}

			cv::Point2f pt = cv::Point2f((st2*r1-st1*r2)/d, (-ct2*r1+ct1*r2)/d);


			if(inBounds(pt, boundingBox))
			{
				pts.push_back(pt);
			}
		}
	}

	return pts;
}

/*void Dipa::setupKDTree()
{
	if(kdtree != NULL)
	{
		delete kdtree;
		kdtree = 0;
	}
	ROS_DEBUG("setting up kdtree");
	kdtree = new cv::flann::Index(cv::Mat(detected_corners).reshape(1), cv::flann::KDTreeIndexParams());
	ROS_DEBUG("tree setup");
}*/

void Dipa::findClosestPoints(Matches& model)
{
	/*
	 * vector<Point2f> pointsForSearch; //Insert all 2D points to this vector
flann::KDTreeIndexParams indexParams;
flann::Index kdtree(Mat(pointsForSearch).reshape(1), indexParams);
vector<float> query;
query.push_back(pnt.x); //Insert the 2D point we need to find neighbours to the query
query.push_back(pnt.y); //Insert the 2D point we need to find neighbours to the query
vector<int> indices;
vector<float> dists;
kdtree.radiusSearch(query, indices, dists, range, numOfPoints);
	 */

	cv::flann::Index tree(cv::Mat(detected_corners).reshape(1), cv::flann::KDTreeIndexParams());

	//ROS_DEBUG("finding nearest neighbors");
	double maxRadius = sqrt(this->image_size.width * this->image_size.width + this->image_size.height * this->image_size.height);

	for(auto& e : model.matches)
	{
		std::vector<float> query;
		query.push_back(e.obj_px.x);
		query.push_back(e.obj_px.y);

		std::vector<int> indexes;
		std::vector<float> dists;

		tree.knnSearch(query, indexes, dists, 4);

		int best = 0;
		double min = DBL_MAX;

		for(auto j : indexes)
		{
			e.measurement = cv::Point2d(detected_corners.at(j).x, detected_corners.at(j).y);

			double temp = e.computePixelNorm();

			if(temp < min)
			{
				best = j;
				min = temp;
			}
		}

		e.measurement = cv::Point2d(detected_corners.at(best).x, detected_corners.at(best).y);
		e.pixelNorm = min;
		//ROS_DEBUG_STREAM("norm: " << e.pixelNorm);
	}

	//ROS_DEBUG("neighbors found");
}

void Dipa::tf2rvecAndtvec(tf::Transform tf, cv::Mat& tvec, cv::Mat& rvec){
	cv::Mat_<double> R = (cv::Mat_<double>(3, 3) << tf.getBasis().getRow(0).x(), tf.getBasis().getRow(0).y(), tf.getBasis().getRow(0).z(),
			tf.getBasis().getRow(1).x(), tf.getBasis().getRow(1).y(), tf.getBasis().getRow(1).z(),
			tf.getBasis().getRow(2).x(), tf.getBasis().getRow(2).y(), tf.getBasis().getRow(2).z());

	//ROS_DEBUG("setting up tvec and rvec");

	cv::Rodrigues(R, rvec);

	tvec = (cv::Mat_<double>(3, 1) << tf.getOrigin().x(), tf.getOrigin().y(), tf.getOrigin().z());

	//ROS_DEBUG_STREAM("tvec: " << tvec << "\nrvec: " << rvec);
}

tf::Transform Dipa::rvecAndtvec2tf(cv::Mat tvec, cv::Mat rvec){
	//ROS_DEBUG("rvectvec to tf");
	cv::Mat_<double> rot;
	cv::Rodrigues(rvec, rot);
	/*ROS_DEBUG_STREAM("rot: " << rot);
		ROS_DEBUG_STREAM("rvec: " << rvec);*/
	//ROS_DEBUG_STREAM("tvec " << tvec);

	tf::Transform trans;

	trans.getBasis().setValue(rot(0), rot(1), rot(2), rot(3), rot(4), rot(5), rot(6), rot(7), rot(8));
	trans.setOrigin(tf::Vector3(tvec.at<double>(0), tvec.at<double>(1), tvec.at<double>(2)));

	//ROS_DEBUG_STREAM("rot: " << trans.getRotation().w() << ", " << trans.getRotation().x() << ", " << trans.getRotation().y() << ", " << trans.getRotation().z());
	/*double x, y, z;
		trans.getBasis().getRPY(x, y, z);
		ROS_DEBUG_STREAM("tf rvec " << x <<", "<<y<<", "<<z);*/
	//ROS_DEBUG_STREAM(trans.getOrigin().x() << ", " << trans.getOrigin().y() << ", " << trans.getOrigin().z());

	//ROS_DEBUG("finished");

	return trans;
}

/*
 * tests if the pose estimate is reasonable by its position estimate
 */
bool Dipa::fitsPositionalConstraints(tf::Transform w2c)
{
	if(w2c.getOrigin().z() < ABSOLUTE_MIN_Z || w2c.getOrigin().z() > ABSOLUTE_MAX_Z)
	{
		return false;
	}

	if(w2c.getOrigin().x() < (-(GRID_WIDTH * GRID_SPACING) / 2) || w2c.getOrigin().x() > ((GRID_WIDTH * GRID_SPACING) / 2))
	{
		return false;
	}

	if(w2c.getOrigin().y() < (-(GRID_HEIGHT * GRID_SPACING) / 2) || w2c.getOrigin().y() > ((GRID_HEIGHT * GRID_SPACING) / 2))
	{
		return false;
	}

	return true;
}

/*
 * runs iterative closest point algorithm modified to work with 2d to 3d correspondences.
 * takes a tf transform representing the transform from the world coordinate frame to the camera coordinate frame
 * this transform should be the current best guess of the transform
 *
 * returns the optimized pose which fits the corner model the best
 */
tf::Transform Dipa::runICP(tf::Transform w2c_guess, double& ppe, bool& pass)
{
	// set the ppe to -1 to tell if it has been set
	ppe = -1;

	//set up the renderer with the current K and size
	this->renderer.setSize(this->image_size);
	this->renderer.setIntrinsic(this->image_K);

	cv::Mat rvec, tvec; // form the rvec and tvec for the error minimization through solvepnp

	this->tf2rvecAndtvec(w2c_guess.inverse(), tvec, rvec); // set the rvec and tvec to the current best guesses inverse (C2W);

	ROS_DEBUG("begining optim");

	//initial setup and sse calculation
	this->renderer.setC2W(this->rvecAndtvec2tf(tvec, rvec)); // the the renderer's current pose
	Matches matches = this->renderer.renderGridCorners(); // render the corners into this frame given our current guess

	this->findClosestPoints(matches); // find the closest points between the model and the observation corners
	double last_sse = matches.computePerPixelError();
	double current_sse = last_sse;

	ROS_DEBUG_STREAM("initial error: " << current_sse);


#if SUPER_DEBUG
	cv::Mat blank = cv::Mat::zeros(this->image_size, CV_8UC3);
	blank = matches.draw(blank, this->detected_corners);
	cv::imshow("render", blank);
	cv::waitKey(30);
	ros::Duration dur(1);
	//dur.sleep();
#endif

	for(int i = 0; i < MAX_ITERATIONS; i++)
	{
		// now we minimize the photometric error between our known model and our observations using the correspondences we have just guessed
#if USE_MAX_NORM
		Matches huber = matches.performHuberMaxNorm(MAX_NORM);
		ROS_DEBUG_STREAM("performed huber max norm size before: " << matches.matches.size() << " now: " << huber.matches.size());

		//check if there are enough matches to reliably align the grid
		if(huber.matches.size() < MINIMUM_INITIAL_MATCHES)
		{
			ROS_WARN("too few matches to reliably align the grid!");

			pass = false;

			return w2c_guess; // return the guess as it is the best answer for now
		}

		cv::solvePnP(huber.getObjectInOrder(), huber.getMeasurementsInOrder(), this->image_K, cv::noArray(), rvec, tvec, true, cv::SOLVEPNP_ITERATIVE); // use the current guess to help convergence
#else
		cv::solvePnP(matches.getObjectInOrder(), matches.getMeasurementsInOrder(), this->image_K, cv::noArray(), rvec, tvec, true, cv::SOLVEPNP_ITERATIVE); // use the current guess to help convergence
#endif
		// recalculate correspondences and sse
		this->renderer.setC2W(this->rvecAndtvec2tf(tvec, rvec)); // the the renderer's current pose
		matches = this->renderer.renderGridCorners(); // render the corners into this frame given our current guess

		this->findClosestPoints(matches); // find the closest points between the model and the observation corners
		current_sse = matches.computePerPixelError(); // compute current error

		ROS_DEBUG_STREAM("current error: " << current_sse);

		if(fabs(current_sse - last_sse) < CONVERGENCE_DELTA){
			ROS_DEBUG("PNP-ICP Converged");
#if USE_MAX_NORM
			double huber_error = huber.computePerPixelError();

			ppe = huber_error;

			ROS_DEBUG_STREAM("huber per point error: " << huber_error);
			if(huber_error > MAX_ICP_ERROR)
			{
				ROS_WARN("final per point error too high!");

				pass = false;

				return w2c_guess;
			}
#else
			ppe = currencurrent_sse;

			if(current_sse > MAX_ICP_ERROR)
			{

				pass = false;

				return w2c_guess;
			}
#endif

#if SUPER_DEBUG
			cv::Mat blank = cv::Mat::zeros(this->image_size, CV_8UC3);
			blank = matches.draw(blank, this->detected_corners);
			cv::imshow("render", blank);
			cv::waitKey(30);
			ros::Duration dur(1);
			//dur.sleep();
#endif
			break;
		}
		else
		{
			last_sse = current_sse; // set the new last sse
		}

#if SUPER_DEBUG
		cv::Mat blank = cv::Mat::zeros(this->image_size, CV_8UC3);
		blank = matches.draw(blank, this->detected_corners);
		cv::imshow("render", blank);
		cv::waitKey(30);
		ros::Duration dur(1);
		//dur.sleep();
#endif

	}

	ROS_DEBUG("end optim");

	ROS_ASSERT(USE_MAX_NORM);

	Matches huber = matches.performHuberMaxNorm(MAX_NORM);

	if(huber.matches.size() < MINIMUM_FINAL_MATCHES)
	{
		ROS_WARN_STREAM("huber matches too low: " << huber.matches.size());
		pass = false;
		return w2c_guess;
	}

	double huberRatio = (double)huber.matches.size() / (double)matches.matches.size();

	if(huberRatio < MINIMUM_HUBER_RATIO)
	{
		ROS_WARN_STREAM("huber ratio too low at: " << huberRatio);
		pass = false;
		return w2c_guess;
	}

	//if error has not been set calculate it
	//this means that icp did not converge
	if(ppe == -1)
	{
		ROS_WARN_STREAM("used maximum icp iters, calculating ppe with huber!");

		ppe = huber.computePerPixelError();

	}

	//finally check if the error is too high
	if(ppe > MAX_ICP_ERROR)
	{
		ROS_WARN_STREAM("huber ppe too high at: " << ppe);
		pass = false;
		return w2c_guess;
	}

	//all outlier tests have passed

	tf::Transform final_w2c = this->rvecAndtvec2tf(tvec, rvec).inverse();

	if(!this->fitsPositionalConstraints(final_w2c))
	{
		ROS_WARN_STREAM("pose does not fit positional constraints: x: " << final_w2c.getOrigin().x() << " y: " << final_w2c.getOrigin().y() << " z: " << final_w2c.getOrigin().z());
		pass = false;
		return w2c_guess;
	}

	pass = true;

	return final_w2c;  // return the best w2c guess

}


void Dipa::publishOdometry()
{

	if(!this->state.twistSet() || !this->state.currentPoseSet())
	{
		ROS_DEBUG("NOT READY TO PUBLISH!");
		return;
	}

	nav_msgs::Odometry msg;

	msg.child_frame_id = BASE_FRAME;
	msg.header.stamp = this->state.getCurrentBestPoseStamp();
	msg.header.frame_id = WORLD_FRAME;

	tf::Quaternion q = this->state.getCurrentBestPose().getRotation();

	msg.pose.pose.orientation.w = q.w();
	msg.pose.pose.orientation.x = q.x();
	msg.pose.pose.orientation.y = q.y();
	msg.pose.pose.orientation.z = q.z();

	msg.pose.pose.position.x = this->state.getCurrentBestPose().getOrigin().x();
	msg.pose.pose.position.y = this->state.getCurrentBestPose().getOrigin().y();
	msg.pose.pose.position.z = this->state.getCurrentBestPose().getOrigin().z();

	msg.twist.twist.angular.x = this->state.getBaseFrameOmega().x();
	msg.twist.twist.angular.y = this->state.getBaseFrameOmega().y();
	msg.twist.twist.angular.z = this->state.getBaseFrameOmega().z();

	msg.twist.twist.linear.x = this->state.getBaseFrameVelocity().x();
	msg.twist.twist.linear.y = this->state.getBaseFrameVelocity().y();
	msg.twist.twist.linear.z = this->state.getBaseFrameVelocity().z();

	// twist covariance = CONST * VO_PPE
	if(this->vo.state.ppe == 0)
		this->vo.state.ppe = 0.00001;

	msg.twist.covariance.at(0) = this->vo.state.ppe;
	msg.twist.covariance.at(7) = this->vo.state.ppe;
	msg.twist.covariance.at(14) = this->vo.state.ppe;
	msg.twist.covariance.at(21) = this->vo.state.ppe;
	msg.twist.covariance.at(28) = this->vo.state.ppe;
	msg.twist.covariance.at(35) = this->vo.state.ppe;

	this->odom_pub.publish(msg);
}


void Dipa::publishInsight(cv::Mat in, bool grid_aligned){

	cv::Mat src;

	cv::cvtColor(in, src, CV_GRAY2BGR);

	if(detected_corners.size() > 0)
	{
		for(auto e : this->detected_corners)
		{
			cv::drawMarker(src, e, cv::Scalar(255, 0, 0), cv::MARKER_DIAMOND, 4);
		}
	}

	for(auto e : this->vo.state.features){
		cv::drawMarker(src, e.px, cv::Scalar(255, 0, 255), cv::MARKER_SQUARE, 6);
	}
	ROS_DEBUG_STREAM("rendering grid");
	this->renderer.setW2C(this->vo.state.currentPose); // render the grid with the current w2c
	Matches m = this->renderer.renderGridCorners();

	ROS_DEBUG_STREAM("rendering grid corners with " << m.matches.size() << "corners");
	if(m.matches.size() > 0)
	{
		//draw
		for(auto e : m.matches){
			if(grid_aligned)
			{
				cv::drawMarker(src, e.obj_px, cv::Scalar(0, 255, 0), cv::MARKER_CROSS, 8);
			}
			else
			{
				cv::drawMarker(src, e.obj_px, cv::Scalar(0, 0, 255), cv::MARKER_CROSS, 8);
			}
		}
	}
	else
	{
		ROS_WARN("there are no projected grid corners for the current estimate.");
	}

	cv_bridge::CvImage cv_img;

	cv_img.image = src;
	cv_img.header.frame_id = CAMERA_FRAME;
	cv_img.encoding = sensor_msgs::image_encodings::BGR8;

	this->insight_pub.publish(cv_img.toImageMsg());
	ROS_DEBUG("end publish");
}
