#include "util/SystemMetrics.hpp"

#include <algorithm>
#include <cmath>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else // Linux
#include <sys/sysinfo.h>
#include <unistd.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#endif

SystemMetrics& SystemMetrics::instance() {
    static SystemMetrics s_inst;
    return s_inst;
}

SystemResourceStats SystemMetrics::sample() {
    SystemResourceStats stats;

#if defined(__APPLE__)
    // 1. Total System Memory
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    int64_t total_mem = 0;
    size_t len = sizeof(total_mem);
    if (sysctl(mib, 2, &total_mem, &len, nullptr, 0) == 0 && total_mem > 0) {
        stats.ram_system_total = static_cast<size_t>(total_mem);
    }

    // 2. Used System Memory
    vm_size_t page_size = 4096;
    mach_port_t host_port = mach_host_self();
    host_page_size(host_port, &page_size);
    vm_statistics64_data_t vm_stat;
    mach_msg_type_number_t host_size = sizeof(vm_statistics64_data_t) / sizeof(integer_t);
    if (host_statistics64(host_port, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_stat), &host_size) == KERN_SUCCESS) {
        uint64_t used_pages = vm_stat.active_count + vm_stat.wire_count + vm_stat.compressor_page_count;
        stats.ram_system_used = used_pages * page_size;
        if (stats.ram_system_total > 0) {
            stats.ram_percent = (static_cast<double>(stats.ram_system_used) / static_cast<double>(stats.ram_system_total)) * 100.0;
        }
    }

    // 3. Process Memory
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info), &count) == KERN_SUCCESS) {
        stats.ram_process_bytes = vm_info.phys_footprint;
    }

    // 4. CPU Usage
    natural_t cpu_count = 0;
    processor_info_array_t cpu_info = nullptr;
    mach_msg_type_number_t info_count = 0;
    if (host_processor_info(host_port, PROCESSOR_CPU_LOAD_INFO, &cpu_count, &cpu_info, &info_count) == KERN_SUCCESS) {
        uint64_t total_user = 0, total_system = 0, total_idle = 0, total_nice = 0;
        auto* load_info = reinterpret_cast<processor_cpu_load_info_t>(cpu_info);
        for (natural_t i = 0; i < cpu_count; ++i) {
            total_user   += load_info[i].cpu_ticks[CPU_STATE_USER];
            total_system += load_info[i].cpu_ticks[CPU_STATE_SYSTEM];
            total_idle   += load_info[i].cpu_ticks[CPU_STATE_IDLE];
            total_nice   += load_info[i].cpu_ticks[CPU_STATE_NICE];
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(cpu_info), info_count * sizeof(int));

        if (m_has_prev) {
            uint64_t d_user   = (total_user >= m_prev_cpu_user)     ? (total_user - m_prev_cpu_user) : 0;
            uint64_t d_system = (total_system >= m_prev_cpu_system) ? (total_system - m_prev_cpu_system) : 0;
            uint64_t d_idle   = (total_idle >= m_prev_cpu_idle)     ? (total_idle - m_prev_cpu_idle) : 0;
            uint64_t d_nice   = (total_nice >= m_prev_cpu_nice)     ? (total_nice - m_prev_cpu_nice) : 0;
            uint64_t total_delta = d_user + d_system + d_idle + d_nice;
            if (total_delta > 0) {
                stats.cpu_percent = (static_cast<double>(d_user + d_system + d_nice) / static_cast<double>(total_delta)) * 100.0;
            }
        }
        m_prev_cpu_user   = total_user;
        m_prev_cpu_system = total_system;
        m_prev_cpu_idle   = total_idle;
        m_prev_cpu_nice   = total_nice;
        m_has_prev = true;
    }
    mach_port_deallocate(mach_task_self(), host_port);

#elif defined(_WIN32)
    // 1. System Memory
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        stats.ram_system_total = memInfo.ullTotalPhys;
        stats.ram_system_used  = (memInfo.ullTotalPhys >= memInfo.ullAvailPhys) ?
                                  (memInfo.ullTotalPhys - memInfo.ullAvailPhys) : 0;
        stats.ram_percent      = static_cast<double>(memInfo.dwMemoryLoad);
    }

    // 2. Process Memory
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        stats.ram_process_bytes = pmc.WorkingSetSize;
    }

    // 3. CPU
    FILETIME idleTime, kernelTime, userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        ULARGE_INTEGER u_idle, u_kernel, u_user;
        u_idle.LowPart = idleTime.dwLowDateTime;     u_idle.HighPart = idleTime.dwHighDateTime;
        u_kernel.LowPart = kernelTime.dwLowDateTime; u_kernel.HighPart = kernelTime.dwHighDateTime;
        u_user.LowPart = userTime.dwLowDateTime;     u_user.HighPart = userTime.dwHighDateTime;

        uint64_t cur_idle   = u_idle.QuadPart;
        uint64_t cur_kernel = u_kernel.QuadPart;
        uint64_t cur_user   = u_user.QuadPart;

        if (m_has_prev) {
            uint64_t d_idle   = (cur_idle >= m_prev_idle)     ? (cur_idle - m_prev_idle) : 0;
            uint64_t d_kernel = (cur_kernel >= m_prev_kernel) ? (cur_kernel - m_prev_kernel) : 0;
            uint64_t d_user   = (cur_user >= m_prev_user)     ? (cur_user - m_prev_user) : 0;
            uint64_t d_sys    = d_kernel + d_user;
            if (d_sys > 0) {
                stats.cpu_percent = (static_cast<double>(d_sys - d_idle) / static_cast<double>(d_sys)) * 100.0;
            }
        }
        m_prev_idle   = cur_idle;
        m_prev_kernel = cur_kernel;
        m_prev_user   = cur_user;
        m_has_prev = true;
    }

#else // Linux
    // 1. System Memory from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        uint64_t total_kb = 0, avail_kb = 0;
        while (std::getline(meminfo, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::sscanf(line.c_str(), "MemTotal: %llu kB", &total_kb);
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                std::sscanf(line.c_str(), "MemAvailable: %llu kB", &avail_kb);
            }
        }
        stats.ram_system_total = total_kb * 1024;
        if (total_kb >= avail_kb) {
            stats.ram_system_used = (total_kb - avail_kb) * 1024;
            if (total_kb > 0) {
                stats.ram_percent = (static_cast<double>(total_kb - avail_kb) / static_cast<double>(total_kb)) * 100.0;
            }
        }
    }

    // 2. Process Memory from /proc/self/statm
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        uint64_t size_pages = 0, resident_pages = 0;
        statm >> size_pages >> resident_pages;
        long page_size = sysconf(_SC_PAGESIZE);
        stats.ram_process_bytes = resident_pages * (page_size > 0 ? static_cast<size_t>(page_size) : 4096);
    }

    // 3. CPU from /proc/stat
    std::ifstream stat("/proc/stat");
    if (stat.is_open()) {
        std::string cpu;
        uint64_t u = 0, n = 0, s = 0, i = 0, w = 0, iq = 0, si = 0, st = 0;
        stat >> cpu >> u >> n >> s >> i >> w >> iq >> si >> st;
        if (m_has_prev) {
            uint64_t prev_idle = m_prev_idle + m_prev_iowait;
            uint64_t curr_idle = i + w;
            uint64_t prev_non_idle = m_prev_user + m_prev_nice + m_prev_system + m_prev_irq + m_prev_softirq + m_prev_steal;
            uint64_t curr_non_idle = u + n + s + iq + si + st;
            uint64_t prev_total = prev_idle + prev_non_idle;
            uint64_t curr_total = curr_idle + curr_non_idle;
            if (curr_total > prev_total) {
                uint64_t d_total = curr_total - prev_total;
                uint64_t d_idle  = (curr_idle >= prev_idle) ? (curr_idle - prev_idle) : 0;
                stats.cpu_percent = (static_cast<double>(d_total - d_idle) / static_cast<double>(d_total)) * 100.0;
            }
        }
        m_prev_user    = u;  m_prev_nice    = n;  m_prev_system = s;  m_prev_idle  = i;
        m_prev_iowait  = w;  m_prev_irq     = iq; m_prev_softirq = si; m_prev_steal = st;
        m_has_prev = true;
    }
#endif

    stats.cpu_percent = std::clamp(stats.cpu_percent, 0.0, 100.0);
    stats.ram_percent = std::clamp(stats.ram_percent, 0.0, 100.0);
    return stats;
}
