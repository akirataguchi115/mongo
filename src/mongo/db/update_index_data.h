/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/field_ref.h"
#include <absl/container/btree_set.h>

namespace mongo {

/**
 * Holds pre-processed index spec information to allow update to quickly determine if an update
 * can be applied as a delta to a document, or if the document must be re-indexed.
 */
class UpdateIndexData {
public:
    UpdateIndexData();

    /**
     * Returns the canonicalized index form for 'path', removing numerical path components as well
     * as '$' path components.
     */
    static FieldRef getCanonicalIndexField(const FieldRef& path);

    /**
     * Returns whether the provided path component can be included in the canonicalized index form
     * of a path.
     */
    static bool isComponentPartOfCanonicalizedIndexPath(StringData pathComponent);

    /**
     * Register a path.  Any update targeting this path (or a parent of this path) will
     * trigger a recomputation of the document's index keys.
     */
    void addPath(const FieldRef& path);

    /**
     * Register a path component.  Any update targeting a path that contains this exact
     * component will trigger a recomputation of the document's index keys.
     */
    void addPathComponent(StringData pathComponent);

    /**
     * Register the "wildcard" path.  All updates will trigger a recomputation of the document's
     * index keys.
     */
    void allPathsIndexed();

    void clear();

    bool mightBeIndexed(const FieldRef& path) const;

    /**
     * Return whether this structure has been cleared or has not been initialized yet.
     */
    bool isEmpty() const;

private:
    /**
     * Returns true if 'b' is a prefix of 'a', or if the two paths are equal.
     */
    bool _startsWith(const FieldRef& a, const FieldRef& b) const;

    absl::btree_set<FieldRef> _canonicalPaths;
    absl::btree_set<std::string> _pathComponents;

    bool _allPathsIndexed;
};
}  // namespace mongo
