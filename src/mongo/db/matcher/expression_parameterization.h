/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/preprocessor/control/iif.hpp>
#include <cstdint>
#include <vector>

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/util/assert_util_core.h"

namespace mongo {
/**
 * A context to track assigned input parameter IDs for auto-parameterization.
 */
struct MatchExpressionParameterizationVisitorContext {
    using InputParamId = MatchExpression::InputParamId;

    MatchExpressionParameterizationVisitorContext(
        boost::optional<size_t> inputMaxParamCount = boost::none, InputParamId startingParamId = 0)
        : maxParamCount(inputMaxParamCount), nextParamId(startingParamId) {}

    /**
     * Reports whether the requested number of parameter IDs can be assigned within the
     * 'maxParamCount' limit. Used by callers that need to parameterize all or none of the arguments
     * of an expression because MatchExpressionSbePlanCacheKeySerializationVisitor visit() methods
     * expect those to either be fully parameterized or unparameterized. This must set
     * 'parameterized' to false if the requested IDs are not available, as the caller will then not
     * parameterize any of its arguments, which means the query will not be fully parameterized
     * even if we do not end up using all the allowed parameter IDs.
     */
    bool availableParamIds(int numIds) {
        if (!parameterized) {
            return false;
        }
        if (maxParamCount &&
            (static_cast<size_t>(nextParamId) + static_cast<size_t>(numIds)) > *maxParamCount) {
            parameterized = false;
            return false;
        }
        return true;
    }

    /**
     * Assigns a parameter ID to `expr` with the ability to reuse an already-assigned parameter id
     * if `expr` is equivalent to an expression we have seen before. This is used to model
     * dependencies within a query (e.g. $or[{a:1}, {a:1, b:1}] --> $or[{a:P0}, {a:P0, b:P1}]) and
     * to reduce the number of parameters. The reusable parameters use the same vector for tracking
     * as the non-reusable to ensure uniqueness of the parameterId.
     *
     * If 'maxParamCount' was specified, this stops creating new parameters once that limit has been
     * reached and returns boost::none instead.
     */
    boost::optional<InputParamId> nextReusableInputParamId(const MatchExpression* expr) {
        if (!parameterized) {
            return boost::none;
        }

        if (expr) {
            // Check to see if the expression is in the map already.
            auto it = std::find_if(
                inputParamIdToExpressionMap.begin(),
                inputParamIdToExpressionMap.end(),
                [expr](const MatchExpression* m) -> bool { return m->equivalent(expr); });
            if (it == inputParamIdToExpressionMap.end()) {
                return nextInputParamId(expr);  // not found; create new param
            }
            return it - inputParamIdToExpressionMap.begin();  // found; reuse old param
        }
        return boost::none;
    }

    /**
     * Assigns a parameter ID to 'expr'. This is not only a helper for nextReusableInputParamId();
     * it is also called directly by visit() methods whose expressions are deemed non-shareable.
     *
     * If 'maxParamCount' was specified, this stops creating new parameters once that limit has been
     * reached and returns boost::none instead.
     */
    boost::optional<InputParamId> nextInputParamId(const MatchExpression* expr) {
        if (!parameterized) {
            return boost::none;
        }
        if (maxParamCount && static_cast<size_t>(nextParamId) >= *maxParamCount) {
            parameterized = false;
            return boost::none;
        }

        inputParamIdToExpressionMap.emplace_back(expr);
        return nextParamId++;
    }

    // Map from assigned InputParamId to parameterised MatchExpression. Although it is called a map,
    // it can be safely represented as a vector because in this class we control that inputParamId
    // is an increasing sequence of integers starting from 0.
    std::vector<const MatchExpression*> inputParamIdToExpressionMap;

    // This is the maximumum number of MatchExpression parameters a single CanonicalQuery may have.
    // A value of boost::none means unlimited.
    boost::optional<size_t> maxParamCount;

    // This is the next input parameter ID to assign. It may be initialized to a value > 0 to enable
    // a forest of match expressions to be parameterized by allowing each tree to continue parameter
    // IDs from where the prior tree left off.
    InputParamId nextParamId;

    // This is changed to false if an attempt to parameterize ever failed (because it would exceed
    // 'maxParamCount').
    bool parameterized = true;
};

/**
 * An implementation of a MatchExpression visitor which assigns an optional input parameter ID to
 * each node which is eligible for auto-parameterization:
 *  - BitsAllClearMatchExpression
 *  - BitsAllSetMatchExpression
 *  - BitsAnyClearMatchExpression
 *  - BitsAnySetMatchExpression
 *  - BitTestMatchExpression (two parameter IDs for the position and mask)
 *  - Comparison expressions, unless compared against MinKey, MaxKey, null or NaN value or array
 *      - EqualityMatchExpression
 *      - GTEMatchExpression
 *      - GTMatchExpression
 *      - LTEMatchExpression
 *      - LTMatchExpression
 *  - InMatchExpression, unless it contains an array, null or regexp value.
 *  - ModMatchExpression (two parameter IDs for the divider and reminder)
 *  - RegexMatchExpression (two parameter IDs for the compiled regex and raw value)
 *  - SizeMatchExpression
 *  - TypeMatchExpression, unless type value is Array
 *  - WhereMatchExpression
 */
class MatchExpressionParameterizationVisitor final : public MatchExpressionMutableVisitor {
public:
    MatchExpressionParameterizationVisitor(MatchExpressionParameterizationVisitorContext* context)
        : _context{context} {
        invariant(_context);
    }

    void visit(AlwaysFalseMatchExpression* expr) final {}
    void visit(AlwaysTrueMatchExpression* expr) final {}
    void visit(AndMatchExpression* expr) final {}
    void visit(BitsAllClearMatchExpression* expr) final;
    void visit(BitsAllSetMatchExpression* expr) final;
    void visit(BitsAnyClearMatchExpression* expr) final;
    void visit(BitsAnySetMatchExpression* expr) final;
    void visit(ElemMatchObjectMatchExpression* matchExpr) final {}
    void visit(ElemMatchValueMatchExpression* matchExpr) final {}
    void visit(EqualityMatchExpression* expr) final;
    void visit(ExistsMatchExpression* expr) final {}
    void visit(ExprMatchExpression* expr) final {}
    void visit(GTEMatchExpression* expr) final;
    void visit(GTMatchExpression* expr) final;
    void visit(GeoMatchExpression* expr) final {}
    void visit(GeoNearMatchExpression* expr) final {}
    void visit(InMatchExpression* expr) final;
    void visit(InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(InternalExprEqMatchExpression* expr) final {}
    void visit(InternalExprGTMatchExpression* expr) final {}
    void visit(InternalExprGTEMatchExpression* expr) final {}
    void visit(InternalExprLTMatchExpression* expr) final {}
    void visit(InternalExprLTEMatchExpression* expr) final {}
    void visit(InternalEqHashedKey* expr) final {
        // Don't support parameterization of InternEqHashedKey because it is not implemented in SBE.
    }
    void visit(InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
    void visit(InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(InternalSchemaCondMatchExpression* expr) final {}
    void visit(InternalSchemaEqMatchExpression* expr) final {}
    void visit(InternalSchemaFmodMatchExpression* expr) final {}
    void visit(InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaObjectMatchExpression* expr) final {}
    void visit(InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(InternalSchemaTypeExpression* expr) final {}
    void visit(InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(InternalSchemaXorMatchExpression* expr) final {}
    void visit(LTEMatchExpression* expr) final;
    void visit(LTMatchExpression* expr) final;
    void visit(ModMatchExpression* expr) final;
    void visit(NorMatchExpression* expr) final {}
    void visit(NotMatchExpression* expr) final {}
    void visit(OrMatchExpression* expr) final {}
    void visit(RegexMatchExpression* expr) final;
    void visit(SizeMatchExpression* expr) final;
    void visit(TextMatchExpression* expr) final {}
    void visit(TextNoOpMatchExpression* expr) final {}
    void visit(TwoDPtInAnnulusExpression* expr) final {}
    void visit(TypeMatchExpression* expr) final;
    void visit(WhereMatchExpression* expr) final;
    void visit(WhereNoOpMatchExpression* expr) final {}

private:
    void visitComparisonMatchExpression(ComparisonMatchExpressionBase* expr);

    void visitBitTestExpression(BitTestMatchExpression* expr);

    MatchExpressionParameterizationVisitorContext* _context;
};

/**
 * A match expression tree walker compatible with tree_walker::walk() to be used with
 * MatchExpressionParameterizationVisitor.
 */
class MatchExpressionParameterizationWalker {
public:
    MatchExpressionParameterizationWalker(MatchExpressionParameterizationVisitor* visitor)
        : _visitor{visitor} {
        invariant(_visitor);
    }

    void preVisit(MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(MatchExpression* expr) {}

    void inVisit(long count, MatchExpression* expr) {}

private:
    MatchExpressionParameterizationVisitor* _visitor;
};

}  // namespace mongo
