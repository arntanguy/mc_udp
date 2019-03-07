#include <mc_udp/client/Client.h>

#include <mc_control/mc_global_controller.h>
#include <mc_rbdyn/rpy_utils.h>

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <signal.h>

static bool running = true;

void handler(int)
{
  running = false;
}

void cli(mc_control::MCGlobalController & ctl)
{
  while(running)
  {
    std::string ui;
    std::getline(std::cin, ui);
    std::stringstream ss;
    ss << ui;
    std::string token;
    ss >> token;
    if(token == "stop")
    {
      LOG_INFO("Stopping connection")
      running = false;
    }
    else if(token == "hs" ||
            token == "GoToHalfSitPose" ||
            token == "half_sitting")
    {
      ctl.GoToHalfSitPose_service();
    }
    else
    {
      std::cerr << "Unkwown command " << token << std::endl;
    }
  }
}

int main(int argc, char * argv[])
{
  std::string conf_file = "";
  std::string host = "";
  int port = 4444;

  po::options_description desc("MCControlTCP options");
  desc.add_options()
    ("help", "Display help message")
    ("host,h", po::value<std::string>(&host), "Connection host")
    ("port,p", po::value<int>(&port), "Connection port")
    ("conf,f", po::value<std::string>(&conf_file), "Configuration file");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  if(vm.count("help"))
  {
    std::cout << desc << std::endl;
    return 1;
  }

  mc_control::MCGlobalController controller(conf_file);
  mc_rtc::Configuration config = controller.configuration().config("UDP", mc_rtc::Configuration{});
  if(!vm.count("host"))
  {
    config("host", host);
  }
  if(!vm.count("port"))
  {
    config("port", port);
  }
  LOG_INFO("Connecting UDP sensors client to " << host << ":" << port)
  mc_udp::Client sensorsClient(host, port);
  LOG_INFO("Connecting UDP control client to " << host << ":" << port + 1)
  mc_udp::Client controlClient(host, port + 1);
  bool init = false;
  // RTC port to robot force sensors
  std::unordered_map<std::string, std::string> fsensors;
  fsensors["rfsensor"] = "RightFootForceSensor";
  fsensors["lfsensor"] = "LeftFootForceSensor";
  fsensors["rhsensor"] = "RightHandForceSensor";
  fsensors["lhsensor"] = "LeftHandForceSensor";
  std::map<std::string, sva::ForceVecd> wrenches;
  // FIXME Write gripper control
  std::vector<std::string> ignoredJoints;
  for(const auto & g : controller.gripperJoints())
  {
    for(const auto & j : g.second)
    {
      ignoredJoints.push_back(j);
    }
  }
  uint64_t prev_id = 0;
  std::vector<double> qInit;
  using duration_ms = std::chrono::duration<double, std::milli>;
  duration_ms tcp_run_dt{0};
  controller.controller().logger().addLogEntry("perf_TCP", [&tcp_run_dt]() { return tcp_run_dt.count(); });
  signal(SIGINT, handler);
  std::thread cli_thread([&controller](){ cli(controller); });
  while(running)
  {
    using clock = typename std::conditional<std::chrono::high_resolution_clock::is_steady,
                                            std::chrono::high_resolution_clock, std::chrono::steady_clock>::type;
    if(sensorsClient.recv())
    {
      auto start = clock::now();
      controller.setEncoderValues(sensorsClient.sensors().encoders);
      controller.setJointTorques(sensorsClient.sensors().torques);
      Eigen::Vector3d rpy;
      rpy << sensorsClient.sensors().orientation[0], sensorsClient.sensors().orientation[1], sensorsClient.sensors().orientation[2];
      controller.setSensorOrientation(Eigen::Quaterniond(mc_rbdyn::rpyToMat(rpy)));
      Eigen::Vector3d vel;
      vel << sensorsClient.sensors().angularVelocity[0], sensorsClient.sensors().angularVelocity[1], sensorsClient.sensors().angularVelocity[2];
      controller.setSensorAngularVelocity(vel);
      Eigen::Vector3d acc;
      acc << sensorsClient.sensors().angularAcceleration[0], sensorsClient.sensors().angularAcceleration[1], sensorsClient.sensors().angularAcceleration[2];
      controller.setSensorAcceleration(acc);
      for(const auto & fs : sensorsClient.sensors().fsensors)
      {
        Eigen::Vector6d reading;
        reading << fs.reading[3], fs.reading[4], fs.reading[5], fs.reading[0], fs.reading[1], fs.reading[2];
        wrenches[fsensors.at(fs.name)] = sva::ForceVecd(reading);
      }
      controller.setWrenches(wrenches);
      if(!init)
      {
        auto init_start = clock::now();
        qInit = sensorsClient.sensors().encoders;
        controller.init(qInit);
        controller.running = true;
        init = true;
        auto init_end = clock::now();
        duration_ms init_dt = init_end - init_start;
        LOG_INFO("[MCUDPControl] Init duration " << init_dt.count())
        sensorsClient.init();
        controlClient.init();
      }
      else
      {
        if(prev_id + 1 != sensorsClient.sensors().id)
        {
          LOG_WARNING("[MCUDPControl] Missed one or more sensors reading (previous id: " << prev_id << ", current id: " << sensorsClient.sensors().id << ")")
        }
        if(controller.run())
        {
          const auto & robot = controller.robot();
          const auto & mbc = robot.mbc();
          const auto & rjo = controller.ref_joint_order();
          auto & qOut = controlClient.control().encoders;
          if(qOut.size() != rjo.size())
          {
            qOut.resize(rjo.size());
          }
          for(size_t i = 0; i < rjo.size(); ++i)
          {
            const auto & jN = rjo[i];
            bool skipJoint = false;
            if(std::find(ignoredJoints.begin(), ignoredJoints.end(), jN) == ignoredJoints.end())
            {
              if(robot.hasJoint(jN))
              {
                auto jIndex = robot.jointIndexByName(jN);
                if(mbc.q[jIndex].size() == 1)
                {
                  qOut[i] = mbc.q[robot.jointIndexByName(jN)][0];
                }
                else
                {
                  skipJoint = true;
                }
              }
            }
            else
            {
              skipJoint = true;
            }
            if(skipJoint)
            {
              qOut[i] = qInit[i];
            }
          }
          controlClient.control().id = sensorsClient.sensors().id;
          controlClient.send();
        }
      }
      prev_id = sensorsClient.sensors().id;
      tcp_run_dt = clock::now() - start;
    }
  }
  return 0;
}
