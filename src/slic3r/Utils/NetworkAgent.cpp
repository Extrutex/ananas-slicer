#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <algorithm>

#include <boost/log/trivial.hpp>
#include "libslic3r/Utils.hpp"
#include "NetworkAgent.hpp"

namespace Slic3r {

namespace {

template<typename Fn>
int invoke_on_all_cloud_agents(const std::map<std::string, std::shared_ptr<ICloudServiceAgent>>& cloud_agents, Fn&& fn)
{
    if (cloud_agents.empty()) {
        return -1;
    }

    int result = 0;
    for (const auto& cloud_agent_pair : cloud_agents) {
        const int ret = fn(*cloud_agent_pair.second);
        if (result == 0 && ret != 0) {
            result = ret;
        }
    }

    return result;
}

} // namespace

// ============================================================================
// Constructors
// ============================================================================

NetworkAgent::NetworkAgent(std::shared_ptr<ICloudServiceAgent> cloud_agent)
{
    if (!cloud_agent) {
        BOOST_LOG_TRIVIAL(warning) << "Null cloud agent provided, skipping agent initialization";
        return;
    }
    if (cloud_agent->get_id().empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Invalid cloud agent with empty ID provided, skipping agent initialization";
        return;
    }
    m_cloud_agents.emplace(cloud_agent->get_id(), std::move(cloud_agent));
}

NetworkAgent::~NetworkAgent() = default;

void NetworkAgent::add_cloud_agent(const std::string& provider, std::shared_ptr<ICloudServiceAgent> agent)
{
    if (agent) {
        m_cloud_agents[provider] = std::move(agent);
    }
}

std::shared_ptr<ICloudServiceAgent> NetworkAgent::get_cloud_agent(const std::string& provider) const
{
    const auto& key = (provider.empty() || provider == ORCA_CLOUD_PROVIDER) ? ORCA_CLOUD_PROVIDER : provider;
    auto it = m_cloud_agents.find(key);
    return it != m_cloud_agents.end() ? it->second : nullptr;
}

// ============================================================================
// Shared agent methods
// ============================================================================

int NetworkAgent::set_queue_on_main_fn(QueueOnMainFn fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    return cloud_agent ? cloud_agent->set_queue_on_main_fn(fn) : -1;
}

// ============================================================================
// Cloud agent methods
// ============================================================================

int NetworkAgent::init_log()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.init_log(); });
}

int NetworkAgent::set_config_dir(std::string config_dir)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [&config_dir](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_config_dir(config_dir); });
}

int NetworkAgent::set_cert_file(std::string folder, std::string filename)
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [&folder, &filename](ICloudServiceAgent& cloud_agent) {
        return cloud_agent.set_cert_file(folder, filename);
    });
}

int NetworkAgent::set_country_code(std::string country_code)
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [&country_code](ICloudServiceAgent& cloud_agent) {
        return cloud_agent.set_country_code(country_code);
    });
}

int NetworkAgent::start()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.start(); });
}

int NetworkAgent::set_on_server_connected_fn(AppOnServerConnectedFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_on_server_connected_fn(fn); });
}

int NetworkAgent::set_on_http_error_fn(AppOnHttpErrorFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_on_http_error_fn(fn); });
}

int NetworkAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_get_country_code_fn(fn); });
}

int NetworkAgent::change_user(std::string user_info, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->change_user(std::move(user_info));
    return -1;
}

bool NetworkAgent::is_user_login(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->is_user_login();
    return false;
}

int NetworkAgent::user_logout(bool request, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->user_logout(request);
    return -1;
}

std::string NetworkAgent::get_user_id(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_id();
    return "";
}

std::string NetworkAgent::get_user_nickname(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_nickname();
    return "";
}

std::string NetworkAgent::build_login_cmd(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_login_cmd();
    return "";
}

std::string NetworkAgent::build_logout_cmd(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_logout_cmd();
    return "";
}

std::string NetworkAgent::build_login_info(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_login_info();
    return "";
}

std::string NetworkAgent::get_cloud_service_host(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_cloud_service_host();
    return "";
}

std::string NetworkAgent::get_cloud_login_url(const std::string& language, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_cloud_login_url(language);
    return "";
}

int NetworkAgent::connect_server()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.connect_server(); });
}

void NetworkAgent::enable_multi_machine(bool enable, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        cloud_agent->enable_multi_machine(enable);
}

int NetworkAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_presets(user_presets);
    return -1;
}

std::string NetworkAgent::request_setting_id(std::string                         name,
                                             std::map<std::string, std::string>* values_map,
                                             unsigned int*                       http_code,
                                             const std::string&                  provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->request_setting_id(std::move(name), values_map, http_code);
    return "";
}

int NetworkAgent::put_setting(std::string                         setting_id,
                              std::string                         name,
                              std::map<std::string, std::string>* values_map,
                              unsigned int*                       http_code,
                              const std::string&                  provider,
                              bool force)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_setting(std::move(setting_id), std::move(name), values_map, http_code, force);
    return -1;
}

int NetworkAgent::get_setting_list2(
    std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_setting_list2(std::move(bundle_version), chk_fn, pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::delete_setting(std::string setting_id, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->delete_setting(std::move(setting_id));
    return -1;
}

int NetworkAgent::get_user_tasks(TaskQueryParams params, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_tasks(params, http_body);
    return -1;
}

int NetworkAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_printer_firmware(std::move(dev_id), http_code, http_body);
    return -1;
}

int NetworkAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_camera_url(std::move(dev_id), std::move(callback));
    return -1;
}

int NetworkAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_subtask(task, getsub_fn);
    return -1;
}

int NetworkAgent::get_my_profile(std::string token, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_profile(std::move(token), http_code, http_body);
    return -1;
}

int NetworkAgent::get_my_token(std::string ticket, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_token(std::move(ticket), http_code, http_body);
    return -1;
}

} // namespace Slic3r
