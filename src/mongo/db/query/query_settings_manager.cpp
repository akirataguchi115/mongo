/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/query_settings_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo::query_settings {

namespace {
static const auto kParameterName = "querySettings";
const auto getQuerySettingsManager =
    ServiceContext::declareDecoration<boost::optional<QuerySettingsManager>>();
}  // namespace

QuerySettingsManager& QuerySettingsManager::get(ServiceContext* service) {
    return *getQuerySettingsManager(service);
}

QuerySettingsManager& QuerySettingsManager::get(OperationContext* opCtx) {
    return *getQuerySettingsManager(opCtx->getServiceContext());
}

void QuerySettingsManager::create(ServiceContext* service) {
    getQuerySettingsManager(service).emplace(service);
}

boost::optional<std::pair<QuerySettings, QueryInstance>>
QuerySettingsManager::getQuerySettingsForQueryShapeHash(
    OperationContext* opCtx,
    const query_shape::QueryShapeHash& queryShapeHash,
    const boost::optional<TenantId>& tenantId) const {
    Lock::SharedLock readLock(opCtx, _mutex);

    // Perform the lookup for query settings map maintained for the given tenant.
    auto versionedQueryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (versionedQueryShapeConfigurationsIt ==
        _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return {};
    }

    // Lookup query settings for 'queryShapeHash'.
    auto& queryShapeConfigurationsMap =
        versionedQueryShapeConfigurationsIt->second.queryShapeConfigurationsMap;
    auto queryShapeConfigurationIt = queryShapeConfigurationsMap.find(queryShapeHash);
    if (queryShapeConfigurationIt == queryShapeConfigurationsMap.end()) {
        return {};
    }

    return queryShapeConfigurationIt->second;
}

void QuerySettingsManager::setQueryShapeConfigurations(
    OperationContext* opCtx,
    std::vector<QueryShapeConfiguration>&& settingsArray,
    LogicalTime parameterClusterTime,
    const boost::optional<TenantId>& tenantId) {
    QueryShapeConfigurationsMap queryShapeConfigurationsMap;
    queryShapeConfigurationsMap.reserve(settingsArray.size());
    for (auto&& queryShapeConfiguration : settingsArray) {
        queryShapeConfigurationsMap.insert({queryShapeConfiguration.getQueryShapeHash(),
                                            {queryShapeConfiguration.getSettings(),
                                             queryShapeConfiguration.getRepresentativeQuery()}});
    }
    Lock::ExclusiveLock writeLock(opCtx, _mutex);
    _tenantIdToVersionedQueryShapeConfigurationsMap.insert_or_assign(
        tenantId,
        VersionedQueryShapeConfigurations{std::move(queryShapeConfigurationsMap),
                                          parameterClusterTime});
}

std::vector<QueryShapeConfiguration> QuerySettingsManager::getAllQueryShapeConfigurations(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const {
    Lock::SharedLock readLock(opCtx, _mutex);
    return getAllQueryShapeConfigurations_inlock(opCtx, tenantId);
}

std::vector<QueryShapeConfiguration> QuerySettingsManager::getAllQueryShapeConfigurations_inlock(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const {
    auto versionedQueryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (versionedQueryShapeConfigurationsIt ==
        _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return {};
    }

    std::vector<QueryShapeConfiguration> configurations;
    for (const auto& [queryShapeHash, value] :
         versionedQueryShapeConfigurationsIt->second.queryShapeConfigurationsMap) {
        configurations.emplace_back(queryShapeHash, value.first, value.second);
    }
    return configurations;
}

void QuerySettingsManager::removeAllQueryShapeConfigurations(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) {
    Lock::ExclusiveLock writeLock(opCtx, _mutex);
    _tenantIdToVersionedQueryShapeConfigurationsMap.erase(tenantId);
}

LogicalTime QuerySettingsManager::getClusterParameterTime(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const {
    Lock::SharedLock readLock(opCtx, _mutex);
    return getClusterParameterTime_inlock(opCtx, tenantId);
}

LogicalTime QuerySettingsManager::getClusterParameterTime_inlock(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const {
    auto versionedQueryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (versionedQueryShapeConfigurationsIt ==
        _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return LogicalTime::kUninitialized;
    }
    return versionedQueryShapeConfigurationsIt->second.clusterParameterTime;
}

void QuerySettingsManager::appendQuerySettingsClusterParameterValue(
    OperationContext* opCtx, BSONObjBuilder* bob, const boost::optional<TenantId>& tenantId) {
    Lock::SharedLock readLock(opCtx, _mutex);
    bob->append("_id"_sd, kParameterName);
    BSONArrayBuilder arrayBuilder(
        bob->subarrayStart(QuerySettingsClusterParameterValue::kSettingsArrayFieldName));
    for (auto&& item : getAllQueryShapeConfigurations_inlock(opCtx, tenantId)) {
        arrayBuilder.append(item.toBSON());
    }
    arrayBuilder.done();
    bob->append(QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName,
                getClusterParameterTime_inlock(opCtx, tenantId).asTimestamp());
}

void QuerySettingsClusterParameter::append(OperationContext* opCtx,
                                           BSONObjBuilder* bob,
                                           StringData name,
                                           const boost::optional<TenantId>& tenantId) {
    auto& querySettingsManager = QuerySettingsManager::get(getGlobalServiceContext());
    querySettingsManager.appendQuerySettingsClusterParameterValue(opCtx, bob, tenantId);
}

Status QuerySettingsClusterParameter::validate(const BSONElement& newValueElement,
                                               const boost::optional<TenantId>& tenantId) const {
    try {
        (void)QuerySettingsClusterParameterValue::parse(
            IDLParserContext("querySettingsParameterValue"), newValueElement.Obj());
        return Status::OK();
    } catch (const AssertionException&) {
        return {ErrorCodes::BadValue,
                "Call setQuerySettings or removeQuerySettings commands in order to set or remove "
                "query settings for a given query shape"};
    }
}

Status QuerySettingsClusterParameter::set(const BSONElement& newValueElement,
                                          const boost::optional<TenantId>& tenantId) {
    auto& querySettingsManager = QuerySettingsManager::get(getGlobalServiceContext());
    auto newSettings = QuerySettingsClusterParameterValue::parse(
        IDLParserContext("querySettingsParameterValue"), newValueElement.Obj());
    querySettingsManager.setQueryShapeConfigurations(Client::getCurrent()->getOperationContext(),
                                                     std::move(newSettings.getSettingsArray()),
                                                     newSettings.getClusterParameterTime(),
                                                     tenantId);
    return Status::OK();
}

Status QuerySettingsClusterParameter::reset(const boost::optional<TenantId>& tenantId) {
    auto& querySettingsManager = QuerySettingsManager::get(getGlobalServiceContext());
    querySettingsManager.removeAllQueryShapeConfigurations(
        Client::getCurrent()->getOperationContext(), tenantId);
    return Status::OK();
}

LogicalTime QuerySettingsClusterParameter::getClusterParameterTime(
    const boost::optional<TenantId>& tenantId) const {
    auto& querySettingsManager = QuerySettingsManager::get(getGlobalServiceContext());
    return querySettingsManager.getClusterParameterTime(Client::getCurrent()->getOperationContext(),
                                                        tenantId);
}

};  // namespace mongo::query_settings
