/*
 * Copyright (c) 2015, 2016, 2017, 2018, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include <sstream>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <iomanip>

#include "Reporter.hpp"
#include "PlatformIO.hpp"
#include "PlatformTopo.hpp"
#include "ApplicationIO.hpp"
#include "Comm.hpp"
#include "TreeComm.hpp"
#include "Exception.hpp"
#include "geopm_hash.h"
#include "geopm_version.h"
#include "config.h"

#ifdef GEOPM_HAS_XMMINTRIN
#include <xmmintrin.h>
#endif

namespace geopm
{
    Reporter::Reporter(const std::string &report_name, IPlatformIO &platform_io, int rank)
        : m_report_name(report_name)
        , m_platform_io(platform_io)
        , m_rank(rank)
    {

    }

    void Reporter::init(void)
    {
        m_energy_pkg_idx = m_platform_io.push_signal("ENERGY_PACKAGE", IPlatformTopo::M_DOMAIN_BOARD, 0);
        m_energy_dram_idx = m_platform_io.push_signal("ENERGY_DRAM", IPlatformTopo::M_DOMAIN_BOARD, 0);
        m_clk_core_idx = m_platform_io.push_signal("CYCLES_THREAD", IPlatformTopo::M_DOMAIN_BOARD, 0);
        m_clk_ref_idx = m_platform_io.push_signal("CYCLES_REFERENCE", IPlatformTopo::M_DOMAIN_BOARD, 0);
        m_platform_io.push_region_signal_total(m_energy_pkg_idx, IPlatformTopo::M_DOMAIN_BOARD, 0);
        m_platform_io.push_region_signal_total(m_energy_dram_idx, IPlatformTopo::M_DOMAIN_BOARD, 0);
        m_platform_io.push_region_signal_total(m_clk_core_idx, IPlatformTopo::M_DOMAIN_BOARD, 0);
        m_platform_io.push_region_signal_total(m_clk_ref_idx, IPlatformTopo::M_DOMAIN_BOARD, 0);

        if (!m_rank) {
            // check if report file can be created
            if (!m_report_name.empty()) {
                std::ofstream test_open(m_report_name);
                if (!test_open.good()) {
                    std::cerr << "Warning: unable to open report file '" << m_report_name
                              << "' for writing: " << strerror(errno) << std::endl;
                }
                std::remove(m_report_name.c_str());
            }
        }
    }

    void Reporter::generate(const std::string &agent_name,
                            const std::vector<std::pair<std::string, std::string> > &agent_report_header,
                            const std::vector<std::pair<std::string, std::string> > &agent_node_report,
                            const std::map<uint64_t, std::vector<std::pair<std::string, std::string> > > &agent_region_report,
                            const IApplicationIO &application_io,
                            std::shared_ptr<IComm> comm,
                            const ITreeComm &tree_comm)
    {
        std::ofstream master_report(application_io.report_name());
        if (!master_report.good()) {
            throw Exception("Failed to open report file", GEOPM_ERROR_INVALID, __FILE__, __LINE__);
        }
        int rank = comm->rank();
        // make header
        if (!rank) {
            master_report << "##### geopm " << geopm_version() << " #####" << std::endl;
            master_report << "Profile: " << application_io.profile_name() << std::endl;
            master_report << "Agent: " << agent_name << std::endl;
            for (const auto &kv : agent_report_header) {
                master_report << kv.first << ": " << kv.second << std::endl;
            }
            master_report << "Policy Mode: deprecated" << std::endl;
            master_report << "Tree Decider: deprecated" << std::endl;
            master_report << "Leaf Decider: deprecated" << std::endl;
            master_report << "Power Budget: -1" << std::endl;
        }
        // per-node report
        std::ostringstream report;
        char hostname[NAME_MAX];
        gethostname(hostname, NAME_MAX);
        report << "\nHost: " << hostname << std::endl;
        for (const auto &kv : agent_node_report) {
            report << kv.first << ": " << kv.second << std::endl;
        }
        // vector of region data, in descending order by runtime
        struct region_info {
                std::string name;
                uint64_t id;
                double runtime;
                double energy;
                int count;
        };
        std::vector<region_info> region_ordered;
        auto region_name_set = application_io.region_name_set();
        for (const auto &region : region_name_set) {
            uint64_t region_id = geopm_crc32_str(0, region.c_str());
            if (region.find("MPI_") == 0) {
                region_id = geopm_region_id_set_mpi(region_id);
            }
            uint64_t mpi_region_id = geopm_region_id_set_mpi(region_id);
            double energy = m_platform_io.sample_region_total(m_energy_pkg_idx, region_id) +
                m_platform_io.sample_region_total(m_energy_pkg_idx, mpi_region_id) +
                m_platform_io.sample_region_total(m_energy_dram_idx, region_id) +
                m_platform_io.sample_region_total(m_energy_dram_idx, mpi_region_id);
            int count = application_io.total_count(region_id);
            if (count > 0) {
                region_ordered.push_back({region,
                                          region_id,
                                          application_io.total_region_runtime(region_id),
                                          energy,
                                          count});
            }
        }
        // sort based on element 2 of the tuple
        std::sort(region_ordered.begin(), region_ordered.end(),
                  [] (const region_info &a,
                      const region_info &b) -> bool {
                      return a.runtime >= b.runtime;
                  });
        // add unmarked and epoch at the end
        uint64_t unmarked_mpi_id = geopm_region_id_set_mpi(GEOPM_REGION_ID_UNMARKED);
        double energy = m_platform_io.sample_region_total(m_energy_pkg_idx, GEOPM_REGION_ID_UNMARKED) +
                        m_platform_io.sample_region_total(m_energy_dram_idx, GEOPM_REGION_ID_UNMARKED) +
                        m_platform_io.sample_region_total(m_energy_pkg_idx, unmarked_mpi_id) +
                        m_platform_io.sample_region_total(m_energy_dram_idx, unmarked_mpi_id);
        region_ordered.push_back({"unmarked-region",
                                  GEOPM_REGION_ID_UNMARKED,
                                  application_io.total_region_runtime(GEOPM_REGION_ID_UNMARKED),
                                  energy,
                                  0});
        /// Total epoch runtime for report includes MPI time and
        /// ignore time, but they are removed from the runtime returned
        /// by the API.
        region_ordered.push_back({"epoch",
                                  GEOPM_REGION_ID_EPOCH,
                                  application_io.total_epoch_runtime() +
                                  application_io.total_epoch_mpi_runtime() +
                                  application_io.total_epoch_ignore_runtime(),
                                  application_io.total_epoch_energy(),
                                  application_io.total_count(GEOPM_REGION_ID_EPOCH)});

        for (const auto &region : region_ordered) {
            uint64_t mpi_region_id = geopm_region_id_set_mpi(region.id);
            report << "Region " << region.name << " (0x" << std::hex
                   << std::setfill('0') << std::setw(16)
                   << region.id << std::dec << "):"
                   << std::setfill('\0') << std::setw(0)
                   << std::endl;
            report << "    runtime (sec): " << region.runtime << std::endl;
            report << "    energy (joules): " << region.energy << std::endl;
            double numer = m_platform_io.sample_region_total(m_clk_core_idx, region.id) +
                           m_platform_io.sample_region_total(m_clk_core_idx, mpi_region_id);
            double denom = m_platform_io.sample_region_total(m_clk_ref_idx, region.id) +
                           m_platform_io.sample_region_total(m_clk_ref_idx, mpi_region_id);
            double freq = denom != 0 ? 100.0 * numer / denom : 0.0;
            report << "    frequency (%): " << freq << std::endl;
            report << "    mpi-runtime (sec): " << application_io.total_region_mpi_runtime(region.id) << std::endl;
            report << "    count: " << region.count << std::endl;
            if (agent_region_report.find(region.id) != agent_region_report.end()) {
                for (const auto &kv : agent_region_report.at(region.id)) {
                    report << "    " << kv.first << ": " << kv.second << std::endl;
                }
            }
        }

        double total_runtime = application_io.total_app_runtime();
        report << "Application Totals:" << std::endl
               << "    runtime (sec): " << total_runtime << std::endl
               << "    energy (joules): " << application_io.total_app_energy() << std::endl
               << "    mpi-runtime (sec): " << application_io.total_app_mpi_runtime() << std::endl
               << "    ignore-time (sec): " << application_io.total_epoch_ignore_runtime() << std::endl;

        std::string max_memory = get_max_memory();
        report << "    geopmctl memory HWM: " << max_memory << std::endl;
        report << "    geopmctl network BW (B/sec): " << tree_comm.overhead_send() / total_runtime << std::endl;

        // aggregate reports from every node
        report.seekp(0, std::ios::end);
        size_t buffer_size = (size_t) report.tellp();
        report.seekp(0, std::ios::beg);
        std::vector<char> report_buffer;
        std::vector<size_t> buffer_size_array;
        std::vector<off_t> buffer_displacement;
        int num_ranks = comm->num_rank();
        buffer_size_array.resize(num_ranks);
        buffer_displacement.resize(num_ranks);
        comm->gather(&buffer_size, sizeof(size_t), buffer_size_array.data(),
                     sizeof(size_t), 0);

        if (!rank) {
            int full_report_size = std::accumulate(buffer_size_array.begin(), buffer_size_array.end(), 0) + 1;
            report_buffer.resize(full_report_size);
            buffer_displacement[0] = 0;
            for (int i = 1; i < num_ranks; ++i) {
                buffer_displacement[i] = buffer_displacement[i-1] + buffer_size_array[i-1];
            }
        }

        comm->gatherv((void *) (report.str().data()), sizeof(char) * buffer_size,
                             (void *) report_buffer.data(), buffer_size_array, buffer_displacement, 0);

        if (!rank) {
            report_buffer.back() = '\0';
            master_report << report_buffer.data();
            master_report << std::endl;
            master_report.close();
        }
    }

    std::string Reporter::get_max_memory()
    {
        char status_buffer[8192];
        status_buffer[8191] = '\0';
        const char *proc_path = "/proc/self/status";

        int fd = open(proc_path, O_RDONLY);
        if (fd == -1) {
            throw Exception("Reporter::generate(): Unable to open " + std::string(proc_path),
                            errno ? errno : GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }

        ssize_t num_read = read(fd, status_buffer, 8191);
        if (num_read == -1) {
            (void)close(fd);
            throw Exception("Reporter::generate(): Unable to read " + std::string(proc_path),
                            errno ? errno : GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }
        status_buffer[num_read] = '\0';

        int err = close(fd);
        if (err) {
            throw Exception("Reporter::generate(): Unable to close " + std::string(proc_path),
                            errno ? errno : GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }


        std::istringstream proc_stream(status_buffer);
        std::string line;
        std::string max_memory;
        const std::string key("VmHWM:");
        while (proc_stream.good()) {
            getline(proc_stream, line);
            if (line.find(key) == 0) {
                max_memory = line.substr(key.length());
                size_t off = max_memory.find_first_not_of(" \t");
                if (off != std::string::npos) {
                    max_memory = max_memory.substr(off);
                }
            }
        }
        if (!max_memory.size()) {
            throw Exception("Controller::generate_report(): Unable to get memory overhead from /proc",
                            GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }
        return max_memory;
    }
}
