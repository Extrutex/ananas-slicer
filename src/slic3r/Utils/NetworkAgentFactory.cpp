#include "NetworkAgentFactory.hpp"

#include <boost/log/trivial.hpp>

#include "ICloudServiceAgent.hpp"
#include "OrcaCloudServiceAgent.hpp"

namespace Slic3r {

std::shared_ptr<ICloudServiceAgent> NetworkAgentFactory::create_cloud_agent(const std::string& provider, const std::string& log_dir)
{
    if (provider == ORCA_CLOUD_PROVIDER)
        return std::make_shared<OrcaCloudServiceAgent>(log_dir);
    return nullptr;
}

std::unique_ptr<NetworkAgent> create_agent_from_config(const std::string& log_dir, AppConfig* app_config)
{
    if (!app_config)
        return std::make_unique<NetworkAgent>(nullptr);

    auto cloud_agent = NetworkAgentFactory::create_cloud_agent(ORCA_CLOUD_PROVIDER, log_dir);
    if (!cloud_agent)
        BOOST_LOG_TRIVIAL(error) << "Failed to create cloud agent";

    auto agent = std::make_unique<NetworkAgent>(std::move(cloud_agent));

    if (auto* orca_cloud = dynamic_cast<OrcaCloudServiceAgent*>(agent->get_cloud_agent().get()))
        orca_cloud->configure_urls(app_config);

    for (const auto& provider : app_config->get_cloud_providers()) {
        if (provider == ORCA_CLOUD_PROVIDER)
            continue; // the primary agent above
        if (auto third_party_agent = NetworkAgentFactory::create_cloud_agent(provider, log_dir)) {
            agent->add_cloud_agent(provider, std::move(third_party_agent));
            BOOST_LOG_TRIVIAL(info) << "Initialized third-party cloud agent: " << provider;
        }
    }
    return agent;
}

} // namespace Slic3r
