#include "snl.hpp"
#include <algorithm>
#include <cassert>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

struct item
{
    std::string name;
    uint64_t price;
};
struct user
{
    std::string name;
    uint64_t balance;
};
struct shop
{
    shop()
    {
        item item;
        std::ifstream listing{ "shop.listing" };
        while (listing >> item.name) {
            assert(listing >> item.price);
            items.emplace_back(std::move(item));
        }
        user user;
        std::ifstream bal{ "shop.bal" };
        while (bal >> user.name) {
            assert(bal >> user.balance);
            users.emplace_back(std::move(user));
        }
    }
    ~shop() = default;

    const std::vector<item>& list_items() const { return items; }
    std::optional<std::reference_wrapper<user>> get_user(const std::string& name)
    {
        auto it = std::find_if(users.begin(), users.end(), [&name](const user& user) { return user.name == name; });
        if (it != users.end()) {
            return *it;
        }
        return {};
    }
    std::optional<std::reference_wrapper<item>> get_item(const std::string& name)
    {
        auto it = std::find_if(items.begin(), items.end(), [&name](const item& item) { return item.name == name; });
        if (it != items.end()) {
            return *it;
        }
        return {};
    }

private:
    std::vector<item> items;
    std::vector<user> users;
};

int main()
{
    std::filesystem::current_path(WORKING_DIRECTORY);
    snl::sync::safe<shop> shop;
    snl::serve(1234, [&shop](snl::connection& conn) {
        auto parser = snl::parsing::message_parser_builder{}
                        .command("list")
                        .end([&](auto args) {
                            auto locked_shop = shop.lock();
                            std::stringstream ss;
                            for (auto& item : locked_shop->list_items()) {
                                ss << item.name << ' ' << item.price << '\n';
                            }
                            auto str = std::move(ss.str());
                            str.pop_back(); // remove last newline
                            conn.send(std::move(str));
                        })
                        .command("bal")
                        .parameter("USER")
                        .end([&](auto args) {
                            auto locked_shop = shop.lock();
                            auto user = locked_shop->get_user(args[0]);
                            if (user.has_value()) {
                                conn.send(std::format("{} {}", user.value().get().name, user.value().get().balance));
                            } else {
                                conn.send(std::format("user {} does not exist", args[0]));
                            }
                        })
                        .command("buy")
                        .parameter("USER")
                        .parameter("ITEM")
                        .parameter("COUNT")
                        .end([&](auto args) {
                            auto locked_shop = shop.lock();
                            auto user_res = locked_shop->get_user(args[0]);
                            if (!user_res.has_value()) {
                                conn.send(std::format("user '{}' does not exist", args[0]));
                                return;
                            }
                            user& user = user_res.value().get();
                            auto item_res = locked_shop->get_item(args[1]);
                            if (!item_res.has_value()) {
                                conn.send(std::format("item '{}' does not exist", args[1]));
                                return;
                            }
                            item& item = item_res.value().get();
                            uint64_t count;
                            try {
                                count = std::stoull(args[2]);
                            } catch (const std::exception&) {
                                conn.send(std::format("invalid cound '{}'", args[2]));
                                return;
                            }
                            uint64_t cost;
                            if (__builtin_mul_overflow(item.price, count, &cost)) {
                                conn.send(std::format("would overflow"));
                                return;
                            }
                            if (cost > user.balance) {
                                conn.send("insufficient balance");
                                return;
                            }
                            user.balance -= cost;
                            std::stringstream ss;
                            ss << std::format("{}x {} ordered\n", count, item.name);
                            ss << std::format("deducted {} from you balance (current balance: {})", cost, user.balance);
                            conn.send(std::move(ss.str()));
                        })
                        .build();
        for (auto& msg : conn) {
            try {
                parser.parse(msg);
            } catch (const snl::parsing::parsing_exception& e) {
                conn.send(e.what());
            }
        }
    });
}