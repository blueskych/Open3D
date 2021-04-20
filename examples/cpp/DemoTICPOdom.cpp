#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include "open3d/Open3D.h"

using namespace open3d;
using namespace open3d::visualization;
using namespace open3d::t::pipelines::registration;

const int WIDTH = 1024;
const int HEIGHT = 768;
float verticalFoV = 25;

const Eigen::Vector3f CENTER_OFFSET(-10.0f, 0.0f, 30.0f);
const std::string CURRENT_CLOUD = "current_scan";

std::vector<float> initial_transform_flat = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
                                             0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
                                             0.0, 0.0, 0.0, 1.0};

class ReconstructionWindow : public gui::Window {
    using Super = gui::Window;

public:
    ReconstructionWindow() : gui::Window("Open3D - Reconstruction", 1600, 900) {
        widget3d_ = std::make_shared<gui::SceneWidget>();
        AddChild(widget3d_);
        widget3d_->SetScene(
                std::make_shared<rendering::Open3DScene>(GetRenderer()));
    }

    ~ReconstructionWindow() {}

protected:
    std::shared_ptr<gui::SceneWidget> widget3d_;
};

//------------------------------------------------------------------------------
class ExampleWindow : public ReconstructionWindow {
public:
    ExampleWindow(const std::string& path_config, const core::Device& device)
        : device_(device),
          host_(core::Device("CPU:0")),
          dtype_(core::Dtype::Float32) {
        ReadConfigFile(path_config);
        pointclouds_device_ = LoadTensorPointClouds();

        transformation_ =
                core::Tensor(initial_transform_flat, {4, 4}, dtype_, device_);

        // Warm Up.
        std::vector<ICPConvergenceCriteria> warm_up_criteria = {
                ICPConvergenceCriteria(0.01, 0.01, 1)};
        result_ = RegistrationMultiScaleICP(
                pointclouds_device_[0].To(device_),
                pointclouds_device_[1].To(device_), voxel_sizes_, criterias_,
                search_radius_, transformation_, *estimation_);

        visualize_output_ = true;

        SetOnClose([this]() {
            is_done_ = true;
            return true;  // false would cancel the close
        });
        update_thread_ = std::thread([this]() { this->UpdateMain(); });
    }

    ~ExampleWindow() { update_thread_.join(); }

private:
    std::thread update_thread_;

    void UpdateMain() {
        core::Tensor initial_transform = core::Tensor::Eye(4, dtype_, device_);
        core::Tensor cumulative_transform = initial_transform.Clone();

        double total_time = 0;
        double total_reg_time = 0;
        double total_viz_time = 0;
        double total_transform_time = 0;

        // geometry::AxisAlignedBoundingBox bounds;
        auto mat = rendering::Material();

        // mat.shader = "unlitSolidColor";

        mat.shader = "defaultUnlit";
        mat.base_color = Eigen::Vector4f(1.f, 0.0f, 0.0f, 1.0f);
        mat.point_size = 5.0f;

        auto pointcloud_mat = rendering::Material();
        pointcloud_mat.shader = "unlitGradient";
        pointcloud_mat.scalar_min = -4.0;
        pointcloud_mat.scalar_max = 1.0;
        pointcloud_mat.point_size = 0.5f;

        pointcloud_mat.gradient = std::make_shared<
                rendering::Gradient>(std::vector<rendering::Gradient::Point>{
                rendering::Gradient::Point{0.000f, {0.0f, 0.0f, 1.0f, 1.0f}},
                rendering::Gradient::Point{0.125f, {0.0f, 0.5f, 1.0f, 1.0f}},
                rendering::Gradient::Point{0.250f, {0.0f, 1.0f, 1.0f, 1.0f}},
                rendering::Gradient::Point{0.375f, {0.0f, 1.0f, 0.5f, 1.0f}},
                rendering::Gradient::Point{0.500f, {0.0f, 1.0f, 0.0f, 1.0f}},
                rendering::Gradient::Point{0.625f, {0.5f, 1.0f, 0.0f, 1.0f}},
                rendering::Gradient::Point{0.750f, {1.0f, 1.0f, 0.0f, 1.0f}},
                rendering::Gradient::Point{0.875f, {1.0f, 0.5f, 0.0f, 1.0f}},
                rendering::Gradient::Point{1.000f, {1.0f, 0.0f, 0.0f, 1.0f}}});

        pcd_ = pointclouds_device_[0].CPU();
        pcd_.DeletePointAttr("normals");

        if (visualize_output_) {
            gui::Application::GetInstance().PostToMainThread(
                    this, [this, &mat, &pointcloud_mat]() {
                        // std::lock_guard<std::mutex> lock(cloud_lock_);
                        this->widget3d_->GetScene()->SetBackground(
                                {0, 0, 0, 1});

                        this->widget3d_->GetScene()->AddGeometry(
                                filenames_[0], &pcd_, pointcloud_mat);

                        this->widget3d_->GetScene()->GetScene()->AddGeometry(
                                CURRENT_CLOUD, pcd_, mat);

                        auto bbox =
                                this->widget3d_->GetScene()->GetBoundingBox();
                        auto center = bbox.GetCenter().cast<float>();
                        this->widget3d_->SetupCamera(verticalFoV, bbox, center);
                    });
        }

        for (int i = 0; i < end_range_ - 1; i++) {
            utility::Timer time_icp_odom_loop, time_total, time_transform;
            time_total.Start();
            auto source = pointclouds_device_[i].To(device_);
            auto target = pointclouds_device_[i + 1].To(device_);

            // target.DeletePointAttr("normals");

            time_icp_odom_loop.Start();
            auto result = RegistrationMultiScaleICP(
                    source, target, voxel_sizes_, criterias_, search_radius_,
                    initial_transform, *estimation_);
            time_icp_odom_loop.Stop();
            total_reg_time += time_icp_odom_loop.GetDuration();

            cumulative_transform = cumulative_transform.Matmul(
                    result.transformation_.Inverse().To(device_, dtype_));

            if (visualize_output_ && i < end_range_ - 3) {
                time_transform.Start();
                {
                    // std::lock_guard<std::mutex> lock(cloud_lock_);
                    pcd_ = target.Transform(cumulative_transform).CPU();
                    pcd_.DeletePointAttr("normals");
                }
                time_transform.Stop();
                total_transform_time += time_transform.GetDuration();

                gui::Application::GetInstance().PostToMainThread(
                        this,
                        [this, &mat, &pointcloud_mat, &i, &total_viz_time]() {
                            // std::lock_guard<std::mutex> lock(cloud_lock_);
                            utility::Timer time_viz;
                            time_viz.Start();

                            // this->widget3d_->GetScene()
                            //     ->GetScene()->RemoveGeometry(
                            //         CURRENT_CLOUD);

                            this->widget3d_->GetScene()
                                    ->GetScene()
                                    ->UpdateGeometry(
                                            CURRENT_CLOUD, pcd_,
                                            rendering::Scene::
                                                            kUpdatePointsFlag |
                                                    rendering::Scene::
                                                            kUpdateColorsFlag);

                            this->widget3d_->GetScene()->AddGeometry(
                                    filenames_[i], &pcd_, pointcloud_mat);

                            auto bbox = this->widget3d_->GetScene()
                                                ->GetBoundingBox();
                            auto center = bbox.GetCenter().cast<float>();
                            this->widget3d_->SetupCamera(verticalFoV, bbox,
                                                         center);
                            time_viz.Stop();
                            total_viz_time += time_viz.GetDuration();
                        });
            }

            time_total.Stop();
            total_time += time_total.GetDuration();
            std::cout << std::endl
                      << " FPS: " << 1000.0 / time_total.GetDuration();
        }
        std::cout << std::endl
                  << std::endl
                  << " Total Avg. FPS: "
                  << 1000.0 * (end_range_ - 1) / total_time << std::endl;
        std::cout << "   Total Avg. Time: " << total_time / (end_range_ - 1)
                  << std::endl;
        std::cout << "   Avg. ICP Processing Time: "
                  << total_reg_time / (end_range_ - 1) << std::endl;
        std::cout << "   Avg. Transformation Time: "
                  << total_transform_time / (end_range_ - 1) << std::endl;
        std::cout << "   Avg. Visualization update Time: "
                  << total_viz_time / (end_range_ - 1) << std::endl;
    }

private:
    // To read parameters from config file.
    void ReadConfigFile(const std::string& path_config) {
        std::ifstream cFile(path_config);
        std::vector<double> relative_fitness;
        std::vector<double> relative_rmse;
        std::vector<int> max_iterations;
        std::string verb;

        if (cFile.is_open()) {
            std::string line;
            while (getline(cFile, line)) {
                line.erase(std::remove_if(line.begin(), line.end(), isspace),
                           line.end());
                if (line[0] == '#' || line.empty()) continue;

                auto delimiterPos = line.find("=");
                auto name = line.substr(0, delimiterPos);
                auto value = line.substr(delimiterPos + 1);

                if (name == "dataset_path") {
                    path_dataset = value;
                } else if (name == "end_range") {
                    std::istringstream is(value);
                    end_range_ = std::stoi(value);
                } else if (name == "registration_method") {
                    registration_method_ = value;
                } else if (name == "criteria.relative_fitness") {
                    std::istringstream is(value);
                    relative_fitness.push_back(std::stod(value));
                } else if (name == "criteria.relative_rmse") {
                    std::istringstream is(value);
                    relative_rmse.push_back(std::stod(value));
                } else if (name == "criteria.max_iterations") {
                    std::istringstream is(value);
                    max_iterations.push_back(std::stoi(value));
                } else if (name == "voxel_size") {
                    std::istringstream is(value);
                    voxel_sizes_.push_back(std::stod(value));
                } else if (name == "search_radii") {
                    std::istringstream is(value);
                    search_radius_.push_back(std::stod(value));
                } else if (name == "verbosity") {
                    std::istringstream is(value);
                    verb = value;
                } else if (name == "ground_truth_tx") {
                    std::istringstream is(value);
                    gt_tx_ = std::stod(value);
                } else if (name == "ground_truth_ty") {
                    std::istringstream is(value);
                    gt_ty_ = std::stod(value);
                }
            }
        } else {
            std::cerr << "Couldn't open config file for reading.\n";
        }

        utility::LogInfo(" Dataset path: {}", path_dataset);
        if (end_range_ > 500) {
            utility::LogWarning(" Too large range. Memory might exceed.");
        }
        utility::LogInfo(" Range: 0 to {} pointcloud files in sequence.",
                         end_range_ - 1);
        utility::LogInfo(" Registrtion method: {}", registration_method_);
        std::cout << std::endl;

        std::cout << " Voxel Sizes: ";
        for (auto voxel_size : voxel_sizes_) std::cout << voxel_size << " ";
        std::cout << std::endl;

        std::cout << " Search Radius Sizes: ";
        for (auto search_radii : search_radius_)
            std::cout << search_radii << " ";
        std::cout << std::endl;

        std::cout << " ICPCriteria: " << std::endl;
        std::cout << "   Max Iterations: ";
        for (auto iteration : max_iterations) std::cout << iteration << " ";
        std::cout << std::endl;
        std::cout << "   Relative Fitness: ";
        for (auto fitness : relative_fitness) std::cout << fitness << " ";
        std::cout << std::endl;
        std::cout << "   Relative RMSE: ";
        for (auto rmse : relative_rmse) std::cout << rmse << " ";
        std::cout << std::endl;

        size_t length = voxel_sizes_.size();
        if (search_radius_.size() != length ||
            max_iterations.size() != length ||
            relative_fitness.size() != length ||
            relative_rmse.size() != length) {
            utility::LogError(
                    " Length of vector: voxel_sizes, search_sizes, "
                    "max_iterations, "
                    "relative_fitness, relative_rmse must be same.");
        }

        for (int i = 0; i < (int)length; i++) {
            auto criteria = ICPConvergenceCriteria(
                    relative_fitness[i], relative_rmse[i], max_iterations[i]);
            criterias_.push_back(criteria);
        }

        if (registration_method_ == "PointToPoint") {
            estimation_ =
                    std::make_shared<TransformationEstimationPointToPoint>();
        } else if (registration_method_ == "PointToPlane") {
            estimation_ =
                    std::make_shared<TransformationEstimationPointToPlane>();
        } else {
            utility::LogError(" Registration method {}, not implemented.",
                              registration_method_);
        }

        if (verb == "Debug") {
            verbosity_ = utility::VerbosityLevel::Debug;
        } else {
            verbosity_ = utility::VerbosityLevel::Info;
        }

        std::cout << " Config file read complete. " << std::endl;
    }

    // To perform required dtype conversion, normal estimation.
    std::vector<t::geometry::PointCloud> LoadTensorPointClouds() {
        for (int i = 0; i < end_range_; i++) {
            filenames_.push_back(path_dataset + std::to_string(i) +
                                 std::string(".pcd"));
        }

        std::vector<t::geometry::PointCloud> pointclouds_device(
                filenames_.size(), t::geometry::PointCloud(device_));

        try {
            int i = 0;
            t::geometry::PointCloud pointcloud_local;
            for (auto& path : filenames_) {
                // utility::LogInfo(" \rLOADING... {}", path);
                std::cout << " \rLOADING DATA... " << i * 100 / end_range_
                          << "%" << std::flush;
                t::io::ReadPointCloud(path, pointcloud_local,
                                      {"auto", false, false, true});

                // Dtype conversion to Float32. Currently only Float32
                // pointcloud is supported.
                for (std::string attr : {"points", "colors", "normals"}) {
                    if (pointcloud_local.HasPointAttr(attr)) {
                        pointcloud_local.SetPointAttr(
                                attr,
                                pointcloud_local.GetPointAttr(attr).To(dtype_));
                    }
                }

                pointcloud_local.SetPointAttr("__visualization_scalar",
                                              pointcloud_local.GetPoints()
                                                      .Slice(0, 0, -1)
                                                      .Slice(1, 2, 3)
                                                      .To(dtype_, true));

                // pointcloud_local.DeletePointAttr("__visualization_scalar");

                // Normal Estimation. Currenly Normal Estimation is not
                // supported by Tensor Pointcloud.
                if (registration_method_ == "PointToPlane" &&
                    !pointcloud_local.HasPointNormals()) {
                    std::cout << " hey " << std::endl;
                    auto pointcloud_legacy =
                            pointcloud_local.ToLegacyPointCloud();
                    pointcloud_legacy.EstimateNormals(
                            open3d::geometry::KDTreeSearchParamKNN(), false);
                    core::Tensor pointcloud_normals =
                            t::geometry::PointCloud::FromLegacyPointCloud(
                                    pointcloud_legacy)
                                    .GetPointNormals()
                                    .To(dtype_);
                    pointcloud_local.SetPointNormals(pointcloud_normals);
                }
                // Adding it to our vector of pointclouds.
                pointclouds_device[i++] =
                        pointcloud_local.To(device_).VoxelDownSample(0.75);
            }
        } catch (...) {
            utility::LogError(
                    " Failed to read pointcloud in sequence. Ensure pointcloud "
                    "files are present in the given dataset path in continuous "
                    "sequence from 0 to {}. Also, in case of large range, the "
                    "system might be going out-of-memory. ",
                    end_range_);
        }
        return pointclouds_device;
    }

private:
    std::mutex cloud_lock_;

    std::atomic<bool> is_done_;
    // std::shared_ptr<visualizer::O3DVisualizer> main_vis_;

    std::vector<open3d::t::geometry::PointCloud> pointclouds_device_;
    t::geometry::PointCloud pcd_;

private:
    std::string path_dataset;
    std::string registration_method_;
    std::vector<std::string> filenames_;
    utility::VerbosityLevel verbosity_;
    int end_range_;
    bool visualize_output_;

private:
    std::vector<double> voxel_sizes_;
    std::vector<double> search_radius_;
    std::vector<ICPConvergenceCriteria> criterias_;
    std::shared_ptr<TransformationEstimation> estimation_;

private:
    core::Tensor transformation_;
    t::pipelines::registration::RegistrationResult result_;

private:
    core::Device device_;
    core::Device host_;
    core::Dtype dtype_;

    double gt_tx_;
    double gt_ty_;
};

//------------------------------------------------------------------------------
int main(int argc, const char* argv[]) {
    if (argc < 3) {
        utility::LogError("Expected dataset path as input");
    }
    const std::string path_config = std::string(argv[2]);

    utility::SetVerbosityLevel(utility::VerbosityLevel::Info);

    auto& app = gui::Application::GetInstance();
    app.Initialize(argc, argv);
    app.AddWindow(std::make_shared<ExampleWindow>(path_config,
                                                  core::Device(argv[1])));
    app.Run();
    return 0;
}