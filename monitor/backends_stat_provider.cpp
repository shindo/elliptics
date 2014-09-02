/*
 * Copyright 2013+ Kirill Smorodinnikov <shaitkir@gmail.com>
 *
 * This file is part of Elliptics.
 *
 * Elliptics is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Elliptics is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Elliptics.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "backends_stat_provider.hpp"

#include "library/elliptics.h"
#include "library/backend.h"

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "cache/cache.hpp"

namespace ioremap { namespace monitor {

backends_stat_provider::backends_stat_provider(struct dnet_node *node)
: m_node(node)
{}

/*
 * Gets statistics from lowlevel backend and writes it to "backend" section
 */
static void fill_backend_backend(rapidjson::Value &stat_value,
                                 rapidjson::Document::AllocatorType &allocator,
                                 const struct dnet_backend_io &backend) {
	char *json_stat = NULL;
	size_t size = 0;
	struct dnet_backend_callbacks *cb = backend.cb;
	if (cb->storage_stat_json) {
		cb->storage_stat_json(cb->command_private, &json_stat, &size);
		if (json_stat && size) {
			rapidjson::Document backend_value(&allocator);
			backend_value.Parse<0>(json_stat);
			stat_value.AddMember("backend",
			                     static_cast<rapidjson::Value&>(backend_value),
			                     allocator);
		}
	}

	free(json_stat);
}

static void dump_list_stats(rapidjson::Value &stat, list_stat &list_stats, rapidjson::Document::AllocatorType &allocator) {
	stat.AddMember("current_size", list_stats.list_size, allocator)
	    .AddMember("min", list_stats.min_list_size, allocator)
	    .AddMember("max", list_stats.max_list_size, allocator)
	    .AddMember("volume", list_stats.volume, allocator);
}

/*
 * Fills io section of one backend
 */
static void fill_backend_io(rapidjson::Value &stat_value,
                            rapidjson::Document::AllocatorType &allocator,
                            const struct dnet_backend_io &backend) {
	rapidjson::Value io_value(rapidjson::kObjectType);

	rapidjson::Value blocking_stat(rapidjson::kObjectType);
	dump_list_stats(blocking_stat, backend.pool.recv_pool.pool->list_stats, allocator);
	io_value.AddMember("blocking", blocking_stat, allocator);

	rapidjson::Value nonblocking_stat(rapidjson::kObjectType);
	dump_list_stats(nonblocking_stat, backend.pool.recv_pool_nb.pool->list_stats, allocator);
	io_value.AddMember("nonblocking", nonblocking_stat, allocator);

	stat_value.AddMember("io", io_value, allocator);
}

/*
 * Fills cache section of one backend
 */
static void fill_backend_cache(rapidjson::Value &stat_value,
                               rapidjson::Document::AllocatorType &allocator,
                               const struct dnet_backend_io &backend) {
	if (backend.cache) {
		ioremap::cache::cache_manager *cache = (ioremap::cache::cache_manager *)backend.cache;
		rapidjson::Document caches_value(&allocator);
		caches_value.Parse<0>(cache->stat_json().c_str());
		stat_value.AddMember("cache",
		                     static_cast<rapidjson::Value&>(caches_value),
		                     allocator);
	}
}

/*
 * Fills status section of one backend
 */
static void fill_backend_status(rapidjson::Value &stat_value,
                                rapidjson::Document::AllocatorType &allocator,
                                struct dnet_node *node,
                                dnet_backend_status &status,
                                size_t backend_id) {
	backend_fill_status_nolock(node, &status, backend_id);

	rapidjson::Value status_value(rapidjson::kObjectType);
	status_value.AddMember("state", status.state, allocator);
	status_value.AddMember("defrag_state", status.defrag_state, allocator);

	rapidjson::Value last_start(rapidjson::kObjectType);
	last_start.AddMember("tv_sec", status.last_start.tsec, allocator);
	last_start.AddMember("tv_usec", status.last_start.tnsec / 1000, allocator);
	status_value.AddMember("last_start", last_start, allocator);
	status_value.AddMember("last_start_err", status.last_start_err, allocator);
	status_value.AddMember("read_only", status.read_only, allocator);

	stat_value.AddMember("status", status_value, allocator);
}

/*
 * Fills config with common backend info like config, group id
 */
static void fill_disabled_backend_config(rapidjson::Value &stat_value,
                                         rapidjson::Document::AllocatorType &allocator,
					 const dnet_backend_info &config_backend) {
	rapidjson::Value backend_value(rapidjson::kObjectType);
	rapidjson::Value config_value(rapidjson::kObjectType);

	for (auto it = config_backend.options.begin(); it != config_backend.options.end(); ++it) {
		const dnet_backend_config_entry &entry = *it;

		rapidjson::Value tmp_val(entry.value_template.data(), allocator);
		config_value.AddMember(entry.entry->key, tmp_val, allocator);
	}

	backend_value.AddMember("config", config_value, allocator);
	stat_value.AddMember("backend", backend_value, allocator);
}

/*
 * Fills all sections of one backend
 */
static rapidjson::Value& backend_stats_json(uint64_t categories,
                                            rapidjson::Value &stat_value,
                                            rapidjson::Document::AllocatorType &allocator,
                                            struct dnet_node *node,
                                            size_t backend_id) {
	dnet_backend_status status;
	memset(&status, 0, sizeof(status));

	const auto &config_backend = node->config_data->backends->backends[backend_id];
	std::lock_guard<std::mutex> guard(*config_backend.state_mutex);

	stat_value.AddMember("backend_id", backend_id, allocator);
	fill_backend_status(stat_value, allocator, node, status, backend_id);

	if (status.state == DNET_BACKEND_ENABLED && node->io) {
		const struct dnet_backend_io & backend = node->io->backends[backend_id];

		if (categories & DNET_MONITOR_BACKEND) {
			fill_backend_backend(stat_value, allocator, backend);
		}
		if (categories & DNET_MONITOR_IO) {
			fill_backend_io(stat_value, allocator, backend);
		}
		if (categories & DNET_MONITOR_CACHE) {
			fill_backend_cache(stat_value, allocator, backend);
		}
	} else if (categories & DNET_MONITOR_BACKEND) {
		fill_disabled_backend_config(stat_value, allocator, config_backend);
	}

	stat_value["backend"]["config"].AddMember("group", config_backend.group, allocator);

	return stat_value;
}

/*
 * Fills all section of all backends
 */
static void backends_stats_json(uint64_t categories,
                                rapidjson::Value &stat_value,
                                rapidjson::Document::AllocatorType &allocator,
                                struct dnet_node *node) {
	const auto &backends = node->config_data->backends->backends;
	for (size_t i = 0; i < backends.size(); ++i) {
		rapidjson::Value backend_stat(rapidjson::kObjectType);
		stat_value.AddMember(std::to_string(static_cast<unsigned long long>(i)).c_str(),
		                     allocator,
		                     backend_stats_json(categories, backend_stat, allocator, node, i),
		                     allocator);
	}
}

/*
 * Generates json statistics from all backends
 */
std::string backends_stat_provider::json(uint64_t categories) const {
	if (!(categories & DNET_MONITOR_IO) &&
	    !(categories & DNET_MONITOR_CACHE) &&
	    !(categories & DNET_MONITOR_BACKEND))
	    return std::string();

	rapidjson::Document doc;
	doc.SetObject();
	auto &allocator = doc.GetAllocator();

	backends_stats_json(categories, doc, allocator, m_node);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	doc.Accept(writer);
	return buffer.GetString();
}

}} /* namespace ioremap::monitor */
