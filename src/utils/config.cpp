/*
    TickTock is an open-source Time Series Database, maintained by
    Yongtao You (yongtao.you@gmail.com) and Yi Lin (ylin30@gmail.com).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <cassert>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include "global.h"
#include "config.h"
#include "timer.h"
#include "logger.h"
#include "utils.h"


namespace tt
{


std::mutex Config::m_lock;
std::map<std::string, std::shared_ptr<Property>> Config::m_properties;
std::map<std::string, std::shared_ptr<Property>> Config::m_overrides;


void
Config::init()
{
    TaskData data;  // not used by reload()
    reload(data);

    g_tstamp_resolution_ms = ts_resolution_ms();
    g_cluster_enabled = Config::exists(CFG_CLUSTER_SERVERS);
    g_self_meter_enabled = Config::get_bool(CFG_TSDB_SELF_METER_ENABLED, CFG_TSDB_SELF_METER_ENABLED_DEF);

    // schedule task to reload() periodically
    if (Config::get_bool(CFG_CONFIG_RELOAD_ENABLED, CFG_CONFIG_RELOAD_ENABLED_DEF))
    {
        Task task;
        task.doit = &Config::reload;
        int freq_sec =
            Config::get_time(CFG_CONFIG_RELOAD_FREQUENCY, TimeUnit::SEC, CFG_CONFIG_RELOAD_FREQUENCY_DEF);
        Timer::inst()->add_task(task, freq_sec, "config_reload");
    }
}

bool
Config::reload(TaskData& data)
{
    std::lock_guard<std::mutex> guard(m_lock);

    try
    {
        m_properties.clear();

        std::fstream file(g_config_file, std::ios::in);
        std::string line;

        if (! file.is_open())
            throw std::ios_base::failure("Failed to open config file for read");

        while (std::getline(file, line))
        {
            if (starts_with(line, ';')) continue;   // comments
            if (starts_with(line, '#')) continue;   // comments

            std::tuple<std::string,std::string> kv;
            if (! tokenize(line, kv, '=')) continue;

            std::string key = std::get<0>(kv);
            std::string value = std::get<1>(kv);
            set_value_no_lock(key, value);
        }

        // let command-line options override config file
        for (auto const& override: m_overrides)
            set_value_no_lock(override.first, override.second->as_str());
    }
    catch (std::exception& ex)
    {
        fprintf(stderr, "Failed to read config file %s: %s\n", g_config_file.c_str(), ex.what());
        throw;
    }

    return false;
}

void
Config::set_value(const std::string& name, const std::string& value)
{
    std::lock_guard<std::mutex> guard(m_lock);
    set_value_no_lock(name, value);
}

void
Config::set_value_no_lock(const std::string& name, const std::string& value)
{
    std::shared_ptr<Property> property = get_property(name);

    if (property == nullptr)
    {
        property = std::shared_ptr<Property>(new Property(name, value));
        m_properties[name] = property;
    }
    else
    {
        property->set_value(value);
    }
}

void
Config::add_override(const char *name, const char *value)
{
    auto search = m_overrides.find(name);
    if (search == m_overrides.end())
        m_overrides[name] = std::shared_ptr<Property>(new Property(name, value));
    else
        search->second->set_value(value);
}

bool
Config::exists(const std::string& name)
{
    std::lock_guard<std::mutex> guard(m_lock);
    return ((get_property(name) != nullptr) || (get_override(name) != nullptr));
}

bool
Config::get_bool(const std::string& name, bool def_value)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) return def_value;
    return property->as_bool();
}

int
Config::get_int(const std::string& name)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) throw std::exception();
    return property->as_int();
}

int
Config::get_int(const std::string& name, int def_value)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) return def_value;
    return property->as_int();
}

float
Config::get_float(const std::string& name)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) throw std::exception();
    return property->as_float();
}

float
Config::get_float(const std::string& name, float def_value)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) return def_value;
    return property->as_float();
}

const std::string &
Config::get_str(const std::string& name)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) return EMPTY_STD_STRING;   //throw std::exception();
    return property->as_str();
}

const std::string &
Config::get_str(const std::string& name, const std::string& def_value)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) return def_value;
    return property->as_str();
}

uint64_t
Config::get_bytes(const std::string& name)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) throw std::exception();
    return property->as_bytes();
}

uint64_t
Config::get_bytes(const std::string& name, const std::string& def_value)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr)
        return Property::as_bytes(def_value);
    else
        return property->as_bytes();
}

Timestamp
Config::get_time(const std::string& name, TimeUnit unit)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr) throw std::exception();
    return property->as_time(unit);
}

Timestamp
Config::get_time(const std::string& name, TimeUnit unit, const std::string& def_value)
{
    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Property> property = get_property(name);
    if (property == nullptr)
        return Property::as_time(def_value, unit);
    else
        return property->as_time(unit);
}

std::shared_ptr<Property>
Config::get_property(const std::string& name)
{
    auto search = m_properties.find(name);
    if (search == m_properties.end()) return nullptr;
    return search->second;
}

std::shared_ptr<Property>
Config::get_override(const std::string& name)
{
    auto search = m_overrides.find(name);
    if (search == m_overrides.end()) return nullptr;
    return search->second;
}

std::string
Config::get_data_dir()
{
    // if 'tsdb.data.dir' is specified, use it
    if (exists(CFG_TSDB_DATA_DIR))
        return get_str(CFG_TSDB_DATA_DIR);

    // if 'ticktock.home' is specified, use <ticktock.home>/data
    if (exists(CFG_TICKTOCK_HOME))
        return get_str(CFG_TICKTOCK_HOME) + "/data";

    // use 'cwd'/data
    return g_working_dir + "/data";
}

std::string
Config::get_log_dir()
{
    // if 'log.file' is specified, use it to find the log dir
    if (exists(CFG_LOG_FILE))
    {
        std::string log_file = Config::get_str(CFG_LOG_FILE);
        auto const pos = log_file.find_last_of('/');
        if (pos == std::string::npos)
            return g_working_dir;
        return log_file.substr(0, pos);
    }

    // if 'ticktock.home' is specified, use <ticktock.home>/log
    if (exists(CFG_TICKTOCK_HOME))
        return get_str(CFG_TICKTOCK_HOME) + "/log";

    // use 'cwd'/log
    return g_working_dir + "/log";
}

std::string
Config::get_log_file()
{
    // if 'log.file' is specified, use it
    if (exists(CFG_LOG_FILE))
        return Config::get_str(CFG_LOG_FILE);

    return get_log_dir() + "/ticktock.log";
}

/* The config could be '6181,6162', '6181,', ',6182', '6182',
 * or nothing at all (not specified).
 */
int
Config::get_count_internal(const char *name, int def_value, int which)
{
    ASSERT(name != nullptr);
    ASSERT((which == 0) || (which == 1));

    int count;

    if (Config::exists(name))
    {
        const std::string& str_count = Config::get_str(name);
        std::tuple<std::string,std::string> kv;

        if (tokenize(str_count, kv, ','))
        {
            // 2 counts present
            count = std::stoi((0 == which) ? std::get<0>(kv) : std::get<1>(kv));
        }
        else
            count = Config::get_int(name);
    }
    else
    {
        count = def_value;
    }

    return count;
}

int
Config::get_http_listener_count(int which)
{
    return get_count_internal(CFG_HTTP_LISTENER_COUNT, CFG_HTTP_LISTENER_COUNT_DEF, which);
}

int
Config::get_http_responders_per_listener(int which)
{
    return get_count_internal(CFG_HTTP_RESPONDERS_PER_LISTENER, CFG_HTTP_RESPONDERS_PER_LISTENER_DEF, which);
}

int
Config::get_tcp_listener_count(int which)
{
    return get_count_internal(CFG_TCP_LISTENER_COUNT, CFG_TCP_LISTENER_COUNT_DEF, which);
}

int
Config::get_tcp_responders_per_listener(int which)
{
    return get_count_internal(CFG_TCP_RESPONDERS_PER_LISTENER, CFG_TCP_RESPONDERS_PER_LISTENER_DEF, which);
}

const char *
Config::c_str(char *buff, size_t size)
{
    std::lock_guard<std::mutex> guard(m_lock);
    ASSERT(size > 0);
    ASSERT(buff != nullptr);

    int idx = 0;

    idx += std::snprintf(&buff[idx], size, "{\n");

    for (auto it = m_properties.begin(); (it != m_properties.end()) && (size > idx); it++)
    {
        std::shared_ptr<Property> property = it->second;

        int n = std::snprintf(&buff[idx], size-idx, "  \"%s\": \"%s\",\n",
            property->get_name().c_str(), property->as_str().c_str());

        if (n <= 0)
        {
            buff[0] = 0;
            break;
        }
        else
        {
            idx += n;
        }
    }

    std::snprintf(&buff[idx-2], size-idx+2, "\n}");
    return buff;
}


}
