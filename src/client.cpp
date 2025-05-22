#include "snl.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <print>
#include <string>

int main()
{
    const char* user_cstr = std::getenv("USER");
    assert(user_cstr && "could not find user env variable");
    const std::string user = user_cstr;
    snl::connect("127.0.0.1", 1234, [&](snl::connection& conn) {
        std::string in;
        auto parser = snl::parsing::message_parser_builder{}
                        .command("list")
                        .end([&](auto args) { conn.send("list"); })
                        .command("bal")
                        .end([&](auto args) { conn.send(std::format("bal {}", user)); })
                        .command("buy")
                        .parameter("ITEM")
                        .parameter("COUNT")
                        .end([&](auto args) { conn.send(std::format("buy {} {} {}", user, args[0], args[1])); })
                        .build();
        while (std::getline(std::cin, in)) {
            try {
                parser.parse(in);
                auto res = conn.recv();
                std::println("\e[0;34m{}\e[0m", res);
            } catch (const snl::parsing::parsing_exception& e) {
                std::println("{}", e.what());
            }
        }
    });
}