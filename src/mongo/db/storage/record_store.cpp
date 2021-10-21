/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

Status RecordStore::oplogDiskLocRegister(OperationContext* opCtx,
                                         const Timestamp& opTime,
                                         bool orderedCommit) {
    // Callers should be updating visibility as part of a write operation. We want to ensure that
    // we never get here while holding an uninterruptible, read-ticketed lock. That would indicate
    // that we are operating with the wrong global lock semantics, and either hold too weak a lock
    // (e.g. IS) or that we upgraded in a way we shouldn't (e.g. IS -> IX).
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->hasReadTicket() ||
              !opCtx->lockState()->uninterruptibleLocksRequested());

    return oplogDiskLocRegisterImpl(opCtx, opTime, orderedCommit);
}

void RecordStore::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const {
    // Callers are waiting for other operations to finish updating visibility. We want to ensure
    // that we never get here while holding an uninterruptible, write-ticketed lock. That could
    // indicate we are holding a stronger lock than we need to, and that we could actually
    // contribute to ticket-exhaustion. That could prevent the write we are waiting on from
    // acquiring the lock it needs to update the oplog visibility.
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->hasWriteTicket() ||
              !opCtx->lockState()->uninterruptibleLocksRequested());

    waitForAllEarlierOplogWritesToBeVisibleImpl(opCtx);
}

}  // namespace mongo
