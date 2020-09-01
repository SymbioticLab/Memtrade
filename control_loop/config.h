#ifndef CONTROL_LOOP_CONFIG_H
#define CONTROL_LOOP_CONFIG_H

#include <string>
#include "yaml-cpp/yaml.h"

using namespace std;

struct control_config {
	string cgroup_name;

	struct {
		string file_path;
		bool higher_better;
	} performance_metric;

	struct {
		long window_size;
		long minimal_baseline_size;
	} baseline_estimation;

	struct {
		long recent_window_size;
		float outlier_prob;
		float ks_distance;
	} performance_drop_detection;

	struct {
		bool enable;
		int sleep_time;
		struct {
			long step_size;
			int sleep_time;
		} harvest;
		struct {
			long step_size;
			float step_mi;
			int sleep_time;
		} recovery;
		struct {
			float ad;
			float mi;
			int min;
			int max;
		} recovery_time;
		struct {
			long size;
			float outlier_prob;
		} prefetch;
	} control_loop;

	struct {
		string file_path;
	} logging;

	struct {
		string stat_path;
		string promotion_rate_path;
		string disk_promotion_rate_path;
		string prefetch_path;
	} silo;

	static control_config parse_yaml(YAML::Node &root);
};


control_config control_config::parse_yaml(YAML::Node &root) {
	control_config config;

	config.cgroup_name = root["cgroup_name"].as<std::string>();

	YAML::Node performance_metric = root["performance_metric"];
	config.performance_metric.file_path = performance_metric["file_path"].as<std::string>();
	config.performance_metric.higher_better = performance_metric["higher_better"].as<bool>();

	YAML::Node baseline_estimation = root["baseline_estimation"];
	config.baseline_estimation.window_size = baseline_estimation["window_size"].as<long>();
	config.baseline_estimation.minimal_baseline_size = baseline_estimation["minimal_baseline_size"].as<long>();

	YAML::Node performance_drop_detection = root["performance_drop_detection"];
	config.performance_drop_detection.recent_window_size = performance_drop_detection["recent_window_size"].as<long>();
	config.performance_drop_detection.outlier_prob = performance_drop_detection["outlier_prob"].as<float>();
	config.performance_drop_detection.ks_distance = performance_drop_detection["ks_distance"].as<float>();

	YAML::Node control_loop = root["control_loop"];
	config.control_loop.enable = control_loop["enable"].as<bool>();
	config.control_loop.sleep_time = control_loop["sleep_time"].as<int>();
	YAML::Node harvest = control_loop["harvest"];
	config.control_loop.harvest.step_size = harvest["step_size"].as<long>();
	config.control_loop.harvest.sleep_time = harvest["sleep_time"].as<int>();
	YAML::Node recovery = control_loop["recovery"];
	config.control_loop.recovery.step_size = recovery["step_size"].as<long>();
	config.control_loop.recovery.step_mi = recovery["step_mi"].as<float>();
	config.control_loop.recovery.sleep_time = recovery["sleep_time"].as<int>();
	YAML::Node recovery_time = control_loop["recovery_time"];
	config.control_loop.recovery_time.ad = recovery_time["ad"].as<float>();
	config.control_loop.recovery_time.mi = recovery_time["mi"].as<float>();
	config.control_loop.recovery_time.min = recovery_time["min"].as<int>();
	config.control_loop.recovery_time.max = recovery_time["max"].as<int>();
	YAML::Node prefetch = control_loop["prefetch"];
	config.control_loop.prefetch.size = prefetch["size"].as<long>();
	config.control_loop.prefetch.outlier_prob = prefetch["outlier_prob"].as<float>();

	YAML::Node logging = root["logging"];
	config.logging.file_path = logging["file_path"].as<string>();

	YAML::Node silo = root["silo"];
	config.silo.stat_path = silo["stat_path"].as<string>();
	config.silo.promotion_rate_path = silo["promotion_rate_path"].as<string>();
	config.silo.disk_promotion_rate_path = silo["disk_promotion_rate_path"].as<string>();
	config.silo.prefetch_path = silo["prefetch_path"].as<string>();

	return config;
}


#endif //CONTROL_LOOP_CONFIG_H
