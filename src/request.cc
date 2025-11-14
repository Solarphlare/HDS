#include "request.h"
#include <vector>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <spanstream>
#include <iostream>
#include <algorithm>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <cstdint>
#include <fstream>
#include <cstring>
#include <memory>
#include "response.h"
#include "multipart.h"

server::request::request(SSL_CTX* ctx, const int client_fd) {
    this->ssl = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new(ctx), &SSL_free);
    SSL_set_fd(this->ssl.get(), client_fd);

    int accept_ret = SSL_accept(this->ssl.get());
    if (accept_ret <= 0) {
        int error = ERR_get_error();
        const char* reason = ERR_reason_error_string(error);
        std::cout << "SSL accept error: " << reason << std::endl;

        SSL_free(this->ssl.get());
        close(client_fd);
        throw std::runtime_error("SSL accept failed");
    }

    // Read until headers are complete or max request size reached
    constexpr size_t max_request_size = 18ULL * 1024 * 1024;
    std::vector<char> buffer(max_request_size);
    size_t used = 0;

    const char* headers_end_ptr = nullptr;
    while (!headers_end_ptr) {
        if (used == buffer.size()) {
            SSL_write(this->ssl.get(), "HTTP/1.1 413 Payload Too Large\r\n\r\n", 34);
            this->terminate();
            throw std::runtime_error("Request headers exceed limit");
        }

        int r = SSL_read(this->ssl.get(), buffer.data() + used, static_cast<int>(buffer.size() - used));
        if (r <= 0) {
            SSL_write(this->ssl.get(), "HTTP/1.1 500 Internal Server Error\r\n\r\n", 36);
            this->terminate();
            throw std::runtime_error("SSL read failed");
        }
        used += static_cast<size_t>(r);

        constexpr char sep[] = "\r\n\r\n";
        auto it = std::search(buffer.data(), buffer.data() + used, std::begin(sep), std::end(sep) - 1);
        headers_end_ptr = (it == buffer.data() + used) ? nullptr : it;
    }

    // Parse only what was read so far
    std::string_view request(buffer.data(), used);
    std::ispanstream stream(request);

    std::string request_line;
    if (!std::getline(stream, request_line) || request_line.empty() || request_line.back() != '\r') {
        SSL_write(this->ssl.get(), "HTTP/1.1 400 Bad Request\r\n\r\n", 28);
        this->terminate();
        throw std::runtime_error("Malformed HTTP request line");
    }

    std::istringstream rl(request_line);
    std::string method, version;
    rl >> method >> this->path >> version;

    if (method == "GET") {
        this->method = http_method::GET;
    }
    else if (method == "POST") {
        this->method = http_method::POST;
    }
    else if (method == "PUT") {
        this->method = http_method::PUT;
    }
    else if (method == "PATCH") {
        this->method = http_method::PATCH;
    }
    else if (method == "HEAD") {
        this->method = http_method::HEAD;
    }
    else if (method == "OPTIONS") {
        this->method = http_method::OPTIONS;
    }
    else if (method == "DELETE") {
        this->method = http_method::DELETE;
    }
    else {
        SSL_write(this->ssl.get(), "HTTP/1.1 400 Bad Request\r\n\r\n", 28);
        this->terminate();
    }

    std::string header_line;
    while (std::getline(stream, header_line) && !header_line.empty() && header_line != "\r") {
        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }

        size_t colon_pos = header_line.find(':');
        if (colon_pos == std::string::npos) {
            SSL_write(this->ssl.get(), "HTTP/1.1 400 Bad Request\r\n\r\n", 28);
            this->terminate();
            throw std::runtime_error("Malformed HTTP header line");
        }

        std::string key = header_line.substr(0, colon_pos);
        std::string value = header_line.substr(colon_pos + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(0, 1);
        }

        // make the first letter of the key uppercase and the rest lowercase
        // anything after a '-' should also be uppercase
        if (!key.empty()) {
            key[0] = std::toupper(key[0]);
            for (size_t i = 1; i < key.size(); i++) {
                if (key[i - 1] == '-') {
                    key[i] = std::toupper(key[i]);
                }
                else {
                    key[i] = std::tolower(key[i]);
                }
            }
        }

        this->headers[key] = value;
    }

    if (this->method == http_method::POST || this->method == http_method::PUT || this->method == http_method::PATCH) {
        // Require Content-Length and enforce 16 MiB cap
        uint64_t max_body_size = 16 * 1024 * 1024;

        if (this->headers.find("Content-Length") == this->headers.end()) {
            SSL_write(this->ssl.get(), "HTTP/1.1 411 Length Required\r\n\r\n", 32);
            this->terminate();
            throw std::runtime_error("Missing Content-Length");
        }

        uint64_t content_length = 0;
        try {
            content_length = std::stoull(this->headers["Content-Length"]);
        } catch (...) {
            SSL_write(this->ssl.get(), "HTTP/1.1 400 Bad Request\r\n\r\n", 28);
            this->terminate();
            throw std::runtime_error("Invalid Content-Length header");
        }

        if (content_length > max_body_size) {
            SSL_write(this->ssl.get(), "HTTP/1.1 413 Payload Too Large\r\n\r\n", 34);
            this->terminate();
            throw std::runtime_error("Payload too large");
        }

        // Locate header/body split and collect body
        const size_t header_end = static_cast<size_t>(headers_end_ptr - buffer.data());
        const size_t body_start = header_end + 4;

        this->body = std::vector<uint8_t>(static_cast<size_t>(content_length));

        const size_t in_buffer = used > body_start ? (used - body_start) : 0;
        const size_t to_copy = std::min(in_buffer, static_cast<size_t>(content_length));
        if (to_copy > 0) {
            std::memcpy(this->body->data(), buffer.data() + body_start, to_copy);
        }

        size_t filled = to_copy;
        while (filled < content_length) {
            int r = SSL_read(this->ssl.get(), reinterpret_cast<char*>(this->body->data() + filled), static_cast<int>(content_length - filled));

            if (r <= 0) {
                SSL_write(this->ssl.get(), "HTTP/1.1 500 Internal Server Error\r\n\r\n", 36);
                this->terminate();
                throw std::runtime_error("SSL read failed while reading body");
            }

            filled += static_cast<size_t>(r);

            if (header_end + 4 + filled > max_body_size) {
                SSL_write(this->ssl.get(), "HTTP/1.1 413 Payload Too Large\r\n\r\n", 34);
                this->terminate();
                throw std::runtime_error("Payload too large");
            }
        }
    }

    if (
        headers.find("Content-Type") != headers.end() &&
        headers["Content-Type"].starts_with("multipart/form-data; ") &&
        (this->method == http_method::POST || this->method == http_method::PUT || this->method == http_method::PATCH)
    ) {
        try {
            this->multipart_body = server::multipart_body(this->body.value(), this->headers);
            this->body.reset();
        }
        catch (const std::exception& e) {
            SSL_write(this->ssl.get(), "HTTP/1.1 400 Bad Request\r\n\r\n", 28);
            this->terminate();
            throw std::runtime_error("Malformed multipart body");
        }
    }
}

void server::request::respond(const response& res) const {
    std::ostringstream response_stream;
    response_stream << "HTTP/1.1 " << res.status_code << " \r\n";
    for (const auto& [key, value] : res.headers) {
        response_stream << key << ": " << value << "\r\n";
    }

    response_stream << "\r\n";
    std::string header_str = response_stream.str();

    SSL_write(this->ssl.get(), header_str.data(), header_str.size());
    SSL_write(this->ssl.get(), res.body.data(), res.body.size());
}

void server::request::terminate() {
    if (this->ssl) {
        SSL_shutdown(this->ssl.get());
        SSL_shutdown(this->ssl.get());

        int fd = SSL_get_fd(this->ssl.get());
        if (fd >= 0) { [[likely]]
            close(fd);
        }
    }
}
