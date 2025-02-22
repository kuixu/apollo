/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/prediction/predictor/predictor_manager.h"

#include <list>
#include <memory>
#include <unordered_map>

#include "modules/prediction/common/feature_output.h"
#include "modules/prediction/common/prediction_constants.h"
#include "modules/prediction/common/prediction_gflags.h"
#include "modules/prediction/common/prediction_system_gflags.h"
#include "modules/prediction/common/prediction_thread_pool.h"
#include "modules/prediction/container/container_manager.h"
#include "modules/prediction/predictor/empty/empty_predictor.h"
#include "modules/prediction/predictor/extrapolation/extrapolation_predictor.h"
#include "modules/prediction/predictor/free_move/free_move_predictor.h"
#include "modules/prediction/predictor/interaction/interaction_predictor.h"
#include "modules/prediction/predictor/junction/junction_predictor.h"
#include "modules/prediction/predictor/lane_sequence/lane_sequence_predictor.h"
#include "modules/prediction/predictor/move_sequence/move_sequence_predictor.h"
#include "modules/prediction/predictor/regional/regional_predictor.h"
#include "modules/prediction/predictor/single_lane/single_lane_predictor.h"
#include "modules/prediction/scenario/scenario_manager.h"

namespace apollo {
namespace prediction {

using apollo::common::adapter::AdapterConfig;
using apollo::perception::PerceptionObstacle;
using IdObstacleListMap = std::unordered_map<int, std::list<Obstacle*>>;
using IdPredictionObstacleMap =
    std::unordered_map<int, std::shared_ptr<PredictionObstacle>>;

namespace {

void GroupObstaclesByObstacleId(const int obstacle_id,
                                ObstaclesContainer* const obstacles_container,
                                IdObstacleListMap* const id_obstacle_map) {
  Obstacle* obstacle_ptr = obstacles_container->GetObstacle(obstacle_id);
  if (obstacle_ptr == nullptr) {
    AERROR << "Null obstacle [" << obstacle_id << "] found";
    return;
  }
  int id_mod = obstacle_id % FLAGS_max_thread_num;
  (*id_obstacle_map)[id_mod].push_back(obstacle_ptr);
}

}  // namespace

PredictorManager::PredictorManager() { RegisterPredictors(); }

void PredictorManager::RegisterPredictors() {
  RegisterPredictor(ObstacleConf::LANE_SEQUENCE_PREDICTOR);
  RegisterPredictor(ObstacleConf::MOVE_SEQUENCE_PREDICTOR);
  RegisterPredictor(ObstacleConf::SINGLE_LANE_PREDICTOR);
  RegisterPredictor(ObstacleConf::FREE_MOVE_PREDICTOR);
  RegisterPredictor(ObstacleConf::REGIONAL_PREDICTOR);
  RegisterPredictor(ObstacleConf::EMPTY_PREDICTOR);
  RegisterPredictor(ObstacleConf::JUNCTION_PREDICTOR);
  RegisterPredictor(ObstacleConf::EXTRAPOLATION_PREDICTOR);
  RegisterPredictor(ObstacleConf::INTERACTION_PREDICTOR);
}

void PredictorManager::Init(const PredictionConf& config) {
  for (const auto& conf : config.obstacle_conf()) {
    if (!conf.has_obstacle_type()) {
      AERROR << "Obstacle config [" << conf.ShortDebugString()
             << "] has not defined obstacle type.";
      continue;
    }

    if (!conf.has_predictor_type()) {
      AERROR << "Obstacle config [" << conf.ShortDebugString()
             << "] has not defined predictor type.";
      continue;
    }

    switch (conf.obstacle_type()) {
      case PerceptionObstacle::VEHICLE: {
        InitVehiclePredictors(conf);
        break;
      }
      case PerceptionObstacle::BICYCLE: {
        InitCyclistPredictors(conf);
        break;
      }
      case PerceptionObstacle::PEDESTRIAN: {
        pedestrian_predictor_ = conf.predictor_type();
        break;
      }
      case PerceptionObstacle::UNKNOWN: {
        InitDefaultPredictors(conf);
        break;
      }
      default: { break; }
    }
  }

  AINFO << "Defined vehicle on lane obstacle predictor ["
        << vehicle_on_lane_predictor_ << "].";
  AINFO << "Defined vehicle off lane obstacle predictor ["
        << vehicle_off_lane_predictor_ << "].";
  AINFO << "Defined bicycle on lane obstacle predictor ["
        << cyclist_on_lane_predictor_ << "].";
  AINFO << "Defined bicycle off lane obstacle predictor ["
        << cyclist_off_lane_predictor_ << "].";
  AINFO << "Defined pedestrian obstacle predictor [" << pedestrian_predictor_
        << "].";
  AINFO << "Defined default on lane obstacle predictor ["
        << default_on_lane_predictor_ << "].";
  AINFO << "Defined default off lane obstacle predictor ["
        << default_off_lane_predictor_ << "].";
}

Predictor* PredictorManager::GetPredictor(
    const ObstacleConf::PredictorType& type) {
  auto it = predictors_.find(type);
  return it != predictors_.end() ? it->second.get() : nullptr;
}

void PredictorManager::Run(
    const ADCTrajectoryContainer* adc_trajectory_container,
    ObstaclesContainer* obstacles_container) {
  prediction_obstacles_.Clear();

  if (FLAGS_enable_multi_thread) {
    PredictObstaclesInParallel(adc_trajectory_container, obstacles_container);
  } else {
    PredictObstacles(adc_trajectory_container, obstacles_container);
  }
}

void PredictorManager::PredictObstacles(
    const ADCTrajectoryContainer* adc_trajectory_container,
    ObstaclesContainer* obstacles_container) {
  for (const int id : obstacles_container->curr_frame_obstacle_ids()) {
    if (id < 0) {
      ADEBUG << "The obstacle has invalid id [" << id << "].";
      continue;
    }

    PredictionObstacle prediction_obstacle;
    Obstacle* obstacle = obstacles_container->GetObstacle(id);

    PerceptionObstacle perception_obstacle =
        obstacles_container->GetPerceptionObstacle(id);
    // if obstacle == nullptr, that means obstacle is unmovable
    // Checkout the logic of unmovable in obstacle.cc
    if (obstacle != nullptr) {
      PredictObstacle(adc_trajectory_container, obstacle, obstacles_container,
                      &prediction_obstacle);
    } else {  // obstacle == nullptr
      prediction_obstacle.set_timestamp(perception_obstacle.timestamp());
      prediction_obstacle.set_is_static(true);
    }

    prediction_obstacle.set_predicted_period(
        FLAGS_prediction_trajectory_time_length);
    prediction_obstacle.mutable_perception_obstacle()->CopyFrom(
        perception_obstacle);

    prediction_obstacles_.add_prediction_obstacle()->CopyFrom(
        prediction_obstacle);
  }
}

void PredictorManager::PredictObstaclesInParallel(
    const ADCTrajectoryContainer* adc_trajectory_container,
    ObstaclesContainer* obstacles_container) {
  IdPredictionObstacleMap id_prediction_obstacle_map;
  for (int id : obstacles_container->curr_frame_obstacle_ids()) {
    id_prediction_obstacle_map[id] = std::make_shared<PredictionObstacle>();
  }
  IdObstacleListMap id_obstacle_map;
  for (int id : obstacles_container->curr_frame_obstacle_ids()) {
    Obstacle* obstacle = obstacles_container->GetObstacle(id);
    const PerceptionObstacle& perception_obstacle =
        obstacles_container->GetPerceptionObstacle(id);
    if (obstacle == nullptr) {
      std::shared_ptr<PredictionObstacle> prediction_obstacle_ptr =
          id_prediction_obstacle_map[id];
      prediction_obstacle_ptr->set_is_static(true);
      prediction_obstacle_ptr->set_timestamp(perception_obstacle.timestamp());
    } else {
      GroupObstaclesByObstacleId(id, obstacles_container, &id_obstacle_map);
    }
  }
  PredictionThreadPool::ForEach(
      id_obstacle_map.begin(), id_obstacle_map.end(),
      [&](IdObstacleListMap::iterator::value_type& obstacles_iter) {
        for (auto obstacle_ptr : obstacles_iter.second) {
          int id = obstacle_ptr->id();
          PredictObstacle(adc_trajectory_container, obstacle_ptr,
                          obstacles_container,
                          id_prediction_obstacle_map[id].get());
        }
      });
  for (auto& item : id_prediction_obstacle_map) {
    int id = item.first;
    auto prediction_obstacle_ptr = item.second;
    const PerceptionObstacle& perception_obstacle =
        obstacles_container->GetPerceptionObstacle(id);
    prediction_obstacle_ptr->set_predicted_period(
        FLAGS_prediction_trajectory_time_length);
    prediction_obstacle_ptr->mutable_perception_obstacle()->CopyFrom(
        perception_obstacle);
    prediction_obstacles_.add_prediction_obstacle()->CopyFrom(
        *prediction_obstacle_ptr);
  }
}

void PredictorManager::PredictObstacle(
    const ADCTrajectoryContainer* adc_trajectory_container, Obstacle* obstacle,
    ObstaclesContainer* obstacles_container,
    PredictionObstacle* const prediction_obstacle) {
  CHECK_NOTNULL(obstacle);
  Predictor* predictor = nullptr;
  prediction_obstacle->set_timestamp(obstacle->timestamp());
  if (obstacle->ToIgnore()) {
    ADEBUG << "Ignore obstacle [" << obstacle->id() << "]";
    predictor = GetPredictor(ObstacleConf::EMPTY_PREDICTOR);
    prediction_obstacle->mutable_priority()->set_priority(
        ObstaclePriority::IGNORE);
  } else if (obstacle->IsStill()) {
    ADEBUG << "Still obstacle [" << obstacle->id() << "]";
    predictor = GetPredictor(ObstacleConf::EMPTY_PREDICTOR);
  } else {
    switch (obstacle->type()) {
      case PerceptionObstacle::VEHICLE: {
        predictor = GetVehiclePredictor(*obstacle);
        break;
      }
      case PerceptionObstacle::PEDESTRIAN: {
        predictor = GetPredictor(pedestrian_predictor_);
        break;
      }
      case PerceptionObstacle::BICYCLE: {
        predictor = GetCyclistPredictor(*obstacle);
        break;
      }
      default: {
        predictor = GetDefaultPredictor(*obstacle);
        break;
      }
    }
  }

  if (predictor != nullptr) {
    predictor->Predict(adc_trajectory_container, obstacle, obstacles_container);
    if (FLAGS_enable_trim_prediction_trajectory &&
        obstacle->type() == PerceptionObstacle::VEHICLE) {
      CHECK_NOTNULL(adc_trajectory_container);
      predictor->TrimTrajectories(*adc_trajectory_container, obstacle);
    }
  }
  for (const auto& trajectory :
       obstacle->latest_feature().predicted_trajectory()) {
    prediction_obstacle->add_trajectory()->CopyFrom(trajectory);
  }
  prediction_obstacle->set_timestamp(obstacle->timestamp());
  prediction_obstacle->mutable_priority()->CopyFrom(
      obstacle->latest_feature().priority());
  prediction_obstacle->set_is_static(obstacle->IsStill());
  if (FLAGS_prediction_offline_mode ==
      PredictionConstants::kDumpPredictionResult) {
    const Scenario& scenario = ScenarioManager::Instance()->scenario();
    FeatureOutput::InsertPredictionResult(obstacle, *prediction_obstacle,
                                          obstacle->obstacle_conf(), scenario);
  }
}

std::unique_ptr<Predictor> PredictorManager::CreatePredictor(
    const ObstacleConf::PredictorType& type) {
  std::unique_ptr<Predictor> predictor_ptr(nullptr);
  switch (type) {
    case ObstacleConf::LANE_SEQUENCE_PREDICTOR: {
      predictor_ptr.reset(new LaneSequencePredictor());
      break;
    }
    case ObstacleConf::MOVE_SEQUENCE_PREDICTOR: {
      predictor_ptr.reset(new MoveSequencePredictor());
      break;
    }
    case ObstacleConf::SINGLE_LANE_PREDICTOR: {
      predictor_ptr.reset(new SingleLanePredictor());
      break;
    }
    case ObstacleConf::FREE_MOVE_PREDICTOR: {
      predictor_ptr.reset(new FreeMovePredictor());
      break;
    }
    case ObstacleConf::REGIONAL_PREDICTOR: {
      predictor_ptr.reset(new RegionalPredictor());
      break;
    }
    case ObstacleConf::JUNCTION_PREDICTOR: {
      predictor_ptr.reset(new JunctionPredictor());
      break;
    }
    case ObstacleConf::EXTRAPOLATION_PREDICTOR: {
      predictor_ptr.reset(new ExtrapolationPredictor());
      break;
    }
    case ObstacleConf::INTERACTION_PREDICTOR: {
      predictor_ptr.reset(new InteractionPredictor());
      break;
    }
    case ObstacleConf::EMPTY_PREDICTOR: {
      predictor_ptr.reset(new EmptyPredictor());
      break;
    }
    default: { break; }
  }
  return predictor_ptr;
}

void PredictorManager::RegisterPredictor(
    const ObstacleConf::PredictorType& type) {
  predictors_[type] = CreatePredictor(type);
  AINFO << "Predictor [" << type << "] is registered.";
}

const PredictionObstacles& PredictorManager::prediction_obstacles() {
  return prediction_obstacles_;
}

void PredictorManager::InitVehiclePredictors(const ObstacleConf& conf) {
  switch (conf.obstacle_status()) {
    case ObstacleConf::ON_LANE: {
      if (conf.priority_type() == ObstaclePriority::CAUTION) {
        vehicle_on_lane_caution_predictor_ = conf.predictor_type();
      } else {
        vehicle_on_lane_predictor_ = conf.predictor_type();
      }
      break;
    }
    case ObstacleConf::OFF_LANE: {
      vehicle_off_lane_predictor_ = conf.predictor_type();
      break;
    }
    case ObstacleConf::IN_JUNCTION: {
      if (conf.priority_type() == ObstaclePriority::CAUTION) {
        vehicle_in_junction_caution_predictor_ = conf.predictor_type();
      } else {
        vehicle_in_junction_predictor_ = conf.predictor_type();
      }
      break;
    }
    default: { break; }
  }
}

void PredictorManager::InitCyclistPredictors(const ObstacleConf& conf) {
  switch (conf.obstacle_status()) {
    case ObstacleConf::ON_LANE: {
      cyclist_on_lane_predictor_ = conf.predictor_type();
      break;
    }
    case ObstacleConf::OFF_LANE: {
      cyclist_off_lane_predictor_ = conf.predictor_type();
      break;
    }
    default: { break; }
  }
}

void PredictorManager::InitDefaultPredictors(const ObstacleConf& conf) {
  switch (conf.obstacle_status()) {
    case ObstacleConf::ON_LANE: {
      default_on_lane_predictor_ = conf.predictor_type();
      break;
    }
    case ObstacleConf::OFF_LANE: {
      default_off_lane_predictor_ = conf.predictor_type();
      break;
    }
    default: { break; }
  }
}

Predictor* PredictorManager::GetVehiclePredictor(const Obstacle& obstacle) {
  if (!obstacle.IsOnLane()) {
    return GetPredictor(vehicle_off_lane_predictor_);
  }
  if (obstacle.HasJunctionFeatureWithExits() &&
      !obstacle.IsCloseToJunctionExit()) {
    if (obstacle.IsCaution()) {
      return GetPredictor(vehicle_in_junction_caution_predictor_);
    }
    return GetPredictor(vehicle_in_junction_predictor_);
  }
  if (obstacle.IsCaution()) {
    return GetPredictor(vehicle_on_lane_caution_predictor_);
  }
  return GetPredictor(vehicle_on_lane_predictor_);
}

Predictor* PredictorManager::GetCyclistPredictor(const Obstacle& obstacle) {
  if (obstacle.IsOnLane()) {
    return GetPredictor(cyclist_on_lane_predictor_);
  }
  return GetPredictor(cyclist_off_lane_predictor_);
}

Predictor* PredictorManager::GetDefaultPredictor(const Obstacle& obstacle) {
  if (obstacle.IsOnLane()) {
    return GetPredictor(default_on_lane_predictor_);
  }
  return GetPredictor(default_off_lane_predictor_);
}

}  // namespace prediction
}  // namespace apollo
