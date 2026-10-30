// SPDX-License-Identifier: proprietary
// hy_operator_resolve.cpp -- the offline operator tool's entry point (D5 slice R3b-2). Everything it does is in
// operator_resolution_tool.hpp and the headers under it; this file only connects them to the real process: the command line, the
// terminal, the clock and the credentials file. See docs/OPERATOR_RESOLUTION_DESIGN.md 6 for the procedure, and `--help` for the arguments.
//
// Deliberately NOT a production code path: it is built only with the demos (it needs Boost and OpenSSL for the authenticated REST client),
// the trading process never includes any of it, and test_operator_resolution_guard.cpp pins that the headers it uses are named by nothing
// else. It sends read-only, signed GETs and appends at most one frame to the audit log, after the operator has typed the confirmation.

#include <cstdio>
#include <string_view>
#include <vector>

#include <hengyuan/operator_resolution_real_exchange.hpp>
#include <hengyuan/operator_resolution_terminal.hpp>
#include <hengyuan/operator_resolution_tool.hpp>
#include <hengyuan/operator_resolution_transport.hpp>

int main(int argc, char** argv) {
    using namespace hy::operator_resolution;

    std::vector<std::string_view> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0u);
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    StdConsole out(stdout);
    StdConsole err(stderr);
    RealSleeper sleeper;
    RealExchangeProvider exchange;
    ToolEnvironment env{out, err, sleeper, exchange};
    return run_operator_tool(args, env);
}
