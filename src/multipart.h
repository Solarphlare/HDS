#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace server {
    class multipart_element {
        public:
            std::string name;
            std::optional<std::string> filename;
            std::string content_type;
            std::vector<uint8_t> data;

            multipart_element(std::string&& name, std::optional<std::string>&& filename, std::string&& content_type, std::vector<uint8_t>&& data) :
                name(std::move(name)),
                filename(std::move(filename)),
                content_type(std::move(content_type)),
                data(std::move(data))
            {}
    };

    class multipart_body {
        private:
            std::string boundary;
        public:
            std::vector<multipart_element> elements;
            multipart_body(std::vector<uint8_t>& body, std::unordered_map<std::string, std::string>& req_headers);
    };
}
