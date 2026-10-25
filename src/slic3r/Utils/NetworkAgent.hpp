#ifndef __NETWORK_Agent_HPP__
#define __NETWORK_Agent_HPP__

#include "bambu_networking.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "ICloudServiceAgent.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

// The cloud side of the network layer: one ICloudServiceAgent per provider, the Orca cloud
// first. Printers are reached through PrintHost (Moonraker.cpp) and MoonrakerFilaments, not
// through an agent.
class NetworkAgent
{
public:
    explicit NetworkAgent(std::shared_ptr<ICloudServiceAgent> cloud_agent);

    ~NetworkAgent();

    std::shared_ptr<ICloudServiceAgent> get_cloud_agent(const std::string& provider = ORCA_CLOUD_PROVIDER) const;

    void add_cloud_agent(const std::string& provider, std::shared_ptr<ICloudServiceAgent> agent);
    int set_queue_on_main_fn(QueueOnMainFn fn, const std::string& provider = ORCA_CLOUD_PROVIDER);

    // Cloud agent methods
    // These methods will be forwarded to all cloud agents
    int init_log();
    int set_config_dir(std::string config_dir);
    int set_cert_file(std::string folder, std::string filename);
    int set_country_code(std::string country_code);
    int start();
    int set_on_server_connected_fn(AppOnServerConnectedFn fn);
    int set_on_http_error_fn(AppOnHttpErrorFn fn);
    int set_get_country_code_fn(GetCountryCodeFn fn);
    int connect_server();

    int change_user(std::string user_info, const std::string& provider = ORCA_CLOUD_PROVIDER);
    bool is_user_login(const std::string& provider = ORCA_CLOUD_PROVIDER);
    int user_logout(bool request = false, const std::string& provider = ORCA_CLOUD_PROVIDER);
    std::string get_user_id(const std::string& provider = ORCA_CLOUD_PROVIDER);
    std::string get_user_nickname(const std::string& provider = ORCA_CLOUD_PROVIDER);
    std::string build_login_cmd(const std::string& provider = ORCA_CLOUD_PROVIDER);
    std::string build_logout_cmd(const std::string& provider = ORCA_CLOUD_PROVIDER);
    std::string build_login_info(const std::string& provider = ORCA_CLOUD_PROVIDER);
    std::string get_cloud_service_host(const std::string& provider = ORCA_CLOUD_PROVIDER);
    std::string get_cloud_login_url(const std::string& language = "", const std::string& provider = ORCA_CLOUD_PROVIDER);

    void enable_multi_machine(bool enable, const std::string& provider = ORCA_CLOUD_PROVIDER);

    // Profile synchronization methods
    // NOTE: this should always call only OrcaCloud
    int get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets, const std::string& provider = ORCA_CLOUD_PROVIDER);
    std::string request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code, const std::string& provider = ORCA_CLOUD_PROVIDER);
    int put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code, const std::string& provider = ORCA_CLOUD_PROVIDER, bool force = false);
    int get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr, const std::string& provider = ORCA_CLOUD_PROVIDER);
    int delete_setting(std::string setting_id, const std::string& provider = ORCA_CLOUD_PROVIDER);

    int get_user_tasks(TaskQueryParams params, std::string* http_body, const std::string& provider = ORCA_CLOUD_PROVIDER);
    int get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body, const std::string& provider = ORCA_CLOUD_PROVIDER);
    int get_camera_url(std::string dev_id, std::function<void(std::string)> callback, const std::string& provider = ORCA_CLOUD_PROVIDER);
    int get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn, const std::string& provider = ORCA_CLOUD_PROVIDER);
    int get_my_profile(std::string token, unsigned int* http_code, std::string* http_body, const std::string& provider = ORCA_CLOUD_PROVIDER);
    int get_my_token(std::string ticket, unsigned int* http_code, std::string* http_body, const std::string& provider = ORCA_CLOUD_PROVIDER);

private:
    // The Orca cloud agent is fixed at construction since it is tied to the user's account;
    // third-party cloud agents can be added and are delegated to by provider name.
    std::map<std::string, std::shared_ptr<ICloudServiceAgent>> m_cloud_agents;
};

}

#endif
