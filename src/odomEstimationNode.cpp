// Author of LiDAR-Odometry_DQ: Edison Velasco
// Email edison.velasco@ua.es

//c++ lib
#include <cmath>
#include <vector>
#include <mutex>
#include <queue>
#include <thread>
#include <chrono>
#include <random> // only for plot arrows with differents color

//ros lib
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <std_msgs/Float64.h>
#include <nav_msgs/Odometry.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <geometry_msgs/PoseArray.h>

#include <tf2_ros/transform_listener.h>
#include <geometry_msgs/TransformStamped.h>

#include <tf2_msgs/TFMessage.h>

//pcl lib
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

//local lib
#include "odomEstimationClass.h"
#include "STDFunctions.h"

// NanoFlann library for kdtree
#include <nanoflann.hpp>

// ///// synchronizer topics
// #include <message_filters/subscriber.h>
// #include <message_filters/synchronizer.h>
// #include <message_filters/sync_policies/approximate_time.h>

typedef pcl::PointXYZI PointType;  // only for std
typedef pcl::PointCloud<PointType> pcSTD;
ConfigSetting config_setting; // configs for STDescritor
STDescManager *std_manager;

OdomEstimationClass odomEstimation;
std::mutex mutex_lock;
std::queue<sensor_msgs::PointCloud2ConstPtr> pointCloudEdgeBuf;
std::queue<sensor_msgs::PointCloud2ConstPtr> pointCloudSurfBuf;
std::queue<geometry_msgs::PoseArrayConstPtr> stdPoseBuf;

std::string childframeID = "os_sesnor";
std::string edge_pcl = "/pcl_edge";
std::string surf_pcl = "/pcl_surf";
std::string stdescri = "/std_curr_poses";
std::string stdMap   = "/std_map_poses";
std::string pcTopic   = "/velodyne_points";

std::string path_odom =  "/epvelasco/docker_ws/ceres_ROS_docker/resultados/00/00.txt";
std::string path_calib = "/epvelasco/dataset/KITTI/rosbags_velodyne/dataset/calib_velo_camera/00.txt";

ros::Publisher pubLaserOdometry;
ros::Publisher pubOdometryDiff;

/// STD publishers
ros::Publisher pose_pub_prev ;
ros::Publisher pose_pub_curr;
ros::Publisher pubSTD ;


ros::Publisher time_average;
bool save_data = true;


void velodyneSurfHandler(const sensor_msgs::PointCloud2ConstPtr &laserCloudMsg)
{
    mutex_lock.lock();
    pointCloudSurfBuf.push(laserCloudMsg);
    mutex_lock.unlock();
}
void velodyneEdgeHandler(const sensor_msgs::PointCloud2ConstPtr &laserCloudMsg)
{

    mutex_lock.lock();
    pointCloudEdgeBuf.push(laserCloudMsg);
    mutex_lock.unlock();
}
  
// read the pointcloud to extract STD descriptor
std::queue<sensor_msgs::PointCloud2::ConstPtr> laser_buffer;
sensor_msgs::PointCloud2::ConstPtr msg_point;
std::mutex laser_mtx;

void laserCloudHandler(const sensor_msgs::PointCloud2::ConstPtr &msg) {
    std::unique_lock<std::mutex> lock(laser_mtx);
    msg_point = msg;
    laser_buffer.push(msg);
}

bool readPC(pcSTD::Ptr &cloud) {
    if (laser_buffer.empty())
        return false;

    auto laser_msg = laser_buffer.front();
    double laser_timestamp = laser_msg->header.stamp.toSec();
    pcl::fromROSMsg(*laser_msg, *cloud);
    std::unique_lock<std::mutex> l_lock(laser_mtx);
    laser_buffer.pop();
    return true;
}


Eigen::Matrix4d imu_to_cam = Eigen::Matrix4d::Identity();
Eigen::Matrix4d imu_to_velo = Eigen::Matrix4d::Identity();
Eigen::Matrix4d velo_to_cam = Eigen::Matrix4d::Identity();

void loadCalib_kitti(std::string path_calib){

// Definir variables para almacenar los valores
    std::string calib_time;
    Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
    Eigen::Vector3d T (0,0,0);

    // Abrir el archivo de texto
    std::ifstream file(path_calib);
    if (!file.is_open()) {
        std::cerr << "Error al abrir el archivo de calibracion camara velodyne." << std::endl;
    }

    // Leer el archivo línea por línea
    std::string line;
    while (std::getline(file, line)) {
        // Leer la línea como un stringstream
        std::istringstream iss(line);
        std::string key;

        // Leer la clave y los valores correspondientes
        iss >> key;
        if (key == "calib_time:") {
            iss >> calib_time;
        } else if (key == "R:") {
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    iss >> R(i, j);
                }
            }
        } else if (key == "T:") {
            for (int i = 0; i < 3; ++i) {
                iss >> T(i);
            }
        }
    }

    // Cerrar el archivo
    file.close();

    // Imprimir los valores leídos
    //std::cout << "calib_time: " << calib_time << std::endl;
    std::cerr << "Matriz de calibracion Velodyne_camara." << std::endl;
    std::cout << "R:" << std::endl << R << std::endl;
    std::cout << "T:" << std::endl << T.transpose() << std::endl;

    // Convertir la matriz de rotación a un cuaternión
    Eigen::Quaterniond q(R);
    velo_to_cam.block<3, 3>(0, 0) = q.toRotationMatrix();
    Eigen::Vector3d T2 (0,0,0);
    velo_to_cam.block<3, 1>(0, 3) = T2;
}
void tfCallback(const tf2_msgs::TFMessage::ConstPtr& msg) {
    // Iterar sobre todas las transformaciones en el mensaje TF
    for (const auto& transform : msg->transforms) {


        if(transform.child_frame_id == "camera_color_left"){

            Eigen::Quaterniond q;

            q.x()=transform.transform.rotation.x;
            q.y()=transform.transform.rotation.y;
            q.z()=transform.transform.rotation.z;            
            q.w()=transform.transform.rotation.w;

            Eigen::Vector3d t;            

            t.x() = transform.transform.translation.x;
            t.y() = transform.transform.translation.y;
            t.z() = transform.transform.translation.z;     


            imu_to_cam.block<3, 3>(0, 0) = q.toRotationMatrix();;
            imu_to_cam.block<3, 1>(0, 3) = t;


        }

        if(transform.child_frame_id == "velo_link"){

            Eigen::Quaterniond q;

            q.x()=transform.transform.rotation.x;
            q.y()=transform.transform.rotation.y;
            q.z()=transform.transform.rotation.z;
            q.w()=transform.transform.rotation.w;

            Eigen::Vector3d t;            

            t.x() = transform.transform.translation.x;
            t.y() = transform.transform.translation.y;
            t.z() = transform.transform.translation.z;

            imu_to_velo.block<3, 3>(0, 0) = q.toRotationMatrix();;
            imu_to_velo.block<3, 1>(0, 3) = t;


            }
    }
}

void STD_matching(std::vector<STDesc> stds_curr, std::deque<STDesc>  std_local_map,
                  std::vector<STDesc> stdC_pair, std::vector<STDesc> stdM_pair,
                  std::unique_ptr<nanoflann::KDTreeEigenMatrixAdaptor<Eigen::MatrixXf>>& index, ros::Publisher pubSTD){

    int cont_desc_pairs= 0;
    int id = 0;
    visualization_msgs::MarkerArray marker_array;

    for (const auto& desc : stds_curr) {
        std::vector<float> query;
        Eigen::Vector3f side_length = desc.side_length_.cast<float>();
        Eigen::Vector3f angle = desc.angle_.cast<float>();
        Eigen::Vector3f center = desc.center_.cast<float>();
        Eigen::Vector3f vertex_A = desc.vertex_A_.cast<float>();
        Eigen::Vector3f vertex_B = desc.vertex_B_.cast<float>();
        Eigen::Vector3f vertex_C = desc.vertex_C_.cast<float>();
        Eigen::Vector3f norms1 = desc.normal1_.cast<float>();
        Eigen::Vector3f norms2 = desc.normal2_.cast<float>();
        Eigen::Vector3f norms3 = desc.normal3_.cast<float>();
        Eigen::Matrix3d axes_f = desc.calculateReferenceFrame();


        query.insert(query.end(), side_length.data(), side_length.data() + 3);
        query.insert(query.end(), angle.data(), angle.data() + 3);
        query.insert(query.end(), center.data(), center.data() + 3);
        query.insert(query.end(), vertex_A.data(), vertex_A.data() + 3);
        query.insert(query.end(), vertex_B.data(), vertex_B.data() + 3);
        query.insert(query.end(), vertex_C.data(), vertex_C.data() + 3);
        query.insert(query.end(), norms1.data(), norms1.data() + 3);
        query.insert(query.end(), norms2.data(), norms2.data() + 3);
        query.insert(query.end(), norms3.data(), norms3.data() + 3);
        query.insert(query.end(), axes_f.data(), axes_f.data() + axes_f.size());

        // Buscar el descriptor más cercano
        const size_t num_results = 1;
        std::vector<size_t> ret_indexes(num_results);
        std::vector<float> out_dists_sqr(num_results);

        nanoflann::KNNResultSet<float> resultSet(num_results);
        resultSet.init(&ret_indexes[0], &out_dists_sqr[0]);
        index->index_->findNeighbors(resultSet, query.data());

        for (size_t i = 0; i < resultSet.size(); i++) {
            
            if (ret_indexes[i] < std_local_map.size() && out_dists_sqr[i] < config_setting.kdtree_threshold_) {
                cont_desc_pairs++;
                generateArrow(desc, std_local_map[ret_indexes[i]], marker_array, id, msg_point->header);

                stdM_pair.push_back(std_local_map[ret_indexes[i]]);
                stdC_pair.push_back(desc);
                
            }

        }        
    }
    //Number of matchs
    std::cout<<"Number of STD matchs: "<<cont_desc_pairs<<std::endl;

    // Publicar las flechas en RViz
    pubSTD.publish(marker_array);
    visualization_msgs::Marker delete_marker_curr;
    delete_marker_curr.action = visualization_msgs::Marker::DELETEALL;
    marker_array.markers.clear();
    marker_array.markers.push_back(delete_marker_curr);
    pubSTD.publish(marker_array);
}


bool is_odom_inited = false;
double total_time =0, cropBox_len, surf_limit;
int total_frame=0;
bool clear_map;

void odom_estimation(){

    float time_delay  = 0;

    Eigen::Quaterniond q_diff;
    Eigen::Vector3d t_diff;
    Eigen::Isometry3d odom_prev = Eigen::Isometry3d::Identity();

    //////////// STD descriptor inicialization ///////////////////////////////////////////////////////
    std::vector<STDesc> stds_curr;
    std::deque<STDesc> std_local_map;
    std::vector<STDesc> stdC_pair;
    std::vector<STDesc> stdM_pair;

    pcSTD::Ptr current_cloud(new pcSTD); // pointcloud of the original sensor. It is used to std extractor
    pcSTD::Ptr current_cloud_world(new pcSTD); // pointcloud of the original sensor. It is used to std extractor
    Eigen::MatrixXf mat(0, 36);
    std::unique_ptr<nanoflann::KDTreeEigenMatrixAdaptor<Eigen::MatrixXf>> index;
    Eigen::Affine3d poseSTD = Eigen::Affine3d::Identity();
    std::deque<int> counts_per_iteration; // use for the cropping data of std_local_map

    //////////////////////////////////////////////////////////////////////////////////////////////////

    // path to save the trajectory/////////////////////
    std::ofstream outputFile(path_odom);

    size_t found = path_odom.find_last_of(".");
    std::string orig_path = path_odom.substr(0, found);
    orig_path += "_time.txt";
    std::ofstream org_outputFile(orig_path);    
    ///////////////////////////////////////////////////

    ////////////Saveing data initialization

    outputFile << std::scientific;
    outputFile <<  1.0 <<" "
               <<  0.0 <<" "
               <<  0.0 <<" "
               <<  0.0 <<" "
               <<  0.0 <<" "
               <<  1.0 <<" "
               <<  0.0 <<" "
               <<  0.0 <<" "
               <<  0.0 <<" "
               <<  0.0 <<" "
               <<  1.0 <<" "
               <<  0.0 << std::endl; 

   
    Eigen::Isometry3d odom = Eigen::Isometry3d::Identity();

    while(1){

        // STD descriptors (current and map matching)
        std::vector<STDesc> stds_curr_pair;
        std::vector<STDesc> stds_map_pair;

        if(!pointCloudEdgeBuf.empty() && !pointCloudSurfBuf.empty()){

            mutex_lock.lock();

            pcl::PointCloud<pcl::PointXYZ>::Ptr pointcloud_surf_in(new pcl::PointCloud<pcl::PointXYZ>());
            pcl::PointCloud<pcl::PointXYZ>::Ptr pointcloud_edge_in(new pcl::PointCloud<pcl::PointXYZ>());  
            pcSTD::Ptr pointcloud_ds(new pcSTD);

            pcl::fromROSMsg(*pointCloudEdgeBuf.front(), *pointcloud_edge_in);
            pcl::fromROSMsg(*pointCloudSurfBuf.front(), *pointcloud_surf_in);

            ros::Time pointcloud_time = (pointCloudSurfBuf.front())->header.stamp;
            pointCloudEdgeBuf.pop();
            pointCloudSurfBuf.pop();

            ////////////// read original pc to extract STD 
            if (readPC(current_cloud)){
                down_sampling_voxel(*current_cloud, config_setting.ds_size_); 
                
            }

            mutex_lock.unlock();

            //////////////////////////////// STD extractor
            poseSTD = odom;
            pcl::transformPointCloud(*current_cloud, *current_cloud_world, poseSTD);
            std_manager->GenerateSTDescs(current_cloud_world, stds_curr);
            ////////////////////////////////////////////////////////////////////////////

            if(is_odom_inited == false){
                // extract std for the initial frame K=0
                std_local_map.insert(std_local_map.end(), stds_curr.begin(), stds_curr.end());

                odomEstimation.initMapWithPoints(pointcloud_edge_in, pointcloud_surf_in);
                is_odom_inited = true;
                ROS_INFO("odom inited");

            }else{
                std::chrono::time_point<std::chrono::system_clock> start, end;
                start = std::chrono::system_clock::now();

                ////////////////////////////////////////////// STD matching
                STD_matching(stds_curr, std_local_map, stdC_pair, stdM_pair, index, pubSTD);
                //////////////////////////////////////////////////////////////////////////////////

                odomEstimation.updatePointsToMap(pointcloud_edge_in, pointcloud_surf_in, stdC_pair, stdM_pair, clear_map, cropBox_len);
                end = std::chrono::system_clock::now();
                std::chrono::duration<float> elapsed_seconds = end - start;
                total_frame++;
                float time_temp = elapsed_seconds.count() * 1000.0;
                total_time+=time_temp;
                time_delay = total_time/total_frame;
                ROS_INFO("average odom estimation time %f mS", time_delay);
                time_delay = time_delay/1000.0;

            }


            Eigen::Quaterniond q_current(odomEstimation.odom.rotation());
            Eigen::Vector3d t_current = odomEstimation.odom.translation();

            //////////// save data/////////////////////////////////////////////////
            if (save_data){

                Eigen::Matrix4d homogeneous_matrix = Eigen::Matrix4d::Identity();
                homogeneous_matrix.block<3, 3>(0, 0) = q_current.normalized().toRotationMatrix();
                homogeneous_matrix.block<3, 1>(0, 3) = t_current;    

                outputFile << std::scientific;

                outputFile <<   homogeneous_matrix(1,1) <<" "
                           <<   homogeneous_matrix(1,2) <<" "
                           <<  -homogeneous_matrix(1,0) <<" "
                           <<  -homogeneous_matrix(1,3) <<" "
                           <<   homogeneous_matrix(2,1) <<" "
                           <<   homogeneous_matrix(2,2) <<" "
                           <<  -homogeneous_matrix(2,0) <<" "
                           <<  -homogeneous_matrix(2,3) <<" "
                           <<  -homogeneous_matrix(0,1) <<" "
                           <<  -homogeneous_matrix(0,2) <<" "
                           <<   homogeneous_matrix(0,0) <<" "
                           <<   homogeneous_matrix(0,3) << std::endl;  
                                    
                org_outputFile << time_delay <<", ";
            }

            ///////////////////////////////////////////////////////
            // Project to 2D!!!
            // t_current.z() = 0.0;
            // double siny_cosp = 2 * (q_current.w() * q_current.z() + q_current.x() * q_current.y());
            // double cosy_cosp = 1 - 2 * (q_current.y() * q_current.y() + q_current.z() * q_current.z());
            // double yaw = std::atan2(siny_cosp, cosy_cosp);
            // Eigen::AngleAxisd yaw_angle(yaw, Eigen::Vector3d::UnitZ());
            // q_current = yaw_angle;
            ///////////////////////////////////////////////////////
            ///////////////////////////////////////////////////////

            /////kitti

            static tf::TransformBroadcaster br;
            tf::Transform transform;
            transform.setOrigin( tf::Vector3(t_current.x(), t_current.y(), t_current.z()) );
            tf::Quaternion q(q_current.x(),q_current.y(),q_current.z(),q_current.w());
            transform.setRotation(q);
            br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "odom", childframeID));      

                // //// TODO: FROM URDF!!!!!!!!
            // transform.setOrigin( tf::Vector3(-0.55, 0.0, -0.645) );
            // tf::Quaternion q2(0.0, 0.0, 0.0, 1.0);
            // transform.setRotation(q2);
            // br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), childframeID, "base_link"));        
            
            odom.linear() = q_current.toRotationMatrix();
            odom.translation() = t_current;

            Eigen::Isometry3d odomdiff = (odom_prev.inverse() * odom);
            q_diff = Eigen::Quaterniond(odomdiff.rotation());
            t_diff = odomdiff.translation();
           
            // Eigen::Isometry3d odom_curr = odom_prev * odomdiff;
            // Eigen::Quaterniond q_c = Eigen::Quaterniond(odom_curr.rotation());;
            // Eigen::Vector3d t_c = odom_curr.translation();            

            odom_prev = odom;


            ////////////////////////////// update STD map //////////////////////////////////////
            Eigen::Affine3d pose_estimated = odom;
            pcl::transformPointCloud(*current_cloud, *current_cloud_world, pose_estimated);
            std_manager->GenerateSTDescs(current_cloud_world, stds_curr);

            std_local_map.insert(std_local_map.end(), stds_curr.begin(), stds_curr.end());

                    ///////////////////// cropping elements per window in std_local_map ///////////////
            counts_per_iteration.push_back(stds_curr.size());
            while (counts_per_iteration.size() > config_setting.max_window_size_) {
                int count_to_remove = counts_per_iteration.front();
                counts_per_iteration.pop_front();
                for (int i = 0; i < count_to_remove; ++i) {
                    std_local_map.pop_front();                    
                }
            }

            // update mat matrix with filtering elements. it's necesary for the kdtree matching
            updateMatrixAndKDTreeWithFiltering(mat, index, std_local_map, config_setting);

            //////////////////////////////////////////////////////////////////////////////

             
            // publish odometry
            nav_msgs::Odometry laserOdometry;
            laserOdometry.header.frame_id = "odom";
            laserOdometry.child_frame_id = childframeID;
            laserOdometry.header.stamp = pointcloud_time;
            laserOdometry.pose.pose.orientation.x = q_current.x();
            laserOdometry.pose.pose.orientation.y = q_current.y();
            laserOdometry.pose.pose.orientation.z = q_current.z();
            laserOdometry.pose.pose.orientation.w = q_current.w();
            laserOdometry.pose.pose.position.x = t_current.x();
            laserOdometry.pose.pose.position.y = t_current.y();
            laserOdometry.pose.pose.position.z = t_current.z();

            nav_msgs::Odometry odomDiff;
            odomDiff.header.frame_id = "odom";
            odomDiff.child_frame_id = childframeID;
            odomDiff.header.stamp = pointcloud_time;
            odomDiff.pose.pose.orientation.x = q_diff.x();
            odomDiff.pose.pose.orientation.y = q_diff.y();
            odomDiff.pose.pose.orientation.z = q_diff.z();
            odomDiff.pose.pose.orientation.w = q_diff.w();
            odomDiff.pose.pose.position.x = t_diff.x();
            odomDiff.pose.pose.position.y = t_diff.y();
            odomDiff.pose.pose.position.z = t_diff.z();



            for(int i = 0; i<36; i++) {
              if(i == 0 || i == 7 || i == 14) {
                laserOdometry.pose.covariance[i] = .01;
               }
               else if (i == 21 || i == 28 || i== 35) {
                 laserOdometry.pose.covariance[i] += 0.1;
               }
               else {
                 laserOdometry.pose.covariance[i] = 0;
               }
            }

            pubLaserOdometry.publish(laserOdometry);
            pubOdometryDiff.publish(odomDiff);

            //publish time

            std_msgs::Float64 time_msg;
            time_msg.data = time_delay*1000.0;
            time_average.publish(time_msg);

         }

        std::chrono::milliseconds dura(2);
        std::this_thread::sleep_for(dura);
    }
    if (save_data)
        outputFile.close();
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "main");
    ros::NodeHandle nh;

    /////////////// Read confing of STD
    read_parameters(nh, config_setting);
    std_manager = new STDescManager(config_setting);
 
    double max_dis = 60.0;
    double min_dis =0;
    double edge_resolution = 0.3;
    double surf_resolution = 0.6;

    clear_map = true;
    cropBox_len = 10000;

    nh.getParam("/max_dis", max_dis);
    nh.getParam("/min_dis", min_dis);
    nh.getParam("/edge_resolution", edge_resolution);
    nh.getParam("/surf_resolution", surf_resolution); 
    nh.getParam("/clear_map", clear_map);
    nh.getParam("/save_data", save_data);    
    nh.getParam("/cropBox_len", cropBox_len);
    nh.getParam("/childframeID",childframeID);
    nh.getParam("/pcl_edge",edge_pcl);
    nh.getParam("/pcl_surf",surf_pcl);
    nh.getParam("/pcTopic",pcTopic);
    
        
    nh.getParam("/path_odom",path_odom);    
    nh.getParam("/path_calib",path_calib);

    loadCalib_kitti(path_calib); // cargar los datos de calibracion de Kitti
    


    odomEstimation.init(edge_resolution, surf_resolution);
    
    ros::Subscriber subEdgeLaserCloud = nh.subscribe<sensor_msgs::PointCloud2>(edge_pcl, 100, velodyneEdgeHandler);
    ros::Subscriber subSurfLaserCloud = nh.subscribe<sensor_msgs::PointCloud2>(surf_pcl, 100, velodyneSurfHandler);
    ros::Subscriber subLaserCloud = nh.subscribe<sensor_msgs::PointCloud2>(pcTopic, 100, laserCloudHandler);


    pubLaserOdometry = nh.advertise<nav_msgs::Odometry>("/odom", 100);
    pubOdometryDiff = nh.advertise<nav_msgs::Odometry>("/odom_diff", 100);
    time_average = nh.advertise<std_msgs::Float64>("/time_average", 100);
    pose_pub_prev = nh.advertise<geometry_msgs::PoseArray>("std_prev_poses", 10);
    pose_pub_curr = nh.advertise<geometry_msgs::PoseArray>("std_curr_poses", 10);
    pubSTD = nh.advertise<visualization_msgs::MarkerArray>("pair_std", 10);


    std::thread odom_estimation_process{odom_estimation};

    ros::spin();

    return 0;
}

