// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// Licensed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <catch2/catch.hpp>
#include <stdio.h>
#include <string.h>
#include <string>
#include <chrono>
#include <future>

#include "utils/span.h"
#include "utils/iosystem.h"
#include "utils/streams.h"
#include "utils/ioredirect.h"

#ifdef _WIN32
void usleep(unsigned long usec) { Sleep((usec+999)/1000); }
#else
#include <unistd.h>
#endif

using namespace netcoredbg;
template <typename T> using span = Utility::span<T>;

// TODO...
TEST_CASE("IORedirect::create")
{
    auto callback = [&](IORedirectHelper::StreamType stream, span<char> text)
    {
       (void)stream, (void)text; 
    };

    IORedirectHelper ior(
        { IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe() },
        callback );
}

TEST_CASE("IORedirect::exec releases child pipe ends")
{
    IORedirectHelper::Pipes pipes {
        IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe()
    };
    IORedirectHelper ior(pipes, [](IORedirectHelper::StreamType, span<char>) {});

    char buffer;
    auto read = IOSystem::async_read(std::get<IOSystem::Stdout>(pipes).first, &buffer, 1);
    REQUIRE(read);
    SECTION("successful invocation") { ior.exec([] {}); }
    SECTION("failed invocation")
    {
        CHECK_THROWS_AS(ior.exec([] { throw std::runtime_error("launch failed"); }), std::runtime_error);
    }

    bool completed = IOSystem::async_wait(&read, &read + 1, std::chrono::milliseconds(500));
    if (completed)
        IOSystem::async_result(read);
    else
        IOSystem::async_cancel(read);
    CHECK(completed);
}

TEST_CASE("IORedirect::finish releases a retained input reader")
{
    auto input = IOSystem::unnamed_pipe();
    InStream stream{InStreamBuf(input.first)};
    auto ior = std::make_shared<IORedirectHelper>(
        IORedirectHelper::Pipes{IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe()},
        [](IORedirectHelper::StreamType, span<char>) {});
    std::promise<IORedirectHelper::AsyncResult> completion;
    auto result = completion.get_future();
    std::thread reader([ior, &stream, &completion] {
        completion.set_value(ior->async_input(stream));
    });
    ior->finish();
    ior.reset();
    bool cancelled = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!cancelled)
        IOSystem::close(input.second);
    reader.join();
    if (cancelled)
        IOSystem::close(input.second);
    CHECK(cancelled);
    CHECK(result.get() == IORedirectHelper::AsyncResult::Canceled);
}

#ifdef WIN32
TEST_CASE("IORedirect::repeated sessions release their handles")
{
    bool launch = false;
    SECTION("unused helper") {}
    SECTION("launched helper") { launch = true; }
    DWORD before = 0, after = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (unsigned session = 0; session < 16; ++session)
    {
        IORedirectHelper ior(
            { IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe() },
            [](IORedirectHelper::StreamType, span<char>) {});
        if (launch)
            ior.exec([] {});
        ior.finish();
        ior.finish();
    }
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after));
    CHECK(after == before);
}
#endif

#if 0  // IORedirect::output function should be changed on async_input().
TEST_CASE("IORedirect::basic")
{
    static const char stdout_str[] = "OUTPUT OUTPUT OUTPUT\r\n";
    static const char stderr_str[] = "ERROR ERROR ERROR\r\n";
    static char const stdin_str[] = "INPUT INPUT INPUT\r\n";

    std::string stdout_res;
    std::string stderr_res;

    auto callback = [&](IORedirectHelper::StreamType stream, span<char> text)
    {
        REQUIRE((stream == IOSystem::Stdout || stream == IOSystem::Stderr));
        (stream == IOSystem::Stdout ? stdout_res : stderr_res).append(text.begin(), text.end());
    };

    auto test = [&]()
    {
        fputs(stdout_str, stdout), fflush(stdout);
        fputs(stderr_str, stderr), fflush(stderr);
        
        char buf[1024];
        memset(buf, 0, sizeof(buf));
        fgets(buf, sizeof(buf), stdin);
        CHECK(strlen(buf) == sizeof(stdin_str) - 1);
        CHECK(!strcmp(buf, stdin_str));
    };

    {
        IORedirectHelper ior(
            { IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe() },
            callback );

        ior.output(stdin_str, sizeof(stdin_str) - 1);
        ior.exec(test);

        usleep(300000);
    }

    CHECK(stdout_str == stdout_res);
    CHECK(stderr_str == stderr_res);
}
#endif
