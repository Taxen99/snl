/*
Copyright 2025 Maximilian Krig

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the “Software”), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
Simple Networking Library by Maximilian Krig

Much thanks to:
 - https://beej.us/guide/bgnet/html/
 - https://internalpointers.com/post/writing-custom-iterators-modern-cpp
*/

#include <arpa/inet.h>
#include <cassert>
#include <exception>
#include <format>
#include <functional>
#include <netdb.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace snl {

class connection_exception : public std::exception
{
public:
    explicit connection_exception(const std::string& message) : message(message) {}
    const char* what() const noexcept override { return message.c_str(); }

private:
    std::string message;
};

class connection_closed_exception : public connection_exception
{
public:
    explicit connection_closed_exception(const std::string& message) : connection_exception(message) {}

private:
};

class unknown_connection_exception : public connection_exception
{
public:
    explicit unknown_connection_exception(const std::string& message) : connection_exception(message) {}

private:
};

using connection_handler = std::function<void(struct connection&)>;

namespace detail {
inline std::string recv(int fd)
{
    constexpr size_t CHUNK_SIZE = 1024;
    int remaining;
    int ret = ::recv(fd, &remaining, sizeof(remaining), 0);
    if (ret == 0)
        throw connection_closed_exception{ "" };
    else if (ret != sizeof(remaining))
        throw unknown_connection_exception{ "" };
    std::string string{};
    string.resize(remaining);
    char* data = string.data();
    while (remaining > 0) {
        int recv = ::recv(fd, data, std::min((int)CHUNK_SIZE, (int)remaining), 0);
        if (recv == 0)
            throw connection_closed_exception{ "" };
        else if (recv == -1)
            throw unknown_connection_exception{ "" };
        remaining -= recv;
        data += recv;
    }
    return string;
}
}

struct connection
{
public:
    connection(const connection&) = delete;
    connection(connection&&) = delete;
    connection& operator=(const connection&) = delete;
    connection& operator=(connection&&) = delete;

    ~connection() = default;

    std::string recv() { return detail::recv(fd); }
    std::optional<std::string> try_recv()
    {
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(fd, &rfd);
        timeval timeout{};
        int ret = select(fd + 1, &rfd, NULL, NULL, &timeout);
        if (ret == -1) {
            throw unknown_connection_exception{ "" };
        } else if (ret == 0) {
            return {};
        } else if (FD_ISSET(fd, &rfd)) {
            return recv();
        } else {
            return {};
        }
    }
    void send(const std::string& data)
    {
        const char* p = data.data();
        int remaining = data.size();
        if (::send(fd, &remaining, sizeof(remaining), 0) != sizeof(remaining)) {
            throw unknown_connection_exception{ "" };
        }
        while (remaining > 0) {
            int sent = ::send(fd, p, remaining, 0);
            if (sent == -1)
                throw unknown_connection_exception{ "" };
            remaining -= sent;
            p += sent;
        }
    }

    struct connection_iterator_end_t
    {};
    struct connection_iterator
    {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = std::string;
        const value_type& operator*() const { return current_value; }
        const value_type* operator->() const { return &current_value; }

        connection_iterator& operator++()
        {
            current_value = detail::recv(fd);
            return *this;
        };
        void operator++(int) { ++*this; } // NOTE: this is some cursed bullshit, but chatgpt says its fine, so who cares

        bool operator==(connection_iterator_end_t) const
        {
            return false; // the iterator cannot reach an end
        }

    private:
        explicit connection_iterator(int fd) : fd(fd)
        {
            ++*this; // we make sure to read a value immediatly
        };
        int fd;
        std::string current_value = "THIS SHOULD NOT BE DISPLAYED";
        friend struct connection;
    };

    static_assert(std::input_iterator<connection_iterator>, "connection_iterator must be an input iterator");
    static_assert(std::sentinel_for<connection_iterator_end_t, connection_iterator>,
                  "connection_iterator_end must be a sentinel for connection_iterator");

    connection_iterator begin() { return connection_iterator{ fd }; }
    connection_iterator_end_t end() { return connection_iterator_end_t{}; }

private:
    connection(int fd) : fd(fd){};

    int fd;

    friend void serve(uint16_t, connection_handler);
    friend void connect(std::string, uint16_t, connection_handler);
};

inline void serve(uint16_t port, connection_handler handler)
{
    int sockfd, new_fd;
    struct addrinfo hints = { 0 };
    struct addrinfo* res;
    struct addrinfo* r;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, std::to_string(port).c_str(), &hints, &res) != 0) {
        throw unknown_connection_exception{ "" };
    }

    for (r = res; r != NULL; r = r->ai_next) {
        sockfd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (sockfd == -1) {
            throw unknown_connection_exception{ "" };
        }
        int yes = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            throw unknown_connection_exception{ "" };
        }
        if (bind(sockfd, r->ai_addr, r->ai_addrlen) == -1) {
            close(sockfd);
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (r == NULL) {
        throw unknown_connection_exception{ "" };
    }
#define BACKLOG 10
    if (listen(sockfd, BACKLOG) == -1) {
        throw unknown_connection_exception{ "" };
    }

    char s[INET6_ADDRSTRLEN];

    struct sockaddr_storage their_addr;
    std::vector<std::thread> threads;
    while (true) {
        socklen_t sin_size = sizeof(their_addr);
        new_fd = accept(sockfd, (struct sockaddr*)&their_addr, &sin_size);
        if (new_fd == -1) {
            continue;
        }

        inet_ntop(their_addr.ss_family, &(((struct sockaddr_in*)&their_addr)->sin_addr), s, sizeof(s));

        threads.emplace_back(
          [](connection_handler&& handler, int fd) {
              connection conn{ fd };
              try {
                  handler(conn);
              } catch (const connection_exception&) {
              }
              close(fd);
          },
          std::move(handler),
          new_fd);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    close(sockfd);
}

inline void connect(std::string addr, uint16_t port, connection_handler handler)
{
    int sockfd;
    struct addrinfo hints = { 0 };
    struct addrinfo* res;
    struct addrinfo* r;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(addr.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        throw unknown_connection_exception{ "" };
    }

    for (r = res; r != NULL; r = r->ai_next) {
        sockfd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (sockfd == -1) {
            throw unknown_connection_exception{ "" };
        }
        if (::connect(sockfd, r->ai_addr, r->ai_addrlen) == -1) {
            close(sockfd);
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (r == NULL) {
        throw unknown_connection_exception{ std::format("could not connect to {}:{}", addr, port) };
    }

    connection conn{ sockfd };
    try {
        handler(conn);
    } catch (const connection_exception&) {
    }

    close(sockfd);
}

namespace sync {

template<class T>
class lock
{
public:
    T* get() { return &data; }
    T* operator->() { return &data; }

private:
    lock(T& data, std::mutex& mtx) : data(data), guard(mtx) {}

    T& data;
    std::lock_guard<std::mutex> guard;

    template<class U>
    friend class safe;
};

template<class T>
class safe
{
public:
    ~safe() = default;
    template<typename... Args>
    safe(Args&&... args)
    {
        data = T(std::forward<Args>(args)...);
    }

    lock<T> lock() { return sync::lock(data, mtx); }

private:
    T data;
    std::mutex mtx;
};

}

namespace parsing {

using command_handler = std::function<void(std::span<std::string>)>;

namespace detail {
struct parameter
{
    parameter(const std::string& name) : name(name) {}
    std::string name;
};
struct command
{
    command() = default;
    command(std::vector<parameter>&& parameters) : parameters(parameters) {}
    std::vector<parameter> parameters;
    command_handler handler;
};
}

class parsing_exception : public std::exception
{
public:
    explicit parsing_exception(const std::string& message) : message(message) {}
    const char* what() const noexcept override { return message.c_str(); }

private:
    std::string message;
};

namespace detail {
template<class K, class V>
std::vector<K> get_map_keys(const std::unordered_map<K, V>& map)
{
    std::vector<K> keys;
    keys.reserve(map.size());
    for (auto& [k, v] : map) {
        keys.push_back(k);
    }
    return keys;
}
inline std::string concat_strings_formatted(const std::vector<std::string>& strings)
{
    std::string res = "[";
    for (auto& str : strings) {
        res += str + ',';
    }
    res[res.size() - 1] = ']';
    return res;
}
}

class message_parser
{
public:
    void parse(const std::string& msg) const
    {
        std::stringstream ss{ std::move(msg) };
        std::string str;
        auto cmd_names = detail::get_map_keys(commands);
        if (!((ss >> str) && commands.contains(str))) {
            throw parsing_exception{ std::format("expected command: {}", detail::concat_strings_formatted(cmd_names)) };
        }
        auto& cmd = commands.at(str);
        std::vector<std::string> args;
        for (auto& param : cmd.parameters) {
            if (!((ss >> str))) {
                throw parsing_exception{ std::format("expected parameter: {}", param.name) };
            }
            args.emplace_back(std::move(str));
        }
        if (ss >> str) {
            throw parsing_exception{ std::format("extraneous parameter: '{}'", str) };
        }
        cmd.handler(args);
    }

private:
    message_parser(std::unordered_map<std::string, detail::command>&& commands) : commands{ std::move(commands) } {}
    std::unordered_map<std::string, detail::command> commands;
    friend class message_parser_builder;
};

class message_parser_builder
{
public:
    message_parser_builder() = default;
    message_parser_builder(const message_parser_builder&) = default;
    message_parser_builder(message_parser_builder&&) = delete;
    message_parser_builder& operator=(const message_parser_builder&) = delete;
    message_parser_builder& operator=(message_parser_builder&&) = delete;
    ~message_parser_builder() = default;

    message_parser_builder& command(const std::string& name)
    {
        assert(!current_command);
        auto [it, inserted] = commands.try_emplace(name, detail::command{});
        assert(inserted == true);
        current_command = &it->second;
        return *this;
    }
    message_parser_builder& end(command_handler&& handler)
    {
        assert(current_command);
        current_command->handler = std::move(handler);
        current_command = nullptr;
        return *this;
    }
    message_parser_builder& parameter(const std::string& name)
    {
        assert(current_command);
        current_command->parameters.push_back({ name });
        return *this;
    }
    message_parser build() { return message_parser{ std::move(commands) }; }

private:
    std::unordered_map<std::string, detail::command> commands;
    struct detail::command* current_command = nullptr;
};

}

}