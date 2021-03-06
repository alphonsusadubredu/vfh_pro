 /*
    By Alphonsus Adu-Bredu - alphonsusbq436@gmail.com
	Architecture:
	1. goal is received from a goal client by goal service
	2. In goal service, in a while loop, plan from current pose to goal
	3. Send plan to handle_waypoint_service client
	4. In ,handle_waypoint_service goal is followed until it reaches bounds 3x6
	5. Returns back to goal service while loop and plans again from 
		vehicle pose to goal. And transmits plan again
	6. while loop breaks when |currentpose - goalpose| < threshold.
	7. returns true to goal client
   */


#include "global_planner.h"

const int laserCloudStackNum = 15;
pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloud(new pcl::PointCloud<pcl::PointXYZI>());
pcl::PointCloud<pcl::PointXYZI>::Ptr plannerCloud(new pcl::PointCloud<pcl::PointXYZI>());
pcl::PointCloud<pcl::PointXYZI>::Ptr plannerCloudCrop(new pcl::PointCloud<pcl::PointXYZI>());
pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudStack[laserCloudStackNum];


GlobalPlanner::GlobalPlanner(ros::NodeHandle* nodehandle):nh(*nodehandle)
{
	
	subclose = nh.subscribe<nav_msgs::Odometry>
								("/odom",5,&GlobalPlanner::odometryCallback,this);
	subvoxels = nh.subscribe<visualization_msgs::MarkerArray>
								("/occupied_cells_vis_array",5,&GlobalPlanner::voxelCallback,this);
	
	pubPath = nh.advertise<nav_msgs::Path> ("/global_path", 5);
	pubPoint = nh.advertise<geometry_msgs::PointStamped>("/goal_point",5);
	backPub = nh.advertise<geometry_msgs::PointStamped>("/way_point",5);
	pubBoundary = nh.advertise<geometry_msgs::PolygonStamped> ("/search_space", 5);
	pubPlannerCloud = nh.advertise<sensor_msgs::PointCloud2> ("/plannercloud", 2);
	pubWaypoint = nh.advertise<geometry_msgs::PointStamped> ("/way_point", 5);
	pubLast = nh.advertise<std_msgs::Bool> ("/last_waypoint",1);
	init_stack();

	service_goal = nh.advertiseService("goal_channel", &GlobalPlanner::receive_goal_service,this);
	plan_to_wp_service = nh.advertiseService("specific_waypoint_channel", &GlobalPlanner::handle_waypoint_service, this);
	ROS_INFO("PLAYER READY SAMA");

}


//receives incoming pointcloud from its topic
void GlobalPlanner::voxelCallback(const visualization_msgs::MarkerArray::ConstPtr& voxels)
{
	voxelarray.clear();
	int markerarray_size = voxels->markers.size();
	for (int i=0; i<markerarray_size; i++)
	{
		int pointsarraysize = voxels->markers[i].points.size();
		for (int j=0; j<pointsarraysize; j++)
		{
			voxelarray.push_back(voxels->markers[i].points[j]);
		}
	}
	

}

//Generates a plan from current robot's position to 
//coordinates (goalX, goalY)
bool GlobalPlanner::plan(double goalX, double goalY)
{
	ros::spinOnce();
	ROS_INFO("Current Position: %f, %f",vehicleX,vehicleY);
	if (!ss) return false;
	ROS_INFO("Planning to coordinates %f,%f",goalX,goalY);

	ob::ScopedState<> start(ss->getStateSpace());
	start[0] = vehicleX;
	start[1] = vehicleY;

	ob::ScopedState<> goal(ss->getStateSpace());
	goal[0] = goalX;
	goal[1] = goalY;

	ss->setStartAndGoalStates(start,goal);
	// ss->setOptimizationObjective(getClearanceObjective(ss->getSpaceInformation()));

	for (int i=0; i<1; ++i)
	{
		if (ss->getPlanner())
			ss->getPlanner()->clear();
		ss->solve(planning_time);
	}

	const std::size_t ns = ss->getProblemDefinition()->getSolutionCount();

	OMPL_INFORM("Found %d solutions",(int)ns);

	if (ss->haveSolutionPath())
	{
		ss->simplifySolution();
		og::PathGeometric &p = ss->getSolutionPath();
		ss->getPathSimplifier()->simplifyMax(p);
		ss->getPathSimplifier()->smoothBSpline(p);

		return true;

	}
	return false;
}


//Transmit's planned trajectory to the specific_waypoint_channel
//service and publishes the path for visualization
bool GlobalPlanner::transmit_plan_client()
{
	ros::spinOnce();
	ROS_INFO("Sending global plan");
	if(!ss || !ss->haveSolutionPath())
		return false;

	nav_msgs::Path path;
	path.header.frame_id="odom";

	og::PathGeometric &p = ss->getSolutionPath();
	p.interpolate();
	ros::ServiceClient client = nh.serviceClient<global_planner::Waypoints>("specific_waypoint_channel");
	global_planner::Waypoints srv;

	int path_size = p.getStateCount();
	ROS_INFO("Starting for loop. There are %d states",path_size);

	path.poses.resize(path_size);
	srv.request.waypoints.poses.resize(path_size);
	
	
	int cutoff_index = path_size;
	double dist=0;

	for(int i=0; i<path_size; i++)
	{
		const double x = 
			(double)p.getState(i)->as<ob::RealVectorStateSpace::StateType>()->values[0];

		const double y = 
			(double)p.getState(i)->as<ob::RealVectorStateSpace::StateType>()->values[1];

		dist = sqrt(pow((vehicleX-x),2) + pow((vehicleY-y),2));
	
		if (dist>confidence_boundary and i >0) 
			{cutoff_index = i; 
				cout <<"cat off: "<<i<<endl; break;
			}	
	}	
			
	for(int i=0; i<path_size; i++)
	{
		const double x = 
			(double)p.getState(i)->as<ob::RealVectorStateSpace::StateType>()->values[0];

		const double y = 
			(double)p.getState(i)->as<ob::RealVectorStateSpace::StateType>()->values[1];

		path.poses[i].pose.position.x = x;
		path.poses[i].pose.position.y = y;
		dist = sqrt(pow((vehicleX-x),2) + pow((vehicleY-y),2));		
		cout << x << "," << y <<endl;
	}



	for (std::size_t i=1; i<path_size; i++) //<cutoff_index
	{
		const double x = 
			(double)p.getState(i)->as<ob::RealVectorStateSpace::StateType>()->values[0];

		const double y = 
			(double)p.getState(i)->as<ob::RealVectorStateSpace::StateType>()->values[1];

		srv.request.waypoints.poses[i-1].position.x = x;
		srv.request.waypoints.poses[i-1].position.y = y;
		
		cout << x << "," << y <<endl;
		
	}
	pubPath.publish(path);
	
	ROS_INFO("Cut-off index is %d", cutoff_index);
	srv.request.waypoints.header.seq = path_size-1;
	
	
	if (client.call(srv))
	{
		ROS_INFO("Global plan reached");
		return true;
	}
		
}


//initializes the stacked pointcloud data
void GlobalPlanner::init_stack()
{
	for (int i = 0; i < laserCloudStackNum; i++) {
    	laserCloudStack[i].reset(new pcl::PointCloud<pcl::PointXYZI>());
  }
}


// rounds a number to the nearest ...
double GlobalPlanner::round_num(double num) const
{
	return floor(num*10+0.5)/10;
}

//stacks incoming pointcloud
void GlobalPlanner::stack_pointcloud()
{
	laserCloudStack[laserCloudCount]->clear();
	*laserCloudStack[laserCloudCount] = *laserCloud;
	laserCloudCount = (laserCloudCount + 1)%laserCloudStackNum;

	plannerCloud->clear();
	for (int i=0; i<laserCloudStackNum; i++)
		*plannerCloud+=*laserCloudStack[i];

 	sensor_msgs::PointCloud2 pcloud;
	pcl::toROSMsg(*plannerCloud, pcloud);
	pcloud.header.frame_id = "/odom";
	pubPlannerCloud.publish(pcloud);
}



//receives incoming robot odometry from its topic
void GlobalPlanner::odometryCallback(const nav_msgs::Odometry::ConstPtr& odom)
{
	double roll,pitch,yaw;
	geometry_msgs::Quaternion geoQuat = odom->pose.pose.orientation;
	tf::Matrix3x3(tf::Quaternion(geoQuat.x, geoQuat.y, geoQuat.z, geoQuat.w))
																.getRPY(roll, pitch, yaw);
	vehicleX = odom->pose.pose.position.x;
  	vehicleY = odom->pose.pose.position.y;
  	vehicleZ = odom->pose.pose.position.z;
  	// cout << voxelarray[5] << endl;
}


//receives the goal coordinates to plan to from the goal_channel service,
//calls the plan method to plan a path to the goal coordinates,
//performs contingency behaviors if no valid plan is found.
bool GlobalPlanner::receive_goal_service(global_planner::Goal::Request &req, global_planner::Goal::Response &res)
{
	

	ros::Rate loop_rate(0.5);
	bool replan = false;
	bool reached_goal = false;
	goalX = req.goal_pose.position.x;	goalY = req.goal_pose.position.y;

	geometry_msgs::PointStamped goal;
	goal.header.frame_id = "/odom";
	goal.point.x = goalX;
	goal.point.y = goalY;
	pubPoint.publish(goal);

	int r = rand()%10;
	if (r < 5) wall_follow_right =true;
	else	wall_follow_right = false;

	while (ros::ok())
	{
		auto space (std::make_shared<ob::RealVectorStateSpace>());
		//set bounds
		double delta = 1.0;
		if(goalY > vehicleY and goalX > vehicleX)
		{
			minBoundX = vehicleX - delta; maxBoundX = goalX+delta;
			minBoundY = vehicleY - delta; maxBoundY = goalY+delta;  
		}

		else if (goalY < vehicleY and goalX > vehicleX)
		{
			minBoundX = vehicleX-delta; maxBoundX = goalX+delta;
			minBoundY = goalY-delta; maxBoundY = vehicleY+delta;
		}

		else if (goalY > vehicleY and goalX < vehicleX)
		{
			minBoundX = goalX-delta; maxBoundX = vehicleX+delta;
			minBoundY = vehicleY-delta; maxBoundY = goalY+delta;
		}

		else if (goalY < vehicleY and goalX < vehicleX)
		{
			minBoundX = goalX-delta; maxBoundX = vehicleX+delta;
			minBoundY = goalY-delta; maxBoundY = vehicleY+delta;
		}

		else
		{
			minBoundX = vehicleX-10.0; maxBoundX = vehicleX+10.0;
			minBoundY = vehicleY-10.0; maxBoundY = vehicleY+10.0;
		}

		minBoundY-=10; maxBoundY+=10;
		minBoundX-=10; maxBoundX+=10;
		space->addDimension(minBoundX, maxBoundX);
		space->addDimension(minBoundY, maxBoundY);


		geometry_msgs::PolygonStamped boundaryMsgs;
		boundaryMsgs.polygon.points.resize(4);
		boundaryMsgs.header.frame_id = "/odom";
		boundaryMsgs.polygon.points[0].x = minBoundX;
		boundaryMsgs.polygon.points[0].y = minBoundY;
		boundaryMsgs.polygon.points[3].x = minBoundX;
		boundaryMsgs.polygon.points[3].y = maxBoundY;
		boundaryMsgs.polygon.points[1].x = maxBoundX;
		boundaryMsgs.polygon.points[1].y = minBoundY;
		boundaryMsgs.polygon.points[2].x = maxBoundX;
		boundaryMsgs.polygon.points[2].y = maxBoundY;

		pubBoundary.publish(boundaryMsgs);

		// auto si (std::make_shared<ob::SpaceInformation>(space));
		// si->setStateValidityChecker(boost::bind(&GlobalPlanner::isStateValid, this, _1));

		// ss.reset(new og::SimpleSetup(ob::StateSpacePtr(space)));
		ss = std::make_shared<og::SimpleSetup>(space);
		ss->setStateValidityChecker([this](const ob::State *state)
												{return isStateValid(state);});

		ss->getSpaceInformation()->setStateValidityCheckingResolution
														(0.05);
		// cout << "ending" << endl; res.status=true; return true;
		// cout << "************received goal************" << endl;
		ss->setPlanner(ob::PlannerPtr (new og::RRTstar(ss->getSpaceInformation())));
		// ss->setPlanner(make_shared<og::RRTstar>(ss->getSpaceInformation()));
		// cout << "************problem above ************" << endl;
		if (plan(goalX,goalY))
		{
			ROS_INFO("REPLANNING...");
			replan = transmit_plan_client();
		}

		else
		{
			geometry_msgs::PointStamped back;
			back.header.frame_id = "/odom";
			// back.header.stamp = ros::Time().fromSec(curTime);
		
			
				back.point.x = vehicleX;
				if (wall_follow_right)
					back.point.y = vehicleY+0.5;
				else	
					back.point.y = vehicleY-0.5;
			
			
			// back.point.z = waypoint_array.poses[wayPointID].position.z;
			backPub.publish(back);
		}
		

		// if(replan)  //gotten to the end of transmitted global plan
		pubPoint.publish(goal);
    	ros::spinOnce();
    	loop_rate.sleep();

    	if (at_goal(goalX,goalY))
    	{
    		reached_goal = true;
    		res.status=true;
    		break;
    	}



	}
	res.status=true;
	return true;

}


// receives planned trajectory from the specific_waypoint_channel service
// and sends waypoints one after the other to the local planner if the current
// waypoint is reached. 
// Triggers replanning after some time interval (currently 5 seconds)
bool GlobalPlanner::handle_waypoint_service(global_planner::Waypoints::Request &req, global_planner::Waypoints::Response &res)
{
  auto t1 = std::chrono::high_resolution_clock::now();
  int size = req.waypoints.header.seq;
  waypoint_array = req.waypoints;
  ROS_INFO("Received path of size: %d", size);

  int wayPointID = 0;
  int waypointSize = waypoint_array.header.seq;
  std_msgs::Bool last;

  
  geometry_msgs::PointStamped waypointMsgs;
  waypointMsgs.header.frame_id = "/odom";

  
  std_msgs::Float32 speedMsgs;

  
  geometry_msgs::PolygonStamped boundaryMsgs;
  boundaryMsgs.header.frame_id = "/odom";


  ros::Rate rate(100);
  bool status = ros::ok();

  // waypointXYRadius=0.5;
  while (status) {
    ros::spinOnce();

    float disX = vehicleX - waypoint_array.poses[wayPointID].position.x;
    float disY = vehicleY - waypoint_array.poses[wayPointID].position.y;
    float disZ = vehicleZ - waypoint_array.poses[wayPointID].position.z;


    // publish waypoint, speed, and boundary messages at certain frame rate
   

    if (curTime - waypointTime > 1.0 / frameRate) {
       if (wayPointID < size-2)
        last.data = false;
      else 
        last.data = true;

      waypointMsgs.header.stamp = ros::Time().fromSec(curTime);
      waypointMsgs.point.x = waypoint_array.poses[wayPointID].position.x;
      waypointMsgs.point.y = waypoint_array.poses[wayPointID].position.y;
      waypointMsgs.point.z = waypoint_array.poses[wayPointID].position.z;
      pubWaypoint.publish(waypointMsgs);
      pubLast.publish(last);

      if (sendSpeed) {
        speedMsgs.data = speed;
        pubSpeed.publish(speedMsgs);
      }

      

      waypointTime = curTime;
    }

    if (wayPointID == size-2) waypointXYRadius = 1.0;
    
    if (sqrt(disX * disX + disY * disY) < waypointXYRadius) {
      wayPointID++;
    }
    
    
    if (wayPointID == size) {break;}


//To get Marco to stop while in the process of following a waypoint
    if (stop_immediately) 
      {
        res.status = true;
        return true;
      }
      auto t2 = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double, std::milli> fp_ms = t2 - t1;

      if (fp_ms.count() > 5000)
        return true;

    status = ros::ok();
    rate.sleep();
  }

  ROS_INFO("I'm there");
  res.status = true;
  return true;
}


// returns true if robot is at goal coordinates and false otherwise
bool GlobalPlanner::at_goal(double goalX, double goalY)
{
	double dist = sqrt(pow((vehicleX-goalX),2) + pow((vehicleY-goalY),2));
	if (dist < 1.2)
		return true;
	else
		return false;
}


// returns true if sampled state is within an obstacle
bool GlobalPlanner::intersects_obstacle(double stateX, double stateY, double pointX, double pointY) const
{
	double vehicle_state_gradient = ((vehicleY-stateY)/(vehicleX-stateX));
	double vehicle_point_gradient = ((vehicleY-pointY)/(vehicleX-pointY));
	double point_state_gradient = ((pointY-stateY)/(pointX-stateX));

	if (vehicle_state_gradient == vehicle_point_gradient)// and vehicle_point_gradient == point_state_gradient)
		return true;
	return false;
}


// used for collision-checking by OMPL.
// returns true if state is valid (not inside an obstacle)
// and false otherwise
bool GlobalPlanner::isStateValid(const ob::State *state) const
{
	cout << "validating"<<endl;
	const double x = std::min((double)state->as<ob::RealVectorStateSpace::StateType>()->values[0],
		maxBoundX);
	const double y = std::min((double)state->as<ob::RealVectorStateSpace::StateType>()->values[1],
		maxBoundY);
	
	bool free = true;

	double disp = sqrt(pow((x-vehicleX),2) + pow((y-vehicleY),2));
	if (disp < laser_radius)
	{
		geometry_msgs::Point point;
		int voxelarray_size = voxelarray.size();
		cout << voxelarray_size << endl;
		for(int i=0; i<voxelarray_size; i++)
		{
			point = voxelarray[i];
			float pointX = point.x;
		    float pointY = point.y;
		    float pointZ = point.z;



		    float dis = sqrt(pow(pointX-x,2)+pow(pointY-y,2));

		    if (dis < too_close_threshold)
		    {
		    	free = false;
		    	return free;
		    }		   
		}
	}

	else
		free = true;
	return free;
}








int main(int argc, char ** argv)
{	
	ros::init(argc,argv,"global_oplanner");
	ros::NodeHandle nh;
	GlobalPlanner globalPlanner(&nh);
	ros::spin();
	return 0;
		
}