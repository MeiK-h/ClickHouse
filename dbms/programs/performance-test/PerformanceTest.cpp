#include <functional>
#include <iostream>
#include <limits>
#include <regex>
#include <thread>
#include <port/unistd.h>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <sys/stat.h>

#include <common/DateLUT.h>
#include <AggregateFunctions/ReservoirSampler.h>
#include <Client/Connection.h>
#include <Common/ConcurrentBoundedQueue.h>
#include <Common/Stopwatch.h>
#include <Common/getFQDNOrHostName.h>
#include <Common/getMultipleKeysFromConfig.h>
#include <Common/getNumberOfPhysicalCPUCores.h>
#include <Core/Types.h>
#include <DataStreams/RemoteBlockInputStream.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/ConnectionTimeouts.h>
#include <IO/UseSSL.h>
#include <Interpreters/Settings.h>
#include <common/ThreadPool.h>
#include <common/getMemoryAmount.h>
#include <Poco/AutoPtr.h>
#include <Poco/Exception.h>
#include <Poco/SAX/InputSource.h>
#include <Poco/Util/XMLConfiguration.h>
#include <Poco/XML/XMLStream.h>
#include <Poco/Util/Application.h>
#include <Common/InterruptListener.h>
#include <Common/Config/configReadClient.h>

#include "JSONString.h"
#include "StopConditionsSet.h"
#include "TestStopConditions.h"
#include "TestStats.h"

#ifndef __clang__
#pragma GCC optimize("-fno-var-tracking-assignments")
#endif


/** Tests launcher for ClickHouse.
  * The tool walks through given or default folder in order to find files with
  * tests' descriptions and launches it.
  */
namespace fs = boost::filesystem;
using String = std::string;
const std::regex QUOTE_REGEX{"\""};

namespace DB
{
namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int FILE_DOESNT_EXIST;
}


using ConfigurationPtr = Poco::AutoPtr<Poco::Util::AbstractConfiguration>;

class PerformanceTest : public Poco::Util::Application
{
public:
    using Strings = std::vector<String>;

    PerformanceTest(const String & host_,
        const UInt16 port_,
        const bool secure_,
        const String & default_database_,
        const String & user_,
        const String & password_,
        const bool lite_output_,
        const String & profiles_file_,
        Strings && input_files_,
        Strings && tests_tags_,
        Strings && skip_tags_,
        Strings && tests_names_,
        Strings && skip_names_,
        Strings && tests_names_regexp_,
        Strings && skip_names_regexp_,
        const ConnectionTimeouts & timeouts)
        : connection(host_, port_, default_database_, user_, password_, timeouts, "performance-test", Protocol::Compression::Enable, secure_ ? Protocol::Secure::Enable : Protocol::Secure::Disable),
          gotSIGINT(false),
          lite_output(lite_output_),
          profiles_file(profiles_file_),
          input_files(input_files_),
          tests_tags(std::move(tests_tags_)),
          skip_tags(std::move(skip_tags_)),
          tests_names(std::move(tests_names_)),
          skip_names(std::move(skip_names_)),
          tests_names_regexp(std::move(tests_names_regexp_)),
          skip_names_regexp(std::move(skip_names_regexp_))
    {
        if (input_files.size() < 1)
        {
            throw DB::Exception("No tests were specified", DB::ErrorCodes::BAD_ARGUMENTS);
        }
    }

    void initialize(Poco::Util::Application & self [[maybe_unused]])
    {
        std::string home_path;
        const char * home_path_cstr = getenv("HOME");
        if (home_path_cstr)
            home_path = home_path_cstr;
        configReadClient(Poco::Util::Application::instance().config(), home_path);
    }

    int main(const std::vector < std::string > & /* args */)
    {
        std::string name;
        UInt64 version_major;
        UInt64 version_minor;
        UInt64 version_patch;
        UInt64 version_revision;
        connection.getServerVersion(name, version_major, version_minor, version_patch, version_revision);

        std::stringstream ss;
        ss << version_major << "." << version_minor << "." << version_patch;
        server_version = ss.str();

        processTestsConfigurations(input_files);

        return 0;
    }

private:
    String test_name;

    using Query = String;
    using Queries = std::vector<Query>;
    using QueriesWithIndexes = std::vector<std::pair<Query, size_t>>;
    Queries queries;

    Connection connection;
    std::string server_version;

    using Keys = std::vector<String>;

    Settings settings;
    Context global_context = Context::createGlobal();

    InterruptListener interrupt_listener;

    using XMLConfiguration = Poco::Util::XMLConfiguration;
    using XMLConfigurationPtr = Poco::AutoPtr<XMLConfiguration>;

    using Paths = std::vector<String>;
    using StringToVector = std::map<String, std::vector<String>>;
    using StringToMap = std::map<String, StringToVector>;
    StringToMap substitutions;

    using StringKeyValue = std::map<String, String>;
    std::vector<StringKeyValue> substitutions_maps;

    bool gotSIGINT;
    std::vector<TestStopConditions> stop_conditions_by_run;
    String main_metric;
    bool lite_output;
    String profiles_file;

    Strings input_files;
    std::vector<XMLConfigurationPtr> tests_configurations;

    Strings tests_tags;
    Strings skip_tags;
    Strings tests_names;
    Strings skip_names;
    Strings tests_names_regexp;
    Strings skip_names_regexp;

    enum class ExecutionType
    {
        Loop,
        Once
    };
    ExecutionType exec_type;

    enum class FilterType
    {
        Tag,
        Name,
        Name_regexp
    };

    size_t times_to_run = 1;
    std::vector<TestStats> statistics_by_run;

    /// Removes configurations that has a given value. If leave is true, the logic is reversed.
    void removeConfigurationsIf(
        std::vector<XMLConfigurationPtr> & configs, FilterType filter_type, const Strings & values, bool leave = false)
    {
        auto checker = [&filter_type, &values, &leave](XMLConfigurationPtr & config)
        {
            if (values.size() == 0)
                return false;

            bool remove_or_not = false;

            if (filter_type == FilterType::Tag)
            {
                Keys tags_keys;
                config->keys("tags", tags_keys);

                Strings tags(tags_keys.size());
                for (size_t i = 0; i != tags_keys.size(); ++i)
                    tags[i] = config->getString("tags.tag[" + std::to_string(i) + "]");

                for (const String & config_tag : tags)
                {
                    if (std::find(values.begin(), values.end(), config_tag) != values.end())
                        remove_or_not = true;
                }
            }

            if (filter_type == FilterType::Name)
            {
                remove_or_not = (std::find(values.begin(), values.end(), config->getString("name", "")) != values.end());
            }

            if (filter_type == FilterType::Name_regexp)
            {
                String config_name = config->getString("name", "");
                auto regex_checker = [&config_name](const String & name_regexp)
                {
                    std::regex pattern(name_regexp);
                    return std::regex_search(config_name, pattern);
                };

                remove_or_not = config->has("name") ? (std::find_if(values.begin(), values.end(), regex_checker) != values.end()) : false;
            }

            if (leave)
                remove_or_not = !remove_or_not;
            return remove_or_not;
        };

        auto new_end = std::remove_if(configs.begin(), configs.end(), checker);
        configs.erase(new_end, configs.end());
    }

    /// Filter tests by tags, names, regexp matching, etc.
    void filterConfigurations()
    {
        /// Leave tests:
        removeConfigurationsIf(tests_configurations, FilterType::Tag, tests_tags, true);
        removeConfigurationsIf(tests_configurations, FilterType::Name, tests_names, true);
        removeConfigurationsIf(tests_configurations, FilterType::Name_regexp, tests_names_regexp, true);


        /// Skip tests
        removeConfigurationsIf(tests_configurations, FilterType::Tag, skip_tags, false);
        removeConfigurationsIf(tests_configurations, FilterType::Name, skip_names, false);
        removeConfigurationsIf(tests_configurations, FilterType::Name_regexp, skip_names_regexp, false);
    }

    /// Checks specified preconditions per test (process cache, table existence, etc.)
    bool checkPreconditions(const XMLConfigurationPtr & config)
    {
        if (!config->has("preconditions"))
            return true;

        Keys preconditions;
        config->keys("preconditions", preconditions);
        size_t table_precondition_index = 0;

        for (const String & precondition : preconditions)
        {
            if (precondition == "flush_disk_cache")
            {
                if (system(
                        "(>&2 echo 'Flushing disk cache...') && (sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches') && (>&2 echo 'Flushed.')"))
                {
                    std::cerr << "Failed to flush disk cache" << std::endl;
                    return false;
                }
            }

            if (precondition == "ram_size")
            {
                size_t ram_size_needed = config->getUInt64("preconditions.ram_size");
                size_t actual_ram = getMemoryAmount();
                if (!actual_ram)
                    throw DB::Exception("ram_size precondition not available on this platform", DB::ErrorCodes::NOT_IMPLEMENTED);

                if (ram_size_needed > actual_ram)
                {
                    std::cerr << "Not enough RAM: need = " << ram_size_needed << ", present = " << actual_ram << std::endl;
                    return false;
                }
            }

            if (precondition == "table_exists")
            {
                String precondition_key = "preconditions.table_exists[" + std::to_string(table_precondition_index++) + "]";
                String table_to_check = config->getString(precondition_key);
                String query = "EXISTS TABLE " + table_to_check + ";";

                size_t exist = 0;

                connection.sendQuery(query, "", QueryProcessingStage::Complete, &settings, nullptr, false);

                while (true)
                {
                    Connection::Packet packet = connection.receivePacket();

                    if (packet.type == Protocol::Server::Data)
                    {
                        for (const ColumnWithTypeAndName & column : packet.block)
                        {
                            if (column.name == "result" && column.column->size() > 0)
                            {
                                exist = column.column->get64(0);
                                if (exist)
                                    break;
                            }
                        }
                    }

                    if (packet.type == Protocol::Server::Exception || packet.type == Protocol::Server::EndOfStream)
                        break;
                }

                if (!exist)
                {
                    std::cerr << "Table " << table_to_check << " doesn't exist" << std::endl;
                    return false;
                }
            }
        }

        return true;
    }

    void processTestsConfigurations(const Paths & paths)
    {
        tests_configurations.resize(paths.size());

        for (size_t i = 0; i != paths.size(); ++i)
        {
            const String path = paths[i];
            tests_configurations[i] = XMLConfigurationPtr(new XMLConfiguration(path));
        }

        filterConfigurations();

        if (tests_configurations.size())
        {
            Strings outputs;

            for (auto & test_config : tests_configurations)
            {
                if (!checkPreconditions(test_config))
                {
                    std::cerr << "Preconditions are not fulfilled for test '" + test_config->getString("name", "") + "' ";
                    continue;
                }

                String output = runTest(test_config);
                if (lite_output)
                    std::cout << output;
                else
                    outputs.push_back(output);
            }

            if (!lite_output && outputs.size())
            {
                std::cout << "[" << std::endl;

                for (size_t i = 0; i != outputs.size(); ++i)
                {
                    std::cout << outputs[i];
                    if (i != outputs.size() - 1)
                        std::cout << ",";

                    std::cout << std::endl;
                }

                std::cout << "]" << std::endl;
            }
        }
    }

    void extractSettings(
        const XMLConfigurationPtr & config, const String & key, const Strings & settings_list, std::map<String, String> & settings_to_apply)
    {
        for (const String & setup : settings_list)
        {
            if (setup == "profile")
                continue;

            String value = config->getString(key + "." + setup);
            if (value.empty())
                value = "true";

            settings_to_apply[setup] = value;
        }
    }

    String runTest(XMLConfigurationPtr & test_config)
    {
        queries.clear();

        test_name = test_config->getString("name");
        std::cerr << "Running: " << test_name << "\n";

        if (test_config->has("settings"))
        {
            std::map<String, String> settings_to_apply;
            Keys config_settings;
            test_config->keys("settings", config_settings);

            /// Preprocess configuration file
            if (std::find(config_settings.begin(), config_settings.end(), "profile") != config_settings.end())
            {
                if (!profiles_file.empty())
                {
                    String profile_name = test_config->getString("settings.profile");
                    XMLConfigurationPtr profiles_config(new XMLConfiguration(profiles_file));

                    Keys profile_settings;
                    profiles_config->keys("profiles." + profile_name, profile_settings);

                    extractSettings(profiles_config, "profiles." + profile_name, profile_settings, settings_to_apply);
                }
            }

            extractSettings(test_config, "settings", config_settings, settings_to_apply);

            /// This macro goes through all settings in the Settings.h
            /// and, if found any settings in test's xml configuration
            /// with the same name, sets its value to settings
            std::map<String, String>::iterator it;
#define EXTRACT_SETTING(TYPE, NAME, DEFAULT, DESCRIPTION) \
    it = settings_to_apply.find(#NAME);      \
    if (it != settings_to_apply.end())       \
        settings.set(#NAME, settings_to_apply[#NAME]);

            APPLY_FOR_SETTINGS(EXTRACT_SETTING)

#undef EXTRACT_SETTING

            if (std::find(config_settings.begin(), config_settings.end(), "average_rows_speed_precision") != config_settings.end())
            {
                TestStats::avg_rows_speed_precision = test_config->getDouble("settings.average_rows_speed_precision");
            }

            if (std::find(config_settings.begin(), config_settings.end(), "average_bytes_speed_precision") != config_settings.end())
            {
                TestStats::avg_bytes_speed_precision = test_config->getDouble("settings.average_bytes_speed_precision");
            }
        }

        if (!test_config->has("query") && !test_config->has("query_file"))
        {
            throw DB::Exception("Missing query fields in test's config: " + test_name, DB::ErrorCodes::BAD_ARGUMENTS);
        }

        if (test_config->has("query") && test_config->has("query_file"))
        {
            throw DB::Exception("Found both query and query_file fields. Choose only one", DB::ErrorCodes::BAD_ARGUMENTS);
        }

        if (test_config->has("query"))
        {
            queries = DB::getMultipleValuesFromConfig(*test_config, "", "query");
        }

        if (test_config->has("query_file"))
        {
            const String filename = test_config->getString("query_file");
            if (filename.empty())
                throw DB::Exception("Empty file name", DB::ErrorCodes::BAD_ARGUMENTS);

            bool tsv = fs::path(filename).extension().string() == ".tsv";

            ReadBufferFromFile query_file(filename);
            Query query;

            if (tsv)
            {
                while (!query_file.eof())
                {
                    readEscapedString(query, query_file);
                    assertChar('\n', query_file);
                    queries.push_back(query);
                }
            }
            else
            {
                readStringUntilEOF(query, query_file);
                queries.push_back(query);
            }
        }

        if (queries.empty())
        {
            throw DB::Exception("Did not find any query to execute: " + test_name, DB::ErrorCodes::BAD_ARGUMENTS);
        }

        if (test_config->has("substitutions"))
        {
            /// Make "subconfig" of inner xml block
            ConfigurationPtr substitutions_view(test_config->createView("substitutions"));
            constructSubstitutions(substitutions_view, substitutions[test_name]);

            auto queries_pre_format = queries;
            queries.clear();
            for (const auto & query : queries_pre_format)
            {
                auto formatted = formatQueries(query, substitutions[test_name]);
                queries.insert(queries.end(), formatted.begin(), formatted.end());
            }
        }

        if (!test_config->has("type"))
        {
            throw DB::Exception("Missing type property in config: " + test_name, DB::ErrorCodes::BAD_ARGUMENTS);
        }

        String config_exec_type = test_config->getString("type");
        if (config_exec_type == "loop")
            exec_type = ExecutionType::Loop;
        else if (config_exec_type == "once")
            exec_type = ExecutionType::Once;
        else
            throw DB::Exception("Unknown type " + config_exec_type + " in :" + test_name, DB::ErrorCodes::BAD_ARGUMENTS);

        times_to_run = test_config->getUInt("times_to_run", 1);

        stop_conditions_by_run.clear();
        TestStopConditions stop_conditions_template;
        if (test_config->has("stop_conditions"))
        {
            ConfigurationPtr stop_conditions_config(test_config->createView("stop_conditions"));
            stop_conditions_template.loadFromConfig(stop_conditions_config);
        }

        if (stop_conditions_template.empty())
            throw DB::Exception("No termination conditions were found in config", DB::ErrorCodes::BAD_ARGUMENTS);

        for (size_t i = 0; i < times_to_run * queries.size(); ++i)
            stop_conditions_by_run.push_back(stop_conditions_template);


        ConfigurationPtr metrics_view(test_config->createView("metrics"));
        Keys metrics;
        metrics_view->keys(metrics);

        main_metric.clear();
        if (test_config->has("main_metric"))
        {
            Keys main_metrics;
            test_config->keys("main_metric", main_metrics);
            if (main_metrics.size())
                main_metric = main_metrics[0];
        }

        if (!main_metric.empty())
        {
            if (std::find(metrics.begin(), metrics.end(), main_metric) == metrics.end())
                metrics.push_back(main_metric);
        }
        else
        {
            if (metrics.empty())
                throw DB::Exception("You shoud specify at least one metric", DB::ErrorCodes::BAD_ARGUMENTS);
            main_metric = metrics[0];
            if (lite_output)
                throw DB::Exception("Specify main_metric for lite output", DB::ErrorCodes::BAD_ARGUMENTS);
        }

        if (metrics.size() > 0)
            checkMetricsInput(metrics);

        statistics_by_run.resize(times_to_run * queries.size());
        for (size_t number_of_launch = 0; number_of_launch < times_to_run; ++number_of_launch)
        {
            QueriesWithIndexes queries_with_indexes;

            for (size_t query_index = 0; query_index < queries.size(); ++query_index)
            {
                size_t statistic_index = number_of_launch * queries.size() + query_index;
                stop_conditions_by_run[statistic_index].reset();

                queries_with_indexes.push_back({queries[query_index], statistic_index});
            }

            if (interrupt_listener.check())
                gotSIGINT = true;

            if (gotSIGINT)
                break;

            runQueries(queries_with_indexes);
        }

        if (lite_output)
            return minOutput();
        else
            return constructTotalInfo(metrics);
    }

    void checkMetricsInput(const Strings & metrics) const
    {
        std::vector<String> loop_metrics
            = {"min_time", "quantiles", "total_time", "queries_per_second", "rows_per_second", "bytes_per_second"};

        std::vector<String> non_loop_metrics
            = {"max_rows_per_second", "max_bytes_per_second", "avg_rows_per_second", "avg_bytes_per_second"};

        if (exec_type == ExecutionType::Loop)
        {
            for (const String & metric : metrics)
                if (std::find(non_loop_metrics.begin(), non_loop_metrics.end(), metric) != non_loop_metrics.end())
                   throw DB::Exception("Wrong type of metric for loop execution type (" + metric + ")", DB::ErrorCodes::BAD_ARGUMENTS);
        }
        else
        {
            for (const String & metric : metrics)
                if (std::find(loop_metrics.begin(), loop_metrics.end(), metric) != loop_metrics.end())
                    throw DB::Exception("Wrong type of metric for non-loop execution type (" + metric + ")", DB::ErrorCodes::BAD_ARGUMENTS);
        }
    }

    void runQueries(const QueriesWithIndexes & queries_with_indexes)
    {
        for (const auto & [query, run_index] : queries_with_indexes)
        {
            TestStopConditions & stop_conditions = stop_conditions_by_run[run_index];
            TestStats & statistics = statistics_by_run[run_index];

            statistics.clear();
            try
            {
                execute(query, statistics, stop_conditions);

                if (exec_type == ExecutionType::Loop)
                {
                    for (size_t iteration = 1; !gotSIGINT; ++iteration)
                    {
                        stop_conditions.reportIterations(iteration);
                        if (stop_conditions.areFulfilled())
                            break;

                        execute(query, statistics, stop_conditions);
                    }
                }
            }
            catch (const DB::Exception & e)
            {
                statistics.exception = e.what() + String(", ") + e.displayText();
            }

            if (!gotSIGINT)
            {
                statistics.ready = true;
            }
        }
    }

    void execute(const Query & query, TestStats & statistics, TestStopConditions & stop_conditions)
    {
        statistics.watch_per_query.restart();
        statistics.last_query_was_cancelled = false;
        statistics.last_query_rows_read = 0;
        statistics.last_query_bytes_read = 0;

        RemoteBlockInputStream stream(connection, query, {}, global_context, &settings);

        stream.setProgressCallback(
            [&](const Progress & value) { this->checkFulfilledConditionsAndUpdate(value, stream, statistics, stop_conditions); });

        stream.readPrefix();
        while (Block block = stream.read())
            ;
        stream.readSuffix();

        if (!statistics.last_query_was_cancelled)
            statistics.updateQueryInfo();

        statistics.setTotalTime();
    }

    void checkFulfilledConditionsAndUpdate(
        const Progress & progress, RemoteBlockInputStream & stream, TestStats & statistics, TestStopConditions & stop_conditions)
    {
        statistics.add(progress.rows, progress.bytes);

        stop_conditions.reportRowsRead(statistics.total_rows_read);
        stop_conditions.reportBytesReadUncompressed(statistics.total_bytes_read);
        stop_conditions.reportTotalTime(statistics.watch.elapsed() / (1000 * 1000));
        stop_conditions.reportMinTimeNotChangingFor(statistics.min_time_watch.elapsed() / (1000 * 1000));
        stop_conditions.reportMaxSpeedNotChangingFor(statistics.max_rows_speed_watch.elapsed() / (1000 * 1000));
        stop_conditions.reportAverageSpeedNotChangingFor(statistics.avg_rows_speed_watch.elapsed() / (1000 * 1000));

        if (stop_conditions.areFulfilled())
        {
            statistics.last_query_was_cancelled = true;
            stream.cancel(false);
        }

        if (interrupt_listener.check())
        {
            gotSIGINT = true;
            statistics.last_query_was_cancelled = true;
            stream.cancel(false);
        }
    }

    void constructSubstitutions(ConfigurationPtr & substitutions_view, StringToVector & out_substitutions)
    {
        Keys xml_substitutions;
        substitutions_view->keys(xml_substitutions);

        for (size_t i = 0; i != xml_substitutions.size(); ++i)
        {
            const ConfigurationPtr xml_substitution(substitutions_view->createView("substitution[" + std::to_string(i) + "]"));

            /// Property values for substitution will be stored in a vector
            /// accessible by property name
            std::vector<String> xml_values;
            xml_substitution->keys("values", xml_values);

            String name = xml_substitution->getString("name");

            for (size_t j = 0; j != xml_values.size(); ++j)
            {
                out_substitutions[name].push_back(xml_substitution->getString("values.value[" + std::to_string(j) + "]"));
            }
        }
    }

    std::vector<String> formatQueries(const String & query, StringToVector substitutions_to_generate)
    {
        std::vector<String> queries_res;
        runThroughAllOptionsAndPush(substitutions_to_generate.begin(), substitutions_to_generate.end(), query, queries_res);
        return queries_res;
    }

    /// Recursive method which goes through all substitution blocks in xml
    /// and replaces property {names} by their values
    void runThroughAllOptionsAndPush(StringToVector::iterator substitutions_left,
        StringToVector::iterator substitutions_right,
        const String & template_query,
        std::vector<String> & out_queries)
    {
        if (substitutions_left == substitutions_right)
        {
            out_queries.push_back(template_query); /// completely substituted query
            return;
        }

        String substitution_mask = "{" + substitutions_left->first + "}";

        if (template_query.find(substitution_mask) == String::npos) /// nothing to substitute here
        {
            runThroughAllOptionsAndPush(std::next(substitutions_left), substitutions_right, template_query, out_queries);
            return;
        }

        for (const String & value : substitutions_left->second)
        {
            /// Copy query string for each unique permutation
            Query query = template_query;
            size_t substr_pos = 0;

            while (substr_pos != String::npos)
            {
                substr_pos = query.find(substitution_mask);

                if (substr_pos != String::npos)
                    query.replace(substr_pos, substitution_mask.length(), value);
            }

            runThroughAllOptionsAndPush(std::next(substitutions_left), substitutions_right, query, out_queries);
        }
    }

public:
    String constructTotalInfo(Strings metrics)
    {
        JSONString json_output;

        json_output.set("hostname", getFQDNOrHostName());
        json_output.set("num_cores", getNumberOfPhysicalCPUCores());
        json_output.set("num_threads", std::thread::hardware_concurrency());
        json_output.set("ram", getMemoryAmount());
        json_output.set("server_version", server_version);
        json_output.set("time", DateLUT::instance().timeToString(time(nullptr)));
        json_output.set("test_name", test_name);
        json_output.set("main_metric", main_metric);

        if (substitutions[test_name].size())
        {
            JSONString json_parameters(2); /// here, 2 is the size of \t padding

            for (auto it = substitutions[test_name].begin(); it != substitutions[test_name].end(); ++it)
            {
                String parameter = it->first;
                std::vector<String> values = it->second;

                String array_string = "[";
                for (size_t i = 0; i != values.size(); ++i)
                {
                    array_string += '"' + std::regex_replace(values[i], QUOTE_REGEX, "\\\"") + '"';
                    if (i != values.size() - 1)
                    {
                        array_string += ", ";
                    }
                }
                array_string += ']';

                json_parameters.set(parameter, array_string);
            }

            json_output.set("parameters", json_parameters.asString());
        }

        std::vector<JSONString> run_infos;
        for (size_t query_index = 0; query_index < queries.size(); ++query_index)
        {
            for (size_t number_of_launch = 0; number_of_launch < times_to_run; ++number_of_launch)
            {
                TestStats & statistics = statistics_by_run[number_of_launch * queries.size() + query_index];

                if (!statistics.ready)
                    continue;

                JSONString runJSON;

                runJSON.set("query", std::regex_replace(queries[query_index], QUOTE_REGEX, "\\\""));
                if (!statistics.exception.empty())
                    runJSON.set("exception", statistics.exception);

                if (substitutions_maps.size())
                {
                    JSONString parameters(4);

                    for (auto it = substitutions_maps[query_index].begin(); it != substitutions_maps[query_index].end(); ++it)
                    {
                        parameters.set(it->first, it->second);
                    }

                    runJSON.set("parameters", parameters.asString());
                }


                if (exec_type == ExecutionType::Loop)
                {
                    /// in seconds
                    if (std::find(metrics.begin(), metrics.end(), "min_time") != metrics.end())
                        runJSON.set("min_time", statistics.min_time / double(1000));

                    if (std::find(metrics.begin(), metrics.end(), "quantiles") != metrics.end())
                    {
                        JSONString quantiles(4); /// here, 4 is the size of \t padding
                        for (double percent = 10; percent <= 90; percent += 10)
                        {
                            String quantile_key = std::to_string(percent / 100.0);
                            while (quantile_key.back() == '0')
                                quantile_key.pop_back();

                            quantiles.set(quantile_key, statistics.sampler.quantileInterpolated(percent / 100.0));
                        }
                        quantiles.set("0.95", statistics.sampler.quantileInterpolated(95 / 100.0));
                        quantiles.set("0.99", statistics.sampler.quantileInterpolated(99 / 100.0));
                        quantiles.set("0.999", statistics.sampler.quantileInterpolated(99.9 / 100.0));
                        quantiles.set("0.9999", statistics.sampler.quantileInterpolated(99.99 / 100.0));

                        runJSON.set("quantiles", quantiles.asString());
                    }

                    if (std::find(metrics.begin(), metrics.end(), "total_time") != metrics.end())
                        runJSON.set("total_time", statistics.total_time);

                    if (std::find(metrics.begin(), metrics.end(), "queries_per_second") != metrics.end())
                        runJSON.set("queries_per_second", double(statistics.queries) / statistics.total_time);

                    if (std::find(metrics.begin(), metrics.end(), "rows_per_second") != metrics.end())
                        runJSON.set("rows_per_second", double(statistics.total_rows_read) / statistics.total_time);

                    if (std::find(metrics.begin(), metrics.end(), "bytes_per_second") != metrics.end())
                        runJSON.set("bytes_per_second", double(statistics.total_bytes_read) / statistics.total_time);
                }
                else
                {
                    if (std::find(metrics.begin(), metrics.end(), "max_rows_per_second") != metrics.end())
                        runJSON.set("max_rows_per_second", statistics.max_rows_speed);

                    if (std::find(metrics.begin(), metrics.end(), "max_bytes_per_second") != metrics.end())
                        runJSON.set("max_bytes_per_second", statistics.max_bytes_speed);

                    if (std::find(metrics.begin(), metrics.end(), "avg_rows_per_second") != metrics.end())
                        runJSON.set("avg_rows_per_second", statistics.avg_rows_speed_value);

                    if (std::find(metrics.begin(), metrics.end(), "avg_bytes_per_second") != metrics.end())
                        runJSON.set("avg_bytes_per_second", statistics.avg_bytes_speed_value);
                }

                run_infos.push_back(runJSON);
            }
        }

        json_output.set("runs", run_infos);

        return json_output.asString();
    }

    String minOutput()
    {
        String output;

        for (size_t query_index = 0; query_index < queries.size(); ++query_index)
        {
            for (size_t number_of_launch = 0; number_of_launch < times_to_run; ++number_of_launch)
            {
                if (queries.size() > 1)
                {
                    output += "query \"" + queries[query_index] + "\", ";
                }

                if (substitutions_maps.size())
                {
                    for (auto it = substitutions_maps[query_index].begin(); it != substitutions_maps[query_index].end(); ++it)
                    {
                        output += it->first + " = " + it->second + ", ";
                    }
                }

                output += "run " + std::to_string(number_of_launch + 1) + ": ";
                output += main_metric + " = ";
                output += statistics_by_run[number_of_launch * queries.size() + query_index].getStatisticByName(main_metric);
                output += "\n";
            }
        }

        return output;
    }
};
}

static void getFilesFromDir(const fs::path & dir, std::vector<String> & input_files, const bool recursive = false)
{
    if (dir.extension().string() == ".xml")
        std::cerr << "Warning: '" + dir.string() + "' is a directory, but has .xml extension" << std::endl;

    fs::directory_iterator end;
    for (fs::directory_iterator it(dir); it != end; ++it)
    {
        const fs::path file = (*it);
        if (recursive && fs::is_directory(file))
            getFilesFromDir(file, input_files, recursive);
        else if (!fs::is_directory(file) && file.extension().string() == ".xml")
            input_files.push_back(file.string());
    }
}


int mainEntryClickHousePerformanceTest(int argc, char ** argv)
try
{
    using boost::program_options::value;
    using Strings = std::vector<String>;

    boost::program_options::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("lite", "use lite version of output")
        ("profiles-file", value<String>()->default_value(""), "Specify a file with global profiles")
        ("host,h", value<String>()->default_value("localhost"), "")
        ("port", value<UInt16>()->default_value(9000), "")
        ("secure,s", "Use TLS connection")
        ("database", value<String>()->default_value("default"), "")
        ("user", value<String>()->default_value("default"), "")
        ("password", value<String>()->default_value(""), "")
        ("tags", value<Strings>()->multitoken(), "Run only tests with tag")
        ("skip-tags", value<Strings>()->multitoken(), "Do not run tests with tag")
        ("names", value<Strings>()->multitoken(), "Run tests with specific name")
        ("skip-names", value<Strings>()->multitoken(), "Do not run tests with name")
        ("names-regexp", value<Strings>()->multitoken(), "Run tests with names matching regexp")
        ("skip-names-regexp", value<Strings>()->multitoken(), "Do not run tests with names matching regexp")
        ("recursive,r", "Recurse in directories to find all xml's");

    /// These options will not be displayed in --help
    boost::program_options::options_description hidden("Hidden options");
    hidden.add_options()
        ("input-files", value<std::vector<String>>(), "");

    /// But they will be legit, though. And they must be given without name
    boost::program_options::positional_options_description positional;
    positional.add("input-files", -1);

    boost::program_options::options_description cmdline_options;
    cmdline_options.add(desc).add(hidden);

    boost::program_options::variables_map options;
    boost::program_options::store(
        boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(positional).run(), options);
    boost::program_options::notify(options);

    if (options.count("help"))
    {
        std::cout << "Usage: " << argv[0] << " [options] [test_file ...] [tests_folder]\n";
        std::cout << desc << "\n";
        return 0;
    }

    Strings input_files;
    bool recursive = options.count("recursive");

    if (!options.count("input-files"))
    {
        std::cerr << "Trying to find test scenario files in the current folder...";
        fs::path curr_dir(".");

        getFilesFromDir(curr_dir, input_files, recursive);

        if (input_files.empty())
        {
            std::cerr << std::endl;
            throw DB::Exception("Did not find any xml files", DB::ErrorCodes::BAD_ARGUMENTS);
        }
        else
            std::cerr << " found " << input_files.size() << " files." << std::endl;
    }
    else
    {
        input_files = options["input-files"].as<Strings>();
        Strings collected_files;

        for (const String & filename : input_files)
        {
            fs::path file(filename);

            if (!fs::exists(file))
                throw DB::Exception("File '" + filename + "' does not exist", DB::ErrorCodes::FILE_DOESNT_EXIST);

            if (fs::is_directory(file))
            {
                getFilesFromDir(file, collected_files, recursive);
            }
            else
            {
                if (file.extension().string() != ".xml")
                    throw DB::Exception("File '" + filename + "' does not have .xml extension", DB::ErrorCodes::BAD_ARGUMENTS);
                collected_files.push_back(filename);
            }
        }

        input_files = std::move(collected_files);
    }

    Strings tests_tags = options.count("tags") ? options["tags"].as<Strings>() : Strings({});
    Strings skip_tags = options.count("skip-tags") ? options["skip-tags"].as<Strings>() : Strings({});
    Strings tests_names = options.count("names") ? options["names"].as<Strings>() : Strings({});
    Strings skip_names = options.count("skip-names") ? options["skip-names"].as<Strings>() : Strings({});
    Strings tests_names_regexp = options.count("names-regexp") ? options["names-regexp"].as<Strings>() : Strings({});
    Strings skip_names_regexp = options.count("skip-names-regexp") ? options["skip-names-regexp"].as<Strings>() : Strings({});

    auto timeouts = DB::ConnectionTimeouts::getTCPTimeoutsWithoutFailover(DB::Settings());

    DB::UseSSL use_ssl;

    DB::PerformanceTest performance_test(
        options["host"].as<String>(),
        options["port"].as<UInt16>(),
        options.count("secure"),
        options["database"].as<String>(),
        options["user"].as<String>(),
        options["password"].as<String>(),
        options.count("lite") > 0,
        options["profiles-file"].as<String>(),
        std::move(input_files),
        std::move(tests_tags),
        std::move(skip_tags),
        std::move(tests_names),
        std::move(skip_names),
        std::move(tests_names_regexp),
        std::move(skip_names_regexp),
        timeouts);
    return performance_test.run();
}
catch (...)
{
    std::cout << DB::getCurrentExceptionMessage(/*with stacktrace = */ true) << std::endl;
    int code = DB::getCurrentExceptionCode();
    return code ? code : 1;
}
