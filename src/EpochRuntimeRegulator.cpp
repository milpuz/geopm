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

#include <algorithm>

#include "geopm.h"
#include "geopm_message.h"
#include "Exception.hpp"
#include "Helper.hpp"
#include "KruntimeRegulator.hpp"
#include "EpochRuntimeRegulator.hpp"
#include "PlatformIO.hpp"
#include "PlatformTopo.hpp"

#include "config.h"

namespace geopm
{
    EpochRuntimeRegulator::EpochRuntimeRegulator(int rank_per_node,
                                                 IPlatformIO &platform_io,
                                                 IPlatformTopo &platform_topo)
        : m_rank_per_node(rank_per_node)
        , m_platform_io(platform_io)
        , m_platform_topo(platform_topo)
        , m_seen_first_epoch(m_rank_per_node, false)
        , m_curr_ignore_runtime(m_rank_per_node, 0.0)
        , m_agg_epoch_ignore_runtime(m_rank_per_node, 0.0)
        , m_curr_mpi_runtime(m_rank_per_node, 0.0)
        , m_agg_epoch_mpi_runtime(m_rank_per_node, 0.0)
        , m_agg_mpi_runtime(m_rank_per_node, 0.0)
        , m_last_epoch_runtime(m_rank_per_node, 0.0)
        , m_agg_epoch_runtime(m_rank_per_node, 0.0)
        , m_pre_epoch_region(m_rank_per_node)
        , m_epoch_start_energy(NAN)
        , m_epoch_total_energy(NAN)
    {
        if (m_rank_per_node <= 0) {
            throw Exception("EpochRuntimeRegulator::EpochRuntimeRegulator(): invalid max rank count", GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }
        m_rid_regulator_map.emplace(std::piecewise_construct,
                                    std::make_tuple(GEOPM_REGION_ID_EPOCH),
                                    std::make_tuple(geopm::make_unique<KruntimeRegulator>
                                                    (m_rank_per_node)));
        m_rid_regulator_map.emplace(std::piecewise_construct,
                                    std::make_tuple(GEOPM_REGION_ID_UNMARKED),
                                    std::make_tuple(geopm::make_unique<KruntimeRegulator>
                                                    (m_rank_per_node)));
    }

    EpochRuntimeRegulator::~EpochRuntimeRegulator() = default;

    void EpochRuntimeRegulator::init_unmarked_region()
    {
        struct geopm_time_s time;
        /// @todo This time should come from the application.
        geopm_time(&time);
        for (int rank = 0; rank < m_rank_per_node; ++rank) {
            record_entry(GEOPM_REGION_ID_UNMARKED, rank, time);
        }
    }

    /// @todo temporarily repeated here and in ApplicationIO, until these classes are combined.
    double EpochRuntimeRegulator::current_energy(void) const
    {
        double energy = 0.0;
        int num_package = m_platform_topo.num_domain(IPlatformTopo::M_DOMAIN_PACKAGE);
        for (int pkg = 0; pkg < num_package; ++pkg) {
            energy += m_platform_io.read_signal("ENERGY_PACKAGE", IPlatformTopo::M_DOMAIN_PACKAGE, 0);
        }
        int num_dram = m_platform_topo.num_domain(IPlatformTopo::M_DOMAIN_BOARD_MEMORY);
        for (int dram = 0; dram < num_dram; ++dram) {
            energy += m_platform_io.read_signal("ENERGY_DRAM", IPlatformTopo::M_DOMAIN_BOARD_MEMORY, 0);
        }
        return energy;
    }

    void EpochRuntimeRegulator::epoch(int rank, struct geopm_time_s epoch_time)
    {
        if (m_seen_first_epoch[rank]) {
            record_exit(GEOPM_REGION_ID_EPOCH, rank, epoch_time);
            m_epoch_total_energy = current_energy() - m_epoch_start_energy;
        }
        else {
            std::fill(m_curr_mpi_runtime.begin(), m_curr_mpi_runtime.end(), 0.0);
            std::fill(m_curr_ignore_runtime.begin(), m_curr_ignore_runtime.end(), 0.0);
            m_seen_first_epoch[rank] = true;
            m_epoch_start_energy = current_energy();
        }
        record_entry(GEOPM_REGION_ID_EPOCH, rank, epoch_time);
    }

    void EpochRuntimeRegulator::record_entry(uint64_t region_id, int rank, struct geopm_time_s entry_time)
    {
        region_id = geopm_region_id_unset_hint(GEOPM_MASK_REGION_HINT, region_id);
        if (!m_seen_first_epoch[rank]) {
            m_pre_epoch_region[rank].insert(region_id);
        }
        auto reg_it = m_rid_regulator_map.emplace(std::piecewise_construct,
                                                  std::forward_as_tuple(region_id),
                                                  std::forward_as_tuple(geopm::make_unique<KruntimeRegulator>
                                                                        (m_rank_per_node)));
        reg_it.first->second->record_entry(rank, entry_time);
        if (region_id != GEOPM_REGION_ID_UNMARKED && rank == 0) {
            m_region_info.push_back({geopm_region_id_unset_hint(GEOPM_MASK_REGION_HINT, region_id),
                                     0.0,
                                     IPlatformIO::agg_max(reg_it.first->second->per_rank_last_runtime())});
        }
    }

    void EpochRuntimeRegulator::record_exit(uint64_t region_id, int rank, struct geopm_time_s exit_time)
    {
        bool is_ignore = geopm_region_id_hint_is_equal(GEOPM_REGION_HINT_IGNORE, region_id);
        bool is_mpi = geopm_region_id_is_mpi(region_id);
        region_id = geopm_region_id_unset_hint(GEOPM_MASK_REGION_HINT, region_id);
        auto pre_epoch_it = m_pre_epoch_region[rank].find(region_id);
        auto reg_it = m_rid_regulator_map.find(region_id);
        if (reg_it == m_rid_regulator_map.end()) {
            throw Exception("EpochRuntimeRegulator::record_exit(): unknown region detected.", GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }
        reg_it->second->record_exit(rank, exit_time);
        if (rank < 0 || rank >= m_rank_per_node) {
            throw Exception("EpochRuntimeRegulator::record_exit(): invalid rank value", GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }
        if (geopm_region_id_is_epoch(region_id)) {
            if (m_seen_first_epoch[rank]) {
                m_last_epoch_runtime[rank] = reg_it->second->per_rank_last_runtime()[rank] -
                                             (m_curr_mpi_runtime[rank] + m_curr_ignore_runtime[rank]);
                m_agg_epoch_runtime[rank] += m_last_epoch_runtime[rank];
                m_agg_epoch_mpi_runtime[rank] += m_curr_mpi_runtime[rank];
                m_agg_epoch_ignore_runtime[rank] += m_curr_ignore_runtime[rank];
                m_curr_mpi_runtime[rank] = 0.0;
                m_curr_ignore_runtime[rank] = 0.0;
            }
        }
        else if (is_mpi) {
            if (pre_epoch_it == m_pre_epoch_region[rank].end()) {
                m_curr_mpi_runtime[rank] += reg_it->second->per_rank_last_runtime()[rank];
            }
            else {
                m_pre_epoch_region[rank].erase(pre_epoch_it);
            }
            m_agg_mpi_runtime[rank] += reg_it->second->per_rank_last_runtime()[rank];
        }
        else if (is_ignore) {
            if (pre_epoch_it == m_pre_epoch_region[rank].end()) {
                m_curr_ignore_runtime[rank] += reg_it->second->per_rank_last_runtime()[rank];
            }
            else {
                m_pre_epoch_region[rank].erase(pre_epoch_it);
            }
        }
        if (region_id != GEOPM_REGION_ID_UNMARKED && rank == 0) {
            m_region_info.push_back({geopm_region_id_unset_hint(GEOPM_MASK_REGION_HINT, region_id),
                                     1.0,
                                     IPlatformIO::agg_max(reg_it->second->per_rank_last_runtime())});
        }
    }

    const IKruntimeRegulator &EpochRuntimeRegulator::region_regulator(uint64_t region_id) const
    {
        region_id = geopm_region_id_unset_hint(GEOPM_MASK_REGION_HINT, region_id);
        auto reg_it = m_rid_regulator_map.find(region_id);
        if (reg_it == m_rid_regulator_map.end()) {
            throw Exception("EpochRuntimeRegulator::region_regulator(): unknown region detected.", GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }
        return *(reg_it->second);
    }

    bool EpochRuntimeRegulator::is_regulated(uint64_t region_id) const
    {
        return m_rid_regulator_map.find(region_id) != m_rid_regulator_map.end();
    }

    std::vector<double> EpochRuntimeRegulator::last_epoch_time() const
    {
        return m_last_epoch_runtime;
    }

    std::vector<double> EpochRuntimeRegulator::epoch_count() const
    {
        return m_rid_regulator_map.at(GEOPM_REGION_ID_EPOCH)->per_rank_count();
    }

    std::vector<double> EpochRuntimeRegulator::per_rank_last_runtime(uint64_t region_id) const
    {
        auto reg_it = m_rid_regulator_map.find(region_id);
        if (reg_it == m_rid_regulator_map.end()) {
            throw Exception("EpochRuntimeRegulator::per_rank_last_runtime(): unknown region detected.", GEOPM_ERROR_LOGIC, __FILE__, __LINE__);
        }

        return reg_it->second->per_rank_last_runtime();
    }

    double EpochRuntimeRegulator::total_region_runtime(uint64_t region_id) const
    {
        double result = 0.0;
        if (region_id == GEOPM_REGION_ID_EPOCH) {
            result = IPlatformIO::agg_average(m_agg_epoch_runtime);
        }
        else {
            result = IPlatformIO::agg_average(region_regulator(region_id).per_rank_total_runtime());
        }
        return result;
    }

    double EpochRuntimeRegulator::total_region_mpi_time(uint64_t region_id) const
    {
        double result = 0.0;
        if (region_id == GEOPM_REGION_ID_EPOCH) {
            result = IPlatformIO::agg_average(m_agg_epoch_mpi_runtime);
        }
        else {
            try {
                region_id = geopm_region_id_set_mpi(region_id);
                result = total_region_runtime(region_id);
            }
            catch (Exception ex) {
                /// @todo catch expected exception only
            }
        }
        return result;
    }

    double EpochRuntimeRegulator::total_epoch_runtime(void) const
    {
        return total_region_runtime(GEOPM_REGION_ID_EPOCH);
    }

    double EpochRuntimeRegulator::total_epoch_mpi_time(void) const
    {
        return total_region_mpi_time(GEOPM_REGION_ID_EPOCH);
    }

    double EpochRuntimeRegulator::total_epoch_ignore_time(void) const
    {
        return IPlatformIO::agg_average(m_agg_epoch_ignore_runtime);
    }

    double EpochRuntimeRegulator::total_epoch_energy(void) const
    {
        return m_epoch_total_energy;
    }

    double EpochRuntimeRegulator::total_app_mpi_time(void) const
    {
        return IPlatformIO::agg_average(m_agg_mpi_runtime);
    }

    int EpochRuntimeRegulator::total_count(uint64_t region_id) const
    {
        int result = 0;
        auto rank_count = region_regulator(region_id).per_rank_count();
        if (rank_count.size() != 0) {
            result = *std::max_element(rank_count.begin(), rank_count.end());
        }
        return result;
    }

    std::list<geopm_region_info_s> EpochRuntimeRegulator::region_info(void) const
    {
        return m_region_info;
    }

    void EpochRuntimeRegulator::clear_region_info(void)
    {
        m_region_info.clear();
    }
}
