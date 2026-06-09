#include "utils.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace bench {

// ── JSON file output ──────────────────────────────────────────────────────────

bool saveJson(const nlohmann::json& data, const std::string& fpath) {
    std::ofstream fout(fpath, std::fstream::app);
    if (!fout.is_open()) {
        std::cerr << "bench::saveJson: could not open '" << fpath << "'\n";
        return false;
    }
    fout << data << '\n';
    std::cout << "Saved benchmark data to " << fpath << '\n';
    return true;
}

// ── Memory stats ──────────────────────────────────────────────────────────────

#if defined(__APPLE__)

#include <mach/mach.h>

int64_t peakResidentSetSize() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    kern_return_t ret = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                  reinterpret_cast<task_info_t>(&info), &count);
    if (ret != KERN_SUCCESS || count != MACH_TASK_BASIC_INFO_COUNT)
        return -1;
    return static_cast<int64_t>(info.resident_size_max);
}

int64_t peakVirtualMemory() {
    // No peak virtual memory on macOS — fall back to resident set size.
    return peakResidentSetSize();
}

#elif defined(__linux__)

static int64_t getProcStatus(const std::string& key) {
    std::ifstream procfile("/proc/self/status");
    std::string word;
    while (procfile.good()) {
        procfile >> word;
        if (word == key) {
            int64_t value = 0;
            procfile >> value;
            return value;
        }
        procfile.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return -1;
}

int64_t peakVirtualMemory()    { return getProcStatus("VmPeak:"); }
int64_t peakResidentSetSize()  { return getProcStatus("VmHWM:");  }

#else

int64_t peakVirtualMemory()   { return -1; }
int64_t peakResidentSetSize() { return -1; }

#endif

}  // namespace bench
