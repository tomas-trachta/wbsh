#pragma once

/**
 * @file sysinfo.h
 * @brief What the machine is doing right now, for the bar along the bottom.
 */

#include "config.h"
#include "status.h"

#include <string>
#include <vector>

namespace wbshterm {

	/** One reading of the machine; a field that could not be read is -1 or 0. */
	struct SystemSample {
		int                cpu_percent     = -1;
		unsigned long long memory_used_mb  = 0;
		unsigned long long memory_total_mb = 0;
		char               disk_letter     = 0;
		unsigned long long disk_free_gb    = 0;
		unsigned long long disk_total_gb   = 0;
		int                battery_percent = -1;
		bool               on_mains        = false;
	};

	/**
	 * @brief Reads the machine on request and keeps the last reading.
	 *
	 * CPU load is the change between two readings, so the first sample
	 * after construction reports no load at all rather than a guess.
	 */
	class SystemMonitor {
	public:
		void sample();
		const SystemSample& latest() const { return sample_; }

	private:
		void readCpu(SystemSample& sample);
		int cpuPercentSince(unsigned long long idle, unsigned long long total);

		SystemSample       sample_;
		unsigned long long last_idle_  = 0;
		unsigned long long last_total_ = 0;
		bool               primed_     = false;
	};

	/**
	 * @brief The reading as the bar shows it, only the parts asked for.
	 *
	 * ASCII only: the bar is drawn with the grid font and no fallback. A
	 * load past kCpuWarmPercent turns its value yellow and past
	 * kCpuHotPercent red; a battery goes green on mains and red once it
	 * is down to kBatteryLowPercent; a drive goes red under kDiskLowGb.
	 */
	std::vector<StatusSegment> systemInfoSegments(const SystemSample& sample,
		const StatusBarSettings& settings);

	static const int                kCpuWarmPercent    = 60;
	static const int                kCpuHotPercent     = 85;
	static const int                kBatteryLowPercent = 20;
	static const unsigned long long kDiskLowGb         = 10;

	std::string userName();
	std::string hostName();

} /* namespace wbshterm */
