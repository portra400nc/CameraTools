// Reads a fixture written by make_fixture.py into FrameEvents, for the tests only.
#pragma once

#include "../core/recipe.h"

#include <string>
#include <vector>

namespace photoreal::fixture
{
    // A draw's inputs as the fixture recorded them. Answers DrawQueries as NativeDraw does in the add-on.
    class FixtureDraw final : public DrawQueries
    {
    public:
        std::vector<Texture> inputs;
        const Texture *ps_inputs(uint32_t *count) override { *count = static_cast<uint32_t>(inputs.size()); return inputs.data(); }
    };

    struct Row
    {
        uint32_t seq = 0;
        enum class Kind { bind, clear, draw } kind = Kind::draw;
        Targets targets;    // bind
        Texture cleared;    // clear
        Color color {};     // clear
        FixtureDraw draw;   // draw
    };

    // Parses <id>:<FORMAT>:<w>x<h> lists. FORMAT names map to Format through a table of the names the census writes,
    // and any other name to Format::unknown, which no matcher accepts.
    std::vector<Row> load(const std::string &path);

    // The event a row stands for. Draw rows refer to the row's FixtureDraw, so rows must outlive the event.
    FrameEvent event(Row &row);

    // Every size equal to from becomes to, and every quarter of from (floored) becomes to_quarter: a capture replayed at
    // another render size.
    void rescale(std::vector<Row> &rows, Size from, Size to, Size to_quarter);

    // Drops the rows whose seq is in [first, last], to replay a frame with a step missing.
    void drop(std::vector<Row> &rows, uint32_t first, uint32_t last);

    // Copies of the rows whose seq is in [first, last].
    std::vector<Row> slice(const std::vector<Row> &rows, uint32_t first, uint32_t last);

    // Replaces resource ids everywhere they appear: targets, depth, clears and draw inputs.
    void remap(std::vector<Row> &rows, const std::vector<std::pair<uint64_t, uint64_t>> &ids);

    // Inserts extra before the row with seq.
    void insert_before(std::vector<Row> &rows, uint32_t seq, const std::vector<Row> &extra);

    Row &find(std::vector<Row> &rows, uint32_t seq);

    // Feeds rows through tracker as one frame (begin_frame first); returns (seq, step) for every event that completed
    // a step.
    struct Moment
    {
        uint32_t seq;
        Step step;
        friend bool operator==(const Moment &a, const Moment &b) { return a.seq == b.seq && a.step == b.step; }
    };
    std::vector<Moment> replay(std::vector<Row> &rows, FrameTracker &tracker);
}
