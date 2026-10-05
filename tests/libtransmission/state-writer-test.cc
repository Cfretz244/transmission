// This file Copyright (C) 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <libtransmission/transmission.h>

#include <libtransmission/error.h>
#include <libtransmission/file-utils.h>
#include <libtransmission/file.h>
#include <libtransmission/state-writer.h>
#include <libtransmission/tr-strbuf.h>

#include "test-fixtures.h"

using namespace std::literals;

class StateWriterTest : public tr::test::SandboxedTest
{
protected:
    [[nodiscard]] std::optional<std::string> readFile(std::string_view filename) const
    {
        auto contents = std::vector<char>{};
        if (!tr_file_read(filename, contents))
        {
            return {};
        }
        return std::string{ std::data(contents), std::size(contents) };
    }
};

TEST_F(StateWriterTest, savesAndRemovesInQueueOrder)
{
    auto writer = tr::StateWriter{};
    auto const a = tr_pathbuf{ sandboxDir(), "/a.resume"sv };
    auto const b = tr_pathbuf{ sandboxDir(), "/b.resume"sv };

    writer.save(std::string{ a }, "first");
    writer.save(std::string{ a }, "second");
    writer.remove(std::string{ b }); // not there yet: not an error
    writer.save(std::string{ b }, "b");
    writer.save(std::string{ b }, "b2");
    writer.remove(std::string{ a });

    writer.flush(a);
    writer.flush(b);
    EXPECT_FALSE(tr_sys_path_exists(a));
    EXPECT_EQ("b2", readFile(b));
    EXPECT_EQ(0U, writer.pending());
}

TEST_F(StateWriterTest, flushReturnsOnceTheFileIsCurrent)
{
    auto writer = tr::StateWriter{};
    auto const a = tr_pathbuf{ sandboxDir(), "/a.resume"sv };

    for (int i = 0; i < 50; ++i)
    {
        writer.save(std::string{ a }, std::to_string(i));
    }
    writer.flush(a);
    EXPECT_EQ("49", readFile(a));

    // flushing a file nobody is writing returns at once
    writer.flush(tr_pathbuf{ sandboxDir(), "/nothing"sv });
}

TEST_F(StateWriterTest, reportsErrorsToTheCallback)
{
    auto writer = tr::StateWriter{};
    auto const missing_dir = tr_pathbuf{ sandboxDir(), "/no-such-dir/a.resume"sv };

    auto save_code = std::optional<int>{};
    writer.save(std::string{ missing_dir }, "x", [&save_code](tr_error const& error) { save_code = error.code(); });
    auto remove_code = std::optional<int>{};
    writer.remove(std::string{ missing_dir }, [&remove_code](tr_error const& error) { remove_code = error.code(); });
    auto ok_code = std::optional<int>{};
    writer.save(
        tr_pathbuf{ sandboxDir(), "/ok"sv }.c_str(),
        "x",
        [&ok_code](tr_error const& error) { ok_code = error.code(); });
    writer.shutdown();

    ASSERT_TRUE(save_code.has_value());
    EXPECT_EQ(ENOENT, *save_code);
    ASSERT_TRUE(remove_code.has_value());
    EXPECT_EQ(0, *remove_code) << "a missing file is not an error for remove()";
    ASSERT_TRUE(ok_code.has_value());
    EXPECT_EQ(0, *ok_code);
}

TEST_F(StateWriterTest, shutdownDrainsEverythingQueued)
{
    auto writer = tr::StateWriter{};
    auto files = std::vector<std::string>{};
    for (int i = 0; i < 200; ++i)
    {
        files.emplace_back(tr_pathbuf{ sandboxDir(), fmt::format("/{}.resume", i) });
        writer.save(files.back(), "x");
    }
    writer.shutdown();

    for (auto const& file : files)
    {
        EXPECT_TRUE(tr_sys_path_exists(file)) << file;
    }

    // after shutdown a job is refused, and the caller hears about it
    auto code = std::optional<int>{};
    writer.save(tr_pathbuf{ sandboxDir(), "/late"sv }.c_str(), "x", [&code](tr_error const& error) { code = error.code(); });
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(ECANCELED, *code);
    EXPECT_FALSE(tr_sys_path_exists(tr_pathbuf{ sandboxDir(), "/late"sv }));
}

TEST_F(StateWriterTest, destructorWithNothingQueuedDoesNotStartAThread)
{
    auto writer = tr::StateWriter{};
    EXPECT_EQ(0U, writer.pending());
}
