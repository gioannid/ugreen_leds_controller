#include "protocol.h"
#include "test_framework.h"

using namespace ugreen;

void run_protocol_tests() {
    std::cout << "Running Protocol Tests...\n";

    test::run_test("StreamBuffer fragmented input", []() {
        StreamBuffer buf;
        std::string line;
        bool overflow = false;

        TEST_ASSERT(!buf.extract_line(line, overflow));
        TEST_ASSERT(!overflow);

        buf.append("HELL", 4);
        TEST_ASSERT(!buf.extract_line(line, overflow));
        TEST_ASSERT(!overflow);

        buf.append("O 1 1\n", 6);
        TEST_ASSERT(buf.extract_line(line, overflow));
        TEST_ASSERT(!overflow);
        TEST_ASSERT_EQ(line, "HELLO 1 1");
        TEST_ASSERT(!buf.extract_line(line, overflow));
    });

    test::run_test("StreamBuffer multiple commands in single read", []() {
        StreamBuffer buf;
        std::string line;
        bool overflow = false;

        buf.append("HELLO 1 1\r\nCAPS 2\nSET 3 disk1 brightness=100\n", 45);

        TEST_ASSERT(buf.extract_line(line, overflow));
        TEST_ASSERT_EQ(line, "HELLO 1 1");

        TEST_ASSERT(buf.extract_line(line, overflow));
        TEST_ASSERT_EQ(line, "CAPS 2");

        TEST_ASSERT(buf.extract_line(line, overflow));
        TEST_ASSERT_EQ(line, "SET 3 disk1 brightness=100");

        TEST_ASSERT(!buf.extract_line(line, overflow));
    });

    test::run_test("StreamBuffer line length limit enforcement", []() {
        StreamBuffer buf;
        std::string line;
        bool overflow = false;

        std::string huge(kMaxLineLength + 10, 'A');
        huge += "\n";
        buf.append(huge.data(), huge.size());

        TEST_ASSERT(buf.extract_line(line, overflow));
        TEST_ASSERT(overflow);
    });

    test::run_test("StreamBuffer total buffer size limit enforcement", []() {
        StreamBuffer buf;
        std::string huge(kMaxRecvBuffer + 1, 'x');
        auto res = buf.append(huge.data(), huge.size());
        TEST_ASSERT(res == StreamBuffer::AppendResult::BufferOverflow);
    });

    test::run_test("Parse HELLO valid and invalid", []() {
        auto r1 = ProtocolParser::parse_line("HELLO 10 1");
        TEST_ASSERT(r1.status == ParseResult::Status::Success);
        TEST_ASSERT(r1.request.has_value());
        TEST_ASSERT_EQ(r1.request->verb, Verb::Hello);
        TEST_ASSERT_EQ(r1.request->request_id, 10ULL);
        TEST_ASSERT_EQ(r1.request->version, 1U);

        auto r2 = ProtocolParser::parse_line("HELLO 11 2");
        TEST_ASSERT(r2.status == ParseResult::Status::Error);
        TEST_ASSERT(r2.error_response.has_value());
        TEST_ASSERT_EQ(r2.error_response->action, "UNSUPPORTED_VERSION");

        auto r3 = ProtocolParser::parse_line("HELLO 12");
        TEST_ASSERT(r3.status == ParseResult::Status::Error);
    });

    test::run_test("Parse CAPS and FLUSH", []() {
        auto c = ProtocolParser::parse_line("CAPS 42");
        TEST_ASSERT(c.status == ParseResult::Status::Success);
        TEST_ASSERT_EQ(c.request->verb, Verb::Caps);
        TEST_ASSERT_EQ(c.request->request_id, 42ULL);

        auto f = ProtocolParser::parse_line("FLUSH 99");
        TEST_ASSERT(f.status == ParseResult::Status::Success);
        TEST_ASSERT_EQ(f.request->verb, Verb::Flush);
        TEST_ASSERT_EQ(f.request->request_id, 99ULL);
    });

    test::run_test("Parse SET valid fields", []() {
        auto r = ProtocolParser::parse_line("SET 101 disk1 mode=blink brightness=128 rgb=255,0,128 on_ms=200 off_ms=300 priority=5");
        TEST_ASSERT(r.status == ParseResult::Status::Success);
        TEST_ASSERT(r.request.has_value());
        const auto& req = *r.request;
        TEST_ASSERT_EQ(req.verb, Verb::Set);
        TEST_ASSERT_EQ(req.request_id, 101ULL);
        TEST_ASSERT_EQ(req.target.id, LedId::Disk1);
        TEST_ASSERT(!req.target.is_all);
        TEST_ASSERT(req.patch.mode.has_value());
        TEST_ASSERT_EQ(*req.patch.mode, OpMode::Blink);
        TEST_ASSERT(req.patch.brightness.has_value());
        TEST_ASSERT_EQ(*req.patch.brightness, 128);
        TEST_ASSERT(req.patch.rgb.has_value());
        TEST_ASSERT_EQ(req.patch.rgb->r, 255);
        TEST_ASSERT_EQ(req.patch.rgb->g, 0);
        TEST_ASSERT_EQ(req.patch.rgb->b, 128);
        TEST_ASSERT(req.patch.on_ms.has_value());
        TEST_ASSERT_EQ(*req.patch.on_ms, 200);
        TEST_ASSERT(req.patch.off_ms.has_value());
        TEST_ASSERT_EQ(*req.patch.off_ms, 300);
        TEST_ASSERT(req.patch.priority.has_value());
        TEST_ASSERT_EQ(*req.patch.priority, 5);
        TEST_ASSERT(!req.reply_none);
    });

    test::run_test("Parse SET with reply=none and all target", []() {
        auto r = ProtocolParser::parse_line("SET 102 all brightness=200 reply=none");
        TEST_ASSERT(r.status == ParseResult::Status::Success);
        TEST_ASSERT(r.request->target.is_all);
        TEST_ASSERT(r.request->reply_none);
        TEST_ASSERT_EQ(*r.request->patch.brightness, 200);
    });

    test::run_test("Parse SET invalid fields and duplicates", []() {
        auto dup = ProtocolParser::parse_line("SET 103 disk1 brightness=50 brightness=60");
        TEST_ASSERT(dup.status == ParseResult::Status::Error);
        TEST_ASSERT_EQ(dup.error_response->action, "INVALID_ARGUMENT");

        auto unk = ProtocolParser::parse_line("SET 104 disk1 unknown_field=123");
        TEST_ASSERT(unk.status == ParseResult::Status::Error);
        TEST_ASSERT_EQ(unk.error_response->action, "INVALID_ARGUMENT");

        auto bad_rgb = ProtocolParser::parse_line("SET 105 disk1 rgb=0,300,0");
        TEST_ASSERT(bad_rgb.status == ParseResult::Status::Error);
        TEST_ASSERT_EQ(bad_rgb.error_response->action, "INVALID_ARGUMENT");

        auto bad_mode = ProtocolParser::parse_line("SET 106 disk1 mode=strobe");
        TEST_ASSERT(bad_mode.status == ParseResult::Status::Error);
        TEST_ASSERT_EQ(bad_mode.error_response->action, "INVALID_ARGUMENT");
    });

    test::run_test("Timing validation constraints", []() {
        // Combined cycle > 65535 must be rejected
        auto r1 = ProtocolParser::parse_line("SET 107 disk1 on_ms=40000 off_ms=30000");
        TEST_ASSERT(r1.status == ParseResult::Status::Error);
        TEST_ASSERT_EQ(r1.error_response->action, "INVALID_ARGUMENT");

        // Zero duration in blink mode must be rejected
        auto r2 = ProtocolParser::parse_line("SET 108 disk1 mode=blink on_ms=0 off_ms=0");
        TEST_ASSERT(r2.status == ParseResult::Status::Error);
        TEST_ASSERT_EQ(r2.error_response->action, "INVALID_ARGUMENT");

        // Valid boundary 65535 sum
        auto r3 = ProtocolParser::parse_line("SET 109 disk1 on_ms=35535 off_ms=30000");
        TEST_ASSERT(r3.status == ParseResult::Status::Success);
    });

    test::run_test("Parse GET and RESET", []() {
        auto g1 = ProtocolParser::parse_line("GET 200 disk1");
        TEST_ASSERT(g1.status == ParseResult::Status::Success);
        TEST_ASSERT_EQ(g1.request->source, "desired");

        auto g2 = ProtocolParser::parse_line("GET 201 disk2 source=hardware");
        TEST_ASSERT(g2.status == ParseResult::Status::Success);
        TEST_ASSERT_EQ(g2.request->source, "hardware");

        auto rst = ProtocolParser::parse_line("RESET 202 power");
        TEST_ASSERT(rst.status == ParseResult::Status::Success);
        TEST_ASSERT_EQ(rst.request->target.id, LedId::Power);

        auto rst_all = ProtocolParser::parse_line("RESET 203 all");
        TEST_ASSERT(rst_all.status == ParseResult::Status::Success);
        TEST_ASSERT(rst_all.request->target.is_all);
    });

    test::run_test("Response serialization", []() {
        Response ok = Response::ok(10, "ACCEPTED", {{"generation", "42"}});
        TEST_ASSERT_EQ(ok.serialize(), "OK 10 ACCEPTED generation=42\n");

        Response err = Response::error(11, "INVALID_ARGUMENT", {{"field", "on_ms"}});
        TEST_ASSERT_EQ(err.serialize(), "ERR 11 INVALID_ARGUMENT field=on_ms\n");
    });
}
