// Copyright 2018 Margarita Grinvald, ASL, ETH Zurich, Switzerland

#include "voxblox_gsm/controller.h"

#include <stdlib.h>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <global_segment_map/label_voxel.h>
#include <global_segment_map/utils/file_utils.h>
#include <global_segment_map/utils/label_utils.h>
#include <glog/logging.h>
#include <minkindr_conversions/kindr_tf.h>

#include <voxblox/core/common.h>
#include <voxblox/integrator/merge_integration.h>
#include <voxblox/utils/layer_utils.h>
#include <voxblox_ros/conversions.h>
#include <voxblox_ros/mesh_vis.h>

#include "voxblox_gsm/conversions.h"

namespace voxblox {
namespace voxblox_gsm {

std::string classes[81] = {"BG",
                           "person",
                           "bicycle",
                           "car",
                           "motorcycle",
                           "airplane",
                           "bus",
                           "train",
                           "truck",
                           "boat",
                           "traffic light",
                           "fire hydrant",
                           "stop sign",
                           "parking meter",
                           "bench",
                           "bird",
                           "cat",
                           "dog",
                           "horse",
                           "sheep",
                           "cow",
                           "elephant",
                           "bear",
                           "zebra",
                           "giraffe",
                           "backpack",
                           "umbrella",
                           "handbag",
                           "tie",
                           "suitcase",
                           "frisbee",
                           "skis",
                           "snowboard",
                           "sports ball",
                           "kite",
                           "baseball bat",
                           "baseball glove",
                           "skateboard",
                           "surfboard",
                           "tennis racket",
                           "bottle",
                           "wine glass",
                           "cup",
                           "fork",
                           "knife",
                           "spoon",
                           "bowl",
                           "banana",
                           "apple",
                           "sandwich",
                           "orange",
                           "broccoli",
                           "carrot",
                           "hot dog",
                           "pizza",
                           "donut",
                           "cake",
                           "chair",
                           "couch",
                           "potted plant",
                           "bed",
                           "dining table",
                           "toilet",
                           "tv",
                           "laptop",
                           "mouse",
                           "remote",
                           "keyboard",
                           "cell phone",
                           "microwave",
                           "oven",
                           "toaster",
                           "sink",
                           "refrigerator",
                           "book",
                           "clock",
                           "vase",
                           "scissors",
                           "teddy bear",
                           "hair drier",
                           "toothbrush"};

Controller::Controller(ros::NodeHandle* node_handle_private)
    : node_handle_private_(node_handle_private),
      // Increased time limit for lookup in the past of tf messages
      // to give some slack to the pipeline and not lose any messages.
      integrated_frames_count_(0u),
      tf_listener_(ros::Duration(1000)),
      world_frame_("world"),
      camera_frame_(""),
      no_update_timeout_(0.0),
      publish_gsm_updates_(false),
      publish_scene_mesh_(false),
      received_first_message_(false),
      updated_mesh_(false),
      need_full_remesh_(false) {
  CHECK_NOTNULL(node_handle_private_);
  node_handle_private_->param<std::string>("world_frame_id", camera_frame_,
                                           world_frame_);
  node_handle_private_->param<std::string>("camera_frame_id", camera_frame_,
                                           camera_frame_);

  // Workaround for OS X on mac mini not having specializations for float
  // for some reason.
  int voxels_per_side = map_config_.voxels_per_side;
  node_handle_private_->param<FloatingPoint>(
      "voxblox/voxel_size", map_config_.voxel_size, map_config_.voxel_size);
  node_handle_private_->param<int>("voxblox/voxels_per_side", voxels_per_side,
                                   voxels_per_side);
  if (!isPowerOfTwo(voxels_per_side)) {
    ROS_ERROR("voxels_per_side must be a power of 2, setting to default value");
    voxels_per_side = map_config_.voxels_per_side;
  }
  map_config_.voxels_per_side = voxels_per_side;

  map_.reset(new LabelTsdfMap(map_config_));

  // Determine TSDF integrator parameters.
  tsdf_integrator_config_.voxel_carving_enabled = false;
  tsdf_integrator_config_.allow_clear = true;
  FloatingPoint truncation_distance_factor = 5.0f;
  tsdf_integrator_config_.max_ray_length_m = 2.5f;

  node_handle_private_->param<bool>(
      "voxblox/voxel_carving_enabled",
      tsdf_integrator_config_.voxel_carving_enabled,
      tsdf_integrator_config_.voxel_carving_enabled);
  node_handle_private_->param<bool>("voxblox/allow_clear",
                                    tsdf_integrator_config_.allow_clear,
                                    tsdf_integrator_config_.allow_clear);
  node_handle_private_->param<FloatingPoint>(
      "voxblox/truncation_distance_factor", truncation_distance_factor,
      truncation_distance_factor);
  node_handle_private_->param<FloatingPoint>(
      "voxblox/min_ray_length_m", tsdf_integrator_config_.min_ray_length_m,
      tsdf_integrator_config_.min_ray_length_m);
  node_handle_private_->param<FloatingPoint>(
      "voxblox/max_ray_length_m", tsdf_integrator_config_.max_ray_length_m,
      tsdf_integrator_config_.max_ray_length_m);

  tsdf_integrator_config_.default_truncation_distance =
      map_config_.voxel_size * truncation_distance_factor;

  std::string method("merged");
  node_handle_private_->param<std::string>("method", method, method);
  if (method.compare("merged") == 0) {
    tsdf_integrator_config_.enable_anti_grazing = false;
  } else if (method.compare("merged_discard") == 0) {
    tsdf_integrator_config_.enable_anti_grazing = true;
  } else {
    tsdf_integrator_config_.enable_anti_grazing = false;
  }

  std::string mesh_color_scheme("label");
  node_handle_private_->param<std::string>(
      "meshing/mesh_color_scheme", mesh_color_scheme, mesh_color_scheme);
  if (mesh_color_scheme.compare("label") == 0) {
    mesh_color_scheme_ = MeshLabelIntegrator::LabelColor;
  } else if (mesh_color_scheme.compare("semantic_label") == 0) {
    mesh_color_scheme_ = MeshLabelIntegrator::SemanticColor;
  } else if (mesh_color_scheme.compare("instance_label") == 0) {
    mesh_color_scheme_ = MeshLabelIntegrator::InstanceColor;
  } else if (mesh_color_scheme.compare("geometric_instance_label") == 0) {
    mesh_color_scheme_ = MeshLabelIntegrator::GeometricInstanceColor;
  } else if (mesh_color_scheme.compare("confidence") == 0) {
    mesh_color_scheme_ = MeshLabelIntegrator::ConfidenceColor;
  } else {
    mesh_color_scheme_ = MeshLabelIntegrator::LabelColor;
  }

  // Determine label integrator parameters.
  node_handle_private_->param<bool>(
      "pairwise_confidence_merging/enable_pairwise_confidence_merging",
      label_tsdf_integrator_config_.enable_pairwise_confidence_merging,
      label_tsdf_integrator_config_.enable_pairwise_confidence_merging);
  node_handle_private_->param<FloatingPoint>(
      "pairwise_confidence_merging/merging_min_overlap_ratio",
      label_tsdf_integrator_config_.merging_min_overlap_ratio,
      label_tsdf_integrator_config_.merging_min_overlap_ratio);
  node_handle_private_->param<int>(
      "pairwise_confidence_merging/merging_min_frame_count",
      label_tsdf_integrator_config_.merging_min_frame_count,
      label_tsdf_integrator_config_.merging_min_frame_count);

  node_handle_private_->param<bool>(
      "semantic_instance_segmentation/enable_semantic_instance_segmentation",
      label_tsdf_integrator_config_.enable_semantic_instance_segmentation,
      label_tsdf_integrator_config_.enable_semantic_instance_segmentation);

  node_handle_private_->param<int>(
      "object_database/max_segment_age",
      label_tsdf_integrator_config_.max_segment_age,
      label_tsdf_integrator_config_.max_segment_age);

  integrator_.reset(new LabelTsdfIntegrator(
      tsdf_integrator_config_, label_tsdf_integrator_config_,
      map_->getTsdfLayerPtr(), map_->getLabelLayerPtr(),
      map_->getHighestLabelPtr(), map_->getHighestInstancePtr()));

  mesh_label_layer_.reset(new MeshLayer(map_->block_size()));

  mesh_label_integrator_.reset(new MeshLabelIntegrator(
      mesh_config_, map_->getTsdfLayerPtr(), map_->getLabelLayerPtr(),
      mesh_label_layer_.get(), all_semantic_labels_,
      integrator_->getInstanceLabelFusionPtr(),
      integrator_->getSemanticLabelFusionPtr(), MeshLabelIntegrator::LabelColor,
      &need_full_remesh_));

  if (label_tsdf_integrator_config_.enable_semantic_instance_segmentation) {
    mesh_semantic_layer_.reset(new MeshLayer(map_->block_size()));
    mesh_instance_layer_.reset(new MeshLayer(map_->block_size()));
    mesh_merged_layer_.reset(new MeshLayer(map_->block_size()));

    mesh_semantic_integrator_.reset(new MeshLabelIntegrator(
        mesh_config_, map_->getTsdfLayerPtr(), map_->getLabelLayerPtr(),
        mesh_semantic_layer_.get(), all_semantic_labels_,
        integrator_->getInstanceLabelFusionPtr(),
        integrator_->getSemanticLabelFusionPtr(),
        MeshLabelIntegrator::SemanticColor, &need_full_remesh_));
    mesh_instance_integrator_.reset(new MeshLabelIntegrator(
        mesh_config_, map_->getTsdfLayerPtr(), map_->getLabelLayerPtr(),
        mesh_instance_layer_.get(), all_semantic_labels_,
        integrator_->getInstanceLabelFusionPtr(),
        integrator_->getSemanticLabelFusionPtr(),
        MeshLabelIntegrator::InstanceColor, &need_full_remesh_));
    mesh_merged_integrator_.reset(new MeshLabelIntegrator(
        mesh_config_, map_->getTsdfLayerPtr(), map_->getLabelLayerPtr(),
        mesh_merged_layer_.get(), all_semantic_labels_,
        integrator_->getInstanceLabelFusionPtr(),
        integrator_->getSemanticLabelFusionPtr(),
        MeshLabelIntegrator::GeometricInstanceColor, &need_full_remesh_));
  }

  // Visualization settings.
  bool visualize = false;
  node_handle_private_->param<bool>("meshing/visualize", visualize, visualize);
  if (visualize) {
    std::vector<std::shared_ptr<MeshLayer>> mesh_layers;
    mesh_layers.push_back(mesh_label_layer_);

    if (label_tsdf_integrator_config_.enable_semantic_instance_segmentation) {
      mesh_layers.push_back(mesh_merged_layer_);
      mesh_layers.push_back(mesh_semantic_layer_);
      mesh_layers.push_back(mesh_instance_layer_);
    }
    visualizer_ =
        new Visualizer(mesh_layers, &updated_mesh_, &updated_mesh_mutex_);
    viz_thread_ = std::thread(&Visualizer::visualizeMesh, visualizer_);
  }

  node_handle_private_->param<bool>("meshing/publish_segment_mesh",
                                    publish_segment_mesh_,
                                    publish_segment_mesh_);
  node_handle_private_->param<bool>("meshing/publish_scene_mesh",
                                    publish_scene_mesh_, publish_scene_mesh_);

  // If set, use a timer to progressively update the mesh.
  double update_mesh_every_n_sec = 0.0;
  node_handle_private_->param<double>("meshing/update_mesh_every_n_sec",
                                      update_mesh_every_n_sec,
                                      update_mesh_every_n_sec);

  if (update_mesh_every_n_sec > 0.0) {
    update_mesh_timer_ = node_handle_private_->createTimer(
        ros::Duration(update_mesh_every_n_sec), &Controller::updateMeshEvent,
        this);
  }

  node_handle_private_->param<std::string>("meshing/mesh_filename",
                                           mesh_filename_, mesh_filename_);

  node_handle_private_->param<bool>("object_database/publish_gsm_updates",
                                    publish_gsm_updates_, publish_gsm_updates_);

  node_handle_private_->param<double>("object_database/no_update_timeout",
                                      no_update_timeout_, no_update_timeout_);
}

Controller::~Controller() { viz_thread_.join(); }

void Controller::subscribeSegmentPointCloudTopic(
    ros::Subscriber* segment_point_cloud_sub) {
  CHECK_NOTNULL(segment_point_cloud_sub);
  std::string segment_point_cloud_topic =
      "/depth_segmentation_node/object_segment";
  node_handle_private_->param<std::string>("segment_point_cloud_topic",
                                           segment_point_cloud_topic,
                                           segment_point_cloud_topic);
  // TODO (margaritaG): make this a param once segments of a frame are
  // refactored to be received as one single message.
  // Large queue size to give slack to the
  // pipeline and not lose any messages.
  *segment_point_cloud_sub = node_handle_private_->subscribe(
      segment_point_cloud_topic, 6000, &Controller::segmentPointCloudCallback,
      this);
}

void Controller::advertiseSegmentGsmUpdateTopic(
    ros::Publisher* segment_gsm_update_pub) {
  CHECK_NOTNULL(segment_gsm_update_pub);
  std::string segment_gsm_update_topic = "gsm_update";
  node_handle_private_->param<std::string>("segment_gsm_update_topic",
                                           segment_gsm_update_topic,
                                           segment_gsm_update_topic);
  // TODO(ff): Reduce this value, once we know some reasonable limit.
  constexpr int kGsmUpdateQueueSize = 2000;

  *segment_gsm_update_pub =
      node_handle_private_->advertise<modelify_msgs::GsmUpdate>(
          segment_gsm_update_topic, kGsmUpdateQueueSize, true);
  segment_gsm_update_pub_ = segment_gsm_update_pub;
}

void Controller::advertiseSceneGsmUpdateTopic(
    ros::Publisher* scene_gsm_update_pub) {
  CHECK_NOTNULL(scene_gsm_update_pub);
  std::string scene_gsm_update_topic = "scene";
  node_handle_private_->param<std::string>(
      "scene_gsm_update_topic", scene_gsm_update_topic, scene_gsm_update_topic);
  constexpr int kGsmSceneQueueSize = 1;

  *scene_gsm_update_pub =
      node_handle_private_->advertise<modelify_msgs::GsmUpdate>(
          scene_gsm_update_topic, kGsmSceneQueueSize, true);
  scene_gsm_update_pub_ = scene_gsm_update_pub;
}

void Controller::advertiseSegmentMeshTopic(ros::Publisher* segment_mesh_pub) {
  CHECK_NOTNULL(segment_mesh_pub);
  *segment_mesh_pub = node_handle_private_->advertise<voxblox_msgs::Mesh>(
      "segment_mesh", 1, true);

  segment_mesh_pub_ = segment_mesh_pub;
}

void Controller::advertiseSceneMeshTopic(ros::Publisher* scene_mesh_pub) {
  CHECK_NOTNULL(scene_mesh_pub);
  *scene_mesh_pub =
      node_handle_private_->advertise<voxblox_msgs::Mesh>("mesh", 1, true);

  scene_mesh_pub_ = scene_mesh_pub;
}

void Controller::advertisePublishSceneService(
    ros::ServiceServer* publish_scene_srv) {
  CHECK_NOTNULL(publish_scene_srv);
  static const std::string kAdvertisePublishSceneServiceName = "publish_scene";
  *publish_scene_srv = node_handle_private_->advertiseService(
      kAdvertisePublishSceneServiceName, &Controller::publishSceneCallback,
      this);
}

void Controller::validateMergedObjectService(
    ros::ServiceServer* validate_merged_object_srv) {
  CHECK_NOTNULL(validate_merged_object_srv);
  static const std::string kValidateMergedObjectTopicRosParam =
      "validate_merged_object";
  std::string validate_merged_object_topic = "validate_merged_object";
  node_handle_private_->param<std::string>(kValidateMergedObjectTopicRosParam,
                                           validate_merged_object_topic,
                                           validate_merged_object_topic);

  *validate_merged_object_srv = node_handle_private_->advertiseService(
      validate_merged_object_topic, &Controller::validateMergedObjectCallback,
      this);
}

void Controller::advertiseGenerateMeshService(
    ros::ServiceServer* generate_mesh_srv) {
  CHECK_NOTNULL(generate_mesh_srv);
  *generate_mesh_srv = node_handle_private_->advertiseService(
      "generate_mesh", &Controller::generateMeshCallback, this);
}

void Controller::advertiseExtractSegmentsService(
    ros::ServiceServer* extract_segments_srv) {
  CHECK_NOTNULL(extract_segments_srv);
  *extract_segments_srv = node_handle_private_->advertiseService(
      // "extract_segments", &Controller::extractTSDFCallback, this);
      "extract_segments", &Controller::extractSegmentsCallback, this);
}

void Controller::segmentPointCloudCallback(
    const sensor_msgs::PointCloud2::Ptr& segment_point_cloud_msg) {
  // Message timestamps are used to detect when all
  // segment messages from a certain frame have arrived.
  // Since segments from the same frame all have the same timestamp,
  // the start of a new frame is detected when the message timestamp changes.
  // TODO(grinvalm): need additional check for the last frame to be
  // integrated.
  if (received_first_message_ &&
      last_segment_msg_timestamp_ != segment_point_cloud_msg->header.stamp) {
    ROS_INFO_STREAM("Timings: " << std::endl << timing::Timing::Print());
    timing::Timer ptcloud_timer("label_propagation");

    ROS_INFO("Integrating frame n.%zu, timestamp of frame: %f",
             ++integrated_frames_count_,
             segment_point_cloud_msg->header.stamp.toSec());
    ros::WallTime start = ros::WallTime::now();

    integrator_->decideLabelPointClouds(&segments_to_integrate_,
                                        &segment_label_candidates,
                                        &segment_merge_candidates_);
    ptcloud_timer.Stop();

    ros::WallTime end = ros::WallTime::now();
    ROS_INFO("Decided labels for %lu pointclouds in %f seconds.",
             segments_to_integrate_.size(), (end - start).toSec());

    constexpr bool kIsFreespacePointcloud = false;

    start = ros::WallTime::now();
    {
      timing::Timer integrate_timer("integrate_frame_pointclouds");
      std::lock_guard<std::mutex> updatedMeshLock(updated_mesh_mutex_);
      for (const auto& segment : segments_to_integrate_) {
        integrator_->integratePointCloud(segment->T_G_C_, segment->points_C_,
                                         segment->colors_, segment->label_,
                                         kIsFreespacePointcloud);
      }
      integrate_timer.Stop();
    }

    end = ros::WallTime::now();
    ROS_INFO(
        "Integrated %lu pointclouds in %f secs, have %lu tsdf "
        "and %lu label blocks.",
        segments_to_integrate_.size(), (end - start).toSec(),
        map_->getTsdfLayerPtr()->getNumberOfAllocatedBlocks(),
        map_->getLabelLayerPtr()->getNumberOfAllocatedBlocks());

    start = ros::WallTime::now();

    integrator_->mergeLabels(&merges_to_publish_);

    integrator_->getLabelsToPublish(&segment_labels_to_publish_);

    end = ros::WallTime::now();
    ROS_INFO(
        "Merged segments and fetched the ones to publish in %f "
        "seconds.",
        (end - start).toSec());
    start = ros::WallTime::now();

    segment_merge_candidates_.clear();
    segment_label_candidates.clear();
    for (Segment* segment : segments_to_integrate_) {
      delete segment;
    }
    segments_to_integrate_.clear();

    end = ros::WallTime::now();
    ROS_INFO("Cleared candidates and memory in %f seconds.",
             (end - start).toSec());

    // if (publish_gsm_updates_ && publishObjects()) {
    //   publishScene();
    // }
  }
  received_first_message_ = true;
  last_update_received_ = ros::Time::now();
  last_segment_msg_timestamp_ = segment_point_cloud_msg->header.stamp;

  // Look up transform from camera frame to world frame.
  Transformation T_G_C;
  std::string from_frame = camera_frame_;
  if (camera_frame_.empty()) {
    from_frame = segment_point_cloud_msg->header.frame_id;
  }
  if (lookupTransform(from_frame, world_frame_,
                      segment_point_cloud_msg->header.stamp, &T_G_C)) {
    // Convert the PCL pointcloud into voxblox format.
    // Horrible hack fix to fix color parsing colors in PCL.
    for (size_t d = 0; d < segment_point_cloud_msg->fields.size(); ++d) {
      if (segment_point_cloud_msg->fields[d].name == std::string("rgb")) {
        segment_point_cloud_msg->fields[d].datatype =
            sensor_msgs::PointField::FLOAT32;
      }
    }
    timing::Timer ptcloud_timer("ptcloud_preprocess");

    Segment* segment;
    if (label_tsdf_integrator_config_.enable_semantic_instance_segmentation) {
      pcl::PointCloud<voxblox::PointSemanticInstanceType>
          point_cloud_semantic_instance;
      pcl::fromROSMsg(*segment_point_cloud_msg, point_cloud_semantic_instance);
      segment = new Segment(point_cloud_semantic_instance, T_G_C);
    } else {
      pcl::PointCloud<voxblox::PointType> point_cloud;
      pcl::fromROSMsg(*segment_point_cloud_msg, point_cloud);
      segment = new Segment(point_cloud, T_G_C);
    }
    segments_to_integrate_.push_back(segment);

    ptcloud_timer.Stop();

    // ros::WallTime start = ros::WallTime::now();

    timing::Timer label_candidates_timer("compute_label_candidates");

    integrator_->computeSegmentLabelCandidates(
        segment, &segment_label_candidates, &segment_merge_candidates_);
    label_candidates_timer.Stop();
    //     "Comput
    // ros::WallTime end = ros::WallTime::now();
    // ROS_INFO(
    //     "Computed label candidates for a pointcloud of size %lu in %f "
    //     "seconds.",
    //     segment->points_C_.size(), (end - start).toSec());
  }
}

bool Controller::publishSceneCallback(std_srvs::SetBool::Request& request,
                                      std_srvs::SetBool::Response& response) {
  bool save_scene_mesh = request.data;
  if (save_scene_mesh) {
    constexpr bool kClearMesh = true;
    generateMesh(kClearMesh);
  }
  publishScene();
  constexpr bool kPublishAllSegments = true;
  publishObjects(kPublishAllSegments);
  return true;
}

bool Controller::validateMergedObjectCallback(
    modelify_msgs::ValidateMergedObject::Request& request,
    modelify_msgs::ValidateMergedObject::Response& response) {
  typedef TsdfVoxel TsdfVoxelType;
  typedef Layer<TsdfVoxelType> TsdfLayer;
  // TODO(ff): Do the following afterwards in modelify.
  // - Check if merged object agrees with whole map (at all poses).
  // - If it doesn't agree at all poses try the merging again with the
  // reduced set of objects.
  // - Optionally put the one that doesn't agree with
  // the others in a list not to merge with the other merged ones

  // Extract TSDF layer of merged object.
  std::shared_ptr<TsdfLayer> merged_object_layer_O;
  CHECK(deserializeMsgToLayer(request.gsm_update.object.tsdf_layer,
                              merged_object_layer_O.get()))
      << "Deserializing of TSDF layer from merged object message failed.";

  // Extract transformations.
  std::vector<Transformation> transforms_W_O;
  voxblox_gsm::transformMsgs2Transformations(
      request.gsm_update.object.transforms, &transforms_W_O);

  const utils::VoxelEvaluationMode voxel_evaluation_mode =
      utils::VoxelEvaluationMode::kEvaluateAllVoxels;

  std::vector<utils::VoxelEvaluationDetails> voxel_evaluation_details_vector;

  evaluateLayerRmseAtPoses<TsdfVoxelType>(
      voxel_evaluation_mode, map_->getTsdfLayer(),
      *(merged_object_layer_O.get()), transforms_W_O,
      &voxel_evaluation_details_vector);

  voxblox_gsm::voxelEvaluationDetails2VoxelEvaluationDetailsMsg(
      voxel_evaluation_details_vector, &response.voxel_evaluation_details);
  return true;
}

bool Controller::generateMeshCallback(
    std_srvs::Empty::Request& request,
    std_srvs::Empty::Response& response) {  // NOLINT
  constexpr bool kClearMesh = true;
  generateMesh(kClearMesh);
  return true;
}

// bool Controller::extractTSDFCallback(std_srvs::Empty::Request& request,
//                                          std_srvs::Empty::Response&
//                                          response)
//                                          {
//
//                                          }

bool Controller::extractSegmentsCallback(std_srvs::Empty::Request& request,
                                         std_srvs::Empty::Response& response) {
  // Get list of all labels in the map.
  std::vector<Label> labels = integrator_->getLabelsList();
  static constexpr bool kConnectedMesh = false;

  std::unordered_map<Label, LayerPair> label_to_layers;
  // Extract the TSDF and label layers corresponding to a segment.
  constexpr bool kLabelsListIsComplete = true;
  extractSegmentLayers(labels, &label_to_layers, kLabelsListIsComplete);

  for (Label label : labels) {
    auto it = label_to_layers.find(label);
    CHECK(it != label_to_layers.end())
        << "Layers for label " << label << " could not be extracted.";

    const Layer<TsdfVoxel>& segment_tsdf_layer = it->second.first;
    const Layer<LabelVoxel>& segment_label_layer = it->second.second;

    voxblox::Mesh segment_mesh;
    if (convertLabelTsdfLayersToMesh(segment_tsdf_layer, segment_label_layer,
                                     &segment_mesh, kConnectedMesh)) {
      CHECK_EQ(voxblox::file_utils::makePath("gsm_segments", 0777), 0);

      std::string mesh_filename = "gsm_segments/gsm_segment_mesh_label_" +
                                  std::to_string(label) + ".ply";

      bool success = outputMeshAsPly(mesh_filename, segment_mesh);
      if (success) {
        ROS_INFO("Output segment file as PLY: %s", mesh_filename.c_str());
      } else {
        ROS_INFO("Failed to output mesh as PLY: %s", mesh_filename.c_str());
      }
    }
  }
  return true;
}

void Controller::extractSegmentLayers(
    const std::vector<Label>& labels,
    std::unordered_map<Label, LayerPair>* label_layers_map,
    bool labels_list_is_complete) {
  CHECK(label_layers_map);

  // Build map from labels to tsdf and label layers. Each will contain the
  // segment of the corresponding layer.
  Layer<TsdfVoxel> tsdf_layer_empty(map_config_.voxel_size,
                                    map_config_.voxels_per_side);
  Layer<LabelVoxel> label_layer_empty(map_config_.voxel_size,
                                      map_config_.voxels_per_side);
  for (const Label& label : labels) {
    label_layers_map->emplace(
        label, std::make_pair(tsdf_layer_empty, label_layer_empty));
  }

  BlockIndexList all_label_blocks;
  map_->getTsdfLayerPtr()->getAllAllocatedBlocks(&all_label_blocks);

  for (const BlockIndex& block_index : all_label_blocks) {
    Block<TsdfVoxel>::Ptr global_tsdf_block =
        map_->getTsdfLayerPtr()->getBlockPtrByIndex(block_index);
    Block<LabelVoxel>::Ptr global_label_block =
        map_->getLabelLayerPtr()->getBlockPtrByIndex(block_index);

    const size_t vps = global_label_block->voxels_per_side();
    for (size_t i = 0; i < vps * vps * vps; ++i) {
      const LabelVoxel& global_label_voxel =
          global_label_block->getVoxelByLinearIndex(i);

      if (global_label_voxel.label == 0u) {
        continue;
      }

      auto it = label_layers_map->find(global_label_voxel.label);
      if (it == label_layers_map->end()) {
        if (labels_list_is_complete) {
          LOG(FATAL) << "At least one voxel in the GSM is assigned to label "
                     << global_label_voxel.label
                     << " which is not in the given "
                        "list of labels to retrieve.";
        }
        continue;
      }

      Layer<TsdfVoxel>& tsdf_layer = it->second.first;
      Layer<LabelVoxel>& label_layer = it->second.second;

      Block<TsdfVoxel>::Ptr tsdf_block =
          tsdf_layer.allocateBlockPtrByIndex(block_index);
      Block<LabelVoxel>::Ptr label_block =
          label_layer.allocateBlockPtrByIndex(block_index);
      CHECK(tsdf_block);
      CHECK(label_block);

      TsdfVoxel& tsdf_voxel = tsdf_block->getVoxelByLinearIndex(i);
      LabelVoxel& label_voxel = label_block->getVoxelByLinearIndex(i);

      const TsdfVoxel& global_tsdf_voxel =
          global_tsdf_block->getVoxelByLinearIndex(i);

      tsdf_voxel = global_tsdf_voxel;
      label_voxel = global_label_voxel;
    }
  }
}

bool Controller::lookupTransform(const std::string& from_frame,
                                 const std::string& to_frame,
                                 const ros::Time& timestamp,
                                 Transformation* transform) {
  tf::StampedTransform tf_transform;
  ros::Time time_to_lookup = timestamp;

  // If this transform isn't possible at the time, then try to just look
  // up the latest (this is to work with bag files and static transform
  // publisher, etc).
  if (!tf_listener_.canTransform(to_frame, from_frame, time_to_lookup)) {
    time_to_lookup = ros::Time(0);
    LOG(ERROR) << "Using latest TF transform instead of timestamp match.";
    return false;
  }

  try {
    tf_listener_.lookupTransform(to_frame, from_frame, time_to_lookup,
                                 tf_transform);
  } catch (tf::TransformException& ex) {
    LOG(ERROR) << "Error getting TF transform from sensor data: " << ex.what();
    return false;
  }

  tf::transformTFToKindr(tf_transform, transform);
  return true;
}

// TODO(ff): Create this somewhere:
// void serializeGsmAsMsg(const map&, const label&, const parent_labels&,
// msg*);

bool Controller::publishObjects(const bool publish_all) {
  CHECK_NOTNULL(segment_gsm_update_pub_);
  bool published_segment_label = false;
  std::vector<Label> labels_to_publish;
  getLabelsToPublish(publish_all, &labels_to_publish);

  std::unordered_map<Label, LayerPair> label_to_layers;
  ros::Time start = ros::Time::now();
  extractSegmentLayers(labels_to_publish, &label_to_layers, publish_all);
  ros::Time stop = ros::Time::now();
  ros::Duration duration = stop - start;
  LOG(INFO) << "Extracting segment layers took " << duration.toSec() << "s";

  for (const Label& label : labels_to_publish) {
    auto it = label_to_layers.find(label);
    CHECK(it != label_to_layers.end())
        << "Layers for " << label << " could not be extracted.";

    Layer<TsdfVoxel>& tsdf_layer = it->second.first;
    Layer<LabelVoxel>& label_layer = it->second.second;

    // TODO(ff): Check what a reasonable size is for this.
    constexpr size_t kMinNumberOfAllocatedBlocksToPublish = 10u;
    if (tsdf_layer.getNumberOfAllocatedBlocks() <
        kMinNumberOfAllocatedBlocksToPublish) {
      continue;
    }

    // Convert to origin and extract translation.
    Point origin_shifted_tsdf_layer_W;
    utils::centerBlocksOfLayer<TsdfVoxel>(&tsdf_layer,
                                          &origin_shifted_tsdf_layer_W);
    // TODO(ff): If this is time consuming we can omit this step.
    Point origin_shifted_label_layer_W;
    utils::centerBlocksOfLayer<LabelVoxel>(&label_layer,
                                           &origin_shifted_label_layer_W);
    CHECK_EQ(origin_shifted_tsdf_layer_W, origin_shifted_label_layer_W);

    // Extract surfel cloud from layer.
    MeshIntegratorConfig mesh_config;
    node_handle_private_->param<float>("mesh_config/min_weight",
                                       mesh_config.min_weight,
                                       mesh_config.min_weight);
    pcl::PointCloud<pcl::PointSurfel>::Ptr surfel_cloud(
        new pcl::PointCloud<pcl::PointSurfel>());
    convertVoxelGridToPointCloud(tsdf_layer, mesh_config, surfel_cloud.get());

    if (surfel_cloud->empty()) {
      LOG(WARNING) << tsdf_layer.getNumberOfAllocatedBlocks()
                   << " blocks didn't produce a surface.";
      LOG(WARNING) << "Labelled segment does not contain enough data to "
                      "extract a surface -> skipping!";
      continue;
    }

    modelify_msgs::GsmUpdate gsm_update_msg;
    constexpr bool kSerializeOnlyUpdated = false;
    gsm_update_msg.header.stamp = last_segment_msg_timestamp_;
    gsm_update_msg.header.frame_id = world_frame_;
    gsm_update_msg.is_scene = false;
    serializeLayerAsMsg<TsdfVoxel>(tsdf_layer, kSerializeOnlyUpdated,
                                   &gsm_update_msg.object.tsdf_layer);
    // TODO(ff): Make sure this works also, there is no LabelVoxel in voxblox
    // yet, hence it doesn't work.
    serializeLayerAsMsg<LabelVoxel>(label_layer, kSerializeOnlyUpdated,
                                    &gsm_update_msg.object.label_layer);

    gsm_update_msg.object.label = label;
    gsm_update_msg.object.semantic_label =
        integrator_->getSemanticLabelFusionPtr()->getSemanticLabel(label);
    gsm_update_msg.old_labels.clear();
    geometry_msgs::Transform transform;
    transform.translation.x = origin_shifted_tsdf_layer_W[0];
    transform.translation.y = origin_shifted_tsdf_layer_W[1];
    transform.translation.z = origin_shifted_tsdf_layer_W[2];
    transform.rotation.w = 1.;
    transform.rotation.x = 0.;
    transform.rotation.y = 0.;
    transform.rotation.z = 0.;
    gsm_update_msg.object.transforms.clear();
    gsm_update_msg.object.transforms.push_back(transform);
    pcl::toROSMsg(*surfel_cloud, gsm_update_msg.object.surfel_cloud);

    if (all_published_segments_.find(label) != all_published_segments_.end()) {
      // Segment previously published, sending update message.
      gsm_update_msg.old_labels.push_back(label);
    } else {
      // Segment never previously published, sending first type of message.
    }
    auto merged_label_it = merges_to_publish_.find(label);
    if (merged_label_it != merges_to_publish_.end()) {
      for (Label merged_label : merged_label_it->second) {
        if (all_published_segments_.find(merged_label) !=
            all_published_segments_.end()) {
          gsm_update_msg.old_labels.push_back(merged_label);
        }
      }
      merges_to_publish_.erase(merged_label_it);
    }
    publishGsmUpdate(*segment_gsm_update_pub_, &gsm_update_msg);
    // TODO(ff): Fill in gsm_update_msg.object.surfel_cloud if needed.

    if (publish_segment_mesh_) {
      // Generate mesh for visualization purposes.
      std::shared_ptr<MeshLayer> mesh_layer;
      mesh_layer.reset(new MeshLayer(tsdf_layer.block_size()));
      // mesh_layer.reset(new MeshLayer(map_->block_size()));
      MeshLabelIntegrator mesh_integrator(mesh_config_, &tsdf_layer,
                                          &label_layer, mesh_layer.get(),
                                          all_semantic_labels_);
      constexpr bool only_mesh_updated_blocks = false;
      constexpr bool clear_updated_flag = true;
      mesh_integrator.generateMesh(only_mesh_updated_blocks,
                                   clear_updated_flag);

      voxblox_msgs::Mesh segment_mesh_msg;
      generateVoxbloxMeshMsg(mesh_layer, ColorMode::kColor, &segment_mesh_msg);
      segment_mesh_msg.header.frame_id = world_frame_;
      segment_mesh_pub_->publish(segment_mesh_msg);
    }
    all_published_segments_.insert(label);
    published_segment_label = true;
  }
  segment_labels_to_publish_.clear();
  return published_segment_label;
}

void Controller::publishScene() {
  CHECK_NOTNULL(scene_gsm_update_pub_);
  modelify_msgs::GsmUpdate gsm_update_msg;

  gsm_update_msg.header.stamp = last_segment_msg_timestamp_;
  gsm_update_msg.header.frame_id = world_frame_;

  constexpr bool kSerializeOnlyUpdated = false;
  serializeLayerAsMsg<TsdfVoxel>(map_->getTsdfLayer(), kSerializeOnlyUpdated,
                                 &gsm_update_msg.object.tsdf_layer);
  // TODO(ff): Make sure this works also, there is no LabelVoxel in voxblox
  // yet, hence it doesn't work.
  serializeLayerAsMsg<LabelVoxel>(map_->getLabelLayer(), kSerializeOnlyUpdated,
                                  &gsm_update_msg.object.label_layer);

  gsm_update_msg.object.label = 0u;
  gsm_update_msg.old_labels.clear();
  gsm_update_msg.is_scene = true;
  geometry_msgs::Transform transform;
  transform.translation.x = 0.0;
  transform.translation.y = 0.0;
  transform.translation.z = 0.0;
  transform.rotation.w = 1.0;
  transform.rotation.x = 0.0;
  transform.rotation.y = 0.0;
  transform.rotation.z = 0.0;
  gsm_update_msg.object.transforms.clear();
  gsm_update_msg.object.transforms.push_back(transform);
  publishGsmUpdate(*scene_gsm_update_pub_, &gsm_update_msg);
}

void Controller::generateMesh(bool clear_mesh) {  // NOLINT
  voxblox::timing::Timer generate_mesh_timer("mesh/generate");
  {
    std::lock_guard<std::mutex> updateMeshLock(updated_mesh_mutex_);
    if (clear_mesh) {
      constexpr bool only_mesh_updated_blocks = false;
      constexpr bool clear_updated_flag = true;
      mesh_label_integrator_->generateMesh(only_mesh_updated_blocks,
                                           clear_updated_flag);
      if (label_tsdf_integrator_config_.enable_semantic_instance_segmentation) {
        all_semantic_labels_.clear();
        mesh_semantic_integrator_->generateMesh(only_mesh_updated_blocks,
                                                clear_updated_flag);
        for (auto sl : all_semantic_labels_) {
          LOG(ERROR) << classes[(unsigned)sl];
        }
        mesh_instance_integrator_->generateMesh(only_mesh_updated_blocks,
                                                clear_updated_flag);
        mesh_merged_integrator_->generateMesh(only_mesh_updated_blocks,
                                              clear_updated_flag);
      }

    } else {
      constexpr bool only_mesh_updated_blocks = true;
      constexpr bool clear_updated_flag = true;
      mesh_label_integrator_->generateMesh(only_mesh_updated_blocks,
                                           clear_updated_flag);

      if (label_tsdf_integrator_config_.enable_semantic_instance_segmentation) {
        mesh_semantic_integrator_->generateMesh(only_mesh_updated_blocks,
                                                clear_updated_flag);
        mesh_instance_integrator_->generateMesh(only_mesh_updated_blocks,
                                                clear_updated_flag);
        mesh_merged_integrator_->generateMesh(only_mesh_updated_blocks,
                                              clear_updated_flag);
      }
    }
    generate_mesh_timer.Stop();

    updated_mesh_ = true;

    if (publish_scene_mesh_) {
      timing::Timer publish_mesh_timer("mesh/publish");
      voxblox_msgs::Mesh mesh_msg;
      generateVoxbloxMeshMsg(mesh_label_layer_, ColorMode::kColor, &mesh_msg);
      mesh_msg.header.frame_id = world_frame_;
      scene_mesh_pub_->publish(mesh_msg);
      publish_mesh_timer.Stop();
    }
  }

  if (!mesh_filename_.empty()) {
    timing::Timer output_mesh_timer("mesh/output");
    bool success = outputMeshLayerAsPly("label_" + mesh_filename_, false,
                                        *mesh_label_layer_);
    if (label_tsdf_integrator_config_.enable_semantic_instance_segmentation) {
      success &= outputMeshLayerAsPly("semantic_" + mesh_filename_, false,
                                      *mesh_semantic_layer_);
      success &= outputMeshLayerAsPly("instance_" + mesh_filename_, false,
                                      *mesh_instance_layer_);
      success &= outputMeshLayerAsPly("merged_" + mesh_filename_, false,
                                      *mesh_merged_layer_);
    }
    output_mesh_timer.Stop();
    if (success) {
      ROS_INFO("Output file as PLY: %s", mesh_filename_.c_str());
    } else {
      ROS_INFO("Failed to output mesh as PLY: %s", mesh_filename_.c_str());
    }
  }

  ROS_INFO_STREAM("Mesh Timings: " << std::endl
                                   << voxblox::timing::Timing::Print());
}

void Controller::updateMeshEvent(const ros::TimerEvent& e) {
  {
    std::lock_guard<std::mutex> updateMeshLock(updated_mesh_mutex_);
    timing::Timer generate_mesh_timer("mesh/update");
    bool only_mesh_updated_blocks = true;
    if (need_full_remesh_) {
      only_mesh_updated_blocks = false;
      need_full_remesh_ = false;
    }
    bool clear_updated_flag = false;
    updated_mesh_ |= mesh_label_integrator_->generateMesh(
        only_mesh_updated_blocks, clear_updated_flag);

    if (label_tsdf_integrator_config_.enable_semantic_instance_segmentation) {
      updated_mesh_ |= mesh_merged_integrator_->generateMesh(
          only_mesh_updated_blocks, clear_updated_flag);

      updated_mesh_ |= mesh_instance_integrator_->generateMesh(
          only_mesh_updated_blocks, clear_updated_flag);

      clear_updated_flag = true;
      updated_mesh_ |= mesh_semantic_integrator_->generateMesh(
          only_mesh_updated_blocks, clear_updated_flag);
    }
    generate_mesh_timer.Stop();

    if (publish_scene_mesh_) {
      timing::Timer publish_mesh_timer("mesh/publish");
      voxblox_msgs::Mesh mesh_msg;
      generateVoxbloxMeshMsg(mesh_label_layer_, ColorMode::kColor, &mesh_msg);
      mesh_msg.header.frame_id = world_frame_;
      scene_mesh_pub_->publish(mesh_msg);
      publish_mesh_timer.Stop();
    }
  }
}

bool Controller::noNewUpdatesReceived() const {
  if (received_first_message_ && no_update_timeout_ != 0.0) {
    return (ros::Time::now() - last_update_received_).toSec() >
           no_update_timeout_;
  }
  return false;
}

void Controller::publishGsmUpdate(const ros::Publisher& publisher,
                                  modelify_msgs::GsmUpdate* gsm_update) {
  publisher.publish(*gsm_update);
}

void Controller::getLabelsToPublish(const bool get_all,
                                    std::vector<Label>* labels) {
  CHECK_NOTNULL(labels);
  if (get_all) {
    *labels = integrator_->getLabelsList();
    ROS_INFO("Publishing all segments");
  } else {
    *labels = segment_labels_to_publish_;
  }
}
}  // namespace voxblox_gsm
}  // namespace voxblox
