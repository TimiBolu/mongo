/**
 * Verifies that showRecordId() returns the ObjectId type for time-series collections.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const timeFieldName = "time";

const coll = db.timeseries_show_record_id;
coll.drop();

assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

Random.setRandomSeed();

const numHosts = 10;
const hosts = TimeseriesTest.generateHosts(numHosts);

for (let i = 0; i < 100; i++) {
    const host = TimeseriesTest.getRandomElem(hosts);
    TimeseriesTest.updateUsages(host.fields);

    assert.commandWorked(coll.insert({
        measurement: "cpu",
        time: ISODate(),
        fields: host.fields,
        tags: host.tags,
    }));
}

function checkRecordId(documents) {
    for (const document of documents) {
        assert(document.hasOwnProperty("$recordId"));
        if (TimeseriesTest.supportsClusteredIndexes(db.getMongo())) {
            assert(isString(document["$recordId"]));
        }
    }
}

// The time-series user view uses aggregation to build a representation of the data.
// showRecordId() is not support in aggregation.
const error = assert.throws(() => {
    coll.find().showRecordId().toArray();
});
assert.commandFailedWithCode(error, ErrorCodes.InvalidPipelineOperator);

const bucketsColl = db.getCollection("system.buckets." + coll.getName());
checkRecordId(bucketsColl.find().showRecordId().toArray());
})();
