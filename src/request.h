#pragma once
#include <string>
#include <unordered_map>
#include <openssl/ssl.h>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory>
#include "response.h"
#include "multipart.h"

namespace server {
    enum class http_method {
        GET,
        POST,
        PUT,
        PATCH,
        DELETE,
        HEAD,
        OPTIONS,
        UNKNOWN
    };

    class request {
        private:
            std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, &SSL_free};
        public:
            http_method method;
            std::string path;
            std::unordered_map<std::string, std::string> headers;
            std::optional<std::vector<uint8_t>> body;
            std::optional<server::multipart_body> multipart_body;

            request() = default;

            request(SSL_CTX* ctx, const int client_fd);
            void respond(const response& response) const;
            void terminate();
        };
}
