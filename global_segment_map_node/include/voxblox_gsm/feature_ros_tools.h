#ifndef VOXBLOX_GSM_FEATURE_ROS_TOOLS_H
#define VOXBLOX_GSM_FEATURE_ROS_TOOLS_H

#include <std_msgs/ColorRGBA.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include <voxblox/common.h>

#include <global_feature_map/feature_block.h>
#include <global_feature_map/feature_layer.h>

namespace voxblox {
namespace voxblox_gsm {

inline std_msgs::ColorRGBA getColorFromBlockFeatures(
    const double min_value_of_features, const double max_value_of_features,
    const double number_of_features) {
  double ratio = 2 * (number_of_features - min_value_of_features) /
                 (max_value_of_features - min_value_of_features);
  b = max(0.0, 1.0 * (1 - ratio));
  r = max(0.0, 1.0 * (ratio - 1));
  g = 1.0 - b - r;

  std_msgs::ColorRGBA color_msg;
  color_msg.r = r;
  color_msg.g = g;
  color_msg.b = b;
  color_msg.a = 0.3;

  return color_msg;
}

template <typename FeatureType>
inline void createOccupancyBlocksFromFeatureLayer(
    const FeatureLayer<FeatureType>& layer, const std::string& frame_id,
    visualization_msgs::MarkerArray* marker_array) {
  CHECK_NOTNULL(marker_array);

  // Cache layer settings.
  FloatingPoint block_size = layer.block_size();

  visualization_msgs::Marker block_marker;
  block_marker.header.frame_id = frame_id;
  block_marker.ns = "occupied_voxels";
  block_marker.id = 0;
  block_marker.type = visualization_msgs::Marker::CUBE_LIST;
  block_marker.scale.x = block_marker.scale.y = block_marker.scale.z =
      block_size;
  block_marker.action = visualization_msgs::Marker::ADD;

  BlockIndexList blocks;
  layer.getAllAllocatedBlocks(&blocks);
  for (const BlockIndex& index : blocks) {
    const FeatureBlock<FeatureType>& block = layer.getBlockByIndex(index);
    Point coord = block.computeBlockCoordinatesFromIndex(linear_index);
    geometry_msgs::Point cube_center;
    cube_center.x = coord.x();
    cube_center.y = coord.y();
    cube_center.z = coord.z();
    block_marker.points.push_back(cube_center);
    std_msgs::ColorRGBA color_msg = getColorFromBlockFeatures();
    block_marker.colors.push_back(color_msg);
  }
  marker_array->markers.push_back(block_marker);
}

}  // namespace voxblox_gsm
}  // namespace voxblox

#endif  // VOXBLOX_GSM_FEATURE_ROS_TOOLS_H
