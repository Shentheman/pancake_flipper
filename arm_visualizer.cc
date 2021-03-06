#include <iostream>
#include <fstream>
#include <string>

#include <unistd.h>

#include <gflags/gflags.h>

#include "drake/common/drake_assert.h"
#include "drake/common/find_resource.h"
#include "drake/common/text_logging.h"
#include "drake/common/type_safe_index.h"
#include "drake/geometry/geometry_visualization.h"
#include "drake/geometry/scene_graph.h"
#include "drake/lcm/drake_lcm.h"
#include "drake/multibody/parsing/parser.h"
#include "drake/multibody/plant/multibody_plant.h"
#include "drake/multibody/tree/weld_joint.h"
#include "drake/systems/analysis/simulator.h"
#include "drake/systems/framework/diagram.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/primitives/trajectory_source.h"
#include <drake/systems/rendering/multibody_position_to_geometry_pose.h>
#include <drake/common/trajectories/piecewise_polynomial.h>

#include <drake/manipulation/planner/constraint_relaxing_ik.h>
#include "drake/math/rigid_transform.h"
#include "drake/math/roll_pitch_yaw.h"
#include "drake/math/rotation_matrix.h"
#include "drake/multibody/parsing/parser.h"
#include "drake/multibody/plant/multibody_plant.h"

namespace drake {
namespace examples {
namespace kuka {

using drake::multibody::MultibodyPlant;

DEFINE_double(simulation_time, 5.0,
              "Desired duration of the simulation in seconds");
DEFINE_double(max_time_step, 1.0e-3,
              "Simulation time step used for integrator.");
DEFINE_double(target_realtime_rate, 1,
              "Desired rate relative to real time.  See documentation for "
              "Simulator::set_target_realtime_rate() for details.");

DEFINE_string(pancake_csv_filename, "results/arm_viz2_trace_umax40_ceiling5_mu0.0_T60.npz_pancake_to.csv",
              "Path to a CSV file containing the trajectory trace for the pancake. This CSV should be generated by convert_npz_trajectory_to_csv.py");
DEFINE_string(flipper_csv_filename, "results/arm_viz2_trace_umax40_ceiling5_mu0.0_T60.npz_flipper_to.csv",
              "Path to a CSV file containing the trajectory trace for the pancake. This CSV should be generated by convert_npz_trajectory_to_csv.py");


using namespace Eigen;
// This function taken from https://stackoverflow.com/questions/34247057/how-to-read-csv-file-and-assign-to-eigen-matrix/39146048
template<typename M>
M load_csv (const std::string & path) {
    std::ifstream indata;
    indata.open(path);
    std::string line;
    std::vector<double> values;
    uint rows = 0;
    while (std::getline(indata, line)) {
        std::stringstream lineStream(line);
        std::string cell;
        while (std::getline(lineStream, cell, ',')) {
            values.push_back(std::stod(cell));
        }
        ++rows;
    }
    return Map<const Matrix<typename M::Scalar, M::RowsAtCompileTime, M::ColsAtCompileTime, RowMajor>>(values.data(), rows, values.size()/rows);
}

double TRAJ_SCALE = 0.25;
double TRAJ_OFFSET_Z = 1.0; // slide
// double TRAJ_OFFSET_Z = 0.8; // flip
double TRAJ_OFFSET_X = 0.4;

void DoMain() {
  DRAKE_DEMAND(FLAGS_simulation_time > 0);

  systems::DiagramBuilder<double> builder;

  geometry::SceneGraph<double>& scene_graph =
      *builder.AddSystem<geometry::SceneGraph>();
  scene_graph.set_name("scene_graph");

  MultibodyPlant<double> arm_plant = MultibodyPlant<double>(FLAGS_max_time_step);
  drake::geometry::SourceId plant_source_id = arm_plant.RegisterAsSourceForSceneGraph(&scene_graph);
  std::string full_name = FindResourceOrThrow(
      "drake/manipulation/models/"
      "iiwa_description/sdf/iiwa14_no_collision.sdf");

  multibody::ModelInstanceIndex plant_index =
      multibody::Parser(&arm_plant).AddModelFromFile(full_name);

  // Weld the arm to the world frame.
  const auto& joint_arm_root = arm_plant.GetBodyByName("iiwa_link_0");
  arm_plant.AddJoint<multibody::WeldJoint>("weld_arm", arm_plant.world_body(), {},
                                           joint_arm_root, {},
                                           drake::math::RigidTransform(Isometry3<double>::Identity()));

  // Attach the pancake flipper
  std::string flipper_model_file = "./models/flipper_arm_scale.urdf";
  plant_index = drake::multibody::Parser(&arm_plant).AddModelFromFile(flipper_model_file);
  // Weld the end effector to the pancake flipper
  const auto& joint_arm_eef_frame = arm_plant.GetFrameByName("iiwa_link_7");
  const auto& flipper_frame = arm_plant.GetFrameByName("flipper");
  drake::math::RigidTransform flipper_xform = drake::math::RigidTransform(Isometry3<double>::Identity());
  Vector3<double> flipper_offset;
  flipper_offset << 0.0, 0.0, 0.05;
  flipper_xform.set_translation(flipper_offset);
  arm_plant.WeldFrames(
      joint_arm_eef_frame,
      flipper_frame,
      flipper_xform);

  // Now the model is complete.
  arm_plant.Finalize();

  // Also make a plant for the pancake
  MultibodyPlant<double> pancake_plant = MultibodyPlant<double>(FLAGS_max_time_step);
  drake::geometry::SourceId pancake_plant_source_id = pancake_plant.RegisterAsSourceForSceneGraph(&scene_graph);
  std::string pancake_model_file = "./models/pancake_arm_scale.urdf";
  multibody::ModelInstanceIndex pancake_plant_index =
      multibody::Parser(&pancake_plant).AddModelFromFile(pancake_model_file);
  pancake_plant.Finalize();

  // Load the trajectory for the pancake
  int T = 61;
  std::vector<double> t_pancake;
  std::vector<Eigen::MatrixXd> q_pancake;
  // Fill t_pancake and q_pancake from the CSV
  std::string pancake_csv_filename = FLAGS_pancake_csv_filename;
  MatrixXd loaded_data = load_csv<MatrixXd>(pancake_csv_filename);
  for (int row_i = 0; row_i < loaded_data.rows(); row_i++) {
    t_pancake.push_back(loaded_data(row_i, 0));
    Eigen::MatrixXd q_i = VectorX<double>::Zero(pancake_plant.num_positions());
    q_i(0) = TRAJ_SCALE * loaded_data(row_i, 1) + TRAJ_OFFSET_X;
    q_i(1) = TRAJ_SCALE * loaded_data(row_i, 2) + TRAJ_OFFSET_Z;
    q_i(2) = loaded_data(row_i, 3);
    q_pancake.push_back(q_i);
  }

  // Make a trajectory source for the pancake
  drake::trajectories::PiecewisePolynomial<double> pancake_trajectory = 
    drake::trajectories::PiecewisePolynomial<double>::FirstOrderHold(t_pancake, q_pancake);
  const auto pancake_traj_source = builder.AddSystem<drake::systems::TrajectorySource<double>>(
    pancake_trajectory
  );

  // Connect the trajectory source directly to the geometry poses
  auto pancake_q_to_pose = 
  builder.AddSystem<drake::systems::rendering::MultibodyPositionToGeometryPose<double>>(
    pancake_plant
  );
  builder.Connect(pancake_traj_source->get_output_port(),
                  pancake_q_to_pose->get_input_port());
  builder.Connect(pancake_q_to_pose->get_output_port(),
                  scene_graph.get_source_pose_port(pancake_plant_source_id));

  // Load the trajectory for the flipper
  std::vector<double> t_flipper;
  std::vector<drake::manipulation::planner::ConstraintRelaxingIk::IkCartesianWaypoint> flipper_waypoints;
  // Fill t_flipper and q_flipper from the CSV
  std::string flipper_csv_filename = FLAGS_flipper_csv_filename;
  loaded_data = load_csv<MatrixXd>(flipper_csv_filename);
  for (int row_i = 0; row_i < loaded_data.rows(); row_i++) {
    t_flipper.push_back(loaded_data(row_i, 0));
    drake::manipulation::planner::ConstraintRelaxingIk::IkCartesianWaypoint wp;
    // wp.pose.set_translation(Eigen::Vector3d(
    //   TRAJ_SCALE * loaded_data(row_i, 1) + TRAJ_OFFSET_X,
    //   0.0,
    //   TRAJ_SCALE * loaded_data(row_i, 2) + TRAJ_OFFSET_Z-0.05
    // ));
    // const drake::math::RollPitchYaw<double> rpy(
    //   0.0,
    //   loaded_data(row_i, 3),
    //   0.0
    // );
    wp.pose.set_translation(Eigen::Vector3d(
      1.0 * TRAJ_SCALE * loaded_data(row_i, 1) + TRAJ_OFFSET_X,
      0.0,
      1.0 * TRAJ_SCALE * loaded_data(row_i, 2) + TRAJ_OFFSET_Z-0.05
    ));
    const drake::math::RollPitchYaw<double> rpy(
      0.0,
      loaded_data(row_i, 3),
      0.0
    );
    wp.pose.set_rotation(rpy);
    wp.constrain_orientation = true;
    wp.pos_tol = Vector3<double>(0.015, 0.015, 0.015);
    wp.rot_tol = 0.05;
    flipper_waypoints.push_back(wp);
  }

  // Solve the IK problem to get the arm waypoints
  drake::manipulation::planner::ConstraintRelaxingIk ik(full_name, "iiwa_link_7");
  Eigen::VectorXd iiwa_q = VectorX<double>::Zero(arm_plant.num_positions());
  std::vector<Eigen::VectorXd> q_sol_arm;
  const bool result = ik.PlanSequentialTrajectory(flipper_waypoints, iiwa_q, &q_sol_arm);

  std::vector<Eigen::MatrixXd> q_sol_arm_mat;
  q_sol_arm.erase(q_sol_arm.begin());
  for (Eigen::VectorXd v : q_sol_arm) {
    // std::cout << v << std::endl;
    q_sol_arm_mat.push_back(v);
  }

  // Make a trajectory source for the arm
  // std::cout << t_flipper.size() << std::endl;
  // std::cout << q_sol_arm_mat.size() << std::endl;
  drake::trajectories::PiecewisePolynomial<double> arm_trajectory = 
    drake::trajectories::PiecewisePolynomial<double>::FirstOrderHold(t_flipper, q_sol_arm_mat);
  const auto arm_traj_source = builder.AddSystem<drake::systems::TrajectorySource<double>>(
    arm_trajectory
  );

  // Connect the trajectory source directly to the geometry poses
  auto arm_q_to_pose = 
    builder.AddSystem<drake::systems::rendering::MultibodyPositionToGeometryPose<double>>(
      arm_plant
    );
  builder.Connect(arm_traj_source->get_output_port(),
                  arm_q_to_pose->get_input_port());
  builder.Connect(arm_q_to_pose->get_output_port(),
                  scene_graph.get_source_pose_port(plant_source_id));

  // Create the visualizer
  geometry::ConnectDrakeVisualizer(&builder, scene_graph);
  std::unique_ptr<systems::Diagram<double>> diagram = builder.Build();

  // Set up simulator.
  systems::Simulator<double> simulator(*diagram);
  simulator.set_publish_every_time_step(true);
  simulator.set_target_realtime_rate(FLAGS_target_realtime_rate);
  simulator.Initialize();
  simulator.AdvanceTo(FLAGS_simulation_time);
}

}  // namespace kuka
}  // namespace examples
}  // namespace drake

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(
      "bazel run"
      "//examples/kuka_iiwa_arm_idc:run_kuka_idc");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  drake::examples::kuka::DoMain();
  return 0;
}