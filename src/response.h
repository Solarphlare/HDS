#pragma once
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>

namespace server {
    class response {
        private:
            std::unordered_map<std::string, std::string> headers;
            std::vector<uint8_t> body;
        public:
            friend class request;
            int status_code;
            response(int status, const std::string& body, const std::string& content_type);
            response(int status, const std::vector<uint8_t>& body, const std::string& content_type);
            void set_header(const std::string& key, const std::string& value);
    };
}
