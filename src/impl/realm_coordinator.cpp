////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "impl/realm_coordinator.hpp"

#include "impl/background_collection.hpp"
#include "impl/external_commit_helper.hpp"
#include "impl/transact_log_handler.hpp"
#include "impl/weak_realm_notifier.hpp"
#include "object_store.hpp"
#include "schema.hpp"

#include <realm/commit_log.hpp>
#include <realm/group_shared.hpp>
#include <realm/lang_bind_helper.hpp>

#include <cassert>
#include <set>
#include <unordered_map>

using namespace realm;
using namespace realm::_impl;

static std::mutex s_coordinator_mutex;
static std::unordered_map<std::string, std::weak_ptr<RealmCoordinator>> s_coordinators_per_path;

std::shared_ptr<RealmCoordinator> RealmCoordinator::get_coordinator(StringData path)
{
    std::lock_guard<std::mutex> lock(s_coordinator_mutex);

    auto& weak_coordinator = s_coordinators_per_path[path];
    if (auto coordinator = weak_coordinator.lock()) {
        return coordinator;
    }

    auto coordinator = std::make_shared<RealmCoordinator>();
    weak_coordinator = coordinator;
    return coordinator;
}

std::shared_ptr<RealmCoordinator> RealmCoordinator::get_existing_coordinator(StringData path)
{
    std::lock_guard<std::mutex> lock(s_coordinator_mutex);
    auto it = s_coordinators_per_path.find(path);
    return it == s_coordinators_per_path.end() ? nullptr : it->second.lock();
}

std::shared_ptr<Realm> RealmCoordinator::get_realm(Realm::Config config)
{
    std::lock_guard<std::mutex> lock(m_realm_mutex);
    if ((!m_config.read_only && !m_notifier) || (m_config.read_only && m_weak_realm_notifiers.empty())) {
        m_config = config;
        if (!config.read_only && !m_notifier && config.automatic_change_notifications) {
            try {
                m_notifier = std::make_unique<ExternalCommitHelper>(*this);
            }
            catch (std::system_error const& ex) {
                throw RealmFileException(RealmFileException::Kind::AccessError, config.path, ex.code().message());
            }
        }
    }
    else {
        if (m_config.read_only != config.read_only) {
            throw MismatchedConfigException("Realm at path already opened with different read permissions.");
        }
        if (m_config.in_memory != config.in_memory) {
            throw MismatchedConfigException("Realm at path already opened with different inMemory settings.");
        }
        if (m_config.encryption_key != config.encryption_key) {
            throw MismatchedConfigException("Realm at path already opened with a different encryption key.");
        }
        if (m_config.schema_version != config.schema_version && config.schema_version != ObjectStore::NotVersioned) {
            throw MismatchedConfigException("Realm at path already opened with different schema version.");
        }
        // FIXME: verify that schema is compatible
        // Needs to verify that all tables present in both are identical, and
        // then updated m_config with any tables present in config but not in
        // it
        // Public API currently doesn't make it possible to have non-matching
        // schemata so it's not a huge issue
        if ((false) && m_config.schema != config.schema) {
            throw MismatchedConfigException("Realm at path already opened with different schema");
        }
    }

    if (config.cache) {
        for (auto& cachedRealm : m_weak_realm_notifiers) {
            if (cachedRealm.is_cached_for_current_thread()) {
                // can be null if we jumped in between ref count hitting zero and
                // unregister_realm() getting the lock
                if (auto realm = cachedRealm.realm()) {
                    return realm;
                }
            }
        }
    }

    auto realm = std::make_shared<Realm>(std::move(config));
    realm->init(shared_from_this());
    m_weak_realm_notifiers.emplace_back(realm, m_config.cache);
    return realm;
}

std::shared_ptr<Realm> RealmCoordinator::get_realm()
{
    return get_realm(m_config);
}

const Schema* RealmCoordinator::get_schema() const noexcept
{
    return m_weak_realm_notifiers.empty() ? nullptr : m_config.schema.get();
}

void RealmCoordinator::update_schema(Schema const& schema)
{
    // FIXME: this should probably be doing some sort of validation and
    // notifying all Realm instances of the new schema in some way
    m_config.schema = std::make_unique<Schema>(schema);
}

RealmCoordinator::RealmCoordinator() = default;

RealmCoordinator::~RealmCoordinator()
{
    std::lock_guard<std::mutex> coordinator_lock(s_coordinator_mutex);
    for (auto it = s_coordinators_per_path.begin(); it != s_coordinators_per_path.end(); ) {
        if (it->second.expired()) {
            it = s_coordinators_per_path.erase(it);
        }
        else {
            ++it;
        }
    }
}

void RealmCoordinator::unregister_realm(Realm* realm)
{
    std::lock_guard<std::mutex> lock(m_realm_mutex);
    auto new_end = remove_if(begin(m_weak_realm_notifiers), end(m_weak_realm_notifiers),
                             [=](auto& notifier) { return notifier.expired() || notifier.is_for_realm(realm); });
    m_weak_realm_notifiers.erase(new_end, end(m_weak_realm_notifiers));
}

void RealmCoordinator::clear_cache()
{
    std::vector<WeakRealm> realms_to_close;
    {
        std::lock_guard<std::mutex> lock(s_coordinator_mutex);

        for (auto& weak_coordinator : s_coordinators_per_path) {
            auto coordinator = weak_coordinator.second.lock();
            if (!coordinator) {
                continue;
            }

            coordinator->m_notifier = nullptr;

            // Gather a list of all of the realms which will be removed
            for (auto& weak_realm_notifier : coordinator->m_weak_realm_notifiers) {
                if (auto realm = weak_realm_notifier.realm()) {
                    realms_to_close.push_back(realm);
                }
            }
        }

        s_coordinators_per_path.clear();
    }

    // Close all of the previously cached Realms. This can't be done while
    // s_coordinator_mutex is held as it may try to re-lock it.
    for (auto& weak_realm : realms_to_close) {
        if (auto realm = weak_realm.lock()) {
            realm->close();
        }
    }
}

void RealmCoordinator::clear_all_caches()
{
    std::vector<std::weak_ptr<RealmCoordinator>> to_clear;
    {
        std::lock_guard<std::mutex> lock(s_coordinator_mutex);
        for (auto iter : s_coordinators_per_path) {
            to_clear.push_back(iter.second);
        }
    }
    for (auto weak_coordinator : to_clear) {
        if (auto coordinator = weak_coordinator.lock()) {
            coordinator->clear_cache();
        }
    }
}

void RealmCoordinator::send_commit_notifications()
{
    REALM_ASSERT(!m_config.read_only);
    if (m_notifier) {
        m_notifier->notify_others();
    }
}

void RealmCoordinator::pin_version(uint_fast64_t version, uint_fast32_t index)
{
    if (m_async_error) {
        return;
    }

    SharedGroup::VersionID versionid(version, index);
    if (!m_advancer_sg) {
        try {
            std::unique_ptr<Group> read_only_group;
            Realm::open_with_config(m_config, m_advancer_history, m_advancer_sg, read_only_group);
            REALM_ASSERT(!read_only_group);
            m_advancer_sg->begin_read(versionid);
        }
        catch (...) {
            m_async_error = std::current_exception();
            m_advancer_sg = nullptr;
            m_advancer_history = nullptr;
        }
    }
    else if (m_new_notifiers.empty()) {
        // If this is the first notifier then we don't already have a read transaction
        m_advancer_sg->begin_read(versionid);
    }
    else if (versionid < m_advancer_sg->get_version_of_current_transaction()) {
        // Ensure we're holding a readlock on the oldest version we have a
        // handover object for, as handover objects don't
        m_advancer_sg->end_read();
        m_advancer_sg->begin_read(versionid);
    }
}

void RealmCoordinator::register_notifier(std::shared_ptr<BackgroundCollection> notifier)
{
    auto version = notifier->version();
    auto& self = Realm::Internal::get_coordinator(*notifier->get_realm());
    {
        std::lock_guard<std::mutex> lock(self.m_notifier_mutex);
        self.pin_version(version.version, version.index);
        self.m_new_notifiers.push_back(std::move(notifier));
    }
}

void RealmCoordinator::clean_up_dead_notifiers()
{
    auto swap_remove = [&](auto& container) {
        bool did_remove = false;
        for (size_t i = 0; i < container.size(); ++i) {
            if (container[i]->is_alive())
                continue;

            // Ensure the notifier is destroyed here even if there's lingering refs
            // to the async notifier elsewhere
            container[i]->release_data();

            if (container.size() > i + 1)
                container[i] = std::move(container.back());
            container.pop_back();
            --i;
            did_remove = true;
        }
        return did_remove;
    };

    if (swap_remove(m_notifiers)) {
        // Make sure we aren't holding on to read versions needlessly if there
        // are no notifiers left, but don't close them entirely as opening shared
        // groups is expensive
        if (m_notifiers.empty() && m_notifier_sg) {
            m_notifier_sg->end_read();
        }
    }
    if (swap_remove(m_new_notifiers)) {
        if (m_new_notifiers.empty() && m_advancer_sg) {
            m_advancer_sg->end_read();
        }
    }
}

void RealmCoordinator::on_change()
{
    run_async_notifiers();

    std::lock_guard<std::mutex> lock(m_realm_mutex);
    for (auto& realm : m_weak_realm_notifiers) {
        realm.notify();
    }
}

void RealmCoordinator::run_async_notifiers()
{
    std::unique_lock<std::mutex> lock(m_notifier_mutex);

    clean_up_dead_notifiers();

    if (m_notifiers.empty() && m_new_notifiers.empty()) {
        return;
    }

    if (!m_async_error) {
        open_helper_shared_group();
    }

    if (m_async_error) {
        std::move(m_new_notifiers.begin(), m_new_notifiers.end(), std::back_inserter(m_notifiers));
        m_new_notifiers.clear();
        return;
    }

    std::vector<TransactionChangeInfo> change_info;
    SharedGroup::VersionID version;

    auto new_notifiers = std::move(m_new_notifiers);
    if (new_notifiers.empty()) {
        change_info.resize(1);
    }
    else {
        change_info.resize(2);

        // Sort newly added notifiers by their source version so that we can pull them
        // all forward to the latest version in a single pass over the transaction log
        std::sort(new_notifiers.begin(), new_notifiers.end(),
                  [](auto&& lft, auto&& rgt) { return lft->version() < rgt->version(); });
        version = m_advancer_sg->get_version_of_current_transaction();
        REALM_ASSERT(version == new_notifiers.front()->version());

        TransactionChangeInfo* info = &change_info.back();

        // Advance each of the new notifiers to the latest version, attaching them
        // to the SG at their handover version. This requires a unique
        // TransactionChangeInfo for each source version, so that things don't
        // see changes from before the version they were handed over from.
        // Each Info has all of the changes between that source version and the
        // next source version, and they'll be merged together later after
        // releasing the lock
        for (auto& notifier : new_notifiers) {
            if (version != notifier->version()) {
                transaction::advance(*m_advancer_sg, *info, notifier->version());
                change_info.push_back({{}, std::move(info->lists)});
                info = &change_info.back();
                version = notifier->version();
            }
            notifier->attach_to(*m_advancer_sg);
            notifier->add_required_change_info(*info);
        }

        transaction::advance(*m_advancer_sg, *info);

        for (auto& notifier : new_notifiers) {
            notifier->detach();
        }
        version = m_advancer_sg->get_version_of_current_transaction();
        m_advancer_sg->end_read();
    }

    // Make a copy of the notifiers vector and then release the lock to avoid
    // blocking other threads trying to register or unregister notifiers while we run them
    auto notifiers = m_notifiers;
    lock.unlock();

    for (auto& notifier : notifiers) {
        notifier->add_required_change_info(change_info[0]);
    }

    transaction::advance(*m_notifier_sg, change_info[0], version);

    // Attach the new notifiers to the main SG and move them to the main list
    for (auto& notifier : new_notifiers) {
        notifier->attach_to(*m_notifier_sg);
    }
    std::move(new_notifiers.begin(), new_notifiers.end(), std::back_inserter(notifiers));

    for (size_t i = change_info.size() - 1; i > 1; --i) {
        auto& cur = change_info[i];
        if (cur.tables.empty())
            continue;
        auto& prev = change_info[i - 1];
        if (prev.tables.empty()) {
            prev.tables = cur.tables;
            continue;
        }

        for (size_t j = 0; j < prev.tables.size() && j < cur.tables.size(); ++j) {
            prev.tables[j].merge(CollectionChangeBuilder{cur.tables[j]});
        }
        prev.tables.reserve(cur.tables.size());
        while (prev.tables.size() < cur.tables.size()) {
            prev.tables.push_back(cur.tables[prev.tables.size()]);
        }
    }

    // Copy the list change info if there's multiple LinkViews for the same LinkList
    auto id = [](auto const& list) { return std::tie(list.table_ndx, list.col_ndx, list.row_ndx); };
    for (auto& info : change_info) {
        for (size_t i = 1; i < info.lists.size(); ++i) {
            for (size_t j = i; j > 0; --j) {
                if (id(info.lists[i]) == id(info.lists[j - 1])) {
                    info.lists[j - 1].changes->merge(CollectionChangeBuilder{*info.lists[i].changes});
                }
            }
        }
    }

    for (auto& notifier : notifiers) {
        notifier->run();
    }

    // Reacquire the lock while updating the fields that are actually read on
    // other threads
    lock.lock();
    for (auto& notifier : notifiers) {
        notifier->prepare_handover();
    }
    m_notifiers = std::move(notifiers);
    clean_up_dead_notifiers();
}

void RealmCoordinator::open_helper_shared_group()
{
    if (!m_notifier_sg) {
        try {
            std::unique_ptr<Group> read_only_group;
            Realm::open_with_config(m_config, m_notifier_history, m_notifier_sg, read_only_group);
            REALM_ASSERT(!read_only_group);
            m_notifier_sg->begin_read();
        }
        catch (...) {
            // Store the error to be passed to the async notifiers
            m_async_error = std::current_exception();
            m_notifier_sg = nullptr;
            m_notifier_history = nullptr;
        }
    }
    else if (m_notifiers.empty()) {
        m_notifier_sg->begin_read();
    }
}

void RealmCoordinator::advance_to_ready(Realm& realm)
{
    decltype(m_notifiers) notifiers;

    auto& sg = Realm::Internal::get_shared_group(realm);

    auto get_notifier_version = [&] {
        for (auto& notifier : m_notifiers) {
            auto version = notifier->version();
            if (version != SharedGroup::VersionID{}) {
                return version;
            }
        }
        return SharedGroup::VersionID{};
    };

    SharedGroup::VersionID version;
    {
        std::lock_guard<std::mutex> lock(m_notifier_mutex);
        version = get_notifier_version();
    }

    // no async notifiers; just advance to latest
    if (version.version == std::numeric_limits<uint_fast64_t>::max()) {
        transaction::advance(sg, realm.m_binding_context.get());
        return;
    }

    // async results are out of date; ignore
    if (version < sg.get_version_of_current_transaction()) {
        return;
    }

    while (true) {
        // Advance to the ready version without holding any locks because it
        // may end up calling user code (in did_change() notifications)
        transaction::advance(sg, realm.m_binding_context.get(), version);

        // Reacquire the lock and recheck the notifier version, as the notifiers may
        // have advanced to a later version while we didn't hold the lock. If
        // so, we need to release the lock and re-advance
        std::lock_guard<std::mutex> lock(m_notifier_mutex);
        version = get_notifier_version();
        if (version.version == std::numeric_limits<uint_fast64_t>::max())
            return;
        if (version != sg.get_version_of_current_transaction())
            continue;

        // Query version now matches the SG version, so we can deliver them
        for (auto& notifier : m_notifiers) {
            if (notifier->deliver(sg, m_async_error)) {
                notifiers.push_back(notifier);
            }
        }
        break;
    }

    for (auto& notifier : notifiers) {
        notifier->call_callbacks();
    }
}

void RealmCoordinator::process_available_async(Realm& realm)
{
    auto& sg = Realm::Internal::get_shared_group(realm);
    decltype(m_notifiers) notifiers;
    {
        std::lock_guard<std::mutex> lock(m_notifier_mutex);
        for (auto& notifier : m_notifiers) {
            if (notifier->deliver(sg, m_async_error)) {
                notifiers.push_back(notifier);
            }
        }
    }

    for (auto& notifier : notifiers) {
        notifier->call_callbacks();
    }
}
