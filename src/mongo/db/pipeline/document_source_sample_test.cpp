/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

std::unique_ptr<ServiceContextNoop> makeTestServiceContext() {
    auto service = stdx::make_unique<ServiceContextNoop>();
    service->setFastClockSource(stdx::make_unique<ClockSourceMock>());
    service->setTickSource(stdx::make_unique<TickSourceMock>());
    return service;
}

namespace {
using boost::intrusive_ptr;

static const char* const ns = "unittests.document_source_sample_tests";

// Stub to avoid including the server environment library.
MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
    setGlobalServiceContext(makeTestServiceContext());
    return Status::OK();
}

class SampleBasics : public AggregationContextFixture {
public:
    SampleBasics() : _mock(DocumentSourceMock::create()) {}

protected:
    virtual void createSample(long long size) {
        BSONObj spec = BSON("$sample" << BSON("size" << size));
        BSONElement specElement = spec.firstElement();
        _sample = DocumentSourceSample::createFromBson(specElement, getExpCtx());
        sample()->setSource(_mock.get());
        checkBsonRepresentation(spec);
    }

    DocumentSource* sample() {
        return _sample.get();
    }

    DocumentSourceMock* source() {
        return _mock.get();
    }

    /**
     * Makes some general assertions about the results of a $sample stage.
     *
     * Creates a $sample stage with the given size, advances it 'nExpectedResults' times, asserting
     * the results come back in sorted order according to their assigned random values, then asserts
     * the stage is exhausted.
     */
    void checkResults(long long size, long long nExpectedResults) {
        createSample(size);

        boost::optional<Document> prevDoc;
        for (long long i = 0; i < nExpectedResults; i++) {
            auto nextResult = sample()->getNext();
            ASSERT_TRUE(nextResult.isAdvanced());
            auto thisDoc = nextResult.releaseDocument();
            ASSERT_TRUE(thisDoc.hasRandMetaField());
            if (prevDoc) {
                ASSERT_LTE(thisDoc.getRandMetaField(), prevDoc->getRandMetaField());
            }
            prevDoc = std::move(thisDoc);
        }
        assertEOF();
    }

    /**
     * Helper to load 'nDocs' documents into the source stage.
     */
    void loadDocuments(int nDocs) {
        for (int i = 0; i < nDocs; i++) {
            _mock->queue.push_back(DOC("_id" << i));
        }
    }

    /**
     * Assert that iterator state accessors consistently report the source is exhausted.
     */
    void assertEOF() const {
        ASSERT(_sample->getNext().isEOF());
        ASSERT(_sample->getNext().isEOF());
        ASSERT(_sample->getNext().isEOF());
    }

protected:
    intrusive_ptr<DocumentSource> _sample;
    intrusive_ptr<DocumentSourceMock> _mock;

private:
    /**
     * Check that the BSON representation generated by the souce matches the BSON it was
     * created with.
     */
    void checkBsonRepresentation(const BSONObj& spec) {
        Value serialized = static_cast<DocumentSourceSample*>(sample())->serialize();
        auto generatedSpec = serialized.getDocument().toBson();
        ASSERT_BSONOBJ_EQ(spec, generatedSpec);
    }
};

/**
 * A sample of size 0 should return 0 results.
 */
TEST_F(SampleBasics, ZeroSize) {
    loadDocuments(2);
    checkResults(0, 0);
}

/**
 * If the source stage is exhausted, the $sample stage should also be exhausted.
 */
TEST_F(SampleBasics, SourceEOFBeforeSample) {
    loadDocuments(5);
    checkResults(10, 5);
}

/**
 * A $sample stage should limit the number of results to the given size.
 */
TEST_F(SampleBasics, SampleEOFBeforeSource) {
    loadDocuments(10);
    checkResults(5, 5);
}

/**
 * The incoming documents should not be modified by a $sample stage (except their metadata).
 */
TEST_F(SampleBasics, DocsUnmodified) {
    createSample(1);
    source()->queue.push_back(DOC("a" << 1 << "b" << DOC("c" << 2)));
    auto next = sample()->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.releaseDocument();
    ASSERT_EQUALS(1, doc["a"].getInt());
    ASSERT_EQUALS(2, doc["b"]["c"].getInt());
    ASSERT_TRUE(doc.hasRandMetaField());
    assertEOF();
}

TEST_F(SampleBasics, ShouldPropagatePauses) {
    createSample(2);
    source()->queue.push_back(Document());
    source()->queue.push_back(DocumentSource::GetNextResult::makePauseExecution());
    source()->queue.push_back(Document());
    source()->queue.push_back(DocumentSource::GetNextResult::makePauseExecution());
    source()->queue.push_back(Document());
    source()->queue.push_back(DocumentSource::GetNextResult::makePauseExecution());

    // The $sample stage needs to populate itself, so should propagate all three pauses before
    // returning any results.
    ASSERT_TRUE(sample()->getNext().isPaused());
    ASSERT_TRUE(sample()->getNext().isPaused());
    ASSERT_TRUE(sample()->getNext().isPaused());
    ASSERT_TRUE(sample()->getNext().isAdvanced());
    ASSERT_TRUE(sample()->getNext().isAdvanced());
    assertEOF();
}

/**
 * Fixture to test error cases of the $sample stage.
 */
class InvalidSampleSpec : public AggregationContextFixture {
public:
    intrusive_ptr<DocumentSource> createSample(BSONObj sampleSpec) {
        auto specElem = sampleSpec.firstElement();
        return DocumentSourceSample::createFromBson(specElem, getExpCtx());
    }

    BSONObj createSpec(BSONObj spec) {
        return BSON("$sample" << spec);
    }
};

TEST_F(InvalidSampleSpec, NonObject) {
    ASSERT_THROWS_CODE(createSample(BSON("$sample" << 1)), UserException, 28745);
    ASSERT_THROWS_CODE(createSample(BSON("$sample"
                                         << "string")),
                       UserException,
                       28745);
}

TEST_F(InvalidSampleSpec, NonNumericSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size"
                                                    << "string"))),
                       UserException,
                       28746);
}

TEST_F(InvalidSampleSpec, NegativeSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size" << -1))), UserException, 28747);
    ASSERT_THROWS_CODE(createSample(createSpec(BSON("size" << -1.0))), UserException, 28747);
}

TEST_F(InvalidSampleSpec, ExtraOption) {
    ASSERT_THROWS_CODE(
        createSample(createSpec(BSON("size" << 1 << "extra" << 2))), UserException, 28748);
}

TEST_F(InvalidSampleSpec, MissingSize) {
    ASSERT_THROWS_CODE(createSample(createSpec(BSONObj())), UserException, 28749);
}

//
// Test the implementation that gets results from a random cursor.
//

class SampleFromRandomCursorBasics : public SampleBasics {
public:
    void createSample(long long size) override {
        _sample = DocumentSourceSampleFromRandomCursor::create(getExpCtx(), size, "_id", 100);
        sample()->setSource(_mock.get());
    }
};

/**
 * A sample of size zero should not return any results.
 */
TEST_F(SampleFromRandomCursorBasics, ZeroSize) {
    loadDocuments(2);
    checkResults(0, 0);
}

/**
 * When sampling with a size smaller than the number of documents our source stage can produce,
 * there should be no more than the sample size output.
 */
TEST_F(SampleFromRandomCursorBasics, SourceEOFBeforeSample) {
    loadDocuments(5);
    checkResults(10, 5);
}

/**
 * When the source stage runs out of documents, the $sampleFromRandomCursors stage should be
 * exhausted.
 */
TEST_F(SampleFromRandomCursorBasics, SampleEOFBeforeSource) {
    loadDocuments(10);
    checkResults(5, 5);
}

/**
 * The $sampleFromRandomCursor stage should not modify the contents of the documents.
 */
TEST_F(SampleFromRandomCursorBasics, DocsUnmodified) {
    createSample(1);
    source()->queue.push_back(DOC("_id" << 1 << "b" << DOC("c" << 2)));
    auto next = sample()->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.releaseDocument();
    ASSERT_EQUALS(1, doc["_id"].getInt());
    ASSERT_EQUALS(2, doc["b"]["c"].getInt());
    ASSERT_TRUE(doc.hasRandMetaField());
    assertEOF();
}

/**
 * The $sampleFromRandomCursor stage should ignore duplicate documents.
 */
TEST_F(SampleFromRandomCursorBasics, IgnoreDuplicates) {
    createSample(2);
    source()->queue.push_back(DOC("_id" << 1));
    source()->queue.push_back(DOC("_id" << 1));  // Duplicate, should ignore.
    source()->queue.push_back(DOC("_id" << 2));

    auto next = sample()->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.releaseDocument();
    ASSERT_EQUALS(1, doc["_id"].getInt());
    ASSERT_TRUE(doc.hasRandMetaField());
    double doc1Meta = doc.getRandMetaField();

    // Should ignore the duplicate {_id: 1}, and return {_id: 2}.
    next = sample()->getNext();
    ASSERT_TRUE(next.isAdvanced());
    doc = next.releaseDocument();
    ASSERT_EQUALS(2, doc["_id"].getInt());
    ASSERT_TRUE(doc.hasRandMetaField());
    double doc2Meta = doc.getRandMetaField();
    ASSERT_GTE(doc1Meta, doc2Meta);

    // Both stages should be exhausted.
    ASSERT_TRUE(source()->getNext().isEOF());
    assertEOF();
}

/**
 * The $sampleFromRandomCursor stage should error if it receives too many duplicate documents.
 */
TEST_F(SampleFromRandomCursorBasics, TooManyDups) {
    createSample(2);
    for (int i = 0; i < 1000; i++) {
        source()->queue.push_back(DOC("_id" << 1));
    }

    // First should be successful, it's not a duplicate.
    ASSERT_TRUE(sample()->getNext().isAdvanced());

    // The rest are duplicates, should error.
    ASSERT_THROWS_CODE(sample()->getNext(), UserException, 28799);
}

/**
 * The $sampleFromRandomCursor stage should error if it receives a document without an _id.
 */
TEST_F(SampleFromRandomCursorBasics, MissingIdField) {
    // Once with only a bad document.
    createSample(2);  // _idField is '_id'.
    source()->queue.push_back(DOC("non_id" << 2));
    ASSERT_THROWS_CODE(sample()->getNext(), UserException, 28793);

    // Again, with some regular documents before a bad one.
    createSample(2);  // _idField is '_id'.
    source()->queue.push_back(DOC("_id" << 1));
    source()->queue.push_back(DOC("_id" << 1));
    source()->queue.push_back(DOC("non_id" << 2));

    // First should be successful.
    ASSERT_TRUE(sample()->getNext().isAdvanced());

    ASSERT_THROWS_CODE(sample()->getNext(), UserException, 28793);
}

/**
 * The $sampleFromRandomCursor stage should set the random meta value in a way that mimics the
 * non-optimized case.
 */
TEST_F(SampleFromRandomCursorBasics, MimicNonOptimized) {
    // Compute the average random meta value on the each doc returned.
    double firstTotal = 0.0;
    double secondTotal = 0.0;
    int nTrials = 10000;
    for (int i = 0; i < nTrials; i++) {
        // Sample 2 out of 3 documents.
        _sample = DocumentSourceSampleFromRandomCursor::create(getExpCtx(), 2, "_id", 3);
        sample()->setSource(_mock.get());

        source()->queue.push_back(DOC("_id" << 1));
        source()->queue.push_back(DOC("_id" << 2));

        auto doc = sample()->getNext();
        ASSERT_TRUE(doc.isAdvanced());
        ASSERT_TRUE(doc.getDocument().hasRandMetaField());
        firstTotal += doc.getDocument().getRandMetaField();

        doc = sample()->getNext();
        ASSERT_TRUE(doc.isAdvanced());
        ASSERT_TRUE(doc.getDocument().hasRandMetaField());
        secondTotal += doc.getDocument().getRandMetaField();
    }
    // The average random meta value of the first document should be about 0.75. We assume that
    // 10000 trials is sufficient for us to apply the Central Limit Theorem. Using an error
    // tolerance of 0.02 gives us a spurious failure rate approximately equal to 10^-24.
    ASSERT_GTE(firstTotal / nTrials, 0.73);
    ASSERT_LTE(firstTotal / nTrials, 0.77);

    // The average random meta value of the second document should be about 0.5.
    ASSERT_GTE(secondTotal / nTrials, 0.48);
    ASSERT_LTE(secondTotal / nTrials, 0.52);
}

DEATH_TEST_F(SampleFromRandomCursorBasics,
             ShouldFailIfGivenPausedInput,
             "Invariant failure Hit a MONGO_UNREACHABLE!") {
    createSample(2);
    source()->queue.push_back(Document{{"_id", 1}});
    source()->queue.push_back(DocumentSource::GetNextResult::makePauseExecution());

    // Should see the first result, then see a pause and fail.
    ASSERT_TRUE(sample()->getNext().isAdvanced());
    sample()->getNext();
}

}  // namespace
}  // namespace mongo