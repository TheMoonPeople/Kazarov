#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "kazarov/log.hpp"

namespace {

struct Line {
    kz::LogLevel level;
    std::string  subsystem;
    std::string  message;
};

std::vector<Line> g_lines;

void RecordLine(kz::LogLevel level, std::string_view subsystem, std::string_view message) {
    g_lines.push_back({level, std::string(subsystem), std::string(message)});
}

// Reports the hook's arguments on stderr, where a death test reads them, and returns.
void PrintingHook(std::string_view subsystem, std::string_view message, bool unimplemented) {
    std::fprintf(stderr, "hook: %.*s; %.*s; %s\n", static_cast<int>(subsystem.size()), subsystem.data(),
                 static_cast<int>(message.size()), message.data(), unimplemented ? "unimplemented" : "fatal");
}

// Fails fatally again from inside the hook: the inner Fatal must return without re-entering it.
void ReenteringHook(std::string_view, std::string_view, bool) {
    static int entries = 0;
    entries++;
    if (entries > 1) {
        std::_Exit(4);
    }
    kz::Fatal("again", "inside the hook", /*unimplemented=*/false);
    std::_Exit(3);
}

} // namespace

TEST(KazarovLog, SinkReceivesEachLine) {
    g_lines.clear();
    kz::LogSink previous = kz::SetLogSink(RecordLine);
    kz::Log(kz::LogLevel::Warning, "audio", "no playback device");
    kz::Logf(kz::LogLevel::Info, "gfx", "{} pipelines ready in {:.2f} s", 12, 0.5);
    EXPECT_EQ(kz::SetLogSink(previous), RecordLine);

    ASSERT_EQ(g_lines.size(), 2U);
    EXPECT_EQ(g_lines[0].level, kz::LogLevel::Warning);
    EXPECT_EQ(g_lines[0].subsystem, "audio");
    EXPECT_EQ(g_lines[0].message, "no playback device");
    EXPECT_EQ(g_lines[1].level, kz::LogLevel::Info);
    EXPECT_EQ(g_lines[1].subsystem, "gfx");
    EXPECT_EQ(g_lines[1].message, "12 pipelines ready in 0.50 s");
}

TEST(KazarovLog, WithoutASinkLinesGoToStderr) {
    kz::LogSink previous = kz::SetLogSink(nullptr);
    testing::internal::CaptureStderr();
    kz::Log(kz::LogLevel::Error, "font", "cannot read a.ttf");
    std::string written = testing::internal::GetCapturedStderr();
    kz::SetLogSink(previous);
    EXPECT_EQ(written, "font: cannot read a.ttf\n");
}

TEST(KazarovLogDeathTest, AbortCallsTheHookThenAborts) {
    EXPECT_DEATH(
        {
            kz::SetLogSink(nullptr);
            kz::SetFatalHook(PrintingHook);
            kz::Abort("arena Test", "allocation overflow, 2 of 1 quadwords");
        },
        "hook: arena Test; allocation overflow, 2 of 1 quadwords; fatal");
}

TEST(KazarovLogDeathTest, FatalMarksUnimplementedCode) {
    EXPECT_DEATH(
        {
            kz::SetLogSink(nullptr);
            kz::SetFatalHook(PrintingHook);
            kz::Fatal("a.cpp:3", "Func: PlayStation 2 code is not implemented on PC", /*unimplemented=*/true);
            std::abort();
        },
        "hook: a.cpp:3; Func: PlayStation 2 code is not implemented on PC; unimplemented");
}

TEST(KazarovLogDeathTest, AFatalInsideTheHookDoesNotReenterIt) {
    EXPECT_EXIT(
        {
            kz::SetLogSink(nullptr);
            kz::SetFatalHook(ReenteringHook);
            kz::Abort("first", "failure");
        },
        testing::ExitedWithCode(3), "again: inside the hook");
}
