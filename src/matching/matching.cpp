/*
 * @Description: 前端里程计算法
 * @Author: Ren Qian
 * @Date: 2020-02-04 18:53:06
 */
#include "lidar_localization/matching/matching.hpp"

#include <pcl/common/transforms.h>
#include <pcl/io/pcd_io.h>
#include "glog/logging.h"

#include "lidar_localization/global_defination/global_defination.h"
#include "lidar_localization/models/registration/ndt_registration.hpp"
#include "lidar_localization/models/cloud_filter/voxel_filter.hpp"
#include "lidar_localization/models/cloud_filter/no_filter.hpp"

namespace lidar_localization {
Matching::Matching()
    : global_map_ptr_(new CloudData::CLOUD()),
      local_map_ptr_(new CloudData::CLOUD()),
      current_scan_ptr_(new CloudData::CLOUD()) 
{
    
    InitWithConfig();

    InitGlobalMap();

    ResetLocalMap(0.0, 0.0, 0.0);
}

bool Matching::InitWithConfig() {
    std::string config_file_path = WORK_SPACE_PATH + "/config/matching/matching.yaml";
    YAML::Node config_node = YAML::LoadFile(config_file_path);

    LOG(INFO) << std::endl
              << "-----------------Init Localization-------------------" 
              << std::endl;

    InitDataPath(config_node);

    InitScanContextManager(config_node);
    InitRegistration(registration_ptr_, config_node);

    // a. global map filter -- downsample point cloud map for visualization:
    InitFilter("global_map", global_map_filter_ptr_, config_node);
    // b. local map filter -- downsample & ROI filtering for scan-map matching:
    InitBoxFilter(config_node);
    InitFilter("local_map", local_map_filter_ptr_, config_node);
    // c. scan filter -- 
    InitFilter("frame", frame_filter_ptr_, config_node);

    return true;
}

bool Matching::InitDataPath(const YAML::Node& config_node) {
    map_path_ = config_node["map_path"].as<std::string>();

    return true;
}

bool Matching::InitScanContextManager(const YAML::Node& config_node) {
    // get loop closure config:
    loop_closure_method_ = config_node["loop_closure_method"].as<std::string>();

    // create instance:
    scan_context_manager_ptr_ = std::make_shared<ScanContextManager>(config_node[loop_closure_method_]);

    // load pre-built index:
    scan_context_path_ = config_node["scan_context_path"].as<std::string>();
    scan_context_manager_ptr_->Load(scan_context_path_);

    return true;
}

bool Matching::InitRegistration(std::shared_ptr<RegistrationInterface>& registration_ptr, const YAML::Node& config_node) {
    std::string registration_method = config_node["registration_method"].as<std::string>();
    std::cout << "\tPoint Cloud Registration Method: " << registration_method << std::endl;

    if (registration_method == "NDT") {
        registration_ptr = std::make_shared<NDTRegistration>(config_node[registration_method]);
    } else {
        LOG(ERROR) << "Registration method " << registration_method << " NOT FOUND!";
        return false;
    }

    return true;
}

bool Matching::InitFilter(std::string filter_user, std::shared_ptr<CloudFilterInterface>& filter_ptr, const YAML::Node& config_node) {
    std::string filter_mothod = config_node[filter_user + "_filter"].as<std::string>();
    std::cout << "\tFilter Method for " << filter_user << ": " << filter_mothod << std::endl;

    if (filter_mothod == "voxel_filter") {
        filter_ptr = std::make_shared<VoxelFilter>(config_node[filter_mothod][filter_user]);
    } else if (filter_mothod == "no_filter") {
        filter_ptr = std::make_shared<NoFilter>();
    } else {
        LOG(ERROR) << "Filter method " << filter_mothod << " for " << filter_user << " NOT FOUND!";
        return false;
    }

    return true;
}

bool Matching::InitBoxFilter(const YAML::Node& config_node) {
    box_filter_ptr_ = std::make_shared<BoxFilter>(config_node);
    return true;
}

bool Matching::InitGlobalMap() {
    pcl::io::loadPCDFile(map_path_, *global_map_ptr_);
    LOG(INFO) << "Load global map, size:" << global_map_ptr_->points.size();

    // since scan-map matching is used, here apply the same filter to local map & scan:
    local_map_filter_ptr_->Filter(global_map_ptr_, global_map_ptr_);
    LOG(INFO) << "Filtered global map, size:" << global_map_ptr_->points.size();

    has_new_global_map_ = true;

    return true;
}

bool Matching::ResetLocalMap(float x, float y, float z) {
    std::vector<float> origin = {x, y, z};

    // use ROI filtering for local map segmentation:
    box_filter_ptr_->SetOrigin(origin);
    box_filter_ptr_->Filter(global_map_ptr_, local_map_ptr_);

    registration_ptr_->SetInputTarget(local_map_ptr_);

    has_new_local_map_ = true;

    std::vector<float> edge = box_filter_ptr_->GetEdge();
    LOG(INFO) << "New local map:" << edge.at(0) << ","
                                  << edge.at(1) << ","
                                  << edge.at(2) << ","
                                  << edge.at(3) << ","
                                  << edge.at(4) << ","
                                  << edge.at(5) << std::endl << std::endl;

    return true;
}

bool Matching::Update(const CloudData& cloud_data, Eigen::Matrix4f& cloud_pose) {
    static Eigen::Matrix4f step_pose = Eigen::Matrix4f::Identity();
    static Eigen::Matrix4f last_pose = init_pose_;
    static Eigen::Matrix4f predict_pose = init_pose_;

    // remove invalid measurements:
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*cloud_data.cloud_ptr, *cloud_data.cloud_ptr, indices);

    // downsample:
    CloudData::CLOUD_PTR filtered_cloud_ptr(new CloudData::CLOUD());
    frame_filter_ptr_->Filter(cloud_data.cloud_ptr, filtered_cloud_ptr);

    if (!has_inited_) {
        predict_pose = current_gnss_pose_;
    }

    // matching:
    CloudData::CLOUD_PTR result_cloud_ptr(new CloudData::CLOUD());
    registration_ptr_->ScanMatch(filtered_cloud_ptr, predict_pose, result_cloud_ptr, cloud_pose);
    pcl::transformPointCloud(*cloud_data.cloud_ptr, *current_scan_ptr_, cloud_pose);

    // update predicted pose:
    step_pose = last_pose.inverse() * cloud_pose;
    predict_pose = cloud_pose * step_pose;
    last_pose = cloud_pose;

    // 匹配之后判断是否需要更新局部地图
    std::vector<float> edge = box_filter_ptr_->GetEdge();
    for (int i = 0; i < 3; i++) {
        if (
            fabs(cloud_pose(i, 3) - edge.at(2 * i)) > 50.0 &&
            fabs(cloud_pose(i, 3) - edge.at(2 * i + 1)) > 50.0
        ) {
            continue;
        }
            
        ResetLocalMap(cloud_pose(0,3), cloud_pose(1,3), cloud_pose(2,3));
        break;
    }

    return true;
}

// TODO: understand this function
bool Matching::SetGNSSPose(const Eigen::Matrix4f& gnss_pose) {
    static int gnss_cnt = 0;

    current_gnss_pose_ = gnss_pose;
    LOG(INFO) << std::endl << "init pose is: " << current_gnss_pose_ << std::endl;

    if ( gnss_cnt == 0 ) {
        SetInitPose(gnss_pose);
    } else if (gnss_cnt > 3) {
        has_inited_ = true;
    }

    gnss_cnt++;

    return true;
}

/**
 * @brief  get init pose using scan context matching
 * @param  init_scan, init key scan
 * @return true if success otherwise false
 */
// TODO: understand this function
bool Matching::SetScanContextPose(const CloudData& init_scan) {
    // get init pose proposal using scan context match:
    Eigen::Matrix4f init_pose =  Eigen::Matrix4f::Identity();

    if (
        !scan_context_manager_ptr_->DetectLoopClosure(init_scan, init_pose)
    ) {
        return false;
    }

    // set init pose:
    SetInitPose(init_pose);
    has_inited_ = true;
    LOG(INFO) << std::endl << "init pose is: " << init_pose << std::endl;


        return true;
}

// TODO: understand this function
bool Matching::SetInitPose(const Eigen::Matrix4f& init_pose) {
    init_pose_ = init_pose;
    ResetLocalMap(init_pose(0,3), init_pose(1,3), init_pose(2,3));

    return true;
}

bool Matching::SetInited(void) {
    has_inited_ = true;

    return true;
}

Eigen::Matrix4f Matching::GetInitPose(void) {
    return init_pose_;
}

void Matching::GetGlobalMap(CloudData::CLOUD_PTR& global_map) {
    // downsample global map for visualization:
    global_map_filter_ptr_->Filter(global_map_ptr_, global_map);

    has_new_global_map_ = false;
}

CloudData::CLOUD_PTR& Matching::GetLocalMap() {
    return local_map_ptr_;
}

CloudData::CLOUD_PTR& Matching::GetCurrentScan() {
    return current_scan_ptr_;
}

bool Matching::HasInited() {
    return has_inited_;
}

bool Matching::HasNewGlobalMap() {
    return has_new_global_map_;
}

bool Matching::HasNewLocalMap() {
    return has_new_local_map_;
}
}