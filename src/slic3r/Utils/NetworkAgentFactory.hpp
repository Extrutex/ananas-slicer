#pragma once

#include "ICloudServiceAgent.hpp"
#include "NetworkAgent.hpp"
#include "libslic3r/AppConfig.hpp"

#include <memory>
#include <string>

namespace Slic3r {

// Builds the cloud agents the NetworkAgent delegates to. Printers are not agents: G-code
// goes to them through PrintHost (Moonraker.cpp) and the filament changer is read through
// MoonrakerFilaments.
class NetworkAgentFactory
{
public:
    // The agent for one cloud provider, or null when the provider is unknown.
    static std::shared_ptr<ICloudServiceAgent> create_cloud_agent(const std::string& provider, const std::string& log_dir);

private:
    NetworkAgentFactory()                                      = delete;
    ~NetworkAgentFactory()                                     = delete;
    NetworkAgentFactory(const NetworkAgentFactory&)            = delete;
    NetworkAgentFactory& operator=(const NetworkAgentFactory&) = delete;
};

// The NetworkAgent of this app: the Orca cloud agent, configured from app_config, plus every
// third-party cloud provider app_config lists. With a null app_config the agent has no cloud.
std::unique_ptr<NetworkAgent> create_agent_from_config(const std::string& log_dir, AppConfig* app_config);

} // namespace Slic3r
